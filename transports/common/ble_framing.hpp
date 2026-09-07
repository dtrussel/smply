// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_COMMON_BLE_FRAMING_HPP
#define SMPLY_TRANSPORTS_COMMON_BLE_FRAMING_HPP

/// \file
/// Splitting one SMP message into GATT-sized pieces.
///
/// **This is the transport's job, not the core's** (ADR-0005). `SmpClient`
/// hands a transport exactly one complete SMP message and knows nothing about
/// MTUs; getting it onto the air is the adapter's business. The arithmetic is
/// small, easy to get subtly wrong, and identical for every BLE adapter, so it
/// lives here rather than inside one.
///
/// It is deliberately **platform-independent and header-only**: this compiles
/// and is tested on Linux, which is the whole reason it is not inside
/// `transports/winrt_ble/`. A serial or TCP adapter would reuse the same shape.
///
/// From docs/protocol-notes.md section 8, quoting the MCUmgr transport
/// specification: *"If an SMP request or response is too large to fit in a
/// single GATT command, the sender fragments it across several packets. **No
/// additional framing is introduced** … Since GATT guarantees ordered delivery
/// of packets, the SMP header in the first fragment contains sufficient
/// information for reassembly."*
///
/// That last sentence is why this file has a sender and no receiver. Inbound
/// bytes go to `TransportListener::on_bytes()` exactly as they arrive, in
/// whatever chunks they arrive in, and smply's `MessageAssembler` reassembles
/// them (ADR-0006). An adapter that tried to reassemble would be duplicating
/// the most security-sensitive parser in the library.

#include "smply/bytes.hpp"

#include <cstddef>
#include <cstdint>

namespace smply::transport {

/// The three bytes of ATT opcode and handle that precede the payload of a
/// write-without-response or a notification.
inline constexpr std::uint16_t kAttHeaderSize = 3;

/// The smallest ATT MTU any BLE link may negotiate, from the Bluetooth core
/// specification. It yields a 20-byte fragment, which is the floor below.
inline constexpr std::uint16_t kMinAttMtu = 23;

/// Fragment payload bounds (docs/design.md section 10).
///
/// The floor is what `kMinAttMtu` leaves. The ceiling is a sanity bound: a
/// stack reporting a larger PDU is not trusted to accept one, and a fragment
/// this size already costs nothing in round trips.
/// @{
inline constexpr std::size_t kMinFragment = 20;
inline constexpr std::size_t kMaxFragment = 512;

/// @}

/// How many payload bytes fit in one GATT packet on a link whose maximum PDU is
/// \p max_pdu.
///
/// `max_pdu − 3`, clamped to `[kMinFragment, kMaxFragment]`. The clamp is not
/// decoration: a stack that reports a PDU below the specification minimum (or
/// zero, before the link is up) would otherwise produce a fragment size of zero
/// and an adapter that never makes progress.
[[nodiscard]] constexpr std::size_t fragment_size(std::uint16_t max_pdu) noexcept
{
    if (max_pdu <= kAttHeaderSize) {
        return kMinFragment;
    }
    const std::size_t usable = static_cast<std::size_t>(max_pdu) - kAttHeaderSize;
    if (usable < kMinFragment) {
        return kMinFragment;
    }
    if (usable > kMaxFragment) {
        return kMaxFragment;
    }
    return usable;
}

/// Walks one SMP message as a sequence of fragments.
///
/// Borrows the message and yields views into it: nothing is copied, because
/// "no additional framing" means each fragment is a plain slice of the original
/// bytes. The message must outlive the fragmenter, and — since the fragments
/// are views into it — must not move while one is in flight.
///
/// \code
/// smply::transport::Fragmenter out{message, fragment_size(session.MaxPduSize())};
/// while (!out.done()) {
///     co_await characteristic.WriteValueWithResultAsync(to_buffer(out.next()), …);
/// }
/// \endcode
class Fragmenter
{
public:
    /// \param message  The complete SMP message. Must outlive this object.
    /// \param fragment Bytes per fragment; normally `fragment_size(max_pdu)`.
    ///                 Clamped to at least 1, so a caller that passes zero gets
    ///                 slow progress rather than an infinite loop.
    constexpr Fragmenter(ConstBytes message, std::size_t fragment) noexcept
        : message_{message}, fragment_{fragment == 0 ? std::size_t{1} : fragment}
    {}

    /// True once every byte has been handed out.
    ///
    /// **An empty message is done immediately**, yielding no fragments. That is
    /// correct rather than a special case to fix: an SMP message is never
    /// empty — it is at least an 8-byte header — so the only way to get here is
    /// a caller bug, and sending a zero-length GATT write would put a packet on
    /// the air that means nothing.
    [[nodiscard]] constexpr bool done() const noexcept
    {
        return offset_ >= message_.size();
    }

    /// The next fragment, advancing past it. Empty once `done()`.
    [[nodiscard]] constexpr ConstBytes next() noexcept
    {
        if (done()) {
            return {};
        }
        const std::size_t take = remaining() < fragment_ ? remaining() : fragment_;
        const ConstBytes piece = message_.subspan(offset_, take);
        offset_ += take;
        return piece;
    }

    /// Bytes not yet handed out.
    [[nodiscard]] constexpr std::size_t remaining() const noexcept
    {
        return done() ? std::size_t{0} : message_.size() - offset_;
    }

    /// How many fragments the whole message takes. Constant for the life of the
    /// object; useful for logging and for pacing.
    [[nodiscard]] constexpr std::size_t count() const noexcept
    {
        return (message_.size() + fragment_ - 1) / fragment_;
    }

private:
    ConstBytes message_;
    std::size_t fragment_;
    std::size_t offset_ = 0;
};

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_COMMON_BLE_FRAMING_HPP
