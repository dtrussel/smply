// SPDX-License-Identifier: Apache-2.0

/// \file
/// `SerialPortTransport` over POSIX: `termios` to configure the port, one I/O
/// thread running `poll()` over the port and a wake-up pipe.
///
/// `win32/serial_port_win32.cpp` mirrors this file section for section, so read
/// the two side by side: this one runs in CI against a pseudo-terminal, and
/// that one can only be compiled there.

#include "serial_port/serial_port_transport.hpp"

#include "serial_port/posix/posix_detail.hpp"
#include "serial_port/serial_link.hpp"
#include "serial_port/serial_port_config.hpp"

#include "common/link_state.hpp"
#include "common/send_queue.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"
#include "smply/util/dispatcher.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace smply::transport {

/// Everything the adapter owns, split by which thread may touch what.
struct SerialPortTransport::State
{
    // --- client context only, no synchronisation needed ---------------------

    TransportListener* listener = nullptr;
    LinkState link;
    std::size_t max_message = 0;

    // --- set once by open(), read-only afterwards ----------------------------

    /// Owned by the application; carries closures to the client context.
    Dispatcher* inbound = nullptr;
    int fd = -1;
    /// The self-pipe that wakes `poll()`: a message is waiting, or stop.
    int wake_read = -1;
    int wake_write = -1;

    // --- shared between the client context and the I/O thread ---------------

    /// Cleared the moment the link stops accepting work -- by `close()`, or by
    /// the I/O thread when the port fails. Read by `send()` so a message
    /// offered after the port has gone is refused rather than queued for a
    /// thread that has exited.
    std::atomic<bool> accepting{true};
    /// Asks the I/O thread to exit.
    std::atomic<bool> stop{false};

    /// Guards `outbound` and `stats`. Never held across a system call that can
    /// block, and never held by the client context while it joins the thread.
    mutable std::mutex mutex;
    SendQueue outbound;
    SerialLinkCounters stats;

    std::thread io;

    // --- the I/O thread only --------------------------------------------------

    SerialInbound rx;
    /// The framed message being written, and how much of it has gone.
    std::vector<std::byte> writing;
    std::size_t written = 0;
};

namespace {

using State = SerialPortTransport::State;

/// Closes a descriptor that may already be closed. `close()` failing leaves
/// nothing to recover: the descriptor is released either way on Linux and
/// macOS, and retrying risks closing one another thread has just opened.
void close_fd(int& fd) noexcept
{
    if (fd >= 0) {
        static_cast<void>(::close(fd));
        fd = -1;
    }
}

/// Nudges the I/O thread out of `poll()`. A full pipe already holds a wake-up,
/// so `EAGAIN` is success.
void wake(const State& state) noexcept
{
    const std::byte token{1};
    // Bound to a name rather than cast to void: glibc marks write() as
    // warn_unused_result under _FORTIFY_SOURCE, which a Release build turns
    // on, and GCC does not accept a void cast as using the result.
    const ssize_t ignored = ::write(state.wake_write, &token, 1);
    static_cast<void>(ignored);
}

/// Stops the thread, joins it, and closes the port. Client context; idempotent.
///
/// The order is the contract (design.md section 13): nothing may be delivered
/// once this begins, so `begin_close()` comes first and disarms every closure
/// already queued. It never drains or clears the `Dispatcher`, which belongs to
/// the application (handoff.md, "The Dispatcher belongs to the application").
void shutdown(State& state) noexcept
{
    if (!state.link.begin_close()) {
        return;
    }
    state.accepting.store(false, std::memory_order_release);
    state.stop.store(true, std::memory_order_release);
    if (state.wake_write >= 0) {
        wake(state);
    }
    if (state.io.joinable()) {
        state.io.join();
    }
    {
        const std::lock_guard<std::mutex> lock{state.mutex};
        state.outbound.discard_queued();
    }
    close_fd(state.fd);
    close_fd(state.wake_read);
    close_fd(state.wake_write);
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

/// Queues the end of the link for the client context. It closes the link
/// before telling the listener, so "no callback after `on_disconnected()`"
/// (smply/transport.hpp) holds by construction.
///
/// Every failure this adapter sees ends the link: a port that cannot be read or
/// written will not start working on the next `poll()`, and a message abandoned
/// part-way cannot be resumed. So there is no `on_transport_error()` path.
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

/// True for the errno of a non-blocking call that would have blocked. POSIX
/// allows `EAGAIN` and `EWOULDBLOCK` to differ; Linux and macOS make them
/// equal, and comparing against both there is a GCC `-Wlogical-op` error.
[[nodiscard]] constexpr bool would_block(int error_number) noexcept
{
#if EAGAIN == EWOULDBLOCK
    return error_number == EAGAIN;
#else
    return error_number == EAGAIN || error_number == EWOULDBLOCK;
#endif
}

/// Takes the next framed message for the writer, if there is one. Releases the
/// writer's claim when there is none (`SendQueue::next_for_writer()`).
void take_next(State& state)
{
    const std::lock_guard<std::mutex> lock{state.mutex};
    std::optional<std::vector<std::byte>> next = state.outbound.next_for_writer();
    if (next.has_value()) {
        state.writing = std::move(*next);
        state.written = 0;
    }
}

/// Everything read so far, turned into packets and posted. \return false when
/// the port has failed and the thread should stop.
[[nodiscard]] bool read_port(const std::shared_ptr<State>& state)
{
    std::array<std::byte, 512> buffer{};
    for (;;) {
        const ssize_t got = ::read(state->fd, buffer.data(), buffer.size());
        if (got > 0) {
            const auto count = static_cast<std::size_t>(got);
            // Counted before anything is posted, so a packet the listener has
            // seen is always one counters() already shows.
            std::vector<std::vector<std::byte>> packets;
            state->rx.feed(ConstBytes{buffer.data(), count}, [&packets](ConstBytes packet) {
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
            continue;
        }
        if (got == 0) {
            // End of file on a tty: the other end hung up.
            post_lost(state, Error{ErrorCode::Disconnected, "serial_port: the port hung up"});
            return false;
        }
        const int error_number = errno;
        if (would_block(error_number)) {
            return true;
        }
        if (error_number == EINTR) {
            continue;
        }
        // EIO is what a vanished USB port and a closed pseudo-terminal master
        // both answer, and it maps to Disconnected; anything else is reported
        // as a transport error, but still ends the link -- a port that cannot
        // be read will not start working on the next poll.
        post_lost(state, posix::error_from_errno(error_number, "serial_port: a read failed"));
        return false;
    }
}

/// Writes as much of the current message as the port takes. \return false when
/// the port has failed and the thread should stop.
[[nodiscard]] bool write_port(const std::shared_ptr<State>& state)
{
    while (state->written < state->writing.size()) {
        const std::size_t left = state->writing.size() - state->written;
        const ssize_t put = ::write(state->fd, state->writing.data() + state->written, left);
        if (put > 0) {
            const auto count = static_cast<std::size_t>(put);
            state->written += count;
            const std::lock_guard<std::mutex> lock{state->mutex};
            state->stats.bytes_written += count;
            continue;
        }
        const int error_number = errno;
        if (put < 0 && would_block(error_number)) {
            return true; // the port's buffer is full; poll() says when it drains
        }
        if (put < 0 && error_number == EINTR) {
            continue;
        }
        // A message abandoned part-way leaves the device holding a partial
        // packet; the next message's first frame carries a fresh start marker,
        // which makes the device discard it (protocol-notes section 8). So a
        // write failure ends the link rather than carrying on and hoping.
        post_lost(state, posix::error_from_errno(error_number, "serial_port: a write failed"));
        return false;
    }
    state->writing.clear();
    state->written = 0;
    return true;
}

/// Drains the wake-up pipe, so the next `poll()` sleeps again.
void drain_wake(const State& state) noexcept
{
    std::array<std::byte, 64> sink{};
    while (::read(state.wake_read, sink.data(), sink.size()) > 0) {
    }
}

/// The I/O thread. Exits on `stop`, or after reporting that the port failed.
void run(const std::shared_ptr<State>& state)
{
    for (;;) {
        if (state->stop.load(std::memory_order_acquire)) {
            return;
        }
        if (state->writing.empty()) {
            take_next(*state);
        }

        std::array<pollfd, 2> fds{};
        fds[0].fd = state->fd;
        fds[0].events = static_cast<short>(state->writing.empty() ? POLLIN : (POLLIN | POLLOUT));
        fds[1].fd = state->wake_read;
        fds[1].events = POLLIN;

        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            post_lost(state, Error{ErrorCode::TransportError, "serial_port: poll failed"});
            break;
        }

        if ((fds[1].revents & POLLIN) != 0) {
            drain_wake(*state);
        }
        const short port = fds[0].revents;
        if ((port & POLLIN) != 0 && !read_port(state)) {
            break;
        }
        if ((port & POLLOUT) != 0 && !write_port(state)) {
            break;
        }
        if ((port & POLLIN) == 0 && (port & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            // Hung up with nothing left to read. With POLLIN also set, the
            // read above has already reported it, from the read's own errno.
            post_lost(state, Error{ErrorCode::Disconnected, "serial_port: the port hung up"});
            break;
        }
    }
    // Whatever made the thread stop, nothing more will be written.
    state->accepting.store(false, std::memory_order_release);
}

/// Opens the wake-up pipe, non-blocking at both ends.
[[nodiscard]] Result<void> open_wake_pipe(State& state)
{
    std::array<int, 2> ends{-1, -1};
    if (::pipe(ends.data()) != 0) {
        return fail(ErrorCode::TransportError, "serial_port: cannot create the wake-up pipe");
    }
    state.wake_read = ends[0];
    state.wake_write = ends[1];
    for (const int end : ends) {
        if (::fcntl(end, F_SETFL, O_NONBLOCK) != 0 || ::fcntl(end, F_SETFD, FD_CLOEXEC) != 0) {
            return fail(ErrorCode::TransportError, "serial_port: cannot configure the pipe");
        }
    }
    return {};
}

/// Opens and configures the port: 8N1, raw, non-blocking, flushed.
[[nodiscard]] Result<void> open_port(State& state, const SerialPortConfig& config)
{
    const std::optional<speed_t> speed = posix::speed_for_baud(config.baud);
    if (!speed.has_value()) {
        return fail(ErrorCode::InvalidArgument, "serial_port: this platform cannot set that rate");
    }

    state.fd = ::open(config.path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (state.fd < 0) {
        return fail(posix::error_from_errno(errno, "serial_port: cannot open the port"));
    }

    termios settings{};
    if (::tcgetattr(state.fd, &settings) != 0) {
        return fail(posix::error_from_errno(errno, "serial_port: not a serial port"));
    }
    ::cfmakeraw(&settings);
    settings.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | CSTOPB);
    settings.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD);
#ifdef CRTSCTS
    // Through a constant rather than a cast: CRTSCTS is already unsigned on
    // Linux, where GCC calls the cast useless, and a plain int elsewhere.
    constexpr tcflag_t kRtsCts = CRTSCTS;
    settings.c_cflag &= ~kRtsCts;
    if (config.flow == FlowControl::RtsCts) {
        settings.c_cflag |= kRtsCts;
    }
#else
    if (config.flow == FlowControl::RtsCts) {
        return fail(ErrorCode::InvalidArgument, "serial_port: no RTS/CTS on this platform");
    }
#endif
    // VMIN 1, not 0. With VMIN and VTIME both 0, Linux answers a read of an
    // empty tty with 0 even under O_NONBLOCK, which is indistinguishable from
    // end of file: the first version of this adapter reported a hang-up
    // after every read. With VMIN 1 an empty non-blocking read is EAGAIN, and
    // 0 means what it says. poll() does the waiting either way.
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    if (::cfsetispeed(&settings, *speed) != 0 || ::cfsetospeed(&settings, *speed) != 0) {
        return fail(ErrorCode::InvalidArgument, "serial_port: this platform cannot set that rate");
    }
    if (::tcsetattr(state.fd, TCSANOW, &settings) != 0) {
        return fail(posix::error_from_errno(errno, "serial_port: cannot configure the port"));
    }
    // Only one process may usefully hold a console (handoff.md, the bench).
    // Best effort: a pseudo-terminal or an old driver may not support it, and
    // failing the open for that would refuse a working port.
    static_cast<void>(::ioctl(state.fd, TIOCEXCL));
    // Bytes left over from before this open -- boot output, half a frame --
    // must not be read as the start of the first response.
    static_cast<void>(::tcflush(state.fd, TCIOFLUSH));
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

    Result<void> opened = open_wake_pipe(*state);
    if (opened.has_value()) {
        opened = open_port(*state, config);
    }
    if (!opened.has_value()) {
        // No thread yet, so this only closes what was opened.
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

    // Framed here, on the client context, and before the queue is offered
    // anything: the message is borrowed for this call only, and framing it is
    // the copy smply/transport.hpp asks a deferring transport to make.
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
    return {}; // LCOV_EXCL_LINE -- every enumerator is handled above
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
