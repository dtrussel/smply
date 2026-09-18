// SPDX-License-Identifier: Apache-2.0

#include "winrt_ble/winrt_ble_transport.hpp"

#include "winrt_ble/detail/winrt_prelude.hpp"

#include "common/ble_framing.hpp"
#include "common/link_state.hpp"
#include "common/send_queue.hpp"
#include "common/smp_ble_uuid.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"
#include "smply/util/dispatcher.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace smply::transport {

// At namespace scope, not inside the anonymous namespace below: `State` is
// declared out here and names these, and relying on an anonymous namespace's
// implicit using-directive to carry an alias outward is the sort of thing that
// is legal, works, and wastes an afternoon the day someone reorders the file.
namespace bluetooth = winrt::Windows::Devices::Bluetooth;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace streams = winrt::Windows::Storage::Streams;

namespace {

/// The most an inbound notification may carry before it is refused.
///
/// A notification cannot exceed the ATT MTU, so this is never reached by a
/// conforming stack -- which is exactly why it is here. Everything arriving
/// from the device is untrusted, and a length is bounded before it is used
/// (CLAUDE.md rule 6); the cost of being wrong about "cannot" is an allocation
/// sized by something we do not control.
constexpr std::uint32_t kMaxInboundNotification = 4096;

/// How long `close()` waits for a write already on the air.
///
/// A bound rather than an unconditional wait: this adapter cannot be tested
/// against a real stack here, and an application that can never be shut down is
/// a worse failure than one that gives up on a stuck write. Proceeding early is
/// safe -- the coroutine holds its own reference to the state, and every
/// callback path it can still take is already refused by then.
///
/// The send queue did not change this number. A message merely *waiting* is
/// discarded rather than flushed (`SendQueue::discard_queued()`), so the worst
/// case `close()` waits for is still one message's remaining fragments plus a
/// continuation -- exactly what five seconds was chosen for. Flushing the queue
/// instead would have doubled the bound for no benefit, since a queued
/// message's request is failed by `close()` anyway.
constexpr std::chrono::seconds kCloseWriteGrace{5};

/// How many times `connect()` re-runs discovery, and how long it waits between.
///
/// Windows answers a *rapid* reconnect with a service whose characteristic
/// collection is **empty** -- its own service cache, stale for a second or two,
/// even though every query here asks for `Uncached` (protocol-notes section 9,
/// A22, seen on a NUCLEO-WB55RG in P17b). Concluding "no SMP characteristic"
/// from that fails a reconnect which would have succeeded a moment later, and on
/// this platform that is a reconnect a real DFU tool depends on.
///
/// Six attempts 400 ms apart bounds the extra wait at 2 s, against a window
/// measured in "a second or two", with headroom. `connect()` is documented as
/// blocking and the HIL per-case deadline is 600 s, so the cost is affordable.
/// These are file-local rather than `WinRtBleConfig` fields deliberately: how
/// long Windows' GATT cache stays stale is not a policy an application can
/// choose informedly, and a config field would be public API P18 must keep.
constexpr int kDiscoveryAttempts = 6;
constexpr std::chrono::milliseconds kDiscoveryRetryDelay{400};

/// Converts a canonical UUID to the projection's GUID structure.
///
/// The byte-order asymmetry lives in `uuid_fields()`, which is tested on Linux
/// (`tests/unit/test_ble_framing.cpp`) precisely because nothing in this file
/// can be.
[[nodiscard]] winrt::guid to_guid(const Uuid128& uuid) noexcept
{
    const Uuid128Fields fields = uuid_fields(uuid);
    return winrt::guid{fields.data1, fields.data2, fields.data3, fields.data4};
}

/// Copies an inbound GATT buffer into owned bytes.
///
/// Two conversions rather than a cast: `reinterpret_cast` over device data is
/// banned (CLAUDE.md, design.md section 11), and at notification sizes the copy
/// is irrelevant. Returns empty when the buffer is implausibly large, which the
/// caller reports rather than allocating for.
[[nodiscard]] std::vector<std::byte> to_bytes(const streams::IBuffer& buffer)
{
    const streams::DataReader reader = streams::DataReader::FromBuffer(buffer);
    const std::uint32_t size = reader.UnconsumedBufferLength();
    if (size == 0 || size > kMaxInboundNotification) {
        return {};
    }

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(size));
    reader.ReadBytes(raw);

    std::vector<std::byte> out(raw.size());
    std::transform(raw.begin(), raw.end(), out.begin(),
                   [](std::uint8_t value) { return static_cast<std::byte>(value); });
    return out;
}

/// Wraps outgoing bytes in the buffer type `WriteValueWithResultAsync` wants.
[[nodiscard]] streams::IBuffer to_buffer(ConstBytes fragment)
{
    std::vector<std::uint8_t> staging(fragment.size());
    std::transform(fragment.begin(), fragment.end(), staging.begin(),
                   [](std::byte value) { return static_cast<std::uint8_t>(value); });

    streams::DataWriter writer;
    writer.WriteBytes(staging);
    return writer.DetachBuffer();
}

} // namespace

/// Everything the adapter owns.
///
/// Split by which thread may touch what, because that division is the whole
/// difficulty of writing a transport (ADR-0004) and a comment is cheaper than
/// rediscovering it.
struct WinRtBleTransport::State
{
    // --- client context only, no synchronisation needed ---------------------

    TransportListener* listener = nullptr;
    LinkState link;
    std::size_t max_message = 0;

    // --- shared with WinRT's thread pool ------------------------------------

    /// Owned by the application; carries closures to the client context.
    Dispatcher* inbound = nullptr;

    /// A cheap early-out for a handler running on a pool thread, which must not
    /// read `link` -- that belongs to the client context. It is cleared
    /// **before** the event handlers are revoked, so a handler already running
    /// either sees it false and does nothing, or posts a closure that finds the
    /// link closed and does nothing. Correctness therefore does not depend on
    /// whether `revoke()` waits for an in-flight handler, which is exactly the
    /// sort of assumption this adapter cannot check.
    std::atomic<bool> accepting{true};

    /// Asks the write coroutine to stop between fragments.
    std::atomic<bool> cancel{false};

    std::mutex send_mutex;
    std::condition_variable send_done;
    /// Who may write, and what waits. Guarded by `send_mutex` -- the queue has
    /// no lock of its own, deliberately (common/send_queue.hpp), so every call
    /// below is made holding this one and never across a `co_await`.
    SendQueue outbound;

    // --- the projection -----------------------------------------------------

    bluetooth::BluetoothLEDevice device{nullptr};
    gatt::GattDeviceService service{nullptr};
    gatt::GattCharacteristic characteristic{nullptr};
    gatt::GattSession session{nullptr};

    gatt::GattCharacteristic::ValueChanged_revoker value_changed;
    bluetooth::BluetoothLEDevice::ConnectionStatusChanged_revoker connection_changed;
};

namespace {

using State = WinRtBleTransport::State;

/// Revokes, quiesces and closes. Runs on the client context; idempotent.
///
/// The order is the contract (design.md section 10): nothing may be delivered
/// once this begins, so `begin_close()` comes first and every queued closure is
/// disarmed by it. `LinkState::begin_close()` returning false is what makes a
/// second call -- from the destructor, or from an application that closes after
/// a disconnect -- a no-op rather than a second teardown.
void shutdown(const std::shared_ptr<State>& state) noexcept
{
    if (!state->link.begin_close()) {
        return;
    }

    state->accepting.store(false, std::memory_order_release);
    state->cancel.store(true, std::memory_order_release);

    // A revoke can throw if the underlying object is already gone, which is
    // precisely the case where there is nothing left to revoke.
    try {
        state->value_changed.revoke();
        state->connection_changed.revoke();
    } catch (...) { // NOLINT(bugprone-empty-catch) -- see above
    }

    {
        std::unique_lock<std::mutex> lock{state->send_mutex};
        // Discard here as well as in the writer: the writer may have read
        // `cancel` before the store above, and this is the same mutex it takes.
        // A message accepted but never started must not reach the medium.
        state->outbound.discard_queued();
        state->send_done.wait_for(lock, kCloseWriteGrace,
                                  [&state] { return !state->outbound.writing(); });
    }

    try {
        if (state->session) {
            state->session.Close();
        }
        if (state->service) {
            state->service.Close();
        }
        if (state->device) {
            state->device.Close();
        }
    } catch (...) { // NOLINT(bugprone-empty-catch) -- closing what is already closed
    }

    state->listener = nullptr;
    state->link.finish_close();
}

/// Queues a failure for delivery on the client context.
void post_failure(const std::shared_ptr<State>& state, Error error, bool fatal)
{
    state->inbound->post([state, error = std::move(error), fatal]() mutable {
        if (!state->link.may_deliver()) {
            return;
        }
        TransportListener* listener = state->listener;
        if (listener == nullptr) {
            return;
        }
        if (!fatal) {
            listener->on_transport_error(std::move(error));
            return;
        }
        // Stop everything *before* reporting, so that "no callback after
        // on_disconnected" (smply/transport.hpp) is true by construction rather
        // than by everyone remembering it.
        shutdown(state);
        listener->on_disconnected(std::move(error));
    });
}

/// Writes whole SMP messages, one GATT packet at a time, until none are left.
///
/// Detached on purpose: `send()` must not block (smply/transport.hpp), so the
/// writes outlive the call that started them and anything they discover is
/// reported later, through the dispatcher, never inline.
///
/// **One writer, and it drains.** `send()` starts this only on
/// `Admission::StartWriter`, and it keeps taking messages until the queue hands
/// back nothing -- which is the same call that releases the claim. That is what
/// lets a message offered while this one is mid-flight be *deferred* rather than
/// refused, and it is why the loop must be left through `next_for_writer()` and
/// no other way: exiting with the claim still set would answer every later
/// `send()` with `TransportBusy` on a healthy link, and would hang a `close()`
/// waiting for a writer that has gone.
winrt::fire_and_forget write_message(std::shared_ptr<State> state)
{
    co_await winrt::resume_background();

    Error failure;
    bool fatal = false;

    for (;;) {
        std::optional<std::vector<std::byte>> message;
        {
            const std::lock_guard<std::mutex> lock{state->send_mutex};
            // A failed or cancelled link puts nothing further on the air, and a
            // failure stops this writer rather than moving to the next message:
            // one abandoned part-way leaves the device's reassembler holding a
            // fragment, and the next message's bytes would be consumed as that
            // one's tail -- a mis-framing nothing reports. The abandoned
            // message's request then times out, which the upload session
            // already covers. Discarding here, at the top, rather than at each
            // failure site is what keeps every exit from this loop the same one.
            if (failure.failed() || state->cancel.load(std::memory_order_acquire)) {
                state->outbound.discard_queued();
            }
            message = state->outbound.next_for_writer();
        }
        if (!message.has_value()) {
            break;
        }

        try {
            // Read the PDU size once per message rather than caching it. A
            // cached value can go stale, and subscribing to MaxPduSizeChanged to
            // keep it fresh would add an event handler -- and a revoker -- to
            // the shutdown sequence, for one property read per message.
            const std::uint16_t pdu =
                state->session ? state->session.MaxPduSize() : std::uint16_t{0};

            Fragmenter out{ConstBytes{*message}, fragment_size(pdu)};
            while (!out.done()) {
                if (state->cancel.load(std::memory_order_acquire)) {
                    break;
                }
                const gatt::GattWriteResult result =
                    co_await state->characteristic.WriteValueWithResultAsync(
                        to_buffer(out.next()), gatt::GattWriteOption::WriteWithoutResponse);

                if (result.Status() != gatt::GattCommunicationStatus::Success) {
                    fatal = result.Status() == gatt::GattCommunicationStatus::Unreachable;
                    failure =
                        fatal ? Error{ErrorCode::Disconnected, "winrt_ble: the device is gone"}
                              : Error{ErrorCode::TransportError, "winrt_ble: a GATT write failed"};
                    break;
                }
            }
        } catch (const winrt::hresult_error&) {
            failure =
                Error{ErrorCode::Disconnected, "winrt_ble: the Bluetooth stack failed a write"};
            fatal = true;
        } catch (...) {
            // Nothing may escape a detached coroutine: fire_and_forget's
            // unhandled_exception() terminates the process, and an exception
            // thrown past the loop would also leave the writer's claim set
            // forever, so every later send() would answer TransportBusy on a
            // healthy link.
            failure = Error{ErrorCode::TransportError, "winrt_ble: a write failed unexpectedly"};
            fatal = false;
        }
    }

    // The claim is already released by the loop's exit, so a waiting close()
    // is free before anything is reported -- it must not be held up by work
    // that is only going to be discarded anyway.
    state->send_done.notify_all();

    if (failure.failed() && state->accepting.load(std::memory_order_acquire)) {
        post_failure(state, std::move(failure), fatal);
    }
}

/// Subscribes to notifications and to loss of the link.
///
/// Both handlers hold a **weak** reference. A strong one would be a cycle --
/// the state owns the revoker, which owns the handler -- and the state would
/// never be destroyed.
void subscribe(const std::shared_ptr<State>& state)
{
    const std::weak_ptr<State> weak = state;

    state->value_changed = state->characteristic.ValueChanged(
        winrt::auto_revoke,
        [weak](const gatt::GattCharacteristic&, const gatt::GattValueChangedEventArgs& args) {
            const std::shared_ptr<State> live = weak.lock();
            if (!live || !live->accepting.load(std::memory_order_acquire)) {
                return;
            }

            std::vector<std::byte> payload = to_bytes(args.CharacteristicValue());
            if (payload.empty()) {
                post_failure(
                    live, Error{ErrorCode::TransportError, "winrt_ble: an unusable notification"},
                    false);
                return;
            }

            // The bytes are copied because an inbound buffer is borrowed for the
            // duration of the callback (design.md section 9), and this one is
            // about to cross a thread boundary.
            live->inbound->post([live, bytes = std::move(payload)] {
                if (!live->link.may_deliver() || live->listener == nullptr) {
                    return;
                }
                live->listener->on_bytes(ConstBytes{bytes});
            });
        });

    state->connection_changed = state->device.ConnectionStatusChanged(
        winrt::auto_revoke, [weak](const bluetooth::BluetoothLEDevice& sender,
                                   const winrt::Windows::Foundation::IInspectable&) {
            const std::shared_ptr<State> live = weak.lock();
            if (!live || !live->accepting.load(std::memory_order_acquire)) {
                return;
            }
            if (sender.ConnectionStatus() != bluetooth::BluetoothConnectionStatus::Disconnected) {
                return;
            }
            post_failure(live, Error{ErrorCode::Disconnected, "winrt_ble: the link dropped"}, true);
        });
}

/// Finds the SMP service and characteristic, riding out Windows' service cache.
///
/// An **empty** collection is not an answer here, it is the cache (A22): the
/// platform hands back a service with no characteristics for a second or two
/// after a rapid reconnect. A bad *status*, by contrast, means unreachable or
/// refused, and retrying that six times only makes a doomed reconnect slower --
/// so the two outcomes are now distinguished, where before they shared one
/// message and one verdict.
///
/// The service object is released between attempts because the stale
/// enumeration is reachable only through the object that produced it, which
/// makes dropping it the one lever available on the cache.
[[nodiscard]] Result<void> discover_smp(const std::shared_ptr<State>& state)
{
    bool saw_service = false;

    for (int attempt = 1; attempt <= kDiscoveryAttempts; ++attempt) {
        if (attempt > 1) {
            try {
                if (state->service) {
                    state->service.Close();
                }
            } catch (...) { // NOLINT(bugprone-empty-catch) -- already gone is the good case
            }
            state->service = nullptr;
            std::this_thread::sleep_for(kDiscoveryRetryDelay);
        }

        const gatt::GattDeviceServicesResult services =
            state->device
                .GetGattServicesForUuidAsync(to_guid(kSmpServiceUuid),
                                             bluetooth::BluetoothCacheMode::Uncached)
                .get();
        if (services.Status() != gatt::GattCommunicationStatus::Success) {
            return fail(ErrorCode::Disconnected, "winrt_ble: service discovery failed");
        }
        if (services.Services().Size() == 0) {
            continue;
        }
        saw_service = true;
        state->service = services.Services().GetAt(0);

        const gatt::GattCharacteristicsResult characteristics =
            state->service
                .GetCharacteristicsForUuidAsync(to_guid(kSmpCharacteristicUuid),
                                                bluetooth::BluetoothCacheMode::Uncached)
                .get();
        if (characteristics.Status() != gatt::GattCommunicationStatus::Success) {
            return fail(ErrorCode::Disconnected, "winrt_ble: characteristic discovery failed");
        }
        if (characteristics.Characteristics().Size() == 0) {
            continue;
        }
        state->characteristic = characteristics.Characteristics().GetAt(0);
        return {};
    }

    // Genuinely absent after every attempt, so the original two verdicts are
    // still told apart rather than collapsed into whichever came last.
    return saw_service
               ? fail(ErrorCode::TransportError,
                      "winrt_ble: the SMP service has no SMP characteristic")
               : fail(ErrorCode::TransportError, "winrt_ble: the device has no SMP service");
}

/// Resolves the device and brings one link up to "ready to carry a message".
///
/// Split out of `connect()` so that every failure has one place to be cleaned
/// up after; see the `shutdown()` call there.
[[nodiscard]] Result<void> open_link(const std::shared_ptr<State>& state,
                                     std::uint64_t bluetooth_address)
{
    try {
        state->device =
            bluetooth::BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address).get();
        if (!state->device) {
            return fail(ErrorCode::Disconnected, "winrt_ble: no device at that address");
        }

        if (const Result<void> found = discover_smp(state); !found.has_value()) {
            return fail(found.error());
        }

        // The session carries MaxPduSize, and asking it to maintain the
        // connection is what stops Windows dropping an idle link mid-update.
        state->session = state->service.Session();
        if (state->session) {
            state->session.MaintainConnection(true);
        }

        // Handlers first, CCCD second. The other order has a window between the
        // descriptor write and the subscription in which the device may already
        // be notifying and nothing is listening -- and the first thing it would
        // drop is the response to whatever the application sends next. This is
        // also why the discovery retry above must finish before this point: the
        // handler binds `state->characteristic`.
        subscribe(state);

        const gatt::GattCommunicationStatus subscribed =
            state->characteristic
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify)
                .get();
        if (subscribed != gatt::GattCommunicationStatus::Success) {
            return fail(ErrorCode::TransportError, "winrt_ble: the device refused notifications");
        }
    } catch (const winrt::hresult_error&) {
        return fail(ErrorCode::Disconnected, "winrt_ble: the Bluetooth stack raised an error");
    }

    return {};
}

} // namespace

// --- WinRtBleTransport ------------------------------------------------------

WinRtBleTransport::WinRtBleTransport(std::shared_ptr<State> state) noexcept
    : state_{std::move(state)}
{}

WinRtBleTransport::~WinRtBleTransport()
{
    close();
}

Result<std::unique_ptr<WinRtBleTransport>>
WinRtBleTransport::connect(std::uint64_t bluetooth_address, Dispatcher& inbound,
                           const WinRtBleConfig& config)
{
    if (config.max_message_size < kMinConfiguredMessageSize) {
        return fail(ErrorCode::InvalidArgument, "winrt_ble: max_message_size below the floor");
    }

    auto state = std::make_shared<State>();
    state->inbound = &inbound;
    state->max_message = config.max_message_size;

    if (const Result<void> opened = open_link(state, bluetooth_address); !opened.has_value()) {
        // Release what was acquired before handing the failure back. This used
        // to drop the `State` and leave the device, the service and a
        // MaintainConnection session to their destructors -- with the
        // notification handler still attached on the CCCD path -- which during a
        // reconnect storm is the kind of debris that makes the next attempt
        // worse. `shutdown()` is the sequence that already knows the order, and
        // with no writer started its wait returns at once.
        shutdown(state);
        return fail(opened.error());
    }

    return std::make_unique<WinRtBleTransport>(std::move(state));
}

Result<void> WinRtBleTransport::send(ConstBytes message)
{
    if (!state_->link.may_send()) {
        return fail(ErrorCode::Disconnected, "winrt_ble: the link is closed");
    }
    if (message.empty()) {
        return fail(ErrorCode::InvalidArgument, "winrt_ble: an empty SMP message");
    }
    if (message.size() > state_->max_message) {
        return fail(ErrorCode::MessageTooLarge, "winrt_ble: beyond the configured cap");
    }

    // Copied *before* the queue is offered anything. The message is borrowed
    // for the duration of this call only and the writes outlive it, so the copy
    // is the one smply/transport.hpp asks a deferring transport to make -- and
    // doing it first means an allocation failure here leaves nothing to unwind.
    std::vector<std::byte> owned{message.begin(), message.end()};

    Admission admission = Admission::Busy;
    {
        // The lock covers an optional move and two flags, and is never held
        // across a suspension point -- holding it over a `co_await` is the one
        // way to make send() block, which the contract forbids.
        const std::lock_guard<std::mutex> lock{state_->send_mutex};
        admission = state_->outbound.offer(std::move(owned));
    }

    if (admission == Admission::Busy) {
        // One message is being written and another already waits. With one
        // request in flight (ADR-0010) that needs a write that has stalled and
        // two deadlines expiring on top of it -- a medium that is not draining,
        // which is what TransportBusy is for, rather than the handover window
        // the queue exists to absorb (protocol-notes section 9, A22).
        return fail(ErrorCode::TransportBusy, "winrt_ble: two messages are already outbound");
    }
    if (admission == Admission::StartWriter) {
        // Detached deliberately; the coroutine owns everything it needs. The
        // cast says so, and keeps /W4 from reading a discarded return as an
        // oversight. Started only on StartWriter: a second writer on one medium
        // would interleave two messages' fragments (ADR-0006).
        static_cast<void>(write_message(state_));
    }
    return {};
}

std::size_t WinRtBleTransport::max_message_size() const noexcept
{
    return state_->max_message;
}

SendCounters WinRtBleTransport::send_counters() const
{
    const std::lock_guard<std::mutex> lock{state_->send_mutex};
    return state_->outbound.counters();
}

void WinRtBleTransport::set_listener(TransportListener* listener) noexcept
{
    state_->listener = listener;
}

void WinRtBleTransport::close() noexcept
{
    shutdown(state_);
}

} // namespace smply::transport
