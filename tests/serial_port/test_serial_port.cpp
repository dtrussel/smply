// SPDX-License-Identifier: Apache-2.0
//
// SerialPortTransport against real operating-system objects.
//
// On POSIX each case opens a pseudo-terminal: the adapter gets the slave end,
// exactly as it would get /dev/ttyACM0, and the test plays the device on the
// master end, framing and deframing with the same portable helpers the stub
// device uses. That makes this the first place a serial byte crosses a real
// tty: the line discipline, non-blocking I/O, poll(), and the hang-up a vanished
// USB port produces.
//
// What it cannot show is a device. Framing agreement with Zephyr is
// test_serial_framing.cpp's job, against a transcription of the C; a whole
// update over a pty is examples/serial_dfu's ctests.

#include "serial_port/serial_link.hpp"
#include "serial_port/serial_port_config.hpp"
#include "serial_port/serial_port_transport.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"
#include "smply/util/dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h> // NOLINT(modernize-deprecated-headers) -- posix_openpt, grantpt: POSIX, not <cstdlib>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#endif

using smply::ConstBytes;
using smply::Dispatcher;
using smply::Error;
using smply::ErrorCode;
using smply::Result;
using smply::TransportListener;
using smply::transport::SerialPortConfig;
using smply::transport::SerialPortTransport;

namespace {

using namespace std::chrono_literals;

/// Long enough never to fire on a loaded CI runner, short enough that a hung
/// case fails the run instead of stalling it.
constexpr auto kPatience = 5s;

/// Records everything the transport reports, on the client context.
class Recorder final : public TransportListener
{
public:
    void on_bytes(ConstBytes bytes) override
    {
        received.emplace_back(bytes.begin(), bytes.end());
    }

    void on_transport_error(Error error) override
    {
        errors.push_back(error);
    }

    void on_disconnected(Error error) override
    {
        disconnects.push_back(error);
    }

    std::vector<std::vector<std::byte>> received;
    std::vector<Error> errors;
    std::vector<Error> disconnects;
};

/// A client context: a dispatcher whose wake-up the test can wait on.
class ClientContext
{
public:
    ClientContext()
        : inbound{[this] {
              {
                  const std::lock_guard<std::mutex> lock{mutex_};
                  woken_ = true;
              }
              wake_.notify_one();
          }}
    {}

    /// Drains until \p done holds, or `kPatience` passes. \return done().
    [[nodiscard]] bool pump_until(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (true) {
            inbound.drain();
            if (done()) {
                return true;
            }
            std::unique_lock<std::mutex> lock{mutex_};
            if (!wake_.wait_until(lock, deadline, [this] { return woken_; })) {
                return done();
            }
            woken_ = false;
        }
    }

    Dispatcher inbound;

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool woken_ = false;
};

} // namespace

TEST_CASE("an invalid configuration is refused before any port is touched", "[serial_port]")
{
    ClientContext context;
    SerialPortConfig config;
    config.path = "";
    const auto opened = SerialPortTransport::open(config, context.inbound);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a port that does not exist is Disconnected, so a reconnect loop retries it",
          "[serial_port]")
{
    ClientContext context;
    SerialPortConfig config;
#ifdef _WIN32
    config.path = "COM255";
#else
    config.path = "/dev/smply-no-such-port";
#endif
    const auto opened = SerialPortTransport::open(config, context.inbound);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code() == ErrorCode::Disconnected);
}

#ifndef _WIN32

namespace {

/// The device's end of a pseudo-terminal. The adapter opens `slave_path`.
class PtyDevice
{
public:
    PtyDevice()
    {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        REQUIRE(master_ >= 0);
        REQUIRE(::grantpt(master_) == 0);
        REQUIRE(::unlockpt(master_) == 0);
        std::array<char, 128> name{};
        REQUIRE(::ptsname_r(master_, name.data(), name.size()) == 0);
        slave_path = name.data();
        REQUIRE(::fcntl(master_, F_SETFL, O_NONBLOCK) == 0);
    }

    PtyDevice(const PtyDevice&) = delete;
    PtyDevice& operator=(const PtyDevice&) = delete;
    PtyDevice(PtyDevice&&) = delete;
    PtyDevice& operator=(PtyDevice&&) = delete;

    ~PtyDevice()
    {
        hang_up();
    }

    /// Closes the master, which is what a USB CDC port does when the device
    /// resets: the slave sees a hang-up and reads fail with EIO.
    void hang_up() noexcept
    {
        if (master_ >= 0) {
            static_cast<void>(::close(master_));
            master_ = -1;
        }
    }

    void write_all(const std::vector<std::byte>& bytes) const
    {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t put = ::write(master_, bytes.data() + done, bytes.size() - done);
            if (put > 0) {
                done += static_cast<std::size_t>(put);
                continue;
            }
            pollfd writable{master_, POLLOUT, 0};
            REQUIRE(::poll(&writable, 1, 1000) > 0);
        }
    }

    /// Reads and deframes until one packet arrives or patience runs out.
    [[nodiscard]] std::optional<std::vector<std::byte>> read_packet()
    {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        std::optional<std::vector<std::byte>> packet;
        while (!packet.has_value() && std::chrono::steady_clock::now() < deadline) {
            pollfd readable{master_, POLLIN, 0};
            if (::poll(&readable, 1, 100) <= 0) {
                continue;
            }
            std::array<std::byte, 512> buffer{};
            const ssize_t got = ::read(master_, buffer.data(), buffer.size());
            if (got <= 0) {
                continue;
            }
            rx_.feed(ConstBytes{buffer.data(), static_cast<std::size_t>(got)},
                     [&packet](ConstBytes p) { packet.emplace(p.begin(), p.end()); });
        }
        return packet;
    }

    std::string slave_path;

private:
    int master_ = -1;
    smply::transport::SerialInbound rx_;
};

/// Only the POSIX cases send anything, and an unreferenced internal function
/// is an error under MSVC /W4 /WX (C4505), so it lives here.
[[nodiscard]] std::vector<std::byte> message_of(std::size_t size)
{
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 13U + 1U) & 0xFFU);
    }
    return out;
}

[[nodiscard]] std::unique_ptr<SerialPortTransport> open_on(const PtyDevice& device,
                                                           ClientContext& context)
{
    SerialPortConfig config;
    config.path = device.slave_path;
    Result<std::unique_ptr<SerialPortTransport>> opened =
        SerialPortTransport::open(config, context.inbound);
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

[[nodiscard]] std::vector<std::byte> text(const std::string& line)
{
    std::vector<std::byte> out;
    for (const char c : line) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

} // namespace

TEST_CASE("a pseudo-terminal opens at any baud rate, as a CDC ACM port does", "[serial_port]")
{
    for (const std::uint32_t baud : {9600U, 115200U, 921600U}) {
        CAPTURE(baud);
        const PtyDevice device;
        ClientContext context;
        SerialPortConfig config;
        config.path = device.slave_path;
        config.baud = baud;
        const auto opened = SerialPortTransport::open(config, context.inbound);
        REQUIRE(opened.has_value());
        CHECK((*opened)->max_message_size() == config.max_message_size);
    }
}

TEST_CASE("a path that is not a tty is refused as InvalidArgument", "[serial_port]")
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "smply_serial_port_not_a_tty.bin";
    {
        std::ofstream{file} << "x";
    }

    ClientContext context;
    SerialPortConfig config;
    config.path = file.string();
    const auto opened = SerialPortTransport::open(config, context.inbound);
    std::filesystem::remove(file);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a sent message crosses the tty as console frames", "[serial_port]")
{
    PtyDevice device;
    ClientContext context;
    Recorder recorder;
    const auto transport = open_on(device, context);
    transport->set_listener(&recorder);

    for (const std::size_t size : {std::size_t{8}, std::size_t{200}, std::size_t{256}}) {
        CAPTURE(size);
        const std::vector<std::byte> message = message_of(size);
        REQUIRE(transport->send(ConstBytes{message}).has_value());
        const std::optional<std::vector<std::byte>> seen = device.read_packet();
        REQUIRE(seen.has_value());
        CHECK(*seen == message);
    }

    const auto counters = transport->counters();
    CHECK(counters.bytes_written > 0);
    CHECK(counters.send.refused == 0);
    CHECK(recorder.errors.empty());
    CHECK(recorder.disconnects.empty());
}

TEST_CASE("an idle link stays up after traffic", "[serial_port]")
{
    // The regression test for VMIN: with VMIN 0 an empty read answered 0, the
    // adapter took it for end of file, and every exchange was followed by a
    // spurious hang-up that the other cases were too quick to notice.
    PtyDevice device;
    ClientContext context;
    Recorder recorder;
    const auto transport = open_on(device, context);
    transport->set_listener(&recorder);

    const std::vector<std::byte> message = message_of(40);
    device.write_all(smply::transport::frame_message(ConstBytes{message}));
    REQUIRE(context.pump_until([&] { return recorder.received.size() == 1; }));
    REQUIRE(transport->send(ConstBytes{message}).has_value());
    REQUIRE(device.read_packet().has_value());

    std::this_thread::sleep_for(300ms);
    context.inbound.drain();
    CHECK(recorder.disconnects.empty());
    CHECK(transport->send(ConstBytes{message}).has_value());
}

TEST_CASE("send refuses what it cannot carry", "[serial_port]")
{
    const PtyDevice device;
    ClientContext context;
    const auto transport = open_on(device, context);

    const Result<void> empty = transport->send(ConstBytes{});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code() == ErrorCode::InvalidArgument);

    const std::vector<std::byte> big = message_of(transport->max_message_size() + 1);
    const Result<void> too_big = transport->send(ConstBytes{big});
    REQUIRE_FALSE(too_big.has_value());
    CHECK(too_big.error().code() == ErrorCode::MessageTooLarge);
}

TEST_CASE("inbound frames reach the listener through the dispatcher, noise counted",
          "[serial_port]")
{
    const PtyDevice device;
    ClientContext context;
    Recorder recorder;
    const auto transport = open_on(device, context);
    transport->set_listener(&recorder);

    const std::vector<std::byte> first = message_of(40);
    const std::vector<std::byte> second = message_of(300);

    std::vector<std::byte> stream = text("*** Booting Zephyr OS ***\r\n");
    const std::vector<std::byte> a = smply::transport::frame_message(ConstBytes{first});
    stream.insert(stream.end(), a.begin(), a.end());
    const std::vector<std::byte> log = text(std::string(300, 'L') + "\n");
    stream.insert(stream.end(), log.begin(), log.end());
    const std::vector<std::byte> b = smply::transport::frame_message(ConstBytes{second});
    stream.insert(stream.end(), b.begin(), b.end());
    device.write_all(stream);

    REQUIRE(context.pump_until([&] { return recorder.received.size() == 2; }));
    CHECK(recorder.received[0] == first);
    CHECK(recorder.received[1] == second);

    const auto counters = transport->counters();
    CHECK(counters.deframe.packets == 2);
    CHECK(counters.deframe.ignored == 1);
    CHECK(counters.deframe.framing_errors == 0);
    CHECK(counters.dropped_lines == 1);
    CHECK(counters.bytes_read == stream.size());
}

TEST_CASE("a hang-up is one on_disconnected, and then nothing", "[serial_port]")
{
    PtyDevice device;
    ClientContext context;
    Recorder recorder;
    const auto transport = open_on(device, context);
    transport->set_listener(&recorder);

    device.hang_up();
    REQUIRE(context.pump_until([&] { return !recorder.disconnects.empty(); }));
    CHECK(recorder.disconnects.size() == 1);
    CHECK(recorder.disconnects[0].code() == ErrorCode::Disconnected);

    const std::vector<std::byte> message = message_of(16);
    const Result<void> after = transport->send(ConstBytes{message});
    REQUIRE_FALSE(after.has_value());
    CHECK(after.error().code() == ErrorCode::Disconnected);

    transport->close(); // already closed by the disconnect; a no-op
    context.inbound.drain();
    CHECK(recorder.disconnects.size() == 1);
    CHECK(recorder.received.empty());
}

TEST_CASE("close is idempotent and silences work already queued", "[serial_port]")
{
    const PtyDevice device;
    ClientContext context;
    Recorder recorder;
    const auto transport = open_on(device, context);
    transport->set_listener(&recorder);

    const std::vector<std::byte> message = message_of(20);
    device.write_all(smply::transport::frame_message(ConstBytes{message}));

    // Wait until the I/O thread has posted the packet, but do not drain: the
    // closure is sitting in the dispatcher. pending() is racy by design; here
    // it can only grow, which is all a wait needs.
    const auto deadline = std::chrono::steady_clock::now() + kPatience;
    while (context.inbound.pending() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(context.inbound.pending() == 1);

    transport->close();
    transport->close();
    CHECK(context.inbound.drain() >= 1); // it ran, and delivered nothing
    CHECK(recorder.received.empty());
    CHECK(recorder.disconnects.empty());

    const Result<void> after = transport->send(ConstBytes{message});
    REQUIRE_FALSE(after.has_value());
    CHECK(after.error().code() == ErrorCode::Disconnected);
}

TEST_CASE("a medium that stops draining answers TransportBusy, and close still returns",
          "[serial_port]")
{
    const PtyDevice device; // never read, so the pty's buffer fills
    ClientContext context;
    const auto transport = open_on(device, context);

    const std::vector<std::byte> message = message_of(transport->max_message_size());
    bool busy = false;
    for (int i = 0; i < 100000 && !busy; ++i) {
        const Result<void> sent = transport->send(ConstBytes{message});
        if (!sent.has_value()) {
            REQUIRE(sent.error().code() == ErrorCode::TransportBusy);
            busy = true;
        }
    }
    CHECK(busy);
    // Only `refused` is certain. `deferred` may be zero: the message a
    // StartWriter admits waits in the queue until the I/O thread takes it, so
    // a third offer can be refused before any second one was ever deferred.
    CHECK(transport->counters().send.refused >= 1);

    transport->close(); // must not wait for a write that cannot finish
}

#endif // !_WIN32
