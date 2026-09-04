// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_SMP_ASSEMBLER_HPP
#define SMPLY_SRC_SMP_ASSEMBLER_HPP

/// \file
/// Turns an arbitrary byte stream into complete SMP messages.
///
/// Internal to smply; not a public header.
///
/// Transports deliver bytes with no message boundaries of their own -- a BLE
/// notification is `ATT_MTU - 3` bytes, not one message -- so somebody must
/// reassemble. Doing it here rather than in each transport means one
/// implementation of the most security-sensitive parsing in the library, with
/// one set of bounds and one fuzz target (ADR-0006).
///
/// Everything arriving here is untrusted. The declared length is checked
/// against configured bounds before a single byte is buffered on its behalf, so
/// a device claiming 60 KiB produces a bounded error rather than a 60 KiB
/// allocation (docs/security.md, T2 and T3).

#include "smply/bytes.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"
#include "smply/smp/header.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace smply {

/// Receives each complete message the assembler recovers.
class MessageSink
{
public:
    MessageSink() = default;
    MessageSink(const MessageSink&) = delete;
    MessageSink(MessageSink&&) = delete;
    MessageSink& operator=(const MessageSink&) = delete;
    MessageSink& operator=(MessageSink&&) = delete;
    virtual ~MessageSink() = default;

    /// \param header  The decoded header.
    /// \param payload `header.length` bytes, borrowed for the duration of this
    ///                call only. Copy anything that must outlive it.
    ///
    /// Must not call back into the assembler that invoked it; doing so would
    /// mutate the buffer this payload points into. The assembler detects and
    /// rejects that rather than corrupting memory.
    virtual void on_message(const Header& header, ConstBytes payload) = 0;
};

/// Bounds on what the assembler will hold on a device's behalf.
struct AssemblerLimits
{
    /// Largest accepted payload, excluding the header.
    std::uint16_t max_payload = limits::kMaxSmpPayload;
    /// Largest number of bytes held while a message is incomplete.
    std::size_t max_buffer = limits::kMaxAssemblyBuffer;
};

/// Reassembles SMP messages from a byte stream.
///
/// Not thread-safe, and not required to be: it is driven from the client
/// context only (ADR-0004).
class MessageAssembler
{
public:
    explicit MessageAssembler(AssemblerLimits limits = {});

    /// Consumes \p input, invoking \p sink once per complete message.
    ///
    /// Fragmentation is irrelevant to the result: feeding a stream whole, one
    /// byte at a time, or split at arbitrary points all produce the same
    /// sequence of messages.
    ///
    /// On failure the assembler is reset and the error returned. A stream whose
    /// framing has been violated cannot be resynchronised -- there is no
    /// sentinel to hunt for -- so discarding it is the only safe response; the
    /// caller is expected to drop the connection.
    [[nodiscard]] Result<void> feed(ConstBytes input, MessageSink& sink);

    /// Discards any partial message. Call on connect and disconnect so a
    /// truncated message cannot bleed into the next session.
    void reset() noexcept;

    /// Bytes currently held for an incomplete message.
    [[nodiscard]] std::size_t buffered() const noexcept
    {
        return buffer_.size();
    }

    /// High-water mark of buffered(), for tests and diagnostics.
    [[nodiscard]] std::size_t peak_buffered() const noexcept
    {
        return peak_buffered_;
    }

    /// Allocated capacity, for tests asserting the bound is real.
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return buffer_.capacity();
    }

    [[nodiscard]] const AssemblerLimits& limits() const noexcept
    {
        return limits_;
    }

private:
    /// Validates a decoded header and returns the whole message's size.
    [[nodiscard]] Result<std::size_t> message_size(const Header& header) const;

    void stash(ConstBytes bytes);

    AssemblerLimits limits_;
    std::vector<std::byte> buffer_;
    std::size_t peak_buffered_ = 0;
    bool feeding_ = false;
};

} // namespace smply

#endif // SMPLY_SRC_SMP_ASSEMBLER_HPP
