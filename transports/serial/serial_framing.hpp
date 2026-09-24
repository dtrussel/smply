// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_SERIAL_SERIAL_FRAMING_HPP
#define SMPLY_TRANSPORTS_SERIAL_SERIAL_FRAMING_HPP

/// \file
/// MCUmgr's serial (console) framing, both directions.
///
/// **A serial link is not a byte pipe the way GATT is.** The Bluetooth
/// transport specification says an SMP message crossing several GATT packets
/// gets *no additional framing*, which is why `common/ble_framing.hpp` has a
/// sender and no receiver: the bytes that arrive are already SMP bytes, and
/// reassembling them belongs to the core
/// ([ADR-0006](../../docs/decisions/ADR-0006-reassembly-location.md)). The
/// serial transport adds a real encapsulation -- markers, base64, a length
/// prefix and a CRC -- and **none of it is SMP**, so it has to be undone before
/// a single SMP byte exists. Doing that is transport work under
/// [ADR-0005](../../docs/decisions/ADR-0005-transport-abstraction.md), and
/// `SerialDeframer` does exactly that much and stops: it never looks at the
/// 8-byte SMP header, never counts payload bytes, and hands whole packets
/// straight to `TransportListener::on_bytes()` for `MessageAssembler` to parse.
/// [ADR-0017](../../docs/decisions/ADR-0017-serial-framing-placement.md) is the
/// decision and the reasoning.
///
/// Platform-independent and header-only, like everything else under
/// `transports/` that is not an adapter. **The port is not here**: opening a
/// tty, `termios`, `CreateFile`, a reader thread and its `smply::Dispatcher`
/// are the application's, exactly as `Transport` intends. What is here is the
/// part that is protocol rather than platform, and therefore the part that can
/// be unit-tested, linted and coverage-measured on every CI job rather than on
/// a bench.
///
/// ### The frame, from `serial_util.c` (S19) and `serial.h` (S18)
///
/// One **frame** is `marker(2) || base64 || '\n'`, at most 127 bytes -- so at
/// most **124 base64 characters**, which is at most **93 raw bytes**. Those
/// three numbers are routinely conflated; docs/protocol-notes.md section 8
/// spells them out.
///
/// The **body** that gets base64'd across the frames of one packet is
///
///     be16(packet.size() + 2) || packet || be16(crc16_xmodem(0, packet))
///
/// so the length field counts the CRC but not itself, and the CRC covers the
/// packet but not the length. The first frame carries `kPacketMarker`, every
/// continuation `kFragmentMarker`.
///
/// **Every frame holds a whole number of base64 quartets** -- whole triplets of
/// body bytes -- except the last. That is not a style choice: the receiver
/// base64-decodes each frame on its own, so a quartet split across two frames
/// decodes as nothing. It is the binding constraint on any splitting strategy.
///
/// ### Composition
///
/// \code
/// // outbound, from Transport::send()
/// std::array<std::byte, smply::transport::kMaxFrame> frame{};
/// smply::transport::SerialFramer framer{message};
/// while (!framer.done()) {
///     const std::size_t n = framer.next_frame(frame);
///     write_all(fd, frame.data(), n);
/// }
///
/// // inbound, on the reader thread, before posting to the Dispatcher
/// ConstBytes rest = chunk;
/// while (const auto line = splitter.next_line(rest)) {
///     if (deframer.feed_line(*line) == SerialDeframer::Outcome::Packet) {
///         post_to_client_context(deframer.packet());   // copies; see design.md
///     }
/// }
/// \endcode

#include "serial/base64.hpp"
#include "serial/crc16.hpp"

#include "smply/bytes.hpp"
#include "smply/limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace smply::transport {

/// The two-byte marker beginning the **first** frame of a packet (`0x06 0x09`).
inline constexpr std::array<std::byte, 2> kPacketMarker{std::byte{0x06}, std::byte{0x09}};

/// The two-byte marker beginning every **continuation** frame (`0x04 0x14`).
inline constexpr std::array<std::byte, 2> kFragmentMarker{std::byte{0x04}, std::byte{0x14}};

/// The whole frame, marker and terminating newline included
/// (`MCUMGR_SERIAL_MAX_FRAME`).
inline constexpr std::size_t kMaxFrame = 127;

/// The marker's two bytes, which are not base64-encoded.
inline constexpr std::size_t kMarkerSize = 2;

/// The terminating newline, which is not base64-encoded either.
inline constexpr std::size_t kTerminatorSize = 1;

/// Base64 characters one frame can hold: `kMaxFrame` less the marker and the
/// newline. **124, and these are characters, not payload bytes** -- the
/// distinction the protocol note used to blur.
inline constexpr std::size_t kMaxBase64PerFrame = kMaxFrame - kMarkerSize - kTerminatorSize;

/// Raw body bytes one frame can hold: `kMaxBase64PerFrame / 4 * 3`, which is
/// the reference transmitter's own `max_input`.
inline constexpr std::size_t kMaxRawPerFrame = (kMaxBase64PerFrame / 4) * 3;

/// The largest SMP message this framing can carry at all.
///
/// The length field is two bytes and holds `packet.size() + 2`, so anything
/// above this cannot be described by a frame -- and a **maximal** SMP message
/// is larger than this, since its header's `length` is itself 16-bit: 8 +
/// 65535 = 65543. So the limit is real rather than theoretical, and a framer
/// that quietly truncated the field would put a length on the wire that
/// disagrees with the bytes following it.
///
/// A serial adapter should report at most this from
/// `Transport::max_message_size()`. `smply::limits::kMaxSmpPayload` is 8192 by
/// default, which is comfortably inside it.
inline constexpr std::size_t kMaxSerialPacket = 65533;

/// How the encoder splits one frame. Shared by `SerialFramer::next_frame()` and
/// `SerialFramer::count()` so the two cannot drift -- the boundary rule is
/// fiddly enough that two copies of it would eventually disagree.
struct FramePlan
{
    /// Whole 3-byte groups of the packet this frame carries, *after* the
    /// first frame's length-and-first-byte group.
    std::size_t triplets = 0;
    /// This frame also carries the CRC, and therefore ends the packet.
    bool carries_crc = false;
};

/// Decides one frame's contents, mirroring `mcumgr_serial_tx_pkt()`.
///
/// The one non-obvious rule is the reference's: **the CRC is never split
/// across frames.** When the remaining data would leave fewer than two spare
/// bytes, one data byte is deferred to the next frame so the CRC can go with
/// it. Reproducing that exactly -- rather than packing 93 bytes per frame,
/// which a conforming receiver would also accept -- means any device that
/// tolerates Zephyr's own transmitter tolerates this one.
[[nodiscard]] constexpr FramePlan plan_frame(std::size_t packet_size, std::size_t offset,
                                             bool first) noexcept
{
    std::size_t budget = kMaxRawPerFrame;
    std::size_t off = offset;
    if (first) {
        // The length prefix pairs with the first data byte to make the opening
        // triplet, so one raw byte and one whole group are already spoken for.
        off += 1;
        budget -= 3;
    }
    const std::size_t left = packet_size - off;

    std::size_t to_process = left < budget ? left : budget;
    FramePlan plan;
    if (budget >= left) {
        if (budget - left <= 1) {
            // The CRC would not fit whole. Defer a byte rather than split it.
            to_process -= 1;
        } else {
            plan.carries_crc = true;
        }
    }
    plan.triplets = to_process / 3;
    return plan;
}

/// Splits one SMP message into console frames.
///
/// Borrows the message, which must outlive the framer and must not move while
/// one is in flight -- the same rule `Fragmenter` states, for the same reason.
/// Unlike `Fragmenter` this one cannot hand out views: every frame is encoded,
/// so it writes into a buffer the caller owns.
///
/// **An empty message yields no frames**, which mirrors the reference's
/// `while (src_off < len)` and is correct rather than a special case to fix: an
/// SMP message is never empty, so the only way here is a caller bug, and a
/// lone CRC on the wire would mean nothing.
class SerialFramer
{
public:
    /// \param packet The complete SMP message. Must outlive this object.
    ///               A packet above `kMaxSerialPacket` is **refused**: `done()`
    ///               is true immediately and `count()` is zero, the same answer
    ///               an empty one gets and for the same reason -- there is no
    ///               correct frame to emit, and truncating the length field
    ///               would put bytes on the wire that contradict it.
    explicit SerialFramer(ConstBytes packet) noexcept
        : packet_{packet.size() > kMaxSerialPacket ? ConstBytes{} : packet},
          crc_{crc16_xmodem(0, packet_)}
    {}

    /// True once the CRC has been written and nothing remains -- and true from
    /// the start for a message that is empty or above `kMaxSerialPacket`.
    [[nodiscard]] bool done() const noexcept
    {
        return offset_ >= packet_.size();
    }

    /// Writes the next frame into \p out, advancing past it.
    ///
    /// \return bytes written, or `0` when `done()` or when \p out is smaller
    ///         than `kMaxFrame`. The buffer is sized by the constant rather
    ///         than by the frame so a caller can reuse one array for every
    ///         frame without asking how big this one happens to be.
    [[nodiscard]] std::size_t next_frame(MutBytes out) noexcept
    {
        if (done() || out.size() < kMaxFrame) {
            return 0;
        }

        const FramePlan plan = plan_frame(packet_.size(), offset_, first_);
        std::size_t written = 0;

        const std::array<std::byte, 2>& marker = first_ ? kPacketMarker : kFragmentMarker;
        out[written++] = marker[0];
        out[written++] = marker[1];

        if (first_) {
            const auto declared = static_cast<std::uint16_t>(packet_.size() + 2);
            const std::array<std::byte, 3> opening{static_cast<std::byte>((declared >> 8U) & 0xFFU),
                                                   static_cast<std::byte>(declared & 0xFFU),
                                                   packet_[0]};
            written += encode_group(ConstBytes{opening}, out.subspan(written));
            offset_ += 1;
        }

        for (std::size_t i = 0; i < plan.triplets; ++i) {
            written += encode_group(packet_.subspan(offset_, 3), out.subspan(written));
            offset_ += 3;
        }

        if (plan.carries_crc) {
            written += encode_tail(out.subspan(written));
        }

        out[written++] = std::byte{'\n'};
        first_ = false;
        return written;
    }

    /// How many frames the whole message takes. Constant for the life of the
    /// object, and useful for pacing a link that needs a gap between writes.
    [[nodiscard]] std::size_t count() const noexcept
    {
        std::size_t frames = 0;
        std::size_t off = 0;
        bool first = true;
        while (off < packet_.size()) {
            const FramePlan plan = plan_frame(packet_.size(), off, first);
            ++frames;
            if (first) {
                off += 1;
            }
            off += 3 * plan.triplets;
            if (plan.carries_crc) {
                off = packet_.size();
            }
            first = false;
        }
        return frames;
    }

private:
    /// Encodes one group of at most three bytes as exactly four characters.
    static std::size_t encode_group(ConstBytes group, MutBytes out) noexcept
    {
        return detail::base64_encode(group, out);
    }

    /// The packet's last one or two bytes, if any, plus the CRC.
    ///
    /// Three shapes, from the reference's own switch. The two-byte case is the
    /// only one that needs a second group, because two data bytes plus two CRC
    /// bytes do not fit in one triplet.
    [[nodiscard]] std::size_t encode_tail(MutBytes out) noexcept
    {
        const auto crc_hi = static_cast<std::byte>((crc_ >> 8U) & 0xFFU);
        const auto crc_lo = static_cast<std::byte>(crc_ & 0xFFU);
        const std::size_t tail = packet_.size() - offset_;

        if (tail == 0) {
            const std::array<std::byte, 2> group{crc_hi, crc_lo};
            return encode_group(ConstBytes{group}, out);
        }
        if (tail == 1) {
            const std::array<std::byte, 3> group{packet_[offset_], crc_hi, crc_lo};
            offset_ += 1;
            return encode_group(ConstBytes{group}, out);
        }

        const std::array<std::byte, 3> group{packet_[offset_], packet_[offset_ + 1], crc_hi};
        offset_ += 2;
        std::size_t written = encode_group(ConstBytes{group}, out);
        const std::array<std::byte, 1> last{crc_lo};
        written += encode_group(ConstBytes{last}, out.subspan(written));
        return written;
    }

    ConstBytes packet_;
    std::uint16_t crc_;
    std::size_t offset_ = 0;
    bool first_ = true;
};

/// Turns an arbitrary inbound byte stream into whole lines.
///
/// A serial read returns whatever happened to be in the driver's buffer, so a
/// frame arrives split across reads, several frames arrive in one read, or
/// both. This does the same job for the serial link that `MessageAssembler`
/// does for the SMP stream, one layer lower and with a terminator to
/// resynchronise on -- which is the difference that makes recovery possible
/// here and impossible there (docs/design.md section 2).
///
/// `'\n'` ends a line; a `'\r'` immediately before it is dropped, because a
/// console peer may send CRLF and the reference transmitter's own terminator
/// is the newline alone.
///
/// **A line longer than `max_line` is discarded, not buffered.** Two reasons,
/// and the first is the bound: without it a device that never sends a newline
/// grows this buffer without limit, which is the whole class of bug rule 6 of
/// CLAUDE.md exists to prevent. The second is that over-long lines are
/// *expected* -- a console carrying a shell and a log backend emits them all
/// day, and none of them is a frame, since no frame can exceed `kMaxFrame`.
class LineSplitter
{
public:
    /// \param max_line The longest line worth keeping. Anything longer cannot
    ///                 be a frame, so the default is `kMaxFrame`.
    explicit LineSplitter(std::size_t max_line = kMaxFrame) noexcept : max_line_{max_line} {}

    /// Takes the next complete line out of \p chunk, advancing it past what
    /// was consumed.
    ///
    /// Call it until it answers `std::nullopt`, then read more from the port.
    /// The returned view points into this object and is **valid only until the
    /// next call** to `next_line()` or `reset()`; copy anything that outlives
    /// that.
    [[nodiscard]] std::optional<ConstBytes> next_line(ConstBytes& chunk) noexcept
    {
        while (!chunk.empty()) {
            const std::byte value = chunk.front();
            chunk = chunk.subspan(1);

            if (value != std::byte{'\n'}) {
                if (discarding_) {
                    continue;
                }
                if (line_.size() == max_line_) {
                    // One byte past what any frame can be. Everything up to the
                    // next newline is somebody else's output.
                    line_.clear();
                    discarding_ = true;
                    continue;
                }
                line_.push_back(value);
                continue;
            }

            if (discarding_) {
                discarding_ = false;
                ++dropped_;
                continue;
            }
            if (!line_.empty() && line_.back() == std::byte{'\r'}) {
                line_.pop_back();
            }
            // Swapped rather than copied, and swapped rather than returning a
            // view into a cleared `line_`: reading past a vector's `size()`
            // works in practice and is not something to rely on. The two
            // buffers trade storage back and forth, so this allocates only
            // while the longest line seen so far is growing.
            ready_.swap(line_);
            line_.clear();
            return ConstBytes{ready_.data(), ready_.size()};
        }
        return std::nullopt;
    }

    /// Discards any partial line. Call it when the port is reopened, so a
    /// truncated line cannot bleed across a reconnect.
    void reset() noexcept
    {
        line_.clear();
        ready_.clear();
        discarding_ = false;
    }

    /// Lines dropped for exceeding `max_line`. A healthy frame stream leaves
    /// this at zero; a console shared with a log backend does not, and that is
    /// not an error.
    [[nodiscard]] std::size_t dropped_lines() const noexcept
    {
        return dropped_;
    }

    /// Bytes held for the line currently being assembled.
    [[nodiscard]] std::size_t buffered() const noexcept
    {
        return line_.size();
    }

private:
    std::vector<std::byte> line_;
    std::vector<std::byte> ready_;
    std::size_t max_line_;
    std::size_t dropped_ = 0;
    bool discarding_ = false;
};

/// What a deframer made of one line.
struct SerialCounters
{
    /// Lines that carried no recognised marker. Ordinary console traffic.
    std::uint64_t ignored = 0;
    /// Packets that completed but whose CRC did not verify.
    std::uint64_t crc_failures = 0;
    /// Framing violations: a continuation with nothing in progress, undecodable
    /// base64, a declared length out of bounds, a body longer than declared.
    std::uint64_t framing_errors = 0;
    /// Packets delivered.
    std::uint64_t packets = 0;
};

/// Turns console frames back into whole SMP packets.
///
/// Feed it one line at a time from a `LineSplitter`. It does **no SMP-level
/// work** -- see the file comment and ADR-0017.
class SerialDeframer
{
public:
    /// \param max_packet The largest packet this will assemble. A device
    ///                   declares its own length in the first frame and that
    ///                   number is checked against this **before the buffer is
    ///                   allowed to grow past one frame's worth**, so the peak
    ///                   footprint of a hostile stream is bounded by the
    ///                   smaller of the two rather than by what the device
    ///                   asked for.
    explicit SerialDeframer(std::size_t max_packet = limits::kMaxAssemblyBuffer) noexcept
        : max_packet_{max_packet}
    {}

    /// What one line produced.
    enum class Outcome : std::uint8_t
    {
        /// Accepted; more frames are expected.
        NeedMore,
        /// A complete, CRC-verified packet is available from `packet()`.
        Packet,
        /// Not a frame. Any partial packet is **kept**.
        Ignored,
        /// A framing violation. Partial state has been discarded.
        Error,
    };

    /// Consumes one line, with its marker and without its newline.
    ///
    /// **An unrecognised line is `Ignored` and does not disturb a packet in
    /// progress.** That is the server's own behaviour (S19's `default:` arm
    /// returns without freeing its context), and it is what lets this run over
    /// a console that also carries an echoing shell and a log backend -- the
    /// exact configuration that stopped a third-party client from completing an
    /// upload on the bench (docs/protocol-notes.md section 9).
    [[nodiscard]] Outcome feed_line(ConstBytes line) noexcept
    {
        if (ready_ != 0) {
            // The previous packet's view dies here, as `packet()` documents.
            buffer_.clear();
            ready_ = 0;
        }

        if (line.size() < kMarkerSize) {
            ++counters_.ignored;
            return Outcome::Ignored;
        }

        const bool starts = line[0] == kPacketMarker[0] && line[1] == kPacketMarker[1];
        const bool continues = line[0] == kFragmentMarker[0] && line[1] == kFragmentMarker[1];
        if (!starts && !continues) {
            ++counters_.ignored;
            return Outcome::Ignored;
        }
        if (starts) {
            // A new packet abandons whatever was in flight, exactly as the
            // server does: the peer has plainly restarted.
            buffer_.clear();
            declared_ = 0;
        } else if (buffer_.empty()) {
            return fail();
        }

        if (!decode_into_buffer(line.subspan(kMarkerSize))) {
            return fail();
        }

        if (starts && !take_declared_length()) {
            return fail();
        }

        if (buffer_.size() < declared_) {
            return Outcome::NeedMore;
        }
        // LCOV_EXCL_START -- an invariant guard, unreachable as the code stands.
        // A body longer than its own header is refused rather than trimmed --
        // extra bytes mean the stream has lost sync, not that the packet has
        // rubbish after it -- but both routes into this state are already
        // closed upstream: `take_declared_length()` refuses an opening frame
        // that overshoots, and `decode_into_buffer()` refuses a continuation
        // that would. It stays because those two are the bound and this is the
        // check that the bound held; a decoder that assumes its input was
        // validated elsewhere is one refactor away from trusting a device.
        if (buffer_.size() > declared_) {
            return fail();
        }
        // LCOV_EXCL_STOP
        if (crc16_xmodem(0, ConstBytes{buffer_.data(), buffer_.size()}) != 0) {
            ++counters_.crc_failures;
            buffer_.clear();
            declared_ = 0;
            return Outcome::Error;
        }

        ready_ = buffer_.size() - 2; // Strip the CRC; the rest is the packet.
        declared_ = 0;
        ++counters_.packets;
        return Outcome::Packet;
    }

    /// The packet the last `feed_line()` completed.
    ///
    /// Valid only until the next call to `feed_line()` or `reset()`, and empty
    /// unless that call answered `Packet`. Same borrowed-buffer rule as
    /// everywhere else in smply (docs/design.md section 9).
    [[nodiscard]] ConstBytes packet() const noexcept
    {
        return ConstBytes{buffer_.data(), ready_};
    }

    /// Discards any partial packet. Call it on connect and on disconnect, for
    /// the reason `MessageAssembler::reset()` exists.
    void reset() noexcept
    {
        buffer_.clear();
        declared_ = 0;
        ready_ = 0;
    }

    /// Bytes held for the packet being assembled.
    [[nodiscard]] std::size_t buffered() const noexcept
    {
        return buffer_.size();
    }

    /// Bytes currently allocated. A test asserts on this rather than on
    /// `buffered()` to show a hostile declared length caused no growth at all,
    /// which is the property worth guaranteeing.
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return buffer_.capacity();
    }

    /// The largest `buffered()` has ever been.
    [[nodiscard]] std::size_t peak_buffered() const noexcept
    {
        return peak_;
    }

    /// The configured bound, for a caller that wants to report it.
    [[nodiscard]] std::size_t max_packet() const noexcept
    {
        return max_packet_;
    }

    [[nodiscard]] SerialCounters counters() const noexcept
    {
        return counters_;
    }

private:
    [[nodiscard]] Outcome fail() noexcept
    {
        ++counters_.framing_errors;
        buffer_.clear();
        declared_ = 0;
        return Outcome::Error;
    }

    /// Appends one frame's decoded body. Bounded by `kMaxRawPerFrame` whatever
    /// the device sent, because a line longer than `kMaxFrame` is not a frame.
    [[nodiscard]] bool decode_into_buffer(ConstBytes body) noexcept
    {
        if (body.size() > kMaxBase64PerFrame) {
            return false;
        }
        std::array<std::byte, kMaxRawPerFrame> scratch{};
        const std::optional<std::size_t> decoded =
            detail::base64_decode(body, MutBytes{scratch.data(), scratch.size()});
        if (!decoded.has_value()) {
            return false;
        }
        // Only after the length is known can this exceed one frame's worth, and
        // `take_declared_length()` bounds it before that happens.
        if (declared_ != 0 && buffer_.size() + *decoded > declared_) {
            return false;
        }
        buffer_.insert(buffer_.end(), scratch.begin(),
                       scratch.begin() + static_cast<std::ptrdiff_t>(*decoded));
        if (buffer_.size() > peak_) {
            peak_ = buffer_.size();
        }
        return true;
    }

    /// Pulls the big-endian length off the front of a freshly started packet.
    [[nodiscard]] bool take_declared_length() noexcept
    {
        if (buffer_.size() < 2) {
            return false;
        }
        const auto declared = static_cast<std::size_t>((static_cast<unsigned>(buffer_[0]) << 8U) |
                                                       static_cast<unsigned>(buffer_[1]));
        // Two bytes is the CRC alone: a packet of nothing. The server rejects
        // it with the same comparison.
        if (declared <= 2 || declared > max_packet_) {
            return false;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
        declared_ = declared;
        return buffer_.size() <= declared_;
    }

    std::vector<std::byte> buffer_;
    std::size_t max_packet_;
    std::size_t declared_ = 0;
    std::size_t ready_ = 0;
    std::size_t peak_ = 0;
    SerialCounters counters_;
};

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_SERIAL_SERIAL_FRAMING_HPP
