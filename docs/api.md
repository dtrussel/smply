# Public API

Everything lives in namespace `smply`. Baseline **C++20**
([ADR-0001](decisions/ADR-0001-cpp-standard.md)).

**Half of this file is shipped API and half is still a proposal — check which
before you rely on a signature.** A *shipped* section must match the header
exactly; if you change the header, change it here in the same commit
([ADR-0013](decisions/ADR-0013-living-documentation.md)). A *proposed* section
is a sketch to be validated by the phase that implements it, and is expected to
change — record the deviations in the roadmap when it does.

| Header | Status |
| ------ | ------ |
| `group.hpp` · `result.hpp` · `error.hpp` · `clock.hpp` · `bytes.hpp` · `limits.hpp` | **Shipped** (P1, extended P7) |
| `smp/header.hpp` | **Shipped** (P2, extended P6) |
| `transport.hpp` | **Shipped** (P4) |
| `smp_client.hpp` | **Shipped** (P6, extended P7) |
| `groups/os.hpp` | **Shipped** (P7) |
| `groups/image.hpp` | **Shipped** (P8: state, erase, slot info; P10: upload) |
| `image_source.hpp` · `mcuboot_image.hpp` | **Shipped** (P9) |
| `dfu/firmware_updater.hpp` | Proposed — P12 |
| `util/dispatcher.hpp` | Proposed — P14 |

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

// The SMP-level codes, mcumgr_err_t (protocol-notes §3, S5). Carried by a
// FLAT rc only -- a group-scoped rc of the same number means something else.
enum class SmpError : std::uint16_t {
    Ok = 0, Unknown = 1, NoMemory = 2, InvalidArgument = 3, TimedOut = 4,
    NoEntry = 5, BadState = 6, ResponseTooLarge = 7,
    NotSupported = 8,        // the command is not built into this firmware
    Corrupt = 9,
    Busy = 10,               // a reset may be retried with force
    AccessDenied = 11, VersionTooOld = 12, VersionTooNew = 13,
    BridgeUnavailable = 14,
    PerUser = 256,
};

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

// nullopt unless the device reported a FLAT rc, so a group-scoped code can
// never be mistaken for an SMP-level one:
//     if (smp_error(e) == SmpError::NotSupported) { /* fall back */ }
std::optional<SmpError> smp_error(const Error&) noexcept;

std::string to_string(const Error&);   // diagnostics only, never for control flow

// std::expected where the standard library has it, otherwise an API-compatible
// subset (ADR-0002). Use only the common subset: has_value/operator bool,
// operator*, operator->, error(), value_or. There is deliberately no value().
template <class T, class E> using expected   = /* std:: or smply's own */;
template <class E>          using unexpected = /* ditto */;
template <class T>          using Result     = expected<T, Error>;

// How every asynchronous operation reports its outcome. Invoked exactly once.
// Callback<void> is the form for an operation with no value but a failure to
// report. Whatever it captures must outlive the SmpClient (see below).
template <class T>          using Callback   = std::function<void(Result<T>)>;

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
using ResponseCallback = Callback<RawResponse>;   // same type, named for its role

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

    // Queues work for the next poll(). This is what lets a layer above keep
    // the same promise: a management group that rejects an argument has no
    // request to attach the failure to, and without this would have to invoke
    // the callback inline. Drained by the destructor, so nothing is dropped.
    void defer(std::function<void()> work);

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

    // The bound transport's max_message_size(), or 0 if it has no opinion.
    // Only the client knows which transport is bound; upload chunk sizing
    // needs it (protocol-notes §8).
    std::size_t transport_max_message_size() const noexcept;
    std::size_t in_flight() const noexcept;

    const SmpClientStats&  stats()  const noexcept;
    const SmpClientConfig& config() const noexcept;
};

} // namespace smply
```

## `smply/groups/os.hpp`

```cpp
namespace smply {

// Callback<T> is declared in smply/result.hpp -- every group shares it:
//     template <class T> using Callback = std::function<void(Result<T>)>;

struct McumgrParameters {
    std::uint32_t buf_size  = 0;   // one SMP buffer, INCLUDING the 8-byte header
    std::uint32_t buf_count = 0;
};

struct ResetOptions {
    bool force = false;            // sent as a CBOR bool, omitted when false (A15)
    std::optional<Duration> timeout;
};

class OsManagement {
public:
    explicit OsManagement(SmpClient&) noexcept;

    // The response means the request was accepted, NOT that the device has
    // restarted. Wait for a transport disconnect to learn that. A reset hook
    // may refuse; SmpError::Busy invites a retry with force.
    RequestHandle reset(const ResetOptions&, Callback<void>);
    RequestHandle reset(Callback<void>);

    // Optional command: a minimal server answers SmpError::NotSupported, which
    // is an ordinary ProtocolError here. Fall back to
    // limits::kDefaultSmpMessageBudget rather than failing.
    RequestHandle mcumgr_parameters(Callback<McumgrParameters>);

    // Rejects text longer than limits::kMaxEchoLength with InvalidArgument,
    // and a reply longer than that with CborDecode.
    RequestHandle echo(std::string_view, Callback<std::string>);
};

} // namespace smply
```

## `smply/groups/image.hpp`

**Shipped (P8)** for state, set-state, erase and slot info. The upload types at
the end of this section are still **proposed** and belong to P10.

```cpp
namespace smply {

// A hash as the DEVICE reports it: MCUboot's IMAGE_TLV_SHA over header + body.
// IMAGE_SHA_LEN is 32, or 64 for a SHA-512 bootloader, so the length is carried
// rather than assumed (protocol-notes §6). Distinct from `Hash`, which is the
// 32-byte upload `sha` over the whole file -- different types so the two cannot
// be swapped by accident (protocol-notes §7).
class ImageHash {
public:
    ImageHash() = default;                              // empty
    static Result<ImageHash> from(ConstBytes);          // rejects empty or > 64
    static ImageHash        from(const Hash&);          // the 32-byte case
    ConstBytes  bytes() const noexcept;
    std::size_t size()  const noexcept;
    bool        empty() const noexcept;
    friend bool operator==(const ImageHash&, const ImageHash&) noexcept;
};

struct ImageVersion {
    std::uint8_t major{}, minor{}; std::uint16_t revision{}; std::uint32_t build{};
    // Accepts "1.2.3", the device's "1.2.3.4" and imgtool's "1.2.3+4".
    // Fails with InvalidArgument -- including on the "<???>" a device sends
    // when it cannot format a version at all.
    static Result<ImageVersion> parse(std::string_view);
    std::string to_string() const;   // the device's form: "1.2.3" or "1.2.3.4"
};

struct ImageSlot {
    std::uint32_t image = 0;          // absent in the response => 0 (A9)
    std::uint32_t slot  = 0;          // 0 = primary, 1 = secondary
    std::string   version;            // raw, exactly as reported
    std::optional<ImageHash> hash;    // IMAGE_TLV_SHA -- NOT the upload "sha"
    bool bootable = false, pending = false, confirmed = false;
    bool active   = false, permanent = false;   // absent => false, and a
                                                // present false is ordinary
};

struct ImageState {
    std::vector<ImageSlot> slots;               // empty is a valid answer
    std::optional<std::int32_t> split_status;

    const ImageSlot* active_slot(std::uint32_t image = 0) const noexcept;
    const ImageSlot* secondary(std::uint32_t image = 0) const noexcept;  // the
                                 // reported slot of that image that is not
                                 // active; nullptr once it has been erased
    const ImageSlot* find_by_hash(const ImageHash&) const noexcept;
};

struct SetStateRequest { std::optional<ImageHash> hash; bool confirm = false; };

struct EraseOptions {
    std::optional<std::uint32_t> slot;      // absent => the device chooses
    std::optional<Duration> timeout;        // absent => limits::kEraseTimeout
};

struct SlotDescriptor {
    std::uint32_t slot = 0;
    std::optional<std::uint32_t> size;             // absent iff open_error
    std::optional<std::uint32_t> upload_image_id;
    std::optional<std::int32_t>  open_error;       // the device's per-slot "rc"
};
struct ImageSlotsInfo { std::uint32_t image = 0; std::vector<SlotDescriptor> slots;
                        std::optional<std::uint32_t> max_image_size; };
struct SlotInfo      { std::vector<ImageSlotsInfo> images; };

// img_mgmt_err_code_t (protocol-notes §3). Group-scoped: an image rc of 3 is
// NoImage, while an SMP rc of 3 is SmpError::InvalidArgument.
enum class ImageError : std::uint16_t {
    Ok = 0, Unknown = 1, FlashConfigQueryFail = 2, NoImage = 3, NoTlvs = 4,
    InvalidTlv = 5, TlvMultipleHashesFound = 6, TlvInvalidSize = 7,
    HashNotFound = 8, NoFreeSlot = 9, FlashOpenFailed = 10, FlashReadFailed = 11,
    FlashWriteFailed = 12, FlashEraseFailed = 13, InvalidSlot = 14,
    NoFreeMemory = 15, FlashContextAlreadySet = 16, FlashContextNotSet = 17,
    FlashAreaDeviceNull = 18, InvalidPageOffset = 19, InvalidOffset = 20,
    InvalidLength = 21, InvalidImageHeader = 22, InvalidImageHeaderMagic = 23,
    InvalidHash = 24, InvalidFlashAddress = 25, VersionGetFailed = 26,
    CurrentVersionIsNewer = 27, ImageAlreadyPending = 28,
    InvalidImageVectorTable = 29, InvalidImageTooLarge = 30,
    InvalidImageDataOverrun = 31, ImageConfirmationDenied = 32,
    ImageSettingTestToActiveDenied = 33, ActiveSlotNotKnown = 34,
};

// nullopt unless the error is group-scoped AND the group is Image. Often
// nullopt even for a real image failure: over SMP v1 the server may translate
// the code onto mcumgr_err_t and drop the group (protocol-notes §9, A16), so
// check smp_error() too.
std::optional<ImageError> image_error(const Error&) noexcept;

class ImageManagement {
public:
    explicit ImageManagement(SmpClient&) noexcept;

    RequestHandle get_state(Callback<ImageState>);

    // Answers with the refreshed slot table. A request with neither hash nor
    // confirm is rejected with InvalidArgument -- the device cannot tell which
    // image is meant and answers ImageError::InvalidHash.
    RequestHandle set_state(const SetStateRequest&, Callback<ImageState>);

    // Carries limits::kEraseTimeout rather than the client's default: erase is
    // synchronous on the device and may take tens of seconds (A12).
    RequestHandle erase(const EraseOptions&, Callback<void>);
    RequestHandle erase(Callback<void>);

    // Optional command, and every field of the answer is optional too. A device
    // without it answers SmpError::NotSupported, which is not a failure (A8).
    RequestHandle get_slot_info(Callback<SlotInfo>);
};

} // namespace smply
```

Decoding bounds, all enforced before anything is copied or sized:
`limits::kMaxImages` entries in `images`, `limits::kMaxSlotsPerImage` in a
`slots` array, `limits::kMaxVersionStringLength` for a version string, and
`limits::kMaxImageHashLength` for a hash. Exceeding any of them, or a field of
the wrong type, is `ErrorCode::CborDecode`; an absent optional field never is.

### Upload

```cpp
namespace smply {

struct UploadOptions {
    std::uint32_t image = 0;
    bool          upgrade_only = false;      // protocol-notes §9 A11 — off by default
    std::optional<Hash> sha;                 // computed from the source when absent
    std::uint32_t chunk_size = 0;            // 0 => negotiate (design §6)

    // The device's SMP buffer, from OsManagement::mcumgr_parameters(). The
    // CALLER fetches it: it belongs to the OS group, and NotSupported from
    // that command is a normal answer to fall back from (A8). Absent =>
    // limits::kDefaultSmpMessageBudget. A present zero is ignored.
    std::optional<std::uint32_t> server_buf_size;

    std::uint32_t max_chunk_retries = limits::kMaxChunkRetries;   // 3
    std::uint32_t max_restarts      = limits::kMaxUploadRestarts; // 2
    std::uint32_t max_no_progress   = limits::kMaxNoProgress;     // 3
    Duration first_chunk_timeout = limits::kFirstChunkTimeout;    // 30 s — A7
    Duration chunk_timeout       = limits::kDefaultTimeout;       // 5 s
};

// `transferred` is the offset the DEVICE acknowledged, never what was sent, so
// it cannot overstate what was stored or move backwards.
struct UploadProgress { std::uint64_t transferred = 0, total = 0; };

struct UploadResult {
    std::uint64_t transferred = 0;
    std::optional<bool> match;   // absent on a device without the image check
                                 // (A6); a false fails with ImageMismatch
};

// Generation-tagged, like RequestHandle: once an upload ends the handle is
// inert forever. It holds no pointer to its ImageManagement, which is why the
// operations live there -- a handle that outlived its group would otherwise
// dangle instead of being an inert value.
class UploadHandle {
public:
    UploadHandle() = default;
    bool valid() const noexcept;
    explicit operator bool() const noexcept;
};

class ImageManagement {                     // upload half; see above for the rest
public:
    // `source` and this ImageManagement must both outlive the upload -- the
    // session lives here, not in the handle (ADR-0008).
    //
    // on_progress fires on every CONFIRMED advance, never on send. It may fire
    // once with everything: given a full sha, a device that already holds this
    // image answers the first packet with "complete" (protocol-notes §6 9a).
    //
    // on_done fires EXACTLY ONCE -- including on Disconnected, on cancel(), and
    // when this object is destroyed. A second upload while one runs is refused
    // with InvalidState.
    UploadHandle upload(ImageSource&, const UploadOptions&,
                        std::function<void(UploadProgress)> on_progress,
                        Callback<UploadResult> on_done);

    // A dropped link completes the upload with Disconnected but KEEPS the
    // session. Once the application has rebound the transport, this re-sends
    // the first packet with the same sha and continues from whatever offset the
    // device reports (§6 rule 6). InvalidState for a stale handle or a session
    // that ended any other way.
    void resume(const UploadHandle&, Callback<UploadResult> on_done);

    void          cancel(const UploadHandle&) noexcept;   // Cancelled on next poll()
    std::uint64_t transferred(const UploadHandle&) const noexcept;
    bool          uploading(const UploadHandle&) const noexcept;
};

} // namespace smply
```

`ImageManagement` is neither copyable nor movable, because an upload session
lives in it. Destroying it mid-upload cancels the outstanding request and
completes the callback with `Cancelled`, inline — the same exception
`~SmpClient` makes, and for the same reason: there is no later `poll()`.

## `smply/image_source.hpp`

Where the firmware bytes come from. smply never opens a file: the application
implements two functions, which keeps the core free of file I/O, of paths and of
a platform.

```cpp
namespace smply {

class ImageSource {
public:
    virtual ~ImageSource() = default;
    virtual std::uint64_t size() const noexcept = 0;

    // Reads into `out` at `offset`, returning the count. Must be out.size()
    // unless the read reached the end; at or past the end, 0.
    //
    // A short read anywhere else is a broken source, and smply reports it as
    // InvalidArgument rather than looping -- a source returning one byte per
    // call would otherwise turn a 16 MiB hash into 16 million calls.
    //
    // Implementations must accept any offset order: the header parse reads the
    // front, the TLV scan seeks near the back and forwards again.
    virtual Result<std::size_t> read(std::uint64_t offset, MutBytes out) = 0;
};

class MemoryImageSource final : public ImageSource {   // borrows the span
public:
    explicit MemoryImageSource(ConstBytes) noexcept;
    std::uint64_t size() const noexcept override;
    Result<std::size_t> read(std::uint64_t, MutBytes) override;   // never fails
};

} // namespace smply
```

## `smply/mcuboot_image.hpp`

The three narrow exceptions to treating the image as opaque
([ADR-0009](decisions/ADR-0009-mcuboot-boundary.md)). smply does **not** verify
signatures, decrypt, evaluate dependency TLVs or reimplement swap logic — **a
successful smply update is not an authenticity statement**
([`security.md`](security.md) §1).

```cpp
namespace smply {

inline constexpr std::uint32_t kMcubootImageMagic   = 0x96F3B83D;
inline constexpr std::uint32_t kMcubootImageMagicV1 = 0x96F3B83C;  // too old to use
inline constexpr std::size_t   kMcubootHeaderSize   = 32;

struct McubootImageInfo {
    std::uint32_t header_size = 0;         // ih_hdr_size, >= 32
    std::uint32_t image_size  = 0;         // ih_img_size, excludes the header
    std::uint32_t protected_tlv_size = 0;  // includes that area's own 4-byte header
    std::uint32_t flags = 0;               // verbatim; unknown bits are kept
    ImageVersion  version;                 // from groups/image.hpp
    bool          encrypted = false;       // IMAGE_F_ENCRYPTED_AES128|AES256
};

// Field by field from the span -- never a struct cast. Fails with
// InvalidArgument for a short span, a wrong magic (an unsigned zephyr.bin is
// the usual cause), an ih_hdr_size below 32, or a declared size above
// limits::kMaxImageSize.
Result<McubootImageInfo> parse_mcuboot_header(ConstBytes first_32_bytes);

// SHA-256 of the WHOLE FILE: the MCUmgr upload `sha`. Streams a few KiB at a
// time. NOT the image-state hash -- see protocol-notes §7.
Result<Hash> sha256(ImageSource&);

// The image-hash TLV (IMAGE_TLV_SHA256/384/512), for correlating this file with
// a device slot: the result compares directly with ImageSlot::hash.
//
// nullopt means there is no hash TLV, which is a normal answer -- as is an
// encrypted image, which is not scanned at all because the flashed bytes differ
// from the file's (A13). MalformedMessage for a structurally broken TLV area:
// a length overrunning the area, a protected size disagreeing with its own
// header, an area past the end of the file, more than limits::kMaxImageTlvs
// entries, or two hash TLVs.
Result<std::optional<ImageHash>> find_image_tlv_hash(ImageSource&,
                                                     const McubootImageInfo&);

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
