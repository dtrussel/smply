// SPDX-License-Identifier: Apache-2.0
//
// The serial port adapter's pure parts: configuration checking, the byte-level
// link helpers both platform implementations share, and -- on POSIX -- the
// baud table and errno mapping. Nothing here opens a port; the adapter over a
// real tty is tests/component/test_serial_port_pty.cpp.
//
// The framing itself is test_serial_framing.cpp's, against a transcription of
// Zephyr's C. What is tested here is only the glue: that `frame_message()`
// produces what `SerialFramer` does, and that `SerialInbound` keeps the counts
// an adapter reports.

#include "serial_port/serial_link.hpp"
#include "serial_port/serial_port_config.hpp"

#include "serial/serial_framing.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include "serial_port/posix/posix_detail.hpp"

#include <termios.h>

#include <cerrno>
#endif

using smply::ConstBytes;
using smply::ErrorCode;
using smply::Result;
using smply::transport::FlowControl;
using smply::transport::frame_message;
using smply::transport::is_supported_baud;
using smply::transport::kDefaultSerialMessageSize;
using smply::transport::kMaxFrame;
using smply::transport::kMaxSerialPacket;
using smply::transport::kMinSerialMessageSize;
using smply::transport::SerialFramer;
using smply::transport::SerialInbound;
using smply::transport::SerialPortConfig;
using smply::transport::validate;

namespace {

[[nodiscard]] SerialPortConfig good_config()
{
    SerialPortConfig config;
    config.path = "/dev/ttyACM0";
    return config;
}

[[nodiscard]] ErrorCode rejection(const SerialPortConfig& config)
{
    const Result<void> checked = validate(config);
    REQUIRE_FALSE(checked.has_value());
    return checked.error().code();
}

[[nodiscard]] std::vector<std::byte> message_of(std::size_t size)
{
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 7U + 3U) & 0xFFU);
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> text(std::string_view line)
{
    std::vector<std::byte> out;
    for (const char c : line) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// Everything `SerialInbound` delivers from \p stream, fed in pieces of \p cut.
[[nodiscard]] std::vector<std::vector<std::byte>>
packets_from(SerialInbound& inbound, const std::vector<std::byte>& stream, std::size_t cut)
{
    std::vector<std::vector<std::byte>> out;
    for (std::size_t at = 0; at < stream.size(); at += cut) {
        const std::size_t n = std::min(cut, stream.size() - at);
        inbound.feed(ConstBytes{stream.data() + at, n},
                     [&out](ConstBytes packet) { out.emplace_back(packet.begin(), packet.end()); });
    }
    return out;
}

} // namespace

// --- configuration -----------------------------------------------------------

TEST_CASE("the default configuration is 115200 baud, no flow control, 256 bytes",
          "[serial_port][config]")
{
    const SerialPortConfig config;
    CHECK(config.baud == 115200);
    CHECK(config.flow == FlowControl::None);
    CHECK(config.max_message_size == kDefaultSerialMessageSize);
    CHECK(kDefaultSerialMessageSize == 256);
    // Below the largest message a default Zephyr netbuf (384) accepts over
    // serial, once the length prefix and CRC share it (protocol-notes A25).
    CHECK(kDefaultSerialMessageSize <= 384 - 4);
    CHECK(validate(good_config()).has_value());
}

TEST_CASE("a configuration without a path is refused", "[serial_port][config]")
{
    SerialPortConfig config = good_config();
    config.path.clear();
    CHECK(rejection(config) == ErrorCode::InvalidArgument);
}

TEST_CASE("only the standard baud rates are accepted", "[serial_port][config]")
{
    for (const std::uint32_t baud :
         {1200U, 9600U, 57600U, 115200U, 230400U, 460800U, 921600U, 1000000U}) {
        CAPTURE(baud);
        CHECK(is_supported_baud(baud));
        SerialPortConfig config = good_config();
        config.baud = baud;
        CHECK(validate(config).has_value());
    }
    for (const std::uint32_t baud : {0U, 1U, 110U, 115201U, 250000U, 4000000U}) {
        CAPTURE(baud);
        CHECK_FALSE(is_supported_baud(baud));
        SerialPortConfig config = good_config();
        config.baud = baud;
        CHECK(rejection(config) == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("max_message_size is bounded by the floor and by the length field",
          "[serial_port][config]")
{
    SerialPortConfig config = good_config();

    config.max_message_size = kMinSerialMessageSize;
    CHECK(validate(config).has_value());
    config.max_message_size = kMinSerialMessageSize - 1;
    CHECK(rejection(config) == ErrorCode::InvalidArgument);

    config.max_message_size = kMaxSerialPacket;
    CHECK(validate(config).has_value());
    config.max_message_size = kMaxSerialPacket + 1;
    CHECK(rejection(config) == ErrorCode::InvalidArgument);

    // Never the frame size: a message spans as many frames as it needs.
    CHECK(kDefaultSerialMessageSize > smply::transport::kMaxRawPerFrame);
}

// --- frame_message -----------------------------------------------------------

TEST_CASE("frame_message is SerialFramer's frames, concatenated", "[serial_port][link]")
{
    for (const std::size_t size : {std::size_t{1}, std::size_t{90}, std::size_t{183},
                                   std::size_t{184}, std::size_t{256}, std::size_t{1000}}) {
        CAPTURE(size);
        const std::vector<std::byte> message = message_of(size);

        std::vector<std::byte> expected;
        SerialFramer framer{ConstBytes{message}};
        std::array<std::byte, kMaxFrame> frame{};
        while (!framer.done()) {
            const std::size_t n = framer.next_frame(frame);
            expected.insert(expected.end(), frame.begin(),
                            frame.begin() + static_cast<std::ptrdiff_t>(n));
        }

        CHECK(frame_message(ConstBytes{message}) == expected);
    }
}

TEST_CASE("frame_message yields nothing for an empty or oversized message", "[serial_port][link]")
{
    CHECK(frame_message(ConstBytes{}).empty());
    const std::vector<std::byte> huge = message_of(kMaxSerialPacket + 1);
    CHECK(frame_message(ConstBytes{huge}).empty());
}

// --- SerialInbound -----------------------------------------------------------

TEST_CASE("SerialInbound delivers a packet however the reads are cut", "[serial_port][link]")
{
    const std::vector<std::byte> message = message_of(300);
    const std::vector<std::byte> stream = frame_message(ConstBytes{message});

    for (const std::size_t cut :
         {std::size_t{1}, std::size_t{7}, std::size_t{127}, std::size_t{512}}) {
        CAPTURE(cut);
        SerialInbound inbound;
        const auto packets = packets_from(inbound, stream, cut);
        REQUIRE(packets.size() == 1);
        CHECK(packets[0] == message);
        CHECK(inbound.deframe_counters().packets == 1);
        CHECK(inbound.deframe_counters().framing_errors == 0);
        CHECK(inbound.dropped_lines() == 0);
    }
}

TEST_CASE("SerialInbound counts console noise, and drops over-long lines", "[serial_port][link]")
{
    const std::vector<std::byte> message = message_of(40);
    const std::vector<std::byte> framed = frame_message(ConstBytes{message});

    std::vector<std::byte> stream = text("*** Booting Zephyr OS build v4.1.0 ***\r\n");
    const std::vector<std::byte> long_line = text(std::string(200, 'x') + "\n");
    stream.insert(stream.end(), long_line.begin(), long_line.end());
    stream.insert(stream.end(), framed.begin(), framed.end());
    const std::vector<std::byte> prompt = text("uart:~$ \n");
    stream.insert(stream.end(), prompt.begin(), prompt.end());

    SerialInbound inbound;
    const auto packets = packets_from(inbound, stream, 16);
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == message);
    CHECK(inbound.deframe_counters().ignored == 2);
    CHECK(inbound.dropped_lines() == 1);
}

TEST_CASE("SerialInbound reports a corrupted packet as a CRC failure", "[serial_port][link]")
{
    const std::vector<std::byte> message = message_of(40);
    std::vector<std::byte> stream = frame_message(ConstBytes{message});
    // One base64 character changed to another valid one: decodable, wrong CRC.
    stream[10] = stream[10] == std::byte{'A'} ? std::byte{'B'} : std::byte{'A'};

    SerialInbound inbound;
    CHECK(packets_from(inbound, stream, 64).empty());
    CHECK(inbound.deframe_counters().crc_failures == 1);
}

TEST_CASE("SerialInbound::reset discards a partial packet but keeps the counts",
          "[serial_port][link]")
{
    const std::vector<std::byte> message = message_of(300);
    const std::vector<std::byte> stream = frame_message(ConstBytes{message});

    SerialInbound inbound;
    // The first frame and part of the second, as a port closed mid-message.
    const std::vector<std::byte> head{stream.begin(), stream.begin() + 150};
    CHECK(packets_from(inbound, head, 150).empty());
    inbound.reset();

    // The tail alone must not complete anything: its first line is a
    // continuation with nothing in progress.
    const std::vector<std::byte> tail{stream.begin() + 150, stream.end()};
    CHECK(packets_from(inbound, tail, 64).empty());
    CHECK(inbound.deframe_counters().framing_errors >= 1);

    // And a whole packet after that is delivered normally.
    const auto packets = packets_from(inbound, stream, 64);
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == message);
    CHECK(inbound.deframe_counters().packets == 1);
}

// --- POSIX-only pure functions ------------------------------------------------

#ifndef _WIN32

TEST_CASE("every rate the portable table accepts that POSIX defines maps", "[serial_port][posix]")
{
    using smply::transport::posix::speed_for_baud;
    CHECK(speed_for_baud(9600) == B9600);
    CHECK(speed_for_baud(115200) == B115200);
    CHECK(speed_for_baud(230400) == B230400);
    CHECK_FALSE(speed_for_baud(0).has_value());
    CHECK_FALSE(speed_for_baud(115201).has_value());
    for (const std::uint32_t baud : {1200U, 2400U, 4800U, 19200U, 38400U, 57600U}) {
        CAPTURE(baud);
        CHECK(speed_for_baud(baud).has_value());
    }
#ifdef B921600
    CHECK(speed_for_baud(460800) == B460800);
    CHECK(speed_for_baud(921600) == B921600);
    CHECK(speed_for_baud(1000000) == B1000000);
#endif
}

TEST_CASE("errno maps onto the transport contract", "[serial_port][posix]")
{
    using smply::transport::posix::error_from_errno;
    for (const int gone : {ENOENT, ENODEV, ENXIO, EIO}) {
        CAPTURE(gone);
        CHECK(error_from_errno(gone, "test").code() == ErrorCode::Disconnected);
    }
    CHECK(error_from_errno(ENOTTY, "test").code() == ErrorCode::InvalidArgument);
    for (const int refused : {EACCES, EBUSY, EPERM, EINVAL}) {
        CAPTURE(refused);
        CHECK(error_from_errno(refused, "test").code() == ErrorCode::TransportError);
    }
    CHECK(std::string{error_from_errno(EIO, "where").where()} == "where");
}

#endif
