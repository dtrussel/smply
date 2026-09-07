// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_COMMON_LINK_STATE_HPP
#define SMPLY_TRANSPORTS_COMMON_LINK_STATE_HPP

/// \file
/// The shutdown half of the transport contract, as a value.
///
/// `Transport::close()` carries an obligation that reads simply and is easy to
/// implement wrongly (docs/design.md sections 9 and 10): it is **synchronous
/// and idempotent, and after it returns no callback may fire**. A real adapter
/// satisfies that by revoking event tokens, waiting for in-flight async
/// operations to observe a cancellation flag, and draining its dispatcher — all
/// of it platform-specific, none of it testable here.
///
/// What *is* testable, and what adapters get wrong, is the bookkeeping around
/// it: closing twice, delivering a callback that was already in flight when
/// `close()` was called, or accepting a `send()` after the link is gone. This
/// type holds that bookkeeping so an adapter asks it rather than reimplementing
/// it, and so the rule is a predicate a test can assert instead of a paragraph
/// somebody has to remember.
///
/// It is not thread-safe, deliberately: it belongs to the client context, like
/// everything else the core touches (ADR-0004). A driver thread that wants to
/// report a disconnect posts to a `smply::Dispatcher` and lets the closure ask.

#include <cstdint>

namespace smply::transport {

/// Where a link has got to.
enum class LinkPhase : std::uint8_t
{
    /// Usable: sends are accepted and callbacks may be delivered.
    Open,
    /// `close()` is running. Nothing new is accepted, and no callback may be
    /// delivered — including one that was already in flight, which is exactly
    /// the case an adapter forgets.
    Closing,
    /// `close()` has returned. Terminal: a link is never reopened, and an
    /// application that wants another one constructs another transport.
    Closed,
};

/// The state machine behind `close()`.
///
/// Terminal by design. A transport that could reopen would let a stale
/// `TransportListener` pointer come back to life after `SmpClient` had detached
/// from it, which is the lifetime hazard the whole contract exists to prevent —
/// so reconnecting means a new transport and `SmpClient::rebind_transport()`,
/// as `FakeTransport` and the `cli_dfu` example both demonstrate.
class LinkState
{
public:
    [[nodiscard]] constexpr LinkPhase phase() const noexcept
    {
        return phase_;
    }

    /// True only while `Open`.
    [[nodiscard]] constexpr bool is_open() const noexcept
    {
        return phase_ == LinkPhase::Open;
    }

    /// True once `close()` has begun, whether or not it has finished.
    [[nodiscard]] constexpr bool is_closing_or_closed() const noexcept
    {
        return phase_ != LinkPhase::Open;
    }

    /// May a `send()` be accepted? Only on an open link.
    [[nodiscard]] constexpr bool may_send() const noexcept
    {
        return is_open();
    }

    /// May a callback be delivered to the listener?
    ///
    /// **False from the moment `close()` begins**, not from when it ends. An
    /// adapter typically has callbacks already queued or in flight at that
    /// point; the contract says none of them may arrive, so the answer has to
    /// change at `begin_close()` rather than at `finish_close()`.
    [[nodiscard]] constexpr bool may_deliver() const noexcept
    {
        return is_open();
    }

    /// Enters `Closing`.
    ///
    /// \return true if this call started the close, false if a close had
    ///         already begun or finished. **That return is what makes `close()`
    ///         idempotent**: an adapter does its revoke-and-drain work only
    ///         when this says true, and a second `close()` becomes a no-op
    ///         rather than a second teardown of already-torn-down state.
    [[nodiscard]] constexpr bool begin_close() noexcept
    {
        if (phase_ != LinkPhase::Open) {
            return false;
        }
        phase_ = LinkPhase::Closing;
        return true;
    }

    /// Enters `Closed`. Call once the teardown is complete; harmless if the
    /// link was already closed.
    constexpr void finish_close() noexcept
    {
        phase_ = LinkPhase::Closed;
    }

private:
    LinkPhase phase_ = LinkPhase::Open;
};

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_COMMON_LINK_STATE_HPP
