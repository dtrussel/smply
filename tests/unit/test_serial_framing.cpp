// SPDX-License-Identifier: Apache-2.0
//
// MCUmgr's serial (console) framing: the CRC, the base64, the encoder, the
// line reader and the decoder. None of it is platform-specific, which is the
// whole reason it is in `transports/serial/` and not inside an adapter -- this
// directory is unit-tested, linted and coverage-measured on every CI job,
// while a port that opens a tty would get none of that.
//
// Two oracles here are deliberately **not** this repository's code, because a
// checker built out of the thing under test proves only that it agrees with
// itself (the same discipline as test_ble_framing.cpp's hand-written hex
// parser):
//
//   * the CRC is checked against the published CRC-16/XMODEM check value and
//     base64 against RFC 4648's vectors;
//   * `SerialPeer` below is a direct transcription of Zephyr's
//     `mcumgr_serial_tx_pkt()` and `mcumgr_serial_process_frag()` (S19),
//     written from the C and sharing no code with the headers. The encoder is
//     required to be **byte-identical** to it, and the decoder is required to
//     accept what it produces.

#include "serial/base64.hpp"
#include "serial/crc16.hpp"
#include "serial/serial_framing.hpp"

#include "smply/bytes.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using smply::ConstBytes;
using smply::MutBytes;
using smply::transport::crc16_xmodem;
using smply::transport::kFragmentMarker;
using smply::transport::kMaxBase64PerFrame;
using smply::transport::kMaxFrame;
using smply::transport::kMaxRawPerFrame;
using smply::transport::kPacketMarker;
using smply::transport::LineSplitter;
using smply::transport::SerialDeframer;
using smply::transport::SerialFramer;
using smply::transport::detail::base64_decode;
using smply::transport::detail::base64_encode;
using smply::transport::detail::base64_encoded_size;

namespace {

using Outcome = SerialDeframer::Outcome;

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char value : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return out;
}

[[nodiscard]] std::string text_of(ConstBytes data)
{
    std::string out;
    out.reserve(data.size());
    for (const std::byte value : data) {
        out.push_back(static_cast<char>(value));
    }
    return out;
}

/// A packet whose every byte is distinguishable, so a misordered walk shows up
/// as wrong *content* rather than as a wrong length.
[[nodiscard]] std::vector<std::byte> counted(std::size_t size)
{
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 37 + 11) & 0xFFU);
    }
    return out;
}

/// Every frame the encoder produces for \p packet, as strings.
[[nodiscard]] std::vector<std::string> frames_of(ConstBytes packet)
{
    std::vector<std::string> frames;
    std::array<std::byte, kMaxFrame> buffer{};
    SerialFramer framer{packet};
    while (!framer.done()) {
        const std::size_t written = framer.next_frame(buffer);
        REQUIRE(written != 0);
        frames.emplace_back(text_of(ConstBytes{buffer.data(), written}));
    }
    return frames;
}

// --- The independent reference ----------------------------------------------

/// Zephyr's own serial framing, transcribed from `serial_util.c`.
///
/// Shares nothing with `transports/serial/` -- its own base64, its own CRC,
/// its own loop structure, `int` arithmetic and all. That is the point: it is
/// the thing the module is required to agree with, and an agreement between
/// two expressions of the same C is worth far more than a round trip through
/// one implementation.
class SerialPeer
{
public:
    /// `mcumgr_serial_tx_pkt()`.
    [[nodiscard]] static std::vector<std::string> transmit(const std::vector<std::uint8_t>& data)
    {
        std::vector<std::string> frames;
        const int len = static_cast<int>(data.size());
        const std::uint16_t crc = crc_itu_t(0, data.data(), len);
        std::uint16_t marker = 0x0609;
        bool first = true;
        bool last = false;
        int src_off = 0;
        std::array<std::uint8_t, 3> raw{};

        while (src_off < len) {
            int max_input = ((127 - 3) >> 2) * 3;
            std::string frame;
            frame.push_back(static_cast<char>((marker >> 8U) & 0xFFU));
            frame.push_back(static_cast<char>(marker & 0xFFU));

            if (first) {
                const auto declared = static_cast<std::uint16_t>(len + 2);
                raw[0] = static_cast<std::uint8_t>(declared >> 8U);
                raw[1] = static_cast<std::uint8_t>(declared & 0xFFU);
                raw[2] = data[0];
                encode(raw.data(), 3, frame);
                ++src_off;
                max_input -= 3;
            }

            int to_process = std::min(max_input, len - src_off);
            const int reminder = max_input - (len - src_off);
            if (reminder == 0 || reminder == 1) {
                to_process -= 1;
                last = false;
            } else if (reminder >= 2) {
                last = true;
            }

            while (to_process >= 3) {
                encode(&data[static_cast<std::size_t>(src_off)], 3, frame);
                src_off += 3;
                to_process -= 3;
            }

            if (last) {
                switch (len - src_off) {
                case 0:
                    raw[0] = static_cast<std::uint8_t>((crc & 0xFF00U) >> 8U);
                    raw[1] = static_cast<std::uint8_t>(crc & 0x00FFU);
                    encode(raw.data(), 2, frame);
                    break;
                case 1:
                    raw[0] = data[static_cast<std::size_t>(src_off++)];
                    raw[1] = static_cast<std::uint8_t>((crc & 0xFF00U) >> 8U);
                    raw[2] = static_cast<std::uint8_t>(crc & 0x00FFU);
                    encode(raw.data(), 3, frame);
                    break;
                default:
                    raw[0] = data[static_cast<std::size_t>(src_off++)];
                    raw[1] = data[static_cast<std::size_t>(src_off++)];
                    raw[2] = static_cast<std::uint8_t>((crc & 0xFF00U) >> 8U);
                    encode(raw.data(), 3, frame);
                    raw[0] = static_cast<std::uint8_t>(crc & 0x00FFU);
                    encode(raw.data(), 1, frame);
                    break;
                }
            }

            frame.push_back('\n');
            frames.push_back(frame);
            marker = 0x0414;
            first = false;
        }
        return frames;
    }

    /// `mcumgr_serial_process_frag()`, minus the net_buf bookkeeping.
    ///
    /// \return the decoded packet once one is complete.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> process(std::string_view frame)
    {
        if (frame.size() < 2) {
            return std::nullopt;
        }
        const auto op = static_cast<std::uint16_t>((static_cast<unsigned char>(frame[0]) << 8U) |
                                                   static_cast<unsigned char>(frame[1]));
        if (op == 0x0609) {
            buffer_.clear();
        } else if (op == 0x0414) {
            if (buffer_.empty()) {
                buffer_.clear();
                return std::nullopt;
            }
        } else {
            return std::nullopt; // Note: partial state is NOT discarded.
        }

        if (!decode(frame.substr(2), buffer_)) {
            buffer_.clear();
            return std::nullopt;
        }
        if (op == 0x0609) {
            if (buffer_.size() < 2) {
                buffer_.clear();
                return std::nullopt;
            }
            pkt_len_ = static_cast<std::uint16_t>((buffer_[0] << 8U) | buffer_[1]);
            buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
            if (pkt_len_ <= 2) {
                buffer_.clear();
                return std::nullopt;
            }
        }
        if (buffer_.size() < pkt_len_) {
            return std::nullopt;
        }
        if (buffer_.size() > pkt_len_ ||
            crc_itu_t(0, buffer_.data(), static_cast<int>(buffer_.size())) != 0) {
            buffer_.clear();
            return std::nullopt;
        }
        buffer_.resize(buffer_.size() - 2);
        return std::exchange(buffer_, {});
    }

private:
    static void encode(const std::uint8_t* data, int len, std::string& out)
    {
        static constexpr std::string_view kAlphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[0]) << 16U) |
                                     (len > 1 ? static_cast<std::uint32_t>(data[1]) << 8U : 0U) |
                                     (len > 2 ? static_cast<std::uint32_t>(data[2]) : 0U);
        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        out.push_back(len > 1 ? kAlphabet[(triple >> 6U) & 0x3FU] : '=');
        out.push_back(len > 2 ? kAlphabet[triple & 0x3FU] : '=');
    }

    [[nodiscard]] static bool decode(std::string_view in, std::vector<std::uint8_t>& out)
    {
        static constexpr std::string_view kAlphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        if ((in.size() % 4) != 0) {
            return false;
        }
        for (std::size_t i = 0; i < in.size(); i += 4) {
            std::uint32_t quad = 0;
            int produced = 3;
            for (std::size_t j = 0; j < 4; ++j) {
                if (in[i + j] == '=') {
                    produced = static_cast<int>(j) - 1;
                    quad <<= (4 - j) * 6U;
                    break;
                }
                const std::size_t value = kAlphabet.find(in[i + j]);
                if (value == std::string_view::npos) {
                    return false;
                }
                quad = (quad << 6U) | static_cast<std::uint32_t>(value);
            }
            for (int k = 0; k < produced; ++k) {
                out.push_back(static_cast<std::uint8_t>(
                    (quad >> (16U - 8U * static_cast<unsigned>(k))) & 0xFFU));
            }
        }
        return true;
    }

    [[nodiscard]] static std::uint16_t crc_itu_t(std::uint16_t seed, const std::uint8_t* data,
                                                 int len)
    {
        unsigned crc = seed;
        for (int i = 0; i < len; ++i) {
            crc ^= static_cast<unsigned>(data[static_cast<std::size_t>(i)]) << 8U;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 0x8000U) != 0U ? ((crc << 1U) ^ 0x1021U) & 0xFFFFU
                                            : (crc << 1U) & 0xFFFFU;
            }
        }
        return static_cast<std::uint16_t>(crc);
    }

    std::vector<std::uint8_t> buffer_;
    std::uint16_t pkt_len_ = 0;
};

[[nodiscard]] std::vector<std::uint8_t> as_octets(ConstBytes data)
{
    std::vector<std::uint8_t> out;
    out.reserve(data.size());
    for (const std::byte value : data) {
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
}

/// The packet sizes every test that sweeps uses: one either side of each frame
/// boundary the encoder has, plus the two the reference treats specially.
const std::vector<std::size_t> kBoundarySizes{1,  2,  8,   9,   10,  90,  91,  92,  93,  94,
                                              95, 96, 182, 183, 184, 185, 186, 512, 1024};

} // namespace

// --- CRC --------------------------------------------------------------------

TEST_CASE("the CRC is CRC-16/XMODEM, against its published check value", "[serial][crc]")
{
    // 0x31C3 over "123456789" is what the catalogue says, and it is the only
    // assertion here that does not come from this repository. Get the variant
    // wrong -- reflect the input, seed 0xFFFF, XOR the output -- and every
    // round trip in this file would still pass while no device would accept a
    // byte of it.
    const std::vector<std::byte> input = bytes_of("123456789");
    CHECK(crc16_xmodem(0, ConstBytes{input}) == 0x31C3);
}

TEST_CASE("appending the CRC big-endian makes the whole run check as zero", "[serial][crc]")
{
    // The identity the receiver relies on: it never recomputes-and-compares,
    // it runs the CRC over the packet *and* its CRC and requires zero.
    const std::vector<std::byte> packet = counted(37);
    const std::uint16_t crc = crc16_xmodem(0, ConstBytes{packet});

    std::vector<std::byte> with_crc = packet;
    with_crc.push_back(static_cast<std::byte>((crc >> 8U) & 0xFFU));
    with_crc.push_back(static_cast<std::byte>(crc & 0xFFU));
    CHECK(crc16_xmodem(0, ConstBytes{with_crc}) == 0);

    // Little-endian would not, which is what makes this a test.
    std::vector<std::byte> swapped = packet;
    swapped.push_back(static_cast<std::byte>(crc & 0xFFU));
    swapped.push_back(static_cast<std::byte>((crc >> 8U) & 0xFFU));
    CHECK(crc16_xmodem(0, ConstBytes{swapped}) != 0);
}

TEST_CASE("a seed continues the computation across blocks", "[serial][crc]")
{
    const std::vector<std::byte> whole = counted(64);
    const ConstBytes view{whole};
    const std::uint16_t split =
        crc16_xmodem(crc16_xmodem(0, view.subspan(0, 20)), view.subspan(20));
    CHECK(split == crc16_xmodem(0, view));
}

TEST_CASE("an empty run leaves the seed alone", "[serial][crc]")
{
    CHECK(crc16_xmodem(0, ConstBytes{}) == 0);
    CHECK(crc16_xmodem(0x1234, ConstBytes{}) == 0x1234);
}

// --- base64 -----------------------------------------------------------------

TEST_CASE("base64 matches RFC 4648's test vectors", "[serial][base64]")
{
    // The second oracle that is not this repository's code. The vectors cover
    // both padding lengths, which is exactly where an encoder goes wrong.
    const std::vector<std::pair<std::string, std::string>> vectors{{"", ""},
                                                                   {"f", "Zg=="},
                                                                   {"fo", "Zm8="},
                                                                   {"foo", "Zm9v"},
                                                                   {"foob", "Zm9vYg=="},
                                                                   {"fooba", "Zm9vYmE="},
                                                                   {"foobar", "Zm9vYmFy"}};

    for (const auto& [plain, encoded] : vectors) {
        const std::vector<std::byte> raw = bytes_of(plain);
        std::array<std::byte, 16> out{};
        const std::size_t written = base64_encode(ConstBytes{raw}, MutBytes{out});
        CHECK(text_of(ConstBytes{out.data(), written}) == encoded);

        const std::vector<std::byte> input = bytes_of(encoded);
        std::array<std::byte, 16> back{};
        const std::optional<std::size_t> decoded = base64_decode(ConstBytes{input}, MutBytes{back});
        REQUIRE(decoded.has_value());
        CHECK(text_of(ConstBytes{back.data(), *decoded}) == plain);
    }
}

TEST_CASE("base64 encoding needs room for the padding", "[serial][base64]")
{
    const std::vector<std::byte> raw = bytes_of("f");
    CHECK(base64_encoded_size(raw.size()) == 4);
    std::array<std::byte, 3> tight{};
    CHECK(base64_encode(ConstBytes{raw}, MutBytes{tight}) == 0);
}

TEST_CASE("base64 decoding rejects everything a device might send by accident", "[serial][base64]")
{
    std::array<std::byte, 64> out{};
    const auto rejects = [&out](std::string_view input) {
        const std::vector<std::byte> bytes = bytes_of(input);
        return !base64_decode(ConstBytes{bytes}, MutBytes{out}).has_value();
    };

    CHECK(rejects("Zm9"));       // Not a whole quartet.
    CHECK(rejects("Zm9vYg="));   // Nor is a truncated padded one.
    CHECK(rejects("Zm9v Ymfy")); // A space is not in the alphabet.
    CHECK(rejects("Zm9v*mFy"));  // Nor is anything else outside it.
    CHECK(rejects("Zg==Zm9v"));  // Padding anywhere but the last quartet.
    CHECK(rejects("Z==="));      // Three pads encode nothing.
    CHECK(rejects("=g=="));      // Padding cannot lead.
    CHECK(rejects("Zm=v"));      // Nor sit inside a quartet with data after it.
}

TEST_CASE("base64 decoding refuses a buffer that could overflow", "[serial][base64]")
{
    const std::vector<std::byte> input = bytes_of("Zm9vYmFy");
    std::array<std::byte, 5> tight{};
    CHECK_FALSE(base64_decode(ConstBytes{input}, MutBytes{tight}).has_value());
}

// --- Frame geometry ---------------------------------------------------------

TEST_CASE("the frame constants are the ones the specification fixes", "[serial][framing]")
{
    // 127 is MCUMGR_SERIAL_MAX_FRAME. The other two are derived, and the
    // derivation is the thing protocol-notes used to blur: 124 is a count of
    // base64 *characters* and 93 a count of payload bytes.
    STATIC_REQUIRE(kMaxFrame == 127);
    STATIC_REQUIRE(kMaxBase64PerFrame == 124);
    STATIC_REQUIRE(kMaxRawPerFrame == 93);
    STATIC_REQUIRE(kMaxRawPerFrame % 3 == 0);
    STATIC_REQUIRE(base64_encoded_size(kMaxRawPerFrame) == kMaxBase64PerFrame);
}

TEST_CASE("the markers are 0x0609 and 0x0414, big-endian", "[serial][framing]")
{
    CHECK(kPacketMarker == std::array<std::byte, 2>{std::byte{0x06}, std::byte{0x09}});
    CHECK(kFragmentMarker == std::array<std::byte, 2>{std::byte{0x04}, std::byte{0x14}});
}

// --- The encoder ------------------------------------------------------------

TEST_CASE("a short packet is one frame, byte for byte", "[serial][framing]")
{
    // Hand-computed from the rule rather than from this encoder: the body is
    // be16(len + 2) || packet || be16(crc), base64'd whole, between the packet
    // marker and a newline. The packet is an OS-group read with seq 0x2A, so
    // its CRC (0xE92D) is not the trivially-zero one an all-zero header gives.
    const std::vector<std::byte> packet{std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x2A}, std::byte{0x00}};
    CHECK(crc16_xmodem(0, ConstBytes{packet}) == 0xE92D);

    const std::vector<std::string> frames = frames_of(ConstBytes{packet});
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == std::string{"\x06\x09"} + "AAoAAAAAAAAqAOkt" + "\n");
    CHECK(frames[0].size() == 19);
}

TEST_CASE("the encoder is byte-identical to the reference transmitter", "[serial][framing]")
{
    // The acceptance criterion for the whole encoder. A simple "93 bytes per
    // frame" split would round-trip through this module perfectly and still
    // differ from Zephyr's own transmitter on two sizes; matching it means a
    // receiver that tolerates Zephyr tolerates this.
    for (std::size_t size = 1; size <= 300; ++size) {
        const std::vector<std::byte> packet = counted(size);
        const std::vector<std::string> mine = frames_of(ConstBytes{packet});
        const std::vector<std::string> theirs = SerialPeer::transmit(as_octets(ConstBytes{packet}));
        INFO("packet size " << size);
        REQUIRE(mine == theirs);
    }
}

TEST_CASE("every frame obeys the four rules a receiver depends on", "[serial][framing]")
{
    for (const std::size_t size : kBoundarySizes) {
        const std::vector<std::byte> packet = counted(size);
        const std::vector<std::string> frames = frames_of(ConstBytes{packet});
        INFO("packet size " << size);
        REQUIRE_FALSE(frames.empty());

        for (std::size_t i = 0; i < frames.size(); ++i) {
            const std::string& frame = frames[i];
            // 1. It fits in a line the device will accept.
            CHECK(frame.size() <= kMaxFrame);
            // 2. It is marked as an opening or a continuation, correctly.
            const bool first = i == 0;
            CHECK(frame[0] == static_cast<char>(first ? kPacketMarker[0] : kFragmentMarker[0]));
            CHECK(frame[1] == static_cast<char>(first ? kPacketMarker[1] : kFragmentMarker[1]));
            // 3. It ends with the terminator and nothing else.
            CHECK(frame.back() == '\n');
            // 4. Its base64 is whole quartets -- the receiver decodes each
            //    frame on its own, so a split quartet decodes as nothing.
            CHECK((frame.size() - 3) % 4 == 0);
        }
    }
}

TEST_CASE("the frame split is the reference's, including the deferred byte", "[serial][framing]")
{
    // Sizes chosen for the arm that refuses to split the CRC across frames:
    // at 183 and 184 the second frame gives up a byte (123 rather than 127) so
    // the CRC can travel whole, while 182 and 185 pack full frames. These
    // counts were computed from the rule, not read off this encoder.
    const auto sizes = [](std::size_t size) {
        std::vector<std::size_t> out;
        for (const std::string& frame : frames_of(ConstBytes{counted(size)})) {
            out.push_back(frame.size());
        }
        return out;
    };

    CHECK(sizes(8) == std::vector<std::size_t>{19});
    CHECK(sizes(91) == std::vector<std::size_t>{123, 11});
    CHECK(sizes(92) == std::vector<std::size_t>{127, 7});
    CHECK(sizes(182) == std::vector<std::size_t>{127, 127});
    CHECK(sizes(183) == std::vector<std::size_t>{127, 123, 11});
    CHECK(sizes(184) == std::vector<std::size_t>{127, 123, 11});
    CHECK(sizes(185) == std::vector<std::size_t>{127, 127, 7});
}

TEST_CASE("count() agrees with the frames actually produced", "[serial][framing]")
{
    for (const std::size_t size : kBoundarySizes) {
        const std::vector<std::byte> packet = counted(size);
        INFO("packet size " << size);
        CHECK(SerialFramer{ConstBytes{packet}}.count() == frames_of(ConstBytes{packet}).size());
    }
}

TEST_CASE("an empty message yields no frames", "[serial][framing]")
{
    // Correct rather than a special case to fix: an SMP message is at least an
    // 8-byte header, so the only way here is a caller bug, and a lone CRC on
    // the wire would mean nothing. The reference's own loop does the same.
    SerialFramer framer{ConstBytes{}};
    CHECK(framer.done());
    CHECK(framer.count() == 0);
    std::array<std::byte, kMaxFrame> buffer{};
    CHECK(framer.next_frame(buffer) == 0);
}

TEST_CASE("a packet too large for the length field is refused", "[serial][framing]")
{
    // The length field is two bytes and carries `size + 2`, so 65533 is the
    // ceiling -- and a *maximal* SMP message is bigger than that, because its
    // own header length is 16-bit too (8 + 65535 = 65543). Truncating the field
    // would put a length on the wire that disagrees with the bytes after it,
    // which a receiver cannot detect as an error; refusing is the only answer
    // that cannot corrupt a stream.
    STATIC_REQUIRE(smply::transport::kMaxSerialPacket == 65533);

    const std::vector<std::byte> too_big(smply::transport::kMaxSerialPacket + 1, std::byte{0});
    SerialFramer refused{ConstBytes{too_big}};
    CHECK(refused.done());
    CHECK(refused.count() == 0);
    std::array<std::byte, kMaxFrame> buffer{};
    CHECK(refused.next_frame(buffer) == 0);

    // One byte under the ceiling is framed normally, so the bound is the
    // documented one and not an off-by-one near it.
    const std::vector<std::byte> largest(smply::transport::kMaxSerialPacket, std::byte{0});
    const SerialFramer accepted{ConstBytes{largest}};
    CHECK_FALSE(accepted.done());
    CHECK(accepted.count() > 0);
}

TEST_CASE("a buffer smaller than a frame gets nothing rather than a truncated frame",
          "[serial][framing]")
{
    const std::vector<std::byte> packet = counted(200);
    SerialFramer framer{ConstBytes{packet}};
    std::array<std::byte, kMaxFrame - 1> tight{};
    CHECK(framer.next_frame(tight) == 0);
    // And the framer did not advance, so a caller that fixes its buffer loses
    // nothing.
    std::array<std::byte, kMaxFrame> buffer{};
    CHECK(framer.next_frame(buffer) != 0);
}

// --- LineSplitter -----------------------------------------------------------

namespace {

/// Every line \p splitter can take out of \p stream, delivered in chunks of
/// \p chunk bytes.
[[nodiscard]] std::vector<std::string> split_all(LineSplitter& splitter, std::string_view stream,
                                                 std::size_t chunk)
{
    const std::vector<std::byte> raw = bytes_of(stream);
    std::vector<std::string> lines;
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const std::size_t take = std::min(chunk, raw.size() - offset);
        ConstBytes rest{raw.data() + offset, take};
        offset += take;
        while (const std::optional<ConstBytes> line = splitter.next_line(rest)) {
            lines.push_back(text_of(*line));
        }
    }
    return lines;
}

} // namespace

TEST_CASE("lines come out the same however the stream is cut", "[serial][lines]")
{
    // The fragmentation invariant, in the shape test_assembler.cpp established:
    // a reader that depends on where a read happened to end is a reader that
    // works on a fast host and fails on a slow one.
    const std::string stream = "\x06\x09"
                               "AAoAAAAAAAAqAOkt\nuart:~$ \nsecond line\r\n\x04\x14"
                               "AAAA\n";
    const std::vector<std::string> expected{"\x06\x09"
                                            "AAoAAAAAAAAqAOkt",
                                            "uart:~$ ", "second line",
                                            "\x04\x14"
                                            "AAAA"};

    for (std::size_t chunk = 1; chunk <= 64; ++chunk) {
        LineSplitter splitter;
        INFO("chunk size " << chunk);
        CHECK(split_all(splitter, stream, chunk) == expected);
    }
}

TEST_CASE("a CR is stripped only where it terminates the line", "[serial][lines]")
{
    LineSplitter splitter;
    const std::vector<std::string> lines = split_all(splitter, "a\rb\r\n\r\n", 1);
    CHECK(lines == std::vector<std::string>{"a\rb", ""});
}

TEST_CASE("a line too long to be a frame is dropped, not buffered", "[serial][lines]")
{
    // The bound. Without it a device that never sends a newline grows this
    // buffer forever -- and a console carrying a log backend emits over-long
    // lines as a matter of course, so this is the normal path, not an attack.
    LineSplitter splitter;
    const std::string noise(kMaxFrame + 500, 'x');
    const std::vector<std::string> lines = split_all(splitter, noise + "\nshort\n", 7);

    CHECK(lines == std::vector<std::string>{"short"});
    CHECK(splitter.dropped_lines() == 1);
    CHECK(splitter.buffered() == 0);
}

TEST_CASE("a line of exactly the maximum survives", "[serial][lines]")
{
    LineSplitter splitter;
    const std::string longest(kMaxFrame, 'y');
    const std::vector<std::string> lines = split_all(splitter, longest + "\n", 5);
    CHECK(lines == std::vector<std::string>{longest});
    CHECK(splitter.dropped_lines() == 0);
}

TEST_CASE("a partial line is held, and reset discards it", "[serial][lines]")
{
    LineSplitter splitter;
    const std::vector<std::byte> partial = bytes_of("\x06\x09"
                                                    "AAoA");
    ConstBytes rest{partial};
    CHECK_FALSE(splitter.next_line(rest).has_value());
    CHECK(splitter.buffered() == 6);

    splitter.reset();
    CHECK(splitter.buffered() == 0);

    // After a reset the held bytes are gone rather than prefixed onto whatever
    // arrives next, which is what keeps a truncated line from bleeding across
    // a reconnect.
    const std::vector<std::string> lines = split_all(splitter, "tail\n", 5);
    CHECK(lines == std::vector<std::string>{"tail"});
}

// --- SerialDeframer ---------------------------------------------------------

namespace {

/// Feeds \p frames to \p deframer and collects every packet it completes.
[[nodiscard]] std::vector<std::vector<std::byte>>
deframe_all(SerialDeframer& deframer, const std::vector<std::string>& frames)
{
    std::vector<std::vector<std::byte>> packets;
    for (const std::string& frame : frames) {
        const std::vector<std::byte> line = bytes_of(frame.substr(0, frame.size() - 1));
        if (deframer.feed_line(ConstBytes{line}) == Outcome::Packet) {
            const ConstBytes packet = deframer.packet();
            packets.emplace_back(packet.begin(), packet.end());
        }
    }
    return packets;
}

/// One line, as a `feed_line()` argument.
[[nodiscard]] Outcome feed(SerialDeframer& deframer, std::string_view line)
{
    const std::vector<std::byte> raw = bytes_of(line);
    return deframer.feed_line(ConstBytes{raw});
}

} // namespace

TEST_CASE("the decoder accepts what the reference transmitter produces", "[serial][deframe]")
{
    // The other half of the interoperability claim: not "this decodes what
    // this encoded", but "this decodes what Zephyr sends".
    for (const std::size_t size : kBoundarySizes) {
        const std::vector<std::byte> packet = counted(size);
        const std::vector<std::string> frames = SerialPeer::transmit(as_octets(ConstBytes{packet}));
        SerialDeframer deframer;
        INFO("packet size " << size);
        const auto packets = deframe_all(deframer, frames);
        REQUIRE(packets.size() == 1);
        CHECK(packets[0] == packet);
    }
}

TEST_CASE("the reference receiver accepts what the encoder produces", "[serial][deframe]")
{
    for (const std::size_t size : kBoundarySizes) {
        const std::vector<std::byte> packet = counted(size);
        SerialPeer peer;
        std::optional<std::vector<std::uint8_t>> received;
        for (const std::string& frame : frames_of(ConstBytes{packet})) {
            if (auto done = peer.process(std::string_view{frame}.substr(0, frame.size() - 1))) {
                received = std::move(done);
            }
        }
        INFO("packet size " << size);
        REQUIRE(received.has_value());
        CHECK(*received == as_octets(ConstBytes{packet}));
    }
}

TEST_CASE("several packets in a row are decoded independently", "[serial][deframe]")
{
    SerialDeframer deframer;
    std::vector<std::string> frames;
    std::vector<std::vector<std::byte>> expected;
    for (const std::size_t size : {8U, 200U, 40U}) {
        expected.push_back(counted(size));
        const auto more = frames_of(ConstBytes{expected.back()});
        frames.insert(frames.end(), more.begin(), more.end());
    }
    CHECK(deframe_all(deframer, frames) == expected);
    CHECK(deframer.counters().packets == 3);
}

TEST_CASE("console noise between frames does not disturb a packet", "[serial][deframe]")
{
    // The reason this module can be used over CONFIG_SHELL_BACKEND_SERIAL at
    // all, and the behaviour Zephyr's own receiver has: a line with no marker
    // is ignored and the partial packet is KEPT. P17c is what this is for --
    // a third-party client could not complete an upload over exactly this
    // console (docs/protocol-notes.md section 9).
    const std::vector<std::byte> packet = counted(300);
    const std::vector<std::string> frames = frames_of(ConstBytes{packet});
    REQUIRE(frames.size() > 2);

    SerialDeframer deframer;
    std::optional<std::vector<std::byte>> received;
    for (const std::string& frame : frames) {
        CHECK(feed(deframer, "uart:~$ ") == Outcome::Ignored);
        CHECK(feed(deframer, "[00:00:12.345,000] <inf> mcumgr: upload") == Outcome::Ignored);
        const Outcome outcome = feed(deframer, frame.substr(0, frame.size() - 1));
        if (outcome == Outcome::Packet) {
            const ConstBytes done = deframer.packet();
            received = std::vector<std::byte>(done.begin(), done.end());
        }
    }
    REQUIRE(received.has_value());
    CHECK(*received == packet);
    CHECK(deframer.counters().ignored == frames.size() * 2);
    CHECK(deframer.counters().framing_errors == 0);
}

TEST_CASE("a line too short to carry a marker is ignored", "[serial][deframe]")
{
    SerialDeframer deframer;
    CHECK(feed(deframer, "") == Outcome::Ignored);
    CHECK(feed(deframer, "\x06") == Outcome::Ignored);
    CHECK(deframer.counters().framing_errors == 0);
}

TEST_CASE("a continuation with nothing in progress is a framing error", "[serial][deframe]")
{
    // The one case the server treats as fatal rather than ignorable: a
    // continuation means the sender believes it is mid-packet and this
    // receiver does not, so the two have lost sync.
    SerialDeframer deframer;
    CHECK(feed(deframer, "\x04\x14"
                         "AAAA") == Outcome::Error);
    CHECK(deframer.counters().framing_errors == 1);
    CHECK(deframer.buffered() == 0);
}

TEST_CASE("a new packet marker abandons whatever was in flight", "[serial][deframe]")
{
    // Not an error: the peer has plainly restarted, and the server does the
    // same (net_buf_reset on HDR_PKT).
    const std::vector<std::byte> first = counted(300);
    const std::vector<std::byte> second = counted(16);
    const std::vector<std::string> abandoned = frames_of(ConstBytes{first});

    SerialDeframer deframer;
    CHECK(feed(deframer, abandoned[0].substr(0, abandoned[0].size() - 1)) == Outcome::NeedMore);
    CHECK(deframer.buffered() > 0);

    const auto packets = deframe_all(deframer, frames_of(ConstBytes{second}));
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == second);
    CHECK(deframer.counters().framing_errors == 0);
}

TEST_CASE("undecodable base64 is a framing error and resets", "[serial][deframe]")
{
    SerialDeframer deframer;
    CHECK(feed(deframer, "\x06\x09"
                         "not base64!") == Outcome::Error);
    CHECK(deframer.buffered() == 0);
    CHECK(deframer.counters().framing_errors == 1);
}

TEST_CASE("a corrupted packet fails on the CRC rather than being delivered", "[serial][deframe]")
{
    const std::vector<std::byte> packet = counted(40);
    std::vector<std::string> frames = frames_of(ConstBytes{packet});
    REQUIRE(frames.size() == 1);
    // Flip one base64 character, which is one bit of payload the CRC covers.
    std::string& frame = frames[0];
    frame[5] = frame[5] == 'A' ? 'B' : 'A';

    SerialDeframer deframer;
    CHECK(feed(deframer, frame.substr(0, frame.size() - 1)) == Outcome::Error);
    CHECK(deframer.counters().crc_failures == 1);
    CHECK(deframer.buffered() == 0);
    // And the next packet is decoded normally: a bad CRC costs one packet, not
    // the stream.
    CHECK(deframe_all(deframer, frames_of(ConstBytes{packet})).size() == 1);
}

TEST_CASE("a declared length above the bound is refused with no buffer growth", "[serial][deframe]")
{
    // Rule 6 of CLAUDE.md, with the assertion on `capacity()` rather than on
    // `buffered()`: the point is not that the buffer was emptied afterwards,
    // it is that it never grew towards what the device asked for.
    SerialDeframer deframer{64};
    const std::vector<std::byte> packet = counted(200);
    const std::vector<std::string> frames = frames_of(ConstBytes{packet});

    CHECK(feed(deframer, frames[0].substr(0, frames[0].size() - 1)) == Outcome::Error);
    CHECK(deframer.buffered() == 0);
    CHECK(deframer.peak_buffered() <= kMaxRawPerFrame);
    CHECK(deframer.capacity() <= kMaxRawPerFrame);
    CHECK(deframer.max_packet() == 64);
}

TEST_CASE("an opening frame too short to carry a length is refused", "[serial][deframe]")
{
    // The smallest legal base64 quartet decodes to one byte, which is not
    // enough for the two-byte length the first frame has to carry. The server
    // checks the same thing, and getting it wrong would read the length out of
    // one byte plus whatever followed it in the buffer.
    SerialDeframer deframer;
    CHECK(feed(deframer, "\x06\x09"
                         "AA==") == Outcome::Error);
    CHECK(deframer.buffered() == 0);

    // A marker with no body at all is the degenerate case of the same thing.
    CHECK(feed(deframer, "\x06\x09") == Outcome::Error);
    CHECK(deframer.counters().framing_errors == 2);
}

TEST_CASE("a declared length of nothing but a CRC is refused", "[serial][deframe]")
{
    // pkt_len <= 2 is a packet of no bytes at all; the server rejects it with
    // the same comparison.
    SerialDeframer deframer;
    // be16(2) || 0x00, base64'd: the length says two, which is the CRC alone.
    const std::vector<std::byte> body{std::byte{0x00}, std::byte{0x02}, std::byte{0x00}};
    std::array<std::byte, 8> encoded{};
    const std::size_t written = base64_encode(ConstBytes{body}, MutBytes{encoded});
    const std::string line = std::string{"\x06\x09"} + text_of(ConstBytes{encoded.data(), written});
    CHECK(feed(deframer, line) == Outcome::Error);
}

TEST_CASE("a body longer than its own header claims is refused", "[serial][deframe]")
{
    // The server refuses this rather than trimming: a packet with extra on the
    // end is not a packet with rubbish after it, it is a stream that has lost
    // sync. Both places it can happen are checked -- in the opening frame,
    // which overshoots before the length has even been applied, and in a
    // continuation that carries more than the remainder.
    const auto opening = [](std::size_t declared, std::size_t body_bytes) {
        std::vector<std::byte> body{static_cast<std::byte>((declared >> 8U) & 0xFFU),
                                    static_cast<std::byte>(declared & 0xFFU)};
        for (std::size_t i = 0; i < body_bytes; ++i) {
            body.push_back(static_cast<std::byte>(i));
        }
        std::array<std::byte, kMaxBase64PerFrame> encoded{};
        const std::size_t written = base64_encode(ConstBytes{body}, MutBytes{encoded});
        return std::string{"\x06\x09"} + text_of(ConstBytes{encoded.data(), written});
    };
    const auto continuation = [](std::size_t body_bytes) {
        std::vector<std::byte> body(body_bytes, std::byte{0x5A});
        std::array<std::byte, kMaxBase64PerFrame> encoded{};
        const std::size_t written = base64_encode(ConstBytes{body}, MutBytes{encoded});
        return std::string{"\x04\x14"} + text_of(ConstBytes{encoded.data(), written});
    };

    SECTION("in the opening frame")
    {
        SerialDeframer deframer;
        // Says six bytes follow; sends twenty-one.
        CHECK(feed(deframer, opening(6, 21)) == Outcome::Error);
        CHECK(deframer.counters().framing_errors == 1);
        CHECK(deframer.buffered() == 0);
    }

    SECTION("in a continuation")
    {
        SerialDeframer deframer;
        // Says sixty bytes follow and sends nine, so nine more are expected --
        // then sends thirty.
        CHECK(feed(deframer, opening(60, 9)) == Outcome::NeedMore);
        CHECK(deframer.buffered() == 9);
        CHECK(feed(deframer, continuation(60)) == Outcome::Error);
        CHECK(deframer.counters().framing_errors == 1);
        CHECK(deframer.buffered() == 0);
    }
}

TEST_CASE("a frame with more base64 than a frame can hold is refused", "[serial][deframe]")
{
    SerialDeframer deframer;
    const std::string overlong = std::string{"\x06\x09"} + std::string(kMaxBase64PerFrame + 4, 'A');
    CHECK(feed(deframer, overlong) == Outcome::Error);
    CHECK(deframer.counters().framing_errors == 1);
}

TEST_CASE("reset discards a packet in progress", "[serial][deframe]")
{
    const std::vector<std::byte> packet = counted(300);
    const std::vector<std::string> frames = frames_of(ConstBytes{packet});
    SerialDeframer deframer;
    CHECK(feed(deframer, frames[0].substr(0, frames[0].size() - 1)) == Outcome::NeedMore);
    CHECK(deframer.buffered() > 0);
    deframer.reset();
    CHECK(deframer.buffered() == 0);
    // The next continuation now has nothing to continue, which is the point.
    CHECK(feed(deframer, frames[1].substr(0, frames[1].size() - 1)) == Outcome::Error);
}

TEST_CASE("the packet view dies at the next line", "[serial][deframe]")
{
    // The same borrowed-buffer rule as everywhere else in smply, stated here
    // because a serial adapter marshals inbound bytes to another thread and
    // has to copy them (docs/design.md section 9).
    const std::vector<std::byte> packet = counted(24);
    SerialDeframer deframer;
    const std::vector<std::string> frames = frames_of(ConstBytes{packet});
    CHECK(feed(deframer, frames[0].substr(0, frames[0].size() - 1)) == Outcome::Packet);
    CHECK(deframer.packet().size() == packet.size());
    CHECK(feed(deframer, "uart:~$ ") == Outcome::Ignored);
    CHECK(deframer.packet().empty());
}

TEST_CASE("the whole path survives an arbitrary cut of the byte stream", "[serial][deframe]")
{
    // Splitter and deframer together, which is how an adapter uses them: one
    // packet's frames concatenated into a stream and delivered in every fixed
    // chunk size from 1 to 64.
    const std::vector<std::byte> packet = counted(400);
    std::string stream;
    for (const std::string& frame : frames_of(ConstBytes{packet})) {
        stream += frame;
    }

    for (std::size_t chunk = 1; chunk <= 64; ++chunk) {
        LineSplitter splitter;
        SerialDeframer deframer;
        const std::vector<std::byte> raw = bytes_of(stream);
        std::optional<std::vector<std::byte>> received;
        std::size_t offset = 0;
        while (offset < raw.size()) {
            const std::size_t take = std::min(chunk, raw.size() - offset);
            ConstBytes rest{raw.data() + offset, take};
            offset += take;
            while (const std::optional<ConstBytes> line = splitter.next_line(rest)) {
                if (deframer.feed_line(*line) == Outcome::Packet) {
                    const ConstBytes done = deframer.packet();
                    received = std::vector<std::byte>(done.begin(), done.end());
                }
            }
        }
        INFO("chunk size " << chunk);
        REQUIRE(received.has_value());
        CHECK(*received == packet);
    }
}
