// SPDX-License-Identifier: Apache-2.0
//
// MCUmgr serial deframing over a hostile console stream, cut where the fuzzer
// chooses.
//
// The first byte of the input is the read size, so the fuzzer controls how the
// stream is chopped as well as what is in it. That matters more here than it
// does for `fuzz_assembler`: the line reader's whole job is to be indifferent
// to where a read ended, and a declared length followed by a newline three
// reads later is exactly the shape a real port produces.
//
// Three properties, none of them "no crash" -- ASan and UBSan give that for
// free, and a target that asserts nothing notices nothing when a bound quietly
// stops holding (docs/testing.md section 7):
//
//   * neither buffer ever exceeds its configured bound, at every step rather
//     than at the end;
//   * a framing error leaves the deframer empty, so a partial packet cannot
//     bleed into whatever the stream says next;
//   * a delivered packet is one the CRC accepted, and it is never longer than
//     the bound that was supposed to cap it.

#include "fuzz_support.hpp"

#include "serial/serial_framing.hpp"

#include "smply/bytes.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {

/// Small enough that a fuzzer can reach the cap with a plausible input, which
/// a 16 KiB default would make expensive to hit by chance.
constexpr std::size_t kMaxPacket = 1024;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2 || size > smply::fuzz::kMaxUsefulInput) {
        return 0;
    }

    const std::size_t read_size = static_cast<std::size_t>(data[0]) + 1;
    const smply::ConstBytes stream = smply::fuzz::view(data + 1, size - 1);

    smply::transport::LineSplitter splitter;
    smply::transport::SerialDeframer deframer{kMaxPacket};

    for (std::size_t offset = 0; offset < stream.size();) {
        const std::size_t take = std::min(read_size, stream.size() - offset);
        smply::ConstBytes rest = stream.subspan(offset, take);
        offset += take;

        while (const auto line = splitter.next_line(rest)) {
            // A line handed on is one that could be a frame. The splitter is
            // the only thing standing between an unterminated stream and an
            // unbounded buffer, so this is checked before the line is used.
            assert(line->size() <= smply::transport::kMaxFrame);

            const auto outcome = deframer.feed_line(*line);
            assert(deframer.buffered() <= kMaxPacket);
            assert(deframer.peak_buffered() <= kMaxPacket);

            switch (outcome) {
            case smply::transport::SerialDeframer::Outcome::Packet:
                // Delivered means CRC-verified, and the length the device
                // declared was bounded before the buffer was allowed to reach
                // it.
                assert(!deframer.packet().empty());
                assert(deframer.packet().size() <= kMaxPacket);
                break;
            case smply::transport::SerialDeframer::Outcome::Error:
                // Nothing partial survives a framing violation.
                assert(deframer.buffered() == 0);
                break;
            case smply::transport::SerialDeframer::Outcome::Ignored:
            case smply::transport::SerialDeframer::Outcome::NeedMore:
                break;
            }
        }
        // Whatever is still being assembled is one line at most.
        assert(splitter.buffered() <= smply::transport::kMaxFrame);
    }
    return 0;
}
