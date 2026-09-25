// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_SERIAL_PORT_SERIAL_LINK_HPP
#define SMPLY_TRANSPORTS_SERIAL_PORT_SERIAL_LINK_HPP

/// \file
/// The two byte-level halves of a serial link, independent of any port.
///
/// Both platform implementations of `SerialPortTransport` do exactly this and
/// nothing else with the bytes, so it lives here, once, where a unit test can
/// reach it without a tty -- and where the stub device in `examples/` can use
/// the same code for the other end of the wire.
///
/// * **Outbound**, `frame_message()` turns one SMP message into the complete
///   run of console frames that carries it, ready to be written as one block.
/// * **Inbound**, `SerialInbound` turns whatever a read returned into whole SMP
///   packets, and keeps the counts that say what it threw away.

#include "serial/serial_framing.hpp"

#include "smply/bytes.hpp"
#include "smply/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace smply::transport {

/// Every console frame for \p message, concatenated.
///
/// Empty for an empty message and for one above `kMaxSerialPacket`, exactly as
/// `SerialFramer` yields no frames for either. A caller that has already
/// bounded the message by its `max_message_size` never sees the second.
[[nodiscard]] inline std::vector<std::byte> frame_message(ConstBytes message)
{
    SerialFramer framer{message};
    // Each frame is written straight into its place. `count()` frames of at
    // most `kMaxFrame` fit by construction, so the tail always has the
    // `kMaxFrame` bytes `next_frame()` asks for until the framer is done.
    std::vector<std::byte> out(framer.count() * kMaxFrame);
    std::size_t used = 0;
    while (!framer.done()) {
        used += framer.next_frame(MutBytes{out.data() + used, out.size() - used});
    }
    out.resize(used);
    return out;
}

/// Inbound bytes to SMP packets: a `LineSplitter` feeding a `SerialDeframer`.
///
/// Not thread-safe; it belongs to whichever thread reads the port. The
/// counters are cumulative for the life of the object and survive `reset()`,
/// because they describe the link, not the packet in progress.
class SerialInbound
{
public:
    /// \param max_packet The largest packet assembled; see `SerialDeframer`.
    ///                   The default is the core's own reassembly bound, so a
    ///                   packet this lets through is one `SmpClient` could
    ///                   accept -- a response may legitimately be larger than
    ///                   anything this side sends.
    explicit SerialInbound(std::size_t max_packet = limits::kMaxAssemblyBuffer) noexcept
        : deframer_{max_packet}
    {}

    /// Consumes \p chunk, calling \p on_packet once per complete, CRC-verified
    /// packet, in order.
    ///
    /// The span handed to \p on_packet is valid **only for that call**: it
    /// points into the deframer, which reuses the storage for the next packet.
    /// A caller crossing a thread boundary copies it there.
    template<typename OnPacket>
    void feed(ConstBytes chunk, OnPacket&& on_packet)
    {
        while (const std::optional<ConstBytes> line = splitter_.next_line(chunk)) {
            if (deframer_.feed_line(*line) == SerialDeframer::Outcome::Packet) {
                on_packet(deframer_.packet());
            }
        }
    }

    /// Discards any partial line and partial packet. For a port that was
    /// reopened, so a fragment from before cannot join one from after.
    void reset() noexcept
    {
        splitter_.reset();
        deframer_.reset();
    }

    /// What the deframer made of every line so far.
    [[nodiscard]] SerialCounters deframe_counters() const noexcept
    {
        return deframer_.counters();
    }

    /// Lines longer than a frame, dropped before reaching the deframer.
    [[nodiscard]] std::uint64_t dropped_lines() const noexcept
    {
        return splitter_.dropped_lines();
    }

private:
    LineSplitter splitter_;
    SerialDeframer deframer_;
};

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_SERIAL_PORT_SERIAL_LINK_HPP
