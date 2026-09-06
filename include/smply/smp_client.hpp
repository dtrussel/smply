// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SMP_CLIENT_HPP
#define SMPLY_SMP_CLIENT_HPP

/// \file
/// The request lifecycle: correlation, timeouts, cancellation, disconnection.
///
/// `SmpClient` is the layer that turns "send bytes and hope" into "issue a
/// request and get exactly one answer". It owns sequence allocation, the
/// pending-request table, deadlines, and the rule that decides whether an
/// arriving message is the answer to a request or something to be thrown away.
///
/// It knows nothing about management groups. Everything above it -- image,
/// OS, DFU -- gets correlation and timing for free and must not reimplement
/// either.
///
/// **Threading and callbacks.** All calls happen on the client context, and so
/// do all callbacks (ADR-0004). A callback is never invoked from inside the
/// call that started the operation: results arrive from `poll()` or from
/// `on_bytes()`. That means `handle = client.request(...)` always assigns a
/// handle for a request that is still live, which the DFU state machine relies
/// on. The single exception is the destructor, which has no later `poll()` to
/// defer to and so completes outstanding requests inline.
///
/// **Re-entrancy.** A callback may issue new requests, cancel others, or
/// destroy nothing it does not own; the client is written to survive all of
/// that. It rests on one obligation from the transport: `Transport::send()`
/// **must not** deliver inbound bytes before it returns. A callback dispatched
/// from `on_bytes()` that issues a request would otherwise re-enter
/// reassembly, which the assembler refuses as an error rather than corrupting
/// its buffer (docs/design.md section 2). Real media cannot do this; a fake or a
/// loopback transport must queue instead.

#include "smply/bytes.hpp"
#include "smply/clock.hpp"
#include "smply/error.hpp"
#include "smply/group.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"
#include "smply/smp/header.hpp"
#include "smply/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace smply {

/// Tuning and defensive bounds for one client (docs/architecture.md section 9).
struct SmpClientConfig
{
    /// Version sent in outgoing requests. V1 by default: there is no
    /// negotiation mechanism, V1 is universally supported, and a V2 request to
    /// an older server fails outright (ADR-0010). Responses are decoded for
    /// both error shapes regardless of this setting.
    Version smp_version = Version::V1;

    Duration default_timeout = limits::kDefaultTimeout;
    std::uint16_t max_smp_payload = limits::kMaxSmpPayload;
    std::size_t max_assembly_bytes = limits::kMaxAssemblyBuffer;

    /// Requests outstanding at once. One by default: Zephyr's server processes
    /// packets sequentially, and a single in-flight request is what makes
    /// upload retransmission unambiguous.
    std::uint8_t max_in_flight = limits::kMaxInFlight;

    /// Completed sequence numbers remembered, so a late response cannot be
    /// mis-attributed after the 8-bit counter wraps (docs/security.md, T5).
    std::uint8_t max_retired_seqs = limits::kMaxRetiredSeqs;
};

/// Identifies an in-flight request.
///
/// Generation-tagged: once a request completes, its handle is inert forever,
/// even after the table slot and the sequence number are reused. Cancelling
/// through a stale handle is a no-op rather than an attack on whatever request
/// happens to occupy that slot now.
class RequestHandle
{
public:
    RequestHandle() = default;

    [[nodiscard]] bool valid() const noexcept
    {
        return generation_ != 0;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    [[nodiscard]] friend bool operator==(const RequestHandle&,
                                         const RequestHandle&) noexcept = default;

private:
    friend class SmpClient;

    RequestHandle(std::size_t slot, std::uint64_t generation) noexcept
        : slot_{slot}, generation_{generation}
    {}

    std::size_t slot_ = 0;
    /// Zero means "never referred to a request".
    std::uint64_t generation_ = 0;
};

/// A successful response, before any group-specific decoding.
struct RawResponse
{
    Header header;
    /// The CBOR payload, borrowed for the duration of the callback only. Copy
    /// anything that must outlive it.
    ConstBytes payload;
};

/// Invoked exactly once per request, with the response or the reason there
/// will not be one.
using ResponseCallback = Callback<RawResponse>;

/// What to send.
struct RequestSpec
{
    Operation op{};
    Group group{};
    std::uint8_t command{};
    /// Pre-encoded CBOR. Borrowed for the duration of `request()` only: it is
    /// copied into the client's send buffer before transmission.
    ConstBytes payload;
    /// Overrides `SmpClientConfig::default_timeout`. Image erase and the first
    /// upload chunk need far longer than the default
    /// (docs/protocol-notes.md section 9, A7 and A12).
    std::optional<Duration> timeout;
};

/// Counters, for diagnostics and for tests asserting that a message was
/// dropped rather than silently mishandled.
struct SmpClientStats
{
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    /// Responses whose sequence number matched nothing, live or recently
    /// retired. Unsolicited, or from a much older exchange.
    std::uint64_t unmatched = 0;
    /// Responses for a request that has already completed. Expected after a
    /// timeout or a cancellation; dropped silently.
    std::uint64_t late = 0;
    /// Responses whose sequence matched but whose group, command or operation
    /// did not. The request is left pending -- see the class documentation.
    std::uint64_t mismatched = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t cancelled = 0;
    /// Malformed framing. Terminal for the stream.
    std::uint64_t malformed = 0;
};

/// Issues SMP requests and matches responses to them.
///
/// Non-copyable and non-movable: it registers itself as the transport's
/// listener, so its address must not change.
///
/// **Lifetime.** Every transport the client has been bound to -- the
/// constructor's and any passed to `rebind_transport()` -- must outlive the
/// client. The destructor detaches from the transport it currently holds, and
/// `rebind_transport()` detaches from the one it is replacing; a transport
/// destroyed first leaves those calls dangling. Declaring the transport before
/// the client is enough.
///
/// The same applies to whatever a callback captures. Because the destructor
/// completes outstanding requests, a callback runs *during* destruction, and
/// anything it refers to must still be alive at that point. Declare it before
/// the client.
class SmpClient final : private TransportListener
{
public:
    SmpClient(Transport& transport, const Clock& clock = system_clock(),
              SmpClientConfig config = {});

    SmpClient(const SmpClient&) = delete;
    SmpClient(SmpClient&&) = delete;
    SmpClient& operator=(const SmpClient&) = delete;
    SmpClient& operator=(SmpClient&&) = delete;

    /// Completes every outstanding request with `Cancelled` before returning,
    /// so no callback can fire after destruction. This is the one place a
    /// callback runs outside `poll()` or `on_bytes()`.
    ///
    /// Those callbacks run here, which is why everything they capture must
    /// outlive the client -- see the class documentation.
    ~SmpClient() override;

    /// Issues a request.
    ///
    /// \return A handle for cancellation. When the request cannot even be
    ///         attempted -- the link is down, the table is full, the payload is
    ///         too large, the transport refused it -- the returned handle is
    ///         invalid and \p on_response is invoked with the reason on the
    ///         next `poll()`. It is never invoked from inside this call.
    RequestHandle request(const RequestSpec& spec, ResponseCallback on_response);

    /// Abandons a request. Its callback receives `Cancelled` on the next
    /// `poll()`.
    ///
    /// The request is removed from the table immediately, so a response that
    /// arrives in the meantime cannot complete it. No attempt is made to recall
    /// the bytes already sent: the device may well answer, and that answer is
    /// discarded by the retired-sequence set. An invalid or stale handle is a
    /// no-op.
    void cancel(RequestHandle handle);

    /// Queues \p work to run on the next `poll()`.
    ///
    /// This exists so a layer above can keep the same promise the client makes:
    /// a callback never runs inside the call that started the operation. A
    /// management group that rejects an argument, or fails to encode a request,
    /// has no request to attach the failure to -- without this it would have to
    /// invoke the callback inline, before its own `RequestHandle` had been
    /// assigned to anything.
    ///
    /// Queued work runs once, in order, and is dropped on nothing: the
    /// destructor drains the queue before returning. An empty `std::function`
    /// is ignored.
    void defer(std::function<void()> work);

    /// Drives deadlines and delivers deferred completions.
    ///
    /// Must be called regularly from the application's pump. Callbacks run
    /// inside it.
    void poll(TimePoint now);

    /// The earliest point at which `poll()` has something to do.
    ///
    /// `std::nullopt` means nothing is outstanding. A value in the past --
    /// specifically `TimePoint::min()` -- means there is work ready now, so an
    /// event loop should poll rather than wait.
    [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept;

    /// Attaches a new transport after the application has re-established the
    /// link.
    ///
    /// Any request still outstanding is failed with `Disconnected`, and the
    /// reassembler is reset so a message truncated by the drop cannot bleed
    /// into the new session.
    ///
    /// \p transport must outlive this client, and so must the one it replaces:
    /// the old one is detached here, the new one on destruction.
    void rebind_transport(Transport& transport);

    /// False once the link has dropped, until `rebind_transport()`.
    [[nodiscard]] bool connected() const noexcept;

    /// The current transport's `max_message_size()`, or 0 if it has no opinion.
    ///
    /// Exposed because upload chunk sizing needs it and only the client knows
    /// which transport is bound -- `rebind_transport()` can change the answer.
    /// It is one of three separate limits that must not be conflated
    /// (docs/protocol-notes.md section 8).
    [[nodiscard]] std::size_t transport_max_message_size() const noexcept;

    /// Requests currently outstanding.
    [[nodiscard]] std::size_t in_flight() const noexcept;

    [[nodiscard]] const SmpClientStats& stats() const noexcept;

    [[nodiscard]] const SmpClientConfig& config() const noexcept;

private:
    // TransportListener. Private: only a transport calls these.
    void on_bytes(ConstBytes bytes) override;
    void on_transport_error(Error error) override;
    void on_disconnected(Error error) override;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace smply

#endif // SMPLY_SMP_CLIENT_HPP
