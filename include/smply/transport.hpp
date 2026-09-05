// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORT_HPP
#define SMPLY_TRANSPORT_HPP

/// \file
/// The boundary between smply's protocol core and the outside world (ADR-0005).
///
/// This is a normative contract, not merely a pair of interfaces. Everything
/// below marked **must** is relied upon by the core; a transport that breaks one
/// of these rules will produce corruption or use-after-free rather than a
/// diagnosable error. The summary table in docs/design.md section 9 is the same
/// contract in one page.
///
/// The division of labour is deliberate and asymmetric:
///
/// * **Outbound**, the transport receives exactly one complete SMP message and
///   is responsible for getting it there -- splitting it into GATT writes or
///   UART frames as its medium requires. The core never sees a fragment.
/// * **Inbound**, the transport hands over whatever bytes arrived, with no
///   framing work at all. The core reassembles (ADR-0006), so that the most
///   security-sensitive parsing in the library exists once rather than once per
///   adapter.
///
/// A transport therefore never needs to know what an SMP message means, and the
/// core never needs to know what an MTU is.

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"

#include <cstddef>

namespace smply {

/// Receives everything a transport has to say. Implemented by the core
/// (`SmpClient`); a transport only calls it.
///
/// **Threading.** Every method must be invoked on the client context -- the
/// thread that drives `SmpClient::poll()`. A transport whose driver delivers on
/// another thread (WinRT's thread pool, a serial reader thread) must marshal
/// first; `smply::Dispatcher` exists for exactly that. This is the adapter's
/// obligation because only the adapter knows its threading environment
/// (ADR-0004).
class TransportListener
{
public:
    TransportListener() = default;
    TransportListener(const TransportListener&) = delete;
    TransportListener(TransportListener&&) = delete;
    TransportListener& operator=(const TransportListener&) = delete;
    TransportListener& operator=(TransportListener&&) = delete;
    virtual ~TransportListener() = default;

    /// Inbound bytes, in arbitrary chunks.
    ///
    /// There is no requirement that a chunk be a message, part of one message,
    /// or aligned to anything. One byte, half a header, three messages and a
    /// half -- all are valid. The core reassembles.
    ///
    /// The transport **must** preserve byte order across calls; GATT and UART
    /// both do. Order is the one property reassembly cannot recover.
    ///
    /// \param bytes Borrowed for the duration of this call only.
    virtual void on_bytes(ConstBytes bytes) = 0;

    /// A recoverable failure that did not take the link down.
    ///
    /// The link is still usable; a subsequent `send()` may succeed. Use this
    /// for a failed write or a transient stack error, and `on_disconnected()`
    /// when the link is actually gone.
    virtual void on_transport_error(Error error) = 0;

    /// The link is gone. Terminal for this transport instance.
    ///
    /// After this returns, the transport **must not** invoke any listener
    /// method again. The core fails every pending request on this signal and
    /// stops accepting new ones, so a late callback would be delivered to a
    /// client that has already torn the exchange down.
    virtual void on_disconnected(Error error) = 0;
};

/// A medium that can carry SMP messages. Implemented by adapters.
///
/// **Threading.** Every method is called on the client context, so an
/// implementation needs no internal locking for its own sake.
class Transport
{
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    /// Sends exactly one complete SMP message: an 8-byte header followed by
    /// `header.length` payload bytes.
    ///
    /// Fragmentation is the implementation's business. A BLE transport splits
    /// this across `ATT_MTU - 3` byte writes; a UART transport wraps it in
    /// base64 frames. Neither concerns the core.
    ///
    /// Does not block. It returns once the message has been accepted for
    /// transmission, which is not the same as delivered.
    ///
    /// \param message Borrowed for the duration of this call only. An
    ///                implementation that defers transmission **must** copy it.
    /// \return Success once accepted; `ErrorCode::TransportBusy` when the
    ///         caller should retry after the medium drains;
    ///         `ErrorCode::Disconnected` when the link is gone; any other error
    ///         for a failure specific to this message.
    ///
    /// Returning `TransportBusy` is a request to retry, not a failure of the
    /// link. The core does not queue on the transport's behalf: with one
    /// request in flight (ADR-0010) there is at most one message outstanding,
    /// so the decision of what to do about a busy medium belongs to the layer
    /// that knows what the message was for.
    [[nodiscard]] virtual Result<void> send(ConstBytes message) = 0;

    /// The largest whole SMP message this transport can carry, or 0 if unknown.
    ///
    /// This is **not** the MTU. A message may span many transport fragments;
    /// this is the point beyond which the transport cannot carry one at all.
    /// It feeds upload chunk sizing together with the device's own `buf_size`
    /// (docs/protocol-notes.md section 8) -- three separate limits that must not
    /// be conflated.
    ///
    /// 0 means "no opinion": the core then uses its configured default.
    [[nodiscard]] virtual std::size_t max_message_size() const noexcept = 0;

    /// Sets the listener, or clears it with nullptr.
    ///
    /// At most one listener at a time. The listener must outlive the transport,
    /// or be cleared before it is destroyed.
    virtual void set_listener(TransportListener* listener) noexcept = 0;

    /// Stops the transport. Synchronous and idempotent.
    ///
    /// After it returns, no listener method can fire -- including from another
    /// thread that was mid-delivery when it was called. That guarantee is what
    /// makes an ordered shutdown possible at all, and it is the hardest part of
    /// writing a transport: see the WinRT adapter's revoke-then-drain sequence
    /// in docs/design.md section 10.
    ///
    /// Does not deliver `on_disconnected()`: the caller asked for this, so
    /// there is nothing to report back to it.
    virtual void close() noexcept = 0;
};

} // namespace smply

#endif // SMPLY_TRANSPORT_HPP
