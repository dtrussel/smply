// SPDX-License-Identifier: Apache-2.0

#include "smp/assembler.hpp"

#include "smply/error.hpp"

#include <algorithm>
#include <cstddef>

namespace smply {
namespace {

/// Initial reservation. Enough for a typical SMP message so steady-state
/// traffic never reallocates, without committing max_buffer up front.
constexpr std::size_t kInitialCapacity = 1024;

} // namespace

MessageAssembler::MessageAssembler(AssemblerLimits limits) : limits_{limits}
{
    buffer_.reserve(std::min(kInitialCapacity, limits_.max_buffer));
}

void MessageAssembler::reset() noexcept
{
    // clear() keeps the capacity, so a reconnect does not pay to grow again.
    buffer_.clear();
}

Result<std::size_t> MessageAssembler::message_size(const Header& header) const
{
    if (header.length > limits_.max_payload) {
        return fail(ErrorCode::MessageTooLarge, "assembler: payload exceeds limit");
    }

    const std::size_t total = header.total_size();
    // Checked before waiting for the rest, not after: a device claiming more
    // than we will ever hold must fail now, otherwise the stream stalls
    // forever waiting for bytes that would be refused on arrival.
    if (total > limits_.max_buffer) {
        return fail(ErrorCode::MessageTooLarge, "assembler: message exceeds buffer limit");
    }
    return total;
}

void MessageAssembler::stash(ConstBytes bytes)
{
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    peak_buffered_ = std::max(peak_buffered_, buffer_.size());
}

Result<void> MessageAssembler::feed(ConstBytes input, MessageSink& sink)
{
    if (feeding_) {
        // A sink that re-enters would mutate the buffer its payload points
        // into. Refusing is a diagnosable error; proceeding is a use-after-free.
        return fail(ErrorCode::InvalidState, "assembler: reentrant feed");
    }
    feeding_ = true;

    struct Guard
    {
        bool& flag;

        ~Guard()
        {
            flag = false;
        }
    } const guard{feeding_};

    while (true) {
        if (buffer_.empty()) {
            // Fast path: parse straight out of the caller's bytes. Whole
            // messages -- the common case -- are never copied at all.
            if (input.size() < kHeaderSize) {
                stash(input);
                return {};
            }

            auto header = decode_header(input);
            if (!header) {
                reset();
                return fail(header.error());
            }

            auto total = message_size(*header);
            if (!total) {
                reset();
                return fail(total.error());
            }

            if (input.size() < *total) {
                stash(input);
                return {};
            }

            sink.on_message(*header, input.subspan(kHeaderSize, header->length));
            input = input.subspan(*total);
            continue;
        }

        // Slow path: a partial message is held. Top it up from the input, one
        // stage at a time, so the buffer never holds more than a single
        // message's worth.
        if (buffer_.size() < kHeaderSize) {
            const std::size_t wanted = kHeaderSize - buffer_.size();
            const std::size_t take = std::min(wanted, input.size());
            stash(input.first(take));
            input = input.subspan(take);
            if (buffer_.size() < kHeaderSize) {
                return {}; // header still incomplete
            }
        }

        auto header = decode_header(ConstBytes{buffer_});
        if (!header) {
            reset();
            return fail(header.error());
        }

        auto total = message_size(*header);
        if (!total) {
            reset();
            return fail(total.error());
        }

        if (buffer_.size() < *total) {
            const std::size_t wanted = *total - buffer_.size();
            const std::size_t take = std::min(wanted, input.size());
            stash(input.first(take));
            input = input.subspan(take);
            if (buffer_.size() < *total) {
                return {}; // payload still incomplete
            }
        }

        sink.on_message(*header, ConstBytes{buffer_}.subspan(kHeaderSize, header->length));
        // Exactly one message was buffered, so the buffer is now spent.
        // clear() rather than erase(): it keeps the capacity and copies nothing.
        buffer_.clear();
    }
}

} // namespace smply
