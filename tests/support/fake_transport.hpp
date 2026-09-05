// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_FAKE_TRANSPORT_HPP
#define SMPLY_TESTS_FAKE_TRANSPORT_HPP

/// \file
/// The transport every test drives the core through.
///
/// It does two jobs. It **records** what the core sent, as whole SMP messages,
/// so a test can assert on the exact command sequence. And it lets a test
/// **inject** inbound bytes with complete control over fragmentation and
/// timing, plus the faults a real link produces.
///
/// It also enforces the contract in `smply/transport.hpp` rather than merely
/// implementing it: delivering after `disconnect()` or `close()` is silently
/// suppressed and counted, so a test that accidentally relies on a
/// post-disconnect callback fails on `suppressed_deliveries()` instead of
/// passing for the wrong reason. A real transport must offer the same
/// guarantee, so the double must not be more permissive than the contract.

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace smply::test {

class FakeTransport final : public Transport
{
public:
    FakeTransport() = default;

    // --- Transport ---------------------------------------------------------

    [[nodiscard]] Result<void> send(ConstBytes message) override;
    [[nodiscard]] std::size_t max_message_size() const noexcept override;
    void set_listener(TransportListener* listener) noexcept override;
    void close() noexcept override;

    // --- Inspection --------------------------------------------------------

    /// Whole SMP messages the core has sent, oldest first.
    [[nodiscard]] const std::vector<std::vector<std::byte>>& sent() const noexcept
    {
        return sent_;
    }

    [[nodiscard]] std::size_t send_count() const noexcept
    {
        return sent_.size();
    }

    /// The most recent message. Precondition: at least one was sent.
    [[nodiscard]] ConstBytes last_sent() const;

    void clear_sent() noexcept
    {
        sent_.clear();
    }

    [[nodiscard]] bool closed() const noexcept
    {
        return closed_;
    }

    [[nodiscard]] bool connected() const noexcept
    {
        return !closed_ && !disconnected_;
    }

    /// Deliveries dropped because the transport was already closed or
    /// disconnected. Non-zero means a test relied on a callback the contract
    /// forbids.
    [[nodiscard]] std::size_t suppressed_deliveries() const noexcept
    {
        return suppressed_deliveries_;
    }

    /// `on_bytes()` calls actually made, for asserting a delivery pattern.
    [[nodiscard]] std::size_t on_bytes_calls() const noexcept
    {
        return on_bytes_calls_;
    }

    // --- Configuration -----------------------------------------------------

    void set_max_message_size(std::size_t size) noexcept
    {
        max_message_size_ = size;
    }

    // --- Injection ---------------------------------------------------------

    /// One `on_bytes()` call with everything.
    void deliver(ConstBytes bytes);

    /// Fixed-size fragments; the last may be short. `fragment` must be > 0.
    void deliver_fragmented(ConstBytes bytes, std::size_t fragment);

    /// One `on_bytes()` call per byte -- the worst case for any reassembler.
    void deliver_byte_by_byte(ConstBytes bytes);

    /// Several buffers concatenated into a single `on_bytes()` call, modelling
    /// a device whose responses arrive back to back.
    void deliver_concatenated(std::span<const ConstBytes> buffers);

    /// Split at the given offsets, so a test can place a boundary exactly where
    /// it wants one -- mid-header, or one byte before a message ends. Offsets
    /// must be strictly increasing and within `bytes`; out-of-range or
    /// duplicate offsets are ignored.
    void deliver_split_at(ConstBytes bytes, std::span<const std::size_t> cuts);

    // --- Faults ------------------------------------------------------------

    /// The next `send()` fails with this error, once.
    void fail_next_send(Error error);

    /// While set, every `send()` returns `TransportBusy`.
    void set_busy(bool busy) noexcept
    {
        busy_ = busy;
    }

    /// Reports a recoverable failure. The link stays up.
    void raise_transport_error(Error error);

    /// Reports the link as gone. Terminal: later deliveries are suppressed.
    void disconnect(Error error = Error{ErrorCode::Disconnected, "fake transport"});

private:
    /// True when a listener callback may still be issued.
    [[nodiscard]] bool deliverable() const noexcept;

    void emit(ConstBytes bytes);

    TransportListener* listener_ = nullptr;
    std::vector<std::vector<std::byte>> sent_;
    std::optional<Error> next_send_failure_;
    std::size_t max_message_size_ = 0;
    std::size_t suppressed_deliveries_ = 0;
    std::size_t on_bytes_calls_ = 0;
    bool busy_ = false;
    bool closed_ = false;
    bool disconnected_ = false;
};

} // namespace smply::test

#endif // SMPLY_TESTS_FAKE_TRANSPORT_HPP
