# Public API proposal

Preliminary but concrete — enough to validate the architecture. Signatures may
change during implementation; **when they do, update this file in the same
change** ([ADR-0013](decisions/ADR-0013-living-documentation.md)).

Everything lives in namespace `smply`. Baseline **C++20**
([ADR-0001](decisions/ADR-0001-cpp-standard.md)).

---

## `smply/group.hpp`

`Group` is core vocabulary rather than an SMP-header detail: the error model
needs it (an SMP v2 code is only meaningful against its group), and so do the
group clients and the request router. `smply/smp/header.hpp` includes it.

```cpp
namespace smply {

// Open by design: any 16-bit value is legal on the wire and round-trips
// unchanged, so an unknown or vendor group is never rejected.
enum class Group : std::uint16_t {
    Os = 0, Image = 1, Stat = 2, Settings = 3, Log = 4, Crash = 5, Split = 6,
    Run = 7, Fs = 8, Shell = 9, Enumeration = 10, Transport = 11,
    ZephyrBasic = 63, PerUser = 64,
};

constexpr std::uint16_t to_underlying(Group) noexcept;
constexpr bool          is_user_defined(Group) noexcept;   // >= PerUser
const char*             group_name(Group) noexcept;        // "unknown" if unnamed

} // namespace smply
```

## `smply/result.hpp`, `smply/error.hpp`

```cpp
namespace smply {

enum class ErrorCode : std::uint16_t {
    Ok = 0,
    InvalidArgument,        // caller misuse: bad size, null source, bad state
    InvalidState,           // operation not legal in the current state
    MalformedMessage,       // SMP header/framing violated
    UnsupportedSmpVersion,  // version bits 0b10 / 0b11
    MessageTooLarge,        // exceeds a configured limit
    CborEncode,
    CborDecode,
    ProtocolError,          // MCUmgr rc / err — see Error::mgmt
    UnexpectedResponse,     // seq/group/command mismatch
    Timeout,
    Cancelled,
    TransportError,
    TransportBusy,          // retry when the transport drains
    Disconnected,
    ImageMismatch,          // device content != what we uploaded
    UpdateFailed,           // DFU state machine terminal failure
    Internal,
};

// Never allocates: the zero-cost path for logging.
std::string_view to_string(ErrorCode) noexcept;

// Device-reported error, both SMP v1 and v2 shapes (protocol-notes §3).
struct MgmtError {
    Group         group{Group::Os}; // only meaningful when group_scoped
    std::uint16_t rc{};             // group-scoped (v2) or mcumgr_err_t (v1)
    bool          group_scoped{};   // true => rc must be read against `group`

    // Named factories, so the flag cannot be set inconsistently with the shape.
    static constexpr MgmtError smp(std::uint16_t rc) noexcept;              // v1 flat
    static constexpr MgmtError scoped(Group, std::uint16_t rc) noexcept;    // v2
    friend constexpr bool operator==(const MgmtError&, const MgmtError&) noexcept = default;
};

class Error {
public:
    Error() noexcept = default;                            // ErrorCode::Ok
    explicit Error(ErrorCode, const char* where = nullptr) noexcept;
    Error(ErrorCode, MgmtError, const char* where = nullptr) noexcept;

    ErrorCode                       code()   const noexcept;
    const std::optional<MgmtError>& mgmt()   const noexcept;
    std::string_view                reason() const noexcept; // device "rsn", may be empty
    const char*                     where()  const noexcept; // static site tag, for logs
    bool                            failed() const noexcept; // code() != Ok

    Error&  with_reason(std::string) &;
    Error&& with_reason(std::string) &&;   // for `return Error{...}.with_reason(...)`

    // Compares code, mgmt and reason. `where` is excluded: it is a logging aid,
    // not part of the error's identity.
    friend bool operator==(const Error&, const Error&) noexcept;
};

std::string to_string(const Error&);   // diagnostics only, never for control flow

// std::expected where the standard library has it, otherwise an API-compatible
// subset (ADR-0002). Use only the common subset: has_value/operator bool,
// operator*, operator->, error(), value_or. There is deliberately no value().
template <class T, class E> using expected   = /* std:: or smply's own */;
template <class E>          using unexpected = /* ditto */;
template <class T>          using Result     = expected<T, Error>;

// Builds the failure state, so call sites never name the backing.
unexpected<Error> fail(Error) noexcept;
unexpected<Error> fail(ErrorCode, const char* where = nullptr) noexcept;

} // namespace smply
```

Under the C++20 baseline `std::expected` does not exist, so `Result` is always
smply's own there. The `linux-gcc-cxx23-std-expected` CI job builds the same
tests against the standard type; `SMPLY_USING_STD_EXPECTED` says which is in
use.

## `smply/clock.hpp`, `smply/bytes.hpp`

```cpp
namespace smply {

using Duration  = std::chrono::milliseconds;
using TimePoint = std::chrono::steady_clock::time_point;

class Clock {                       // injectable; tests use ManualClock
public:
    virtual ~Clock() = default;
    // Must be monotonic: successive calls never decrease.
    virtual TimePoint now() const noexcept = 0;
};
const Clock& system_clock() noexcept;   // static storage duration

using ConstBytes = std::span<const std::byte>;
using MutBytes   = std::span<std::byte>;

// NB: two distinct SHA-256 values appear in MCUmgr and must not be confused --
// the upload "sha" (whole file) and image-state "hash" (IMAGE_TLV_SHA256, over
// header and body only). See protocol-notes §7.
using Hash       = std::array<std::byte, 32>;

} // namespace smply
```

## `smply/limits.hpp`

The defensive bounds from [`architecture.md`](architecture.md) §9, as named
constants in `namespace smply::limits`: `kMaxSmpPayload`, `kMaxAssemblyBuffer`,
`kMaxCborNesting`, `kMaxInFlight`, `kMaxRetiredSeqs`, `kMaxImages`,
`kMaxSlotsPerImage`, `kMaxVersionStringLength`, `kMaxReasonLength`,
`kDefaultTimeout`, `kFirstChunkTimeout`, `kEraseTimeout`, `kUploadChunkMax`,
`kUploadChunkMin`, `kDefaultSmpMessageBudget`, `kMaxImageSize`. These are the
defaults; `SmpClientConfig` and `UploadOptions` override them per instance.

## `smply/smp/header.hpp`

```cpp
namespace smply {

enum class Operation : std::uint8_t {
    Read = 0, ReadResponse = 1, Write = 2, WriteResponse = 3,
};
enum class Version : std::uint8_t { V1 = 0, V2 = 1 };  // 0b10/0b11 are reserved

// Group comes from smply/group.hpp, which this header includes.

inline constexpr std::size_t kHeaderSize = 8;
inline constexpr std::uint32_t kMaxEncodableLength = 0xFFFF;

struct Header {
    Operation     op{};
    Version       version{Version::V1};
    std::uint8_t  flags{};      // undefined by the spec; unknown bits preserved
    std::uint16_t length{};     // payload size, EXCLUDING this header
    Group         group{};
    std::uint8_t  seq{};        // wraps at 8 bits; a response echoes it
    std::uint8_t  command{};

    // Bytes of the whole message: kHeaderSize + length. This relationship is
    // what lets a byte stream be split into messages without transport
    // framing (ADR-0006).
    constexpr std::size_t total_size() const noexcept;

    friend constexpr bool operator==(const Header&, const Header&) noexcept = default;
};

// Total function: every Header encodes. Reserved bits are always written zero.
std::array<std::byte, kHeaderSize> encode(const Header&) noexcept;

// Rejects set reserved bits and op > 3 (MalformedMessage) and the reserved
// versions 0b10/0b11 (UnsupportedSmpVersion). Deliberately does NOT validate
// `length`: bounds belong to the reassembler, the only component that knows
// the configured limit.
Result<Header> decode_header(std::span<const std::byte, kHeaderSize>) noexcept;
Result<Header> decode_header(ConstBytes) noexcept;   // short buffer => Malformed

constexpr bool      is_response(Operation) noexcept;
// The response operation a device must use when answering this request.
// Correlation compares against it, so a Read cannot be answered by a
// WriteResponse (ADR-0010). An operation that is already a response is
// returned unchanged.
constexpr Operation response_to(Operation request) noexcept;

const char* operation_name(Operation) noexcept;

} // namespace smply
```

## `smply/transport.hpp` — the transport contract

```cpp
namespace smply {

// Implemented by the CORE; called by the transport. All three methods must be
// invoked on the client context (architecture §5). No callback may be issued
// after on_disconnected().
class TransportListener {
public:
    virtual ~TransportListener() = default;

    // Arbitrary inbound chunk. `bytes` is borrowed for the duration of the call.
    virtual void on_bytes(ConstBytes bytes) = 0;

    // Recoverable, operation-scoped transport failure; the link is still up.
    virtual void on_transport_error(Error) = 0;

    // The link is gone. Terminal for this transport instance.
    virtual void on_disconnected(Error) = 0;
};

// Implemented by adapters (WinRT BLE, UART, FakeTransport).
class Transport {
public:
    virtual ~Transport() = default;

    // Exactly ONE complete SMP message. The transport fragments as needed.
    // `message` is borrowed for the duration of the call — copy if you defer.
    // Returns TransportBusy when the caller should retry later, Disconnected
    // when the link is gone.
    [[nodiscard]] virtual Result<void> send(ConstBytes message) = 0;

    // Largest whole SMP message this transport can carry. 0 = unknown.
    // NOT the MTU: a message may span many transport fragments.
    virtual std::size_t max_message_size() const noexcept = 0;

    // Exactly one listener; nullptr detaches. Called on the client context.
    virtual void set_listener(TransportListener*) noexcept = 0;

    // Synchronous, idempotent. After it returns no callback can fire, including
    // from a thread that was mid-delivery. Does NOT deliver on_disconnected():
    // the caller asked for this.
    virtual void close() noexcept = 0;
};

} // namespace smply
```

## `smply/smp_client.hpp`

```cpp
namespace smply {

struct SmpClientConfig {
    Version       smp_version        = Version::V1;              // protocol-notes §9 A1
    Duration      default_timeout    = limits::kDefaultTimeout;      // 5 s
    std::uint16_t max_smp_payload    = limits::kMaxSmpPayload;       // 8192
    std::size_t   max_assembly_bytes = limits::kMaxAssemblyBuffer;   // 16 KiB
    std::uint8_t  max_in_flight      = limits::kMaxInFlight;         // 1   (A10)
    std::uint8_t  max_retired_seqs   = limits::kMaxRetiredSeqs;      // 64
};

class RequestHandle {                 // generation-tagged; safe when stale
public:
    RequestHandle() = default;
    bool valid() const noexcept;
    explicit operator bool() const noexcept;
    friend bool operator==(const RequestHandle&, const RequestHandle&) noexcept = default;
};

// Raw response: the decoded header plus the CBOR payload, borrowed for the
// duration of the callback.
struct RawResponse { Header header; ConstBytes payload; };
using ResponseCallback = std::function<void(Result<RawResponse>)>;

struct RequestSpec {
    Operation    op{};
    Group        group{};
    std::uint8_t command{};
    ConstBytes   payload;                     // pre-encoded CBOR, borrowed for the call
    std::optional<Duration> timeout;          // else config.default_timeout
};

// Counters, so a test can assert a message was dropped for a named reason
// rather than silently mishandled.
struct SmpClientStats {
    std::uint64_t sent = 0, received = 0;
    std::uint64_t unmatched = 0;   // matched no request, live or retired
    std::uint64_t late = 0;        // answer to something already completed
    std::uint64_t mismatched = 0;  // seq matched, group/command/op did not
    std::uint64_t timeouts = 0, cancelled = 0;
    std::uint64_t malformed = 0;   // framing failure; terminal for the stream
};

class SmpClient final : private TransportListener {
public:
    // Every transport bound to a client -- this one and any later passed to
    // rebind_transport() -- must outlive it: the client detaches on destruction
    // and on rebind. Declare the transport first.
    SmpClient(Transport&, const Clock& = system_clock(), SmpClientConfig = {});

    SmpClient(const SmpClient&)            = delete;   // stable address:
    SmpClient(SmpClient&&)                 = delete;   // it is the transport's listener
    SmpClient& operator=(const SmpClient&) = delete;
    SmpClient& operator=(SmpClient&&)      = delete;

    // Completes every outstanding request with Cancelled before returning, so
    // no callback can fire after destruction. The one place a callback runs
    // outside poll() or on_bytes().
    ~SmpClient() override;

    // Returns an invalid handle when the request could not be attempted; the
    // callback then receives the reason on the next poll(). A callback is
    // never invoked from inside this call.
    RequestHandle request(const RequestSpec&, ResponseCallback);

    // Removes the request at once; its callback receives Cancelled on the next
    // poll(). A stale or already-completed handle is a no-op.
    void cancel(RequestHandle);

    // Drives deadlines and delivers deferred completions. Callbacks run inside
    // it. Call from the pump.
    void poll(TimePoint now);

    // nullopt: nothing outstanding. TimePoint::min(): work is ready now, so
    // poll rather than wait.
    std::optional<TimePoint> next_deadline() const noexcept;

    // After the application re-establishes the link. Fails anything still
    // outstanding with Disconnected and resets reassembly.
    void rebind_transport(Transport&);

    bool        connected() const noexcept;   // false once the link drops
    std::size_t in_flight() const noexcept;

    const SmpClientStats&  stats()  const noexcept;
    const SmpClientConfig& config() const noexcept;
};

} // namespace smply
```

## `smply/groups/os.hpp`

```cpp
namespace smply {

template <class T> using Callback = std::function<void(Result<T>)>;

struct McumgrParameters { std::uint32_t buf_size; std::uint32_t buf_count; };
struct ResetOptions {
    bool force = false;
    std::optional<std::uint32_t> boot_mode{};
    std::optional<Duration> timeout{};
};

class OsManagement {
public:
    explicit OsManagement(SmpClient&);

    RequestHandle reset(ResetOptions, Callback<void>);
    RequestHandle mcumgr_parameters(Callback<McumgrParameters>);
    RequestHandle echo(std::string_view, Callback<std::string>);
};

} // namespace smply
```

## `smply/groups/image.hpp`

```cpp
namespace smply {

struct ImageVersion {
    std::uint8_t major{}, minor{}; std::uint16_t revision{}; std::uint32_t build{};
    static Result<ImageVersion> parse(std::string_view);  // "1.2.3+4"
    std::string to_string() const;
};

struct ImageSlot {
    std::uint32_t image = 0;          // absent in the response => 0
    std::uint32_t slot  = 0;          // 0 = primary, 1 = secondary
    std::string   version;            // raw string as reported
    std::optional<Hash> hash;         // IMAGE_TLV_SHA256 — NOT the upload "sha"
    bool bootable = false, pending = false, confirmed = false;
    bool active   = false, permanent = false;   // absent => false
};

struct ImageState {
    std::vector<ImageSlot> slots;
    std::optional<std::int32_t> split_status;

    const ImageSlot* active_slot(std::uint32_t image = 0) const noexcept;
    const ImageSlot* find_by_hash(const Hash&) const noexcept;
    const ImageSlot* secondary(std::uint32_t image = 0) const noexcept;
};

struct SetStateRequest { std::optional<Hash> hash; bool confirm = false; };

struct SlotDescriptor { std::uint32_t slot, size; std::optional<std::uint32_t> upload_image_id; };
struct ImageSlotsInfo { std::uint32_t image; std::vector<SlotDescriptor> slots;
                        std::optional<std::uint32_t> max_image_size; };
struct SlotInfo { std::vector<ImageSlotsInfo> images; };

struct UploadOptions {
    std::uint32_t image = 0;
    bool          upgrade_only = false;      // protocol-notes §9 A11 — off by default
    std::optional<Hash> sha;                 // computed from the source when absent
    std::uint32_t chunk_size = 0;            // 0 => negotiate (design §6)
    std::uint32_t max_chunk_retries = 3;
    std::uint32_t max_restarts = 2;
    Duration first_chunk_timeout = std::chrono::seconds{30};   // A7
    Duration chunk_timeout       = std::chrono::seconds{5};
};

struct UploadProgress { std::uint64_t transferred, total; };
struct UploadResult   { std::uint64_t transferred; std::optional<bool> match; };

// Owns one upload. Move-only. Destroying it abandons the upload (no callback).
class UploadHandle {
public:
    void cancel() noexcept;
    std::uint64_t transferred() const noexcept;
    bool active() const noexcept;
};

class ImageManagement {
public:
    explicit ImageManagement(SmpClient&);

    RequestHandle get_state(Callback<ImageState>);
    RequestHandle set_state(const SetStateRequest&, Callback<ImageState>);
    RequestHandle erase(std::optional<std::uint32_t> slot, Callback<void>);
    RequestHandle get_slot_info(Callback<SlotInfo>);

    // `source` must outlive the upload. Progress fires on every CONFIRMED
    // advance; completion fires exactly once.
    UploadHandle upload(ImageSource& source, UploadOptions,
                        std::function<void(UploadProgress)> on_progress,
                        Callback<UploadResult> on_done);

    // Resume after a reconnect: re-sends the first packet with the same sha and
    // continues from the offset the device reports (protocol-notes §6 rule 6).
    void resume(UploadHandle&, SmpClient& rebound);
};

} // namespace smply
```

## `smply/image_source.hpp`

```cpp
namespace smply {

class ImageSource {
public:
    virtual ~ImageSource() = default;
    virtual std::uint64_t size() const noexcept = 0;
    // Reads into `out`; returns bytes read (short reads only at EOF).
    virtual Result<std::size_t> read(std::uint64_t offset, MutBytes out) = 0;
};

class MemoryImageSource final : public ImageSource {   // wraps a borrowed span
public:
    explicit MemoryImageSource(ConstBytes);
};

struct McubootImageInfo {
    std::uint32_t header_size, image_size, flags;
    ImageVersion  version;
    bool          encrypted;
};

Result<McubootImageInfo>    parse_mcuboot_header(ConstBytes first_32_bytes);
Result<Hash>                sha256(ImageSource&);              // upload "sha"
Result<std::optional<Hash>> find_tlv_sha256(ImageSource&, const McubootImageInfo&);

} // namespace smply
```

## `smply/dfu/firmware_updater.hpp`

```cpp
namespace smply {

enum class UpdateMode { TestThenConfirm, ConfirmImmediately, UploadOnly };

enum class UpdateState {
    Idle, QueryingParameters, InspectingImages, Planning, Uploading,
    VerifyingUpload, MarkingForTest, Resetting, AwaitingDisconnect,
    AwaitingReconnect, VerifyingBooted, Confirming, VerifyingConfirmed,
    Completed, Failed, Cancelled,
};
std::string_view to_string(UpdateState) noexcept;

struct UpdatePlan {
    UpdateMode    mode  = UpdateMode::TestThenConfirm;
    std::uint32_t image = 0;
    UploadOptions upload{};
    // Skip the upload when the device already holds this image (by TLV hash).
    bool          skip_if_already_present = true;
    // Grace period between a reset being accepted and giving up on the link drop.
    Duration      disconnect_grace = std::chrono::seconds{10};
    // Hint passed to the application in ReconnectRequired.
    Duration      reconnect_hint   = std::chrono::seconds{3};
};

struct UpdateReport {
    UpdateState  final_state{};
    std::uint64_t bytes_transferred{};
    std::uint32_t chunk_retries{}, upload_restarts{};
    std::optional<Hash> target_hash;
    std::optional<ImageState> final_device_state;
    std::optional<Error> cause;     // set iff the update failed
    bool rolled_back = false;       // MCUboot reverted (protocol-notes §7)
};

struct UpdateEvent {
    enum class Kind { StateChanged, Progress, DisconnectExpected,
                      ReconnectRequired, Finished };
    Kind kind{};
    UpdateState from{}, to{};
    UploadProgress progress{};
    Duration reconnect_hint{};
    const Result<UpdateReport>* result = nullptr;   // valid iff kind == Finished
};

class FirmwareUpdater {
public:
    FirmwareUpdater(SmpClient&, ImageManagement&, OsManagement&);

    // `source` and the target hash must outlive the update.
    Result<void> start(ImageSource&, UpdatePlan, std::function<void(const UpdateEvent&)>);
    void         cancel() noexcept;
    void         poll(TimePoint now);          // drives DFU-level timers

    // Called by the application after it has re-established the link and called
    // SmpClient::rebind_transport(). Legal only in AwaitingReconnect.
    Result<void> resume_after_reconnect();

    // Called by the application when a reconnect attempt has permanently failed.
    void reconnect_failed(Error);

    UpdateState  state() const noexcept;
    const UpdateReport& report() const noexcept;
};

} // namespace smply
```

## `smply/util/dispatcher.hpp` — adapter helper (not used by the core)

```cpp
namespace smply {

// Thread-safe MPSC queue of closures, drained on the client context. Transport
// adapters use it to marshal driver-thread callbacks (architecture §5).
class Dispatcher {
public:
    explicit Dispatcher(std::function<void()> on_wake = {});
    void post(std::function<void()>);   // any thread
    std::size_t drain();                // client context only; returns count run
    void        clear() noexcept;
};

} // namespace smply
```

---

## Representative usage

### Portable: one update, application-driven pump

```cpp
MyTransport      transport{/* ... */};
smply::SmpClient client{transport};
smply::ImageManagement img{client};
smply::OsManagement    os{client};
smply::FirmwareUpdater updater{client, img, os};

smply::MemoryImageSource source{firmware_bytes};
smply::UpdatePlan plan;                       // TestThenConfirm by default

bool done = false;
updater.start(source, plan, [&](const smply::UpdateEvent& ev) {
    using K = smply::UpdateEvent::Kind;
    switch (ev.kind) {
    case K::Progress:
        ui.set_progress(ev.progress.transferred, ev.progress.total);
        break;
    case K::StateChanged:
        ui.set_status(smply::to_string(ev.to));
        break;
    case K::DisconnectExpected:
        ui.set_status("device rebooting");
        break;
    case K::ReconnectRequired:
        app.reconnect_async(ev.reconnect_hint, [&](auto& new_transport) {
            client.rebind_transport(new_transport);
            updater.resume_after_reconnect();
        });
        break;
    case K::Finished:
        done = true;
        if (*ev.result) ui.done();
        else            ui.error(smply::to_string(ev.result->error()));
        break;
    }
});

while (!done) {                                // the pump: one thread, no magic
    dispatcher.drain();                        // inbound bytes -> client
    auto now = std::chrono::steady_clock::now();
    client.poll(now);
    updater.poll(now);
    app.wait_until(client.next_deadline());
}
```

### Low-level: a single request

```cpp
img.get_state([](smply::Result<smply::ImageState> r) {
    if (!r) { log(smply::to_string(r.error())); return; }
    for (const auto& s : r->slots)
        log("image {} slot {} v{} active={} confirmed={}",
            s.image, s.slot, s.version, s.active, s.confirmed);
});
```

### Cancellation

```cpp
auto handle = img.upload(source, {}, on_progress, on_done);
// later, from the same thread:
handle.cancel();          // on_done fires once with ErrorCode::Cancelled
```
