// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_WINRT_BLE_TRANSPORT_HPP
#define SMPLY_TRANSPORTS_WINRT_BLE_TRANSPORT_HPP

/// \file
/// A `smply::Transport` over Bluetooth LE, using C++/WinRT's GATT API.
///
/// The reference adapter (docs/design.md section 10). It speaks SMP over the
/// service and characteristic in docs/protocol-notes.md section 8: requests go
/// out as GATT write-without-response, responses arrive as notifications, and a
/// message too large for one packet is split across several with **no
/// additional framing** -- so the splitting is `transports/common`'s
/// `Fragmenter` and the reassembly is the core's (ADR-0006).
///
/// **This header names no WinRT type.** Everything from the projection lives in
/// the implementation behind a `State` that is only forward-declared here, so
/// including this file costs an ordinary MSVC translation unit nothing and no
/// consumer inherits a dependency on the Windows SDK's headers through it.
///
/// ### What the caller has to get right
///
/// * **A multi-threaded apartment.** `connect()` blocks on WinRT asynchronous
///   operations, which deadlocks on a single-threaded apartment. The caller
///   owns `winrt::init_apartment(winrt::apartment_type::multi_threaded)`, and
///   there is no diagnostic if it is wrong -- it simply hangs. See the README.
/// * **Lifetime.** Like every transport, this must outlive every `SmpClient`
///   bound to it (smply/transport.hpp): `~SmpClient` and `rebind_transport()`
///   both detach through `set_listener(nullptr)`.
/// * **The `Dispatcher` outlives this object**, and something drains it. It is
///   how inbound bytes reach the client context; nothing arrives without a
///   `drain()`.
///
/// ### What it does not do
///
/// It does not scan, pair, or decide when to reconnect: it is handed a
/// Bluetooth address and connects to it. A dropped link stays dropped --
/// reconnecting means a new transport and `SmpClient::rebind_transport()`, as
/// `examples/cli_dfu/main.cpp` demonstrates against a stub device.

#include "common/send_queue.hpp"

#include "smply/bytes.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"
#include "smply/util/dispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace smply::transport {

/// Everything about the adapter a caller may choose.
struct WinRtBleConfig
{
    /// The largest whole SMP message this transport reports it can carry.
    ///
    /// **Not the MTU**, and not derived from it: a message is split across as
    /// many GATT packets as it needs, so the two are unrelated limits and
    /// docs/protocol-notes.md section 8 keeps them in separate rows of its
    /// table. This is the point beyond which the transport declines to carry a
    /// message at all, and it feeds upload chunk sizing together with the
    /// device's own `buf_size`.
    ///
    /// 1024 is comfortable for any Zephyr build and small enough that a
    /// retransmission is cheap.
    std::size_t max_message_size = 1024;
};

/// The smallest `WinRtBleConfig::max_message_size` that is accepted.
///
/// A budget below this cannot carry an 8-byte SMP header plus the CBOR overhead
/// of a first upload packet and still leave the 32-byte minimum chunk that
/// protocol-notes section 6 rule 2 requires, so a smaller value is a
/// configuration mistake rather than a conservative choice.
inline constexpr std::size_t kMinConfiguredMessageSize = 128;

/// One Bluetooth LE link to a device running Zephyr's SMP server.
class WinRtBleTransport final : public Transport
{
public:
    /// Everything the adapter owns, defined in the implementation.
    ///
    /// Opaque by design: this is the pimpl, and it is declared here only so
    /// that the implementation's own helpers can name it. A caller can neither
    /// construct one nor reach anything inside it.
    struct State;

    /// Connects to \p bluetooth_address and subscribes to notifications.
    ///
    /// Performs the whole opening sequence -- resolve the device, find the SMP
    /// service and characteristic, enable notifications -- and returns a
    /// transport that is ready to carry a message, or the reason it could not.
    ///
    /// **Blocks** until the Bluetooth stack answers, which is why it must not
    /// be called from a single-threaded apartment. That is acceptable here in a
    /// way it never is afterwards: this runs before there is a pump to stall.
    /// It may also spend **up to about two seconds more than the stack needs**,
    /// re-running discovery while it comes back empty: after a rapid reconnect
    /// Windows answers from its own service cache, and a service with no
    /// characteristics is a reconnect that would have succeeded a moment later
    /// (protocol-notes section 9, A22). That bound is paid only when a
    /// collection is empty, never when discovery is refused outright.
    ///
    /// Every failure closes what it had already opened before returning, so a
    /// caller may retry without leaking a session that is holding a connection
    /// open.
    ///
    /// \param bluetooth_address The 48-bit device address, as WinRT reports it.
    /// \param inbound  Carries device-thread callbacks onto the client context.
    ///                 Must outlive this transport, and must be drained.
    /// \param config   Sizing; see `WinRtBleConfig`.
    /// \return The transport, or `InvalidArgument` for a bad configuration,
    ///         `Disconnected` when the device cannot be reached or discovery
    ///         is refused outright, and `TransportError` when it answers but,
    ///         after every attempt, has no SMP service -- or has one with no
    ///         SMP characteristic, which carries its own message because the
    ///         two say different things about the device.
    [[nodiscard]] static Result<std::unique_ptr<WinRtBleTransport>>
    connect(std::uint64_t bluetooth_address, Dispatcher& inbound,
            const WinRtBleConfig& config = {});

    WinRtBleTransport(const WinRtBleTransport&) = delete;
    WinRtBleTransport(WinRtBleTransport&&) = delete;
    WinRtBleTransport& operator=(const WinRtBleTransport&) = delete;
    WinRtBleTransport& operator=(WinRtBleTransport&&) = delete;

    /// Calls `close()`, so destroying the transport is a safe shutdown.
    ~WinRtBleTransport() override;

    // --- Transport. Every one of these is called on the client context ------

    /// Splits \p message into `MaxPduSize - 3` byte fragments and writes them.
    ///
    /// Returns as soon as the message is accepted, without blocking and without
    /// delivering anything to the listener -- both required by
    /// smply/transport.hpp. The writes themselves happen on a background
    /// thread, so a failure discovered after this returns arrives later as
    /// `on_transport_error()` or `on_disconnected()`.
    ///
    /// **One message may wait while another is being written.** A boolean
    /// "a write is in progress" refused the next message during the window
    /// between the device answering and the local write's continuation running,
    /// which killed uploads under load (protocol-notes section 9, A22); the
    /// admission rules now live in `SendQueue`.
    ///
    /// \return `TransportBusy` if **two** messages are already outbound -- with
    ///         one request in flight that means the medium has stalled, so it is
    ///         a retry request and not a broken link -- and `Disconnected` once
    ///         the link is closing or gone.
    [[nodiscard]] Result<void> send(ConstBytes message) override;

    /// The configured cap, unchanged. See `WinRtBleConfig::max_message_size`.
    [[nodiscard]] std::size_t max_message_size() const noexcept override;

    void set_listener(TransportListener* listener) noexcept override;

    /// Revokes the WinRT event handlers, waits for any write still in flight,
    /// and closes the link. Synchronous, idempotent, and no listener callback
    /// can fire once it has returned -- including one that was already queued.
    void close() noexcept override;

    // --- Diagnostics ---------------------------------------------------------

    /// How often a message waited for the writer, and how often one was refused.
    ///
    /// Not part of `Transport`, and here because a bench run needs it to be
    /// evidence: a hardware suite that passes with `deferred == 0` has not shown
    /// that deferring works, only that the race did not happen that time
    /// (ADR-0015). Takes the send mutex, so it is not `noexcept`.
    [[nodiscard]] SendCounters send_counters() const;

    /// Not for callers, despite being reachable: `connect()` is the only thing
    /// that can produce a `State`, because a transport not attached to a live
    /// characteristic has nothing it could do.
    explicit WinRtBleTransport(std::shared_ptr<State> state) noexcept;

private:
    /// A `shared_ptr` rather than a `unique_ptr`, and that is load-bearing.
    ///
    /// Closures posted to the `Dispatcher` capture a copy, so a notification
    /// that arrived just before `close()` keeps the state it names alive until
    /// it is drained -- where it finds the link closed and does nothing. The
    /// alternative, clearing the queue on close, is not available: the
    /// dispatcher belongs to the application and may be carrying another
    /// transport's work.
    std::shared_ptr<State> state_;
};

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_WINRT_BLE_TRANSPORT_HPP
