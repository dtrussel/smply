// SPDX-License-Identifier: Apache-2.0
//
// SMP reassembly over a hostile stream, split where the fuzzer chooses.
//
// The first byte of the input is the fragment size, so the fuzzer controls
// framing as well as content: a length field claiming 60 KiB followed by four
// bytes is the case the bound exists for, and it is only interesting if the
// four bytes can arrive separately.
//
// The property is the bound itself. Whatever the stream says, the assembler
// must never buffer more than its configured limit -- a device that can make a
// client allocate is a denial of service at best.

#include "fuzz_support.hpp"

#include "smp/assembler.hpp"

#include "smply/limits.hpp"
#include "smply/result.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {

/// Counts messages and holds nothing, so the assembler's own buffering is the
/// only thing this target measures.
class CountingSink final : public smply::MessageSink
{
public:
    void on_message(const smply::Header& header, smply::ConstBytes payload) override
    {
        // A delivered message must be internally consistent, or reassembly has
        // handed its caller something the stream never contained.
        assert(payload.size() == header.length);
        ++messages_;
    }

    [[nodiscard]] std::size_t messages() const noexcept
    {
        return messages_;
    }

private:
    std::size_t messages_ = 0;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2 || size > smply::fuzz::kMaxUsefulInput) {
        return 0;
    }

    // The first byte chooses the fragmentation; the rest is the stream.
    const std::size_t fragment = static_cast<std::size_t>(data[0]) + 1;
    const smply::ConstBytes stream = smply::fuzz::view(data + 1, size - 1);

    smply::MessageAssembler assembler;
    CountingSink sink;

    for (std::size_t offset = 0; offset < stream.size();) {
        const std::size_t take = std::min(fragment, stream.size() - offset);
        const smply::Result<void> fed = assembler.feed(stream.subspan(offset, take), sink);
        offset += take;

        // The bound holds at every step, not merely at the end.
        assert(assembler.buffered() <= assembler.limits().max_buffer);
        assert(assembler.peak_buffered() <= assembler.limits().max_buffer);

        if (!fed.has_value()) {
            // Framing violated: the stream cannot be resynchronised, and the
            // assembler must have reset itself rather than keeping a partial
            // message that would bleed into whatever came next.
            assert(assembler.buffered() == 0);
            return 0;
        }
    }
    return 0;
}
