// SPDX-License-Identifier: Apache-2.0

/// \file
/// `SerialPortTransport` over Win32: `CreateFile` and a `DCB` to configure the
/// port, one I/O thread waiting on overlapped reads and writes and a wake-up
/// event.
///
/// **This file mirrors `posix/serial_port_posix.cpp` section for section**, and
/// should be reviewed against it: that one runs in CI against a
/// pseudo-terminal; this one is compiled by the `windows-msvc` job and has
/// never opened a real port (ADR-0020). It is outside clang-tidy and cppcheck,
/// like `transports/winrt_ble/`, because neither can see a Windows translation
/// unit from the Linux build they run in; MSVC `/W4 /WX` stands in for them.

#include "serial_port/serial_port_transport.hpp"

#include "serial_port/serial_link.hpp"
#include "serial_port/serial_port_config.hpp"

#include "common/link_state.hpp"
#include "common/send_queue.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"
#include "smply/util/dispatcher.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace smply::transport {

/// Everything the adapter owns, split by which thread may touch what. The same
/// fields as the POSIX `State`, with handles for descriptors.
struct SerialPortTransport::State
{
    // --- client context only, no synchronisation needed ---------------------

    TransportListener* listener = nullptr;
    LinkState link;
    std::size_t max_message = 0;

    // --- set once by open(), read-only afterwards ----------------------------

    Dispatcher* inbound = nullptr;
    HANDLE port = INVALID_HANDLE_VALUE;
    /// Auto-reset: wakes the I/O thread for a waiting message, or for stop.
    HANDLE wake = nullptr;
    /// Manual-reset, one per overlapped direction.
    HANDLE read_done = nullptr;
    HANDLE write_done = nullptr;

    // --- shared between the client context and the I/O thread ---------------

    std::atomic<bool> accepting{true};
    std::atomic<bool> stop{false};

    mutable std::mutex mutex;
    SendQueue outbound;
    SerialLinkCounters stats;

    std::thread io;

    // --- the I/O thread only --------------------------------------------------

    SerialInbound rx;
    std::vector<std::byte> writing;
    std::size_t written = 0;
    /// Overlapped buffers must outlive the operation, so they live here rather
    /// than on the thread's stack.
    std::array<std::byte, 512> read_buffer{};
    OVERLAPPED read_op{};
    OVERLAPPED write_op{};
    bool read_pending = false;
    bool write_pending = false;
};

namespace {

using State = SerialPortTransport::State;

void close_handle(HANDLE& handle, HANDLE empty) noexcept
{
    if (handle != empty && handle != nullptr) {
        static_cast<void>(::CloseHandle(handle));
        handle = empty;
    }
}

void wake(const State& state) noexcept
{
    static_cast<void>(::SetEvent(state.wake));
}

/// What a failed Win32 call means to a `Transport` caller while **opening**.
/// Once open, every failure ends the link and is reported as `Disconnected`,
/// because a vanished USB CDC port answers with several different codes
/// (`ERROR_ACCESS_DENIED`, `ERROR_GEN_FAILURE`, `ERROR_DEVICE_REMOVED`) and
/// none of them means the port will work again.
[[nodiscard]] Error open_error(DWORD code, const char* where) noexcept
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DEVICE_REMOVED:
        return Error{ErrorCode::Disconnected, where};
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NOT_SUPPORTED:
        return Error{ErrorCode::InvalidArgument, where};
    default:
        return Error{ErrorCode::TransportError, where};
    }
}

/// Stops the thread, joins it, and closes the port. Client context; idempotent.
/// The same order as the POSIX `shutdown()`, and for the same reasons.
void shutdown(State& state) noexcept
{
    if (!state.link.begin_close()) {
        return;
    }
    state.accepting.store(false, std::memory_order_release);
    state.stop.store(true, std::memory_order_release);
    if (state.wake != nullptr) {
        wake(state);
    }
    if (state.io.joinable()) {
        state.io.join();
    }
    {
        const std::lock_guard<std::mutex> lock{state.mutex};
        state.outbound.discard_queued();
    }
    close_handle(state.port, INVALID_HANDLE_VALUE);
    close_handle(state.wake, nullptr);
    close_handle(state.read_done, nullptr);
    close_handle(state.write_done, nullptr);
    state.listener = nullptr;
    state.link.finish_close();
}

/// Queues one inbound packet for the client context. It arrives already
/// copied out of the deframer, whose own view dies at its next line (the
/// borrowed-buffer rule), and the closure takes ownership of that copy.
void post_packet(const std::shared_ptr<State>& state, std::vector<std::byte> packet)
{
    state->inbound->post([state, owned = std::move(packet)] {
        if (!state->link.may_deliver() || state->listener == nullptr) {
            return;
        }
        state->listener->on_bytes(ConstBytes{owned});
    });
}

void post_lost(const std::shared_ptr<State>& state, const Error& error)
{
    state->inbound->post([state, error] {
        if (!state->link.may_deliver()) {
            return;
        }
        TransportListener* listener = state->listener;
        shutdown(*state);
        if (listener != nullptr) {
            listener->on_disconnected(error);
        }
    });
}

void take_next(State& state)
{
    const std::lock_guard<std::mutex> lock{state.mutex};
    std::optional<std::vector<std::byte>> next = state.outbound.next_for_writer();
    if (next.has_value()) {
        state.writing = std::move(*next);
        state.written = 0;
    }
}

/// Issues the next overlapped read. \return false when the port has failed.
[[nodiscard]] bool start_read(State& state) noexcept
{
    state.read_op = OVERLAPPED{};
    state.read_op.hEvent = state.read_done;
    if (::ReadFile(state.port, state.read_buffer.data(),
                   static_cast<DWORD>(state.read_buffer.size()), nullptr, &state.read_op) == 0 &&
        ::GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    // Completed or pending, the event is signalled when the result is ready,
    // so both are collected the same way.
    state.read_pending = true;
    return true;
}

/// Issues an overlapped write of what is left of the current message.
[[nodiscard]] bool start_write(State& state) noexcept
{
    const std::size_t left = state.writing.size() - state.written;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(left, 0x10000U));
    state.write_op = OVERLAPPED{};
    state.write_op.hEvent = state.write_done;
    if (::WriteFile(state.port, state.writing.data() + state.written, chunk, nullptr,
                    &state.write_op) == 0 &&
        ::GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    state.write_pending = true;
    return true;
}

/// Collects a finished read and posts what it completed.
[[nodiscard]] bool finish_read(const std::shared_ptr<State>& state)
{
    DWORD got = 0;
    state->read_pending = false;
    if (::GetOverlappedResult(state->port, &state->read_op, &got, FALSE) == 0) {
        return false;
    }
    if (got > 0) {
        const auto count = static_cast<std::size_t>(got);
        // Counted before anything is posted, as on POSIX: a packet the listener
        // has seen is always one counters() already shows.
        std::vector<std::vector<std::byte>> packets;
        state->rx.feed(ConstBytes{state->read_buffer.data(), count}, [&packets](ConstBytes packet) {
            packets.emplace_back(packet.begin(), packet.end());
        });
        {
            const std::lock_guard<std::mutex> lock{state->mutex};
            state->stats.bytes_read += count;
            state->stats.deframe = state->rx.deframe_counters();
            state->stats.dropped_lines = state->rx.dropped_lines();
        }
        for (std::vector<std::byte>& packet : packets) {
            post_packet(state, std::move(packet));
        }
    }
    // A zero-byte completion is the read timeout expiring with nothing to
    // read (see the COMMTIMEOUTS in open_port()), not an end of file.
    return true;
}

[[nodiscard]] bool finish_write(State& state)
{
    DWORD put = 0;
    state.write_pending = false;
    if (::GetOverlappedResult(state.port, &state.write_op, &put, FALSE) == 0) {
        return false;
    }
    state.written += static_cast<std::size_t>(put);
    {
        const std::lock_guard<std::mutex> lock{state.mutex};
        state.stats.bytes_written += put;
    }
    if (state.written >= state.writing.size()) {
        state.writing.clear();
        state.written = 0;
    }
    return true;
}

/// Cancels whatever is outstanding and waits for it, so no operation can
/// write into `State` after the thread has returned.
void cancel_pending(State& state) noexcept
{
    if (!state.read_pending && !state.write_pending) {
        return;
    }
    static_cast<void>(::CancelIoEx(state.port, nullptr));
    DWORD ignored = 0;
    if (state.read_pending) {
        static_cast<void>(::GetOverlappedResult(state.port, &state.read_op, &ignored, TRUE));
        state.read_pending = false;
    }
    if (state.write_pending) {
        static_cast<void>(::GetOverlappedResult(state.port, &state.write_op, &ignored, TRUE));
        state.write_pending = false;
    }
}

/// The I/O thread. Exits on `stop`, or after reporting that the port failed.
void run(const std::shared_ptr<State>& state)
{
    const Error lost{ErrorCode::Disconnected, "serial_port: the port failed or went away"};
    if (!start_read(*state)) {
        post_lost(state, lost);
        state->accepting.store(false, std::memory_order_release);
        return;
    }
    for (;;) {
        if (state->stop.load(std::memory_order_acquire)) {
            break;
        }
        if (state->writing.empty()) {
            take_next(*state);
        }
        if (!state->writing.empty() && !state->write_pending && !start_write(*state)) {
            post_lost(state, lost);
            break;
        }

        std::array<HANDLE, 3> handles{state->wake, state->read_done, state->write_done};
        const DWORD count = state->write_pending ? 3U : 2U;
        const DWORD woke = ::WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE);
        if (woke == WAIT_FAILED) {
            post_lost(state, Error{ErrorCode::TransportError, "serial_port: wait failed"});
            break;
        }

        // Check each direction rather than trusting the index: several may be
        // ready at once, and WaitForMultipleObjects reports only the first.
        if (state->read_pending && ::WaitForSingleObject(state->read_done, 0) == WAIT_OBJECT_0) {
            if (!finish_read(state) || !start_read(*state)) {
                post_lost(state, lost);
                break;
            }
        }
        if (state->write_pending && ::WaitForSingleObject(state->write_done, 0) == WAIT_OBJECT_0) {
            if (!finish_write(*state)) {
                post_lost(state, lost);
                break;
            }
        }
    }
    cancel_pending(*state);
    state->accepting.store(false, std::memory_order_release);
}

[[nodiscard]] Result<void> open_events(State& state)
{
    state.wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state.read_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state.write_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state.wake == nullptr || state.read_done == nullptr || state.write_done == nullptr) {
        return fail(ErrorCode::TransportError, "serial_port: cannot create the I/O events");
    }
    return {};
}

/// `COM4` becomes `\\.\COM4`, which is the only spelling that works for
/// `COM10` and above. A path already in that form is used as it is.
[[nodiscard]] std::wstring device_name(const std::string& path)
{
    std::wstring name;
    if (path.rfind("\\\\.\\", 0) != 0) {
        name = L"\\\\.\\";
    }
    for (const char c : path) {
        name.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return name;
}

/// Opens and configures the port: 8N1, binary, no software flow control,
/// timeouts that make an overlapped read return as soon as any byte arrives.
[[nodiscard]] Result<void> open_port(State& state, const SerialPortConfig& config)
{
    state.port = ::CreateFileW(device_name(config.path).c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, // exclusive: only one process may hold a console
                               nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (state.port == INVALID_HANDLE_VALUE) {
        return fail(open_error(::GetLastError(), "serial_port: cannot open the port"));
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (::GetCommState(state.port, &dcb) == 0) {
        return fail(open_error(::GetLastError(), "serial_port: not a serial port"));
    }
    dcb.BaudRate = config.baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    // Constants assigned in branches, not a conditional expression: these are
    // one- and two-bit fields, and a computed value narrowed into one is the
    // kind of conversion /W4 reports.
    if (config.flow == FlowControl::RtsCts) {
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
    } else {
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
    }
    dcb.fOutxDsrFlow = FALSE;
    // DTR asserted, as a POSIX open does: some USB CDC ACM firmware waits for
    // it before sending anything.
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (::SetCommState(state.port, &dcb) == 0) {
        return fail(open_error(::GetLastError(), "serial_port: cannot configure the port"));
    }

    // MAXDWORD / MAXDWORD / a constant is the documented combination for "return
    // as soon as anything has arrived, or after the constant with nothing".
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 1000;
    if (::SetCommTimeouts(state.port, &timeouts) == 0) {
        return fail(open_error(::GetLastError(), "serial_port: cannot set the port's timeouts"));
    }
    static_cast<void>(
        ::PurgeComm(state.port, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT));
    return {};
}

} // namespace

// --- SerialPortTransport -----------------------------------------------------

SerialPortTransport::SerialPortTransport(std::shared_ptr<State> state) noexcept
    : state_{std::move(state)}
{}

SerialPortTransport::~SerialPortTransport()
{
    close();
}

Result<std::unique_ptr<SerialPortTransport>>
SerialPortTransport::open(const SerialPortConfig& config, Dispatcher& inbound)
{
    if (const Result<void> valid = validate(config); !valid.has_value()) {
        return fail(valid.error());
    }

    auto state = std::make_shared<State>();
    state->inbound = &inbound;
    state->max_message = config.max_message_size;

    Result<void> opened = open_events(*state);
    if (opened.has_value()) {
        opened = open_port(*state, config);
    }
    if (!opened.has_value()) {
        shutdown(*state);
        return fail(opened.error());
    }

    state->io = std::thread{[state] { run(state); }};
    return std::make_unique<SerialPortTransport>(std::move(state));
}

Result<void> SerialPortTransport::send(ConstBytes message)
{
    if (!state_->link.may_send() || !state_->accepting.load(std::memory_order_acquire)) {
        return fail(ErrorCode::Disconnected, "serial_port: the link is closed");
    }
    if (message.empty()) {
        return fail(ErrorCode::InvalidArgument, "serial_port: an empty SMP message");
    }
    if (message.size() > state_->max_message) {
        return fail(ErrorCode::MessageTooLarge, "serial_port: beyond the configured cap");
    }

    std::vector<std::byte> framed = frame_message(message);

    Admission admission = Admission::Busy;
    {
        const std::lock_guard<std::mutex> lock{state_->mutex};
        admission = state_->outbound.offer(std::move(framed));
    }
    switch (admission) {
    case Admission::Busy:
        return fail(ErrorCode::TransportBusy, "serial_port: two messages are already outbound");
    case Admission::StartWriter:
        wake(*state_);
        return {};
    case Admission::Queued:
        return {};
    }
    return {};
}

std::size_t SerialPortTransport::max_message_size() const noexcept
{
    return state_->max_message;
}

void SerialPortTransport::set_listener(TransportListener* listener) noexcept
{
    state_->listener = listener;
}

void SerialPortTransport::close() noexcept
{
    shutdown(*state_);
}

SerialLinkCounters SerialPortTransport::counters() const
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    SerialLinkCounters out = state_->stats;
    out.send = state_->outbound.counters();
    return out;
}

} // namespace smply::transport
