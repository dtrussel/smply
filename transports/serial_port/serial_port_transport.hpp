// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_SERIAL_PORT_TRANSPORT_HPP
#define SMPLY_TRANSPORTS_SERIAL_PORT_TRANSPORT_HPP

/// \file
/// A `smply::Transport` over a serial port: a hardware UART, a USB CDC ACM
/// port, or anything else that looks like a tty.
///
/// The reference serial adapter (docs/design.md section 13,
/// [ADR-0020](../../docs/decisions/ADR-0020-serial-port-reference-adapter.md)).
/// It speaks MCUmgr's console framing (`serial/serial_framing.hpp`,
/// docs/protocol-notes.md section 8): each outbound SMP message becomes a run
/// of base64 frames, and inbound frames are undone into whole SMP packets
/// before they reach the core, which reassembles them as it would bytes off
/// any other transport (ADR-0006).
///
/// **This header names no OS type.** POSIX (`posix/`, termios and `poll()`) and
/// Win32 (`win32/`, overlapped I/O) implement it behind an opaque `State`.
///
/// ### Threading
///
/// Each open port has **one I/O thread, owned by this object**, which reads,
/// writes and deframes. Nothing it produces reaches the listener directly: each
/// packet is copied into a closure posted to the `Dispatcher` the caller
/// supplied, and runs on the client context when the application drains it
/// (ADR-0004). The core still starts no thread; this one is the adapter's.
///
/// ### What the caller has to get right
///
/// * **Lifetime.** Like every transport, this must outlive every `SmpClient`
///   bound to it (smply/transport.hpp). The `Dispatcher` must outlive this
///   object, and something must drain it: nothing arrives without a `drain()`.
/// * **Reconnecting is opening again.** A dropped link stays dropped. After a
///   device reset the application calls `open()` again, by path, and
///   `SmpClient::rebind_transport()`, as `examples/serial_dfu/` does.
///
/// ### A device reset (roadmap O7)
///
/// The adapter reports what the medium says, and assumes nothing else.
/// * A **USB CDC ACM** port disappears when the device resets. The read fails,
///   and the listener gets `on_disconnected(Disconnected)`.
/// * A **hardware UART** stays open, so nothing is reported, and
///   `FirmwareUpdater` moves on when `UpdatePlan::disconnect_grace` expires. A
///   serial application should set that to a few seconds.
///
/// What the device prints while it boots arrives as ignored console lines and
/// is counted, not delivered.

#include "serial_port/serial_port_config.hpp"

#include "smply/bytes.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"
#include "smply/util/dispatcher.hpp"

#include <cstddef>
#include <memory>

namespace smply::transport {

/// One open serial port carrying MCUmgr's console framing.
class SerialPortTransport final : public Transport
{
public:
    /// Everything the adapter owns, defined per platform.
    ///
    /// Opaque by design: this is the pimpl, declared here only so that the
    /// implementation's own helpers can name it. A caller can neither construct
    /// one nor reach anything inside it.
    struct State;

    /// Opens and configures the port, and starts its I/O thread.
    ///
    /// Configures 8N1 at `config.baud` with the requested flow control, and
    /// discards anything already buffered in either direction, so bytes left
    /// over from before the open cannot be read as the start of a frame.
    ///
    /// Every failure releases what it had opened before returning, so a caller
    /// may retry, which is what reconnecting after a reset is.
    ///
    /// \param config  The port and its settings; see `SerialPortConfig`.
    /// \param inbound Carries I/O-thread callbacks onto the client context.
    ///                Must outlive this transport, and must be drained.
    /// \return The transport. Or `InvalidArgument` for a configuration
    ///         `validate()` refuses, a rate the platform cannot set, or a path
    ///         that is not a serial port. `Disconnected` when the port does not
    ///         exist (yet), which is what a USB port mid-reset looks like.
    ///         `TransportError` when it exists but cannot be opened or
    ///         configured, for example because another process holds it.
    [[nodiscard]] static Result<std::unique_ptr<SerialPortTransport>>
    open(const SerialPortConfig& config, Dispatcher& inbound);

    SerialPortTransport(const SerialPortTransport&) = delete;
    SerialPortTransport(SerialPortTransport&&) = delete;
    SerialPortTransport& operator=(const SerialPortTransport&) = delete;
    SerialPortTransport& operator=(SerialPortTransport&&) = delete;

    /// Calls `close()`, so destroying the transport is a safe shutdown.
    ~SerialPortTransport() override;

    // --- Transport. Every one of these is called on the client context ------

    /// Frames \p message and hands it to the I/O thread.
    ///
    /// Returns once the message is accepted, without blocking and without
    /// delivering anything to the listener, as smply/transport.hpp requires.
    /// A write that fails later arrives as `on_transport_error()`, or as
    /// `on_disconnected()` when the port has gone.
    ///
    /// \return `TransportBusy` when **two** messages are already outbound (one
    ///         being written, one waiting; `common/send_queue.hpp`),
    ///         `MessageTooLarge` above `max_message_size()`,
    ///         `InvalidArgument` for an empty message, and `Disconnected` once
    ///         the link is closing or gone.
    [[nodiscard]] Result<void> send(ConstBytes message) override;

    /// `SerialPortConfig::max_message_size`, unchanged.
    [[nodiscard]] std::size_t max_message_size() const noexcept override;

    void set_listener(TransportListener* listener) noexcept override;

    /// Stops the I/O thread, joins it, and closes the port. Synchronous and
    /// idempotent, and no listener callback can fire once it has returned,
    /// including one already queued in the `Dispatcher`. It does **not**
    /// drain or clear that `Dispatcher`, which belongs to the application.
    void close() noexcept override;

    // --- Diagnostics ---------------------------------------------------------

    /// A snapshot of what the link has seen; see `SerialLinkCounters`.
    ///
    /// Not part of `Transport`. It is here because a serial link that loses
    /// frames looks exactly like a silent device, and only the counts tell them
    /// apart. Takes the adapter's mutex, so it is not `noexcept`. Callable from
    /// any thread, and still readable after `close()`.
    [[nodiscard]] SerialLinkCounters counters() const;

    /// Not for callers, despite being reachable: `open()` is the only thing
    /// that can produce a `State`.
    explicit SerialPortTransport(std::shared_ptr<State> state) noexcept;

private:
    /// A `shared_ptr` because closures posted to the `Dispatcher` capture a
    /// copy. A packet that arrived just before `close()` then keeps the state
    /// alive until it is drained, where it finds the link closed and does
    /// nothing. The alternative, clearing the queue on close, is not available:
    /// the dispatcher is the application's and may carry another link's work.
    std::shared_ptr<State> state_;
};

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_SERIAL_PORT_TRANSPORT_HPP
