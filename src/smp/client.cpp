// SPDX-License-Identifier: Apache-2.0

#include "smply/smp_client.hpp"

#include "cbor/mgmt_error.hpp"
#include "smp/assembler.hpp"
#include "smply/error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace smply {
namespace {

/// One outstanding request.
struct Pending
{
    bool active = false;
    /// Zero is never issued, so a default-constructed handle matches nothing.
    std::uint64_t generation = 0;
    std::uint8_t seq = 0;
    Group group{};
    std::uint8_t command = 0;
    /// The operation a response must carry to be this request's answer.
    Operation expected_op{};
    TimePoint deadline;
    ResponseCallback callback;
};

} // namespace

class SmpClient::Impl final : public MessageSink
{
public:
    Impl(Transport& transport, const Clock& clock, SmpClientConfig config)
        : transport_{&transport}, clock_{&clock}, config_{config},
          assembler_{AssemblerLimits{.max_payload = config.max_smp_payload,
                                     .max_buffer = config.max_assembly_bytes}}
    {
        pending_.resize(std::max<std::size_t>(1, config_.max_in_flight));
        send_buffer_.resize(kHeaderSize + static_cast<std::size_t>(config_.max_smp_payload));
        retired_ring_.reserve(std::max<std::size_t>(1, config_.max_retired_seqs));
    }

    Impl(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() override = default;

    // --- issuing -----------------------------------------------------------

    RequestHandle request(const RequestSpec& spec, ResponseCallback on_response)
    {
        if (!connected_) {
            return defer(std::move(on_response),
                         Error{ErrorCode::Disconnected, "smp client: link is down"});
        }
        // One check, not two: max_smp_payload is a uint16_t, so it can never
        // exceed what the 16-bit length field encodes. A second comparison
        // against kMaxEncodableLength would be dead code.
        static_assert(std::numeric_limits<decltype(SmpClientConfig::max_smp_payload)>::max() <=
                          kMaxEncodableLength,
                      "max_smp_payload must fit the SMP length field");
        if (spec.payload.size() > config_.max_smp_payload) {
            return defer(std::move(on_response),
                         Error{ErrorCode::MessageTooLarge, "smp client: payload exceeds limit"});
        }

        const std::optional<std::size_t> slot = free_slot();
        if (!slot.has_value()) {
            return defer(std::move(on_response),
                         Error{ErrorCode::InvalidState, "smp client: too many requests in flight"});
        }

        const std::optional<std::uint8_t> seq = allocate_seq();
        if (!seq.has_value()) {
            // Every sequence number is either pending or recently retired. Not
            // reachable with a sane configuration, but the allocator must not
            // pretend otherwise.
            return defer(std::move(on_response),
                         Error{ErrorCode::InvalidState, "smp client: no free sequence number"});
        }

        const Header header{.op = spec.op,
                            .version = config_.smp_version,
                            .flags = 0,
                            .length = static_cast<std::uint16_t>(spec.payload.size()),
                            .group = spec.group,
                            .seq = *seq,
                            .command = spec.command};

        const auto encoded = encode(header);
        std::copy(encoded.begin(), encoded.end(), send_buffer_.begin());
        std::copy(spec.payload.begin(), spec.payload.end(),
                  send_buffer_.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));

        const auto sent = transport_->send(ConstBytes{send_buffer_}.first(header.total_size()));
        if (!sent.has_value()) {
            // Retire the number even though the send failed: whether the
            // transport managed to put it on the wire before failing is not
            // knowable, and a late answer should be dropped quietly rather
            // than counted as unsolicited.
            retire(*seq);
            return defer(std::move(on_response), sent.error());
        }

        Pending& entry = pending_[*slot];
        entry.active = true;
        entry.generation = next_generation_++;
        entry.seq = *seq;
        entry.group = spec.group;
        entry.command = spec.command;
        entry.expected_op = response_to(spec.op);
        entry.deadline = clock_->now() + spec.timeout.value_or(config_.default_timeout);
        entry.callback = std::move(on_response);

        ++stats_.sent;
        return RequestHandle{*slot, entry.generation};
    }

    void cancel(RequestHandle handle)
    {
        Pending* entry = resolve(handle);
        if (entry == nullptr) {
            return; // stale or already completed: a no-op by design
        }
        ++stats_.cancelled;
        // Removed from the table at once, so a response arriving before the
        // next poll() cannot complete it. The callback is deferred because
        // cancel() is a call the application made.
        ResponseCallback callback = detach(*entry);
        static_cast<void>(
            defer(std::move(callback), Error{ErrorCode::Cancelled, "smp client: cancelled"}));
    }

    // --- driving -----------------------------------------------------------

    void defer_work(std::function<void()> work)
    {
        if (work) {
            deferred_.push_back(std::move(work));
        }
    }

    void poll(TimePoint now)
    {
        deliver_deferred();

        // Snapshot first: completing a request runs a callback that may issue
        // or cancel others, and mutating the table underneath an iteration is
        // how that becomes a crash.
        std::vector<RequestHandle> expired;
        for (std::size_t slot = 0; slot < pending_.size(); ++slot) {
            const Pending& entry = pending_[slot];
            if (entry.active && entry.deadline <= now) {
                expired.emplace_back(RequestHandle{slot, entry.generation});
            }
        }

        for (const RequestHandle& handle : expired) {
            Pending* entry = resolve(handle);
            if (entry == nullptr) {
                continue; // a callback already completed it
            }
            ++stats_.timeouts;
            complete(*entry, fail(Error{ErrorCode::Timeout, "smp client: no response"}));
        }

        // A callback may have queued more work.
        deliver_deferred();
    }

    [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept
    {
        if (!deferred_.empty()) {
            return TimePoint::min(); // work ready now
        }
        std::optional<TimePoint> earliest;
        for (const Pending& entry : pending_) {
            if (entry.active && (!earliest.has_value() || entry.deadline < *earliest)) {
                earliest = entry.deadline;
            }
        }
        return earliest;
    }

    void rebind_transport(Transport& transport)
    {
        fail_all(Error{ErrorCode::Disconnected, "smp client: transport rebound"});
        transport_->set_listener(nullptr);
        transport_ = &transport;
        // A message truncated by the drop must not be parsed against the new
        // session's bytes.
        assembler_.reset();
        connected_ = true;
    }

    void shutdown(TransportListener* self)
    {
        // The destructor has no later poll() to defer to, so callbacks run
        // inline here. Deferred work first, so nothing is silently dropped.
        deliver_deferred();
        fail_all(Error{ErrorCode::Cancelled, "smp client: destroyed"});
        deliver_deferred();
        if (transport_ != nullptr) {
            static_cast<void>(self);
            transport_->set_listener(nullptr);
        }
    }

    // --- transport callbacks -----------------------------------------------

    void on_bytes(ConstBytes bytes)
    {
        if (auto fed = assembler_.feed(bytes, *this); !fed.has_value()) {
            ++stats_.malformed;
            // Framing is broken and SMP has no sync word, so nothing further on
            // this stream can be correlated. Every outstanding request is
            // failed. The link itself is the application's to drop -- the
            // client does not presume to close a transport it does not own.
            fail_all(fed.error());
        }
    }

    void on_transport_error(Error error)
    {
        // Recoverable and not tied to any particular request, so nothing is
        // completed: an outstanding request either still gets its answer or
        // times out. Recorded so a diagnostic can show it happened.
        last_transport_error_ = std::move(error);
    }

    void on_disconnected(const Error& error)
    {
        connected_ = false;
        assembler_.reset();
        // Inline rather than deferred: this is a transport callback, not a call
        // the application made, and the application should learn immediately.
        fail_all(error);
    }

    // --- MessageSink -------------------------------------------------------

    void on_message(const Header& header, ConstBytes payload) override
    {
        ++stats_.received;

        Pending* entry = find_by_seq(header.seq);
        if (entry == nullptr) {
            if (is_retired(header.seq)) {
                // The answer to something already timed out or cancelled.
                // Expected, and precisely what the retired set exists to
                // absorb: without it this would match a later request that
                // reused the number.
                ++stats_.late;
            } else {
                ++stats_.unmatched;
            }
            return;
        }

        if (header.group != entry->group || header.command != entry->command ||
            header.op != entry->expected_op) {
            // The sequence matched but nothing else did. The message is
            // discarded and the request left pending, to time out normally.
            // Completing it would let a stale or hostile message answer a live
            // request with someone else's data (ADR-0010).
            ++stats_.mismatched;
            return;
        }

        complete(*entry, interpret(header, payload));
    }

    // --- accessors ---------------------------------------------------------

    [[nodiscard]] bool connected() const noexcept
    {
        return connected_;
    }

    [[nodiscard]] std::size_t in_flight() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            pending_.begin(), pending_.end(), [](const Pending& entry) { return entry.active; }));
    }

    [[nodiscard]] const SmpClientStats& stats() const noexcept
    {
        return stats_;
    }

    [[nodiscard]] const SmpClientConfig& config() const noexcept
    {
        return config_;
    }

    Transport& transport() noexcept
    {
        return *transport_;
    }

private:
    /// Turns a matched response into a result: a device-reported error becomes
    /// a failure, anything else is handed up as-is.
    [[nodiscard]] static Result<RawResponse> interpret(const Header& header, ConstBytes payload)
    {
        if (is_user_defined(header.group)) {
            // Groups at 64 and above may define their own payload encoding, so
            // there is no CBOR error map to look for.
            return RawResponse{header, payload};
        }

        auto decoded = cbor::extract_mgmt_error(payload);
        if (!decoded.has_value()) {
            return fail(decoded.error());
        }

        // Bound to one named object, and tested against the optional itself
        // rather than through MgmtOutcome::failed(): the engaged state is then
        // visible -- to a reader and to static analysis -- where the value is
        // read.
        cbor::MgmtOutcome& outcome = *decoded;
        if (!outcome.error.has_value()) {
            return RawResponse{header, payload};
        }

        Error error{ErrorCode::ProtocolError, *outcome.error, "smp client"};
        if (!outcome.reason.empty()) {
            error.with_reason(std::move(outcome.reason));
        }
        return fail(std::move(error));
    }

    /// Removes an entry from the table and hands back its callback. Done before
    /// the callback runs, so a callback that issues a new request may reuse
    /// this slot safely.
    ResponseCallback detach(Pending& entry)
    {
        ResponseCallback callback;
        // Swapped rather than moved: a moved-from std::function is valid but
        // unspecified, and the slot must be left definitively empty.
        callback.swap(entry.callback);
        entry.active = false;
        retire(entry.seq);
        return callback;
    }

    void complete(Pending& entry, Result<RawResponse> result)
    {
        const ResponseCallback callback = detach(entry);
        if (callback) {
            callback(std::move(result));
        }
    }

    void fail_all(const Error& error)
    {
        for (Pending& entry : pending_) {
            if (!entry.active) {
                continue;
            }
            const ResponseCallback callback = detach(entry);
            if (callback) {
                callback(fail(error));
            }
        }
    }

    /// Queues a completion for the next poll() and returns an invalid handle,
    /// so `request()` never runs a callback inside itself.
    RequestHandle defer(ResponseCallback callback, Error error)
    {
        if (callback) {
            defer_work([cb = std::move(callback), err = std::move(error)]() mutable {
                cb(fail(std::move(err)));
            });
        }
        return {};
    }

    void deliver_deferred()
    {
        // Callbacks may defer more work, so drain in batches rather than
        // iterating a container that is being appended to.
        while (!deferred_.empty()) {
            std::vector<std::function<void()>> batch;
            batch.swap(deferred_);
            for (const std::function<void()>& item : batch) {
                item();
            }
        }
    }

    [[nodiscard]] Pending* resolve(RequestHandle handle) noexcept
    {
        if (!handle.valid() || handle.slot_ >= pending_.size()) {
            return nullptr;
        }
        Pending& entry = pending_[handle.slot_];
        if (!entry.active || entry.generation != handle.generation_) {
            return nullptr;
        }
        return &entry;
    }

    [[nodiscard]] Pending* find_by_seq(std::uint8_t seq) noexcept
    {
        for (Pending& entry : pending_) {
            if (entry.active && entry.seq == seq) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<std::size_t> free_slot() const noexcept
    {
        for (std::size_t slot = 0; slot < pending_.size(); ++slot) {
            if (!pending_[slot].active) {
                return slot;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool is_retired(std::uint8_t seq) const noexcept
    {
        return retired_flags_[seq];
    }

    void retire(std::uint8_t seq)
    {
        if (config_.max_retired_seqs == 0) {
            return;
        }
        if (retired_flags_[seq]) {
            return; // already remembered
        }
        if (retired_ring_.size() == config_.max_retired_seqs) {
            // Evict the oldest, so the set stays bounded however many requests
            // pass through.
            retired_flags_[retired_ring_[retired_head_]] = false;
            retired_ring_[retired_head_] = seq;
            retired_head_ = (retired_head_ + 1) % retired_ring_.size();
        } else {
            retired_ring_.push_back(seq);
        }
        retired_flags_[seq] = true;
    }

    /// The next sequence number not currently pending and not recently retired.
    [[nodiscard]] std::optional<std::uint8_t> allocate_seq() noexcept
    {
        constexpr unsigned kSpace = 256;
        for (unsigned attempt = 0; attempt < kSpace; ++attempt) {
            const auto candidate = static_cast<std::uint8_t>((next_seq_ + attempt) % kSpace);
            if (find_by_seq(candidate) == nullptr && !is_retired(candidate)) {
                next_seq_ = static_cast<std::uint8_t>((candidate + 1) % kSpace);
                return candidate;
            }
        }
        return std::nullopt;
    }

    Transport* transport_;
    const Clock* clock_;
    SmpClientConfig config_;
    MessageAssembler assembler_;

    std::vector<Pending> pending_;
    std::vector<std::byte> send_buffer_;
    std::vector<std::function<void()>> deferred_;

    /// Membership test for the retired set, and the ring that bounds it.
    std::array<bool, 256> retired_flags_{};
    std::vector<std::uint8_t> retired_ring_;
    std::size_t retired_head_ = 0;

    std::uint8_t next_seq_ = 0;
    std::uint64_t next_generation_ = 1;
    bool connected_ = true;
    std::optional<Error> last_transport_error_;
    SmpClientStats stats_;
};

// ---------------------------------------------------------------------------

SmpClient::SmpClient(Transport& transport, const Clock& clock, SmpClientConfig config)
    : impl_{std::make_unique<Impl>(transport, clock, config)}
{
    transport.set_listener(this);
}

SmpClient::~SmpClient()
{
    impl_->shutdown(this);
}

RequestHandle SmpClient::request(const RequestSpec& spec, ResponseCallback on_response)
{
    return impl_->request(spec, std::move(on_response));
}

void SmpClient::cancel(RequestHandle handle)
{
    impl_->cancel(handle);
}

void SmpClient::defer(std::function<void()> work)
{
    impl_->defer_work(std::move(work));
}

void SmpClient::poll(TimePoint now)
{
    impl_->poll(now);
}

std::optional<TimePoint> SmpClient::next_deadline() const noexcept
{
    return impl_->next_deadline();
}

void SmpClient::rebind_transport(Transport& transport)
{
    impl_->rebind_transport(transport);
    transport.set_listener(this);
}

bool SmpClient::connected() const noexcept
{
    return impl_->connected();
}

std::size_t SmpClient::in_flight() const noexcept
{
    return impl_->in_flight();
}

const SmpClientStats& SmpClient::stats() const noexcept
{
    return impl_->stats();
}

const SmpClientConfig& SmpClient::config() const noexcept
{
    return impl_->config();
}

void SmpClient::on_bytes(ConstBytes bytes)
{
    impl_->on_bytes(bytes);
}

void SmpClient::on_transport_error(Error error)
{
    impl_->on_transport_error(std::move(error));
}

void SmpClient::on_disconnected(Error error)
{
    impl_->on_disconnected(error);
}

} // namespace smply
