// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_GROUPS_IMAGE_HPP
#define SMPLY_GROUPS_IMAGE_HPP

/// \file
/// The image management group, group 1 (docs/protocol-notes.md section 6).
///
/// Everything group 1 offers except upload, which is its own state machine
/// (docs/design.md section 6): read the slot table, mark an image for test or
/// confirm it, erase a slot, and ask what the slots look like.
///
/// Like the OS group this is a thin encoder/decoder over `SmpClient`. It
/// allocates no sequence numbers, sets no deadlines beyond the erase command's
/// documented long one, and interprets no `rc`.
///
/// **Two different SHA-256 values live in this protocol and must not be
/// confused** (docs/protocol-notes.md section 7). The one here is `ImageHash`:
/// MCUboot's `IMAGE_TLV_SHA256` over the image header and body, computed by
/// imgtool at signing time and reported by the device. The *other* is the
/// upload `sha`, taken over the whole file, which smply computes itself and
/// which has the type `Hash`. They are deliberately different C++ types so that
/// passing one where the other belongs does not compile.
///
/// **Threading and lifetime.** As everywhere: calls and callbacks happen on the
/// client context, a callback never runs inside the call that started the
/// operation, and whatever a callback captures must outlive the `SmpClient`
/// (see `smply/smp_client.hpp`). An `ImageManagement` is a handle onto a
/// client, so it must not outlive it either.

#include "smply/bytes.hpp"
#include "smply/clock.hpp"
#include "smply/error.hpp"
#include "smply/image_source.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smply {

/// A hash as the *device* reports it for an image slot.
///
/// This is MCUboot's image-hash TLV over the header and body -- not the hash of
/// the uploaded file (docs/protocol-notes.md section 7). Its length is
/// `IMAGE_SHA_LEN` on the device: 32 bytes for the usual SHA-256 build and 64
/// for a bootloader built with `CONFIG_MCUBOOT_BOOTLOADER_USES_SHA512`, so the
/// length is carried rather than assumed.
///
/// Fixed capacity: the value is bounded by `limits::kMaxImageHashLength` before
/// it is stored, so a device cannot size an allocation.
class ImageHash
{
public:
    /// An empty hash. Present so the type is a regular value; a decoded hash is
    /// always non-empty, because an absent field decodes to `std::nullopt`.
    ImageHash() = default;

    /// Copies \p bytes, rejecting anything empty or longer than
    /// `limits::kMaxImageHashLength`.
    [[nodiscard]] static Result<ImageHash> from(ConstBytes bytes) noexcept;

    /// \overload The 32-byte SHA-256 case, which cannot fail.
    ///
    /// The conversion is deliberately explicit and one-way: it exists so a hash
    /// read out of a firmware file's TLVs can be compared against what a device
    /// reports, not so the upload `sha` can be passed as an image hash.
    [[nodiscard]] static ImageHash from(const Hash& hash) noexcept;

    /// The bytes, borrowed for as long as this object lives.
    [[nodiscard]] ConstBytes bytes() const noexcept
    {
        return ConstBytes{data_.data(), size_};
    }

    /// Length in bytes. Zero only for a default-constructed value.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    /// True for a default-constructed value.
    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    /// Compares length and contents. Hashes of different lengths are never
    /// equal, even if one is a prefix of the other.
    [[nodiscard]] friend bool operator==(const ImageHash& lhs, const ImageHash& rhs) noexcept
    {
        return lhs.size_ == rhs.size_ &&
               std::equal(lhs.data_.begin(),
                          lhs.data_.begin() + static_cast<std::ptrdiff_t>(lhs.size_),
                          rhs.data_.begin());
    }

private:
    std::array<std::byte, limits::kMaxImageHashLength> data_{};
    std::size_t size_ = 0;
};

/// An MCUboot image version, `major.minor.revision` plus a build number.
struct ImageVersion
{
    std::uint8_t major = 0;     ///< `ih_ver.iv_major`.
    std::uint8_t minor = 0;     ///< `ih_ver.iv_minor`.
    std::uint16_t revision = 0; ///< `ih_ver.iv_revision`.
    std::uint32_t build = 0;    ///< `ih_ver.iv_build_num`; 0 means unset.

    /// Parses a version string, or fails with `ErrorCode::InvalidArgument`.
    ///
    /// Accepts `"major.minor.revision"`, the device's own
    /// `"major.minor.revision.build"` and imgtool's `"major.minor.revision+build"`.
    /// Zephyr formats the dotted form and appends the build number only when it
    /// is non-zero (docs/protocol-notes.md section 6), so the dotted form is
    /// what a response actually carries; the `+` form is accepted because it is
    /// what a person types.
    ///
    /// A device may report `"<???>"` when it cannot format the version at all,
    /// which fails here like any other unparseable string. That is why
    /// `ImageSlot::version` keeps the raw text and parsing is a separate,
    /// fallible step.
    [[nodiscard]] static Result<ImageVersion> parse(std::string_view text);

    /// Renders the device's form: `"1.2.3"`, or `"1.2.3.4"` when `build` is
    /// non-zero. Round-trips through `parse()`.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend constexpr bool operator==(const ImageVersion&,
                                                   const ImageVersion&) noexcept = default;
};

/// One slot of one image, as reported by the state command.
///
/// Every flag defaults to false, which is also what an absent field means. A
/// Zephyr server sends the flags explicitly unless it was built with
/// `CONFIG_MCUMGR_GRP_IMG_FRUGAL_LIST`, so both an absent field and a present
/// `false` are ordinary (docs/protocol-notes.md section 6).
struct ImageSlot
{
    /// Image number. Absent in a single-image device's response, which means
    /// zero (docs/protocol-notes.md section 9, A9).
    std::uint32_t image = 0;
    /// 0 = primary (running), 1 = secondary (upload target).
    std::uint32_t slot = 0;
    /// The version string exactly as reported, including `"<???>"` when the
    /// device could not format one. Use `ImageVersion::parse()` to interpret it.
    std::string version;
    /// The image hash, when the device reported one. Optional only in MCUboot
    /// serial-recovery configurations; an MCUmgr application always sends it.
    std::optional<ImageHash> hash;
    bool bootable = false;  ///< The image can be booted.
    bool pending = false;   ///< Marked to run on the next boot.
    bool confirmed = false; ///< Will still be booted after the next reset.
    bool active = false;    ///< Currently running.
    bool permanent = false; ///< Marked for the next boot without a trial.

    [[nodiscard]] friend bool operator==(const ImageSlot&, const ImageSlot&) = default;
};

/// The device's slot table.
///
/// An empty list is a normal, successful answer: *"a response will only contain
/// information for valid images, if an image can not be identified as valid it
/// is simply skipped"* (docs/protocol-notes.md section 6), which is exactly the
/// state a freshly erased secondary slot is in.
struct ImageState
{
    /// One entry per reported slot, in the order the device sent them. Named
    /// for what an entry is; the wire calls the array "images".
    std::vector<ImageSlot> slots;
    /// Zephyr always reports zero, and omits the field entirely in a frugal
    /// build. Carried through rather than interpreted.
    std::optional<std::int32_t> split_status;

    /// The running slot of \p image, or nullptr if the device did not report
    /// one.
    [[nodiscard]] const ImageSlot* active_slot(std::uint32_t image = 0) const noexcept;

    /// The other slot of \p image -- the upload target -- or nullptr.
    ///
    /// Defined as the reported slot of that image which is not `active`, rather
    /// than as slot 1: which physical slot is secondary depends on which one is
    /// running. Returns nullptr after the slot has been erased, because the
    /// device then reports nothing for it.
    [[nodiscard]] const ImageSlot* secondary(std::uint32_t image = 0) const noexcept;

    /// The slot whose reported hash equals \p hash, or nullptr.
    [[nodiscard]] const ImageSlot* find_by_hash(const ImageHash& hash) const noexcept;
};

/// What to ask for when setting image state.
struct SetStateRequest
{
    /// Which image to act on, identified by its hash.
    ///
    /// Required to mark an image for test. May be omitted when confirming, in
    /// which case the device confirms the image it is currently running.
    std::optional<ImageHash> hash;

    /// Confirm rather than test.
    ///
    /// False marks the image for a **trial** boot: MCUboot swaps it in, and
    /// reverts on the next reset unless it is confirmed in the meantime. True
    /// makes it permanent. Confirming an image that has not yet been booted
    /// skips the trial altogether, which is legitimate but gives up the
    /// rollback safety net (docs/protocol-notes.md section 6).
    bool confirm = false;
};

/// What to ask for when erasing a slot.
struct EraseOptions
{
    /// Which slot to erase. Absent leaves the choice to the device, which
    /// erases the slot opposite the running one -- the upload target.
    std::optional<std::uint32_t> slot;

    /// Overrides `limits::kEraseTimeout`. Erase is synchronous on the device
    /// and can take tens of seconds (docs/protocol-notes.md section 9, A12),
    /// which is why it does not use the client's ordinary deadline.
    std::optional<Duration> timeout;
};

/// One slot in a slot-info response.
///
/// Everything but `slot` is optional, because everything but `slot` depends on
/// a Kconfig option or on the flash area opening successfully.
struct SlotDescriptor
{
    std::uint32_t slot = 0;
    /// Size of the flash area in bytes. Absent when `open_error` is set.
    std::optional<std::uint32_t> size;
    /// The image number to upload to in order to write this slot. Requires
    /// `CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD` to be meaningful for every slot.
    std::optional<std::uint32_t> upload_image_id;
    /// The device's `flash_area_open()` error for this slot, when it failed.
    ///
    /// Not in the specification; the server emits it in place of `size`
    /// (docs/protocol-notes.md section 6). It is a Zephyr `errno`, negative,
    /// and is reported rather than interpreted.
    std::optional<std::int32_t> open_error;

    [[nodiscard]] friend constexpr bool operator==(const SlotDescriptor&,
                                                   const SlotDescriptor&) noexcept = default;
};

/// One image's slots in a slot-info response.
struct ImageSlotsInfo
{
    std::uint32_t image = 0;
    std::vector<SlotDescriptor> slots;
    /// Largest image the device will accept for this image number. Requires one
    /// of the `CONFIG_MCUMGR_GRP_IMG_TOO_LARGE_*` options.
    std::optional<std::uint32_t> max_image_size;

    [[nodiscard]] friend bool operator==(const ImageSlotsInfo&, const ImageSlotsInfo&) = default;
};

/// The slot-info response.
///
/// **Best-effort pre-flight information only.** The whole command is optional,
/// and a device that does not implement it answers `SmpError::NotSupported`,
/// which is an ordinary outcome and not a reason to abandon an update
/// (docs/protocol-notes.md section 9, A8).
struct SlotInfo
{
    std::vector<ImageSlotsInfo> images;
};

/// The image group's own error codes, `img_mgmt_err_code_t`
/// (docs/protocol-notes.md section 3, S6).
///
/// These are **group-scoped**: an image `rc` of 3 is `NoImage`, while an
/// SMP-level `rc` of 3 is `SmpError::InvalidArgument`. Use `image_error()`,
/// which checks the group before reading the number, rather than casting an
/// `rc` directly.
///
/// The enumeration is append-only and grows with each Zephyr release, so a
/// value outside this list is normal and is carried through numerically rather
/// than rejected (docs/protocol-notes.md section 9, A2).
enum class ImageError : std::uint16_t
{
    Ok = 0,
    Unknown = 1,
    FlashConfigQueryFail = 2,
    NoImage = 3,
    NoTlvs = 4,
    InvalidTlv = 5,
    TlvMultipleHashesFound = 6,
    TlvInvalidSize = 7,
    HashNotFound = 8,
    NoFreeSlot = 9,
    FlashOpenFailed = 10,
    FlashReadFailed = 11,
    FlashWriteFailed = 12,
    FlashEraseFailed = 13,
    InvalidSlot = 14,
    NoFreeMemory = 15,
    FlashContextAlreadySet = 16,
    FlashContextNotSet = 17,
    FlashAreaDeviceNull = 18,
    InvalidPageOffset = 19,
    InvalidOffset = 20,
    InvalidLength = 21,
    InvalidImageHeader = 22,
    InvalidImageHeaderMagic = 23,
    InvalidHash = 24,
    InvalidFlashAddress = 25,
    VersionGetFailed = 26,
    CurrentVersionIsNewer = 27,
    ImageAlreadyPending = 28,
    InvalidImageVectorTable = 29,
    InvalidImageTooLarge = 30,
    InvalidImageDataOverrun = 31,
    ImageConfirmationDenied = 32,
    ImageSettingTestToActiveDenied = 33,
    ActiveSlotNotKnown = 34,
};

/// The image-group code an error carries, if it carries one.
///
/// Returns `std::nullopt` unless the device reported a **group-scoped** `rc`
/// belonging to the image group -- a flat SMP `rc` and another group's code
/// both mean something else entirely.
///
/// **A device will often not send one.** Group-scoped errors reach a client
/// only over SMP v2, and smply requests v1 by default (ADR-0010). A v1 server
/// built with `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` translates the code
/// onto `mcumgr_err_t` and *discards* the group detail: `HashNotFound` arrives
/// as `SmpError::Unknown`, `NoFreeSlot` as `SmpError::BadState`
/// (docs/protocol-notes.md section 9, A16). Check `smp_error()` as well, and do
/// not treat a missing image code as a malformed response.
[[nodiscard]] std::optional<ImageError> image_error(const Error& error) noexcept;

/// What to ask for when uploading an image.
struct UploadOptions
{
    /// Which image to write. Zephyr supports two today; 0 is the usual one.
    std::uint32_t image = 0;

    /// Ask the server to refuse a version that is not newer than the running
    /// one.
    ///
    /// Off by default: whether the comparison includes the build number is a
    /// Kconfig option, so the same image can be accepted by one device and
    /// refused by another (docs/protocol-notes.md section 9, A11).
    bool upgrade_only = false;

    /// SHA-256 of the whole file, as `sha256(ImageSource&)` computes it.
    ///
    /// Computed from the source when absent, which costs one extra pass. It is
    /// worth sending: the full 32-byte value is what lets the device resume an
    /// interrupted upload, skip an image it already holds, and verify what it
    /// flashed. Omitting it disables all three.
    std::optional<Hash> sha;

    /// Payload bytes per chunk. Zero negotiates one from the budget below.
    ///
    /// An explicit value above `limits::kUploadChunkMax` or below
    /// `limits::kUploadChunkMin` is rejected with `ErrorCode::InvalidArgument`.
    std::uint32_t chunk_size = 0;

    /// The device's SMP buffer size, from `OsManagement::mcumgr_parameters()`.
    ///
    /// The caller fetches it, because it belongs to the OS group and because
    /// `SmpError::NotSupported` from that command is a normal answer to fall
    /// back from, not an upload failure (A8). Absent means
    /// `limits::kDefaultSmpMessageBudget`.
    std::optional<std::uint32_t> server_buf_size;

    std::uint32_t max_chunk_retries = limits::kMaxChunkRetries;
    std::uint32_t max_restarts = limits::kMaxUploadRestarts;
    std::uint32_t max_no_progress = limits::kMaxNoProgress;

    /// Deadline for the first chunk, which may trigger an implicit slot erase
    /// of unbounded duration (A7).
    Duration first_chunk_timeout = limits::kFirstChunkTimeout;
    /// Deadline for every other chunk.
    Duration chunk_timeout = limits::kDefaultTimeout;
};

/// How far an upload has got.
///
/// `transferred` is the offset the **device** has acknowledged, never what was
/// put on the wire, so it cannot overstate what was stored.
struct UploadProgress
{
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
};

/// What an upload achieved.
struct UploadResult
{
    std::uint64_t transferred = 0;

    /// The device already held this image: it answered the **first** packet
    /// with the image complete, so nothing beyond that packet was sent
    /// (docs/protocol-notes.md section 6, rule 9a).
    ///
    /// The server runs that check itself, on any request at offset zero
    /// carrying a full `sha`, and it is the reason an upload can finish in one
    /// round trip. Without this flag a caller cannot tell that from a transfer
    /// that happened to be one chunk long -- `transferred` reads as the whole
    /// image either way, because the device acknowledged the whole image.
    bool already_present = false;
    /// The device's own verdict on the flashed bytes, when it has one.
    ///
    /// Absent on a device built without the image check, which is not a failure
    /// (docs/protocol-notes.md section 9, A6) -- a `false` never reaches here,
    /// because it fails the upload with `ErrorCode::ImageMismatch`.
    std::optional<bool> match;
};

/// Identifies one upload.
///
/// Generation-tagged, like `RequestHandle`: once an upload finishes the handle
/// is inert forever, so acting through a stale one is a no-op rather than an
/// attack on whatever upload started since.
///
/// It carries no pointer to its `ImageManagement`, which is why the operations
/// on it are methods there rather than here -- a handle that outlived its group
/// would otherwise be a dangling pointer instead of an inert value.
class UploadHandle
{
public:
    UploadHandle() = default;

    /// False for a default-constructed handle, and for one whose `upload()`
    /// could not start.
    [[nodiscard]] bool valid() const noexcept
    {
        return generation_ != 0;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    [[nodiscard]] friend constexpr bool operator==(const UploadHandle&,
                                                   const UploadHandle&) noexcept = default;

private:
    friend class ImageManagement;

    explicit UploadHandle(std::uint64_t generation) noexcept : generation_{generation} {}

    /// Zero means "never referred to an upload".
    std::uint64_t generation_ = 0;
};

/// Image state, set-state, erase and slot info.
///
/// The one-shot commands hold no state: destroying this object does not cancel
/// a request one of them issued -- the returned `RequestHandle` does that.
///
/// **`upload()` is different.** An upload is a long-running session, and it
/// lives here rather than in its handle (ADR-0008), which makes this the only
/// stateful object in the group layer. Three consequences: it is neither
/// copyable nor movable, it must outlive any upload it started, and destroying
/// it mid-upload cancels the outstanding request and completes the callback
/// with `ErrorCode::Cancelled`.
class ImageManagement
{
public:
    explicit ImageManagement(SmpClient& client) noexcept;

    ImageManagement(const ImageManagement&) = delete;
    ImageManagement(ImageManagement&&) = delete;
    ImageManagement& operator=(const ImageManagement&) = delete;
    ImageManagement& operator=(ImageManagement&&) = delete;

    /// Cancels an upload in progress and completes its callback.
    ~ImageManagement();

    /// Reads the device's slot table.
    ///
    /// An empty list is a success, not an error: the device reports only slots
    /// holding an image it considers valid.
    RequestHandle get_state(Callback<ImageState> on_done);

    /// Marks an image for test, or confirms one.
    ///
    /// The response is the refreshed slot table, so the caller can see the
    /// effect without a second round trip.
    ///
    /// A request with neither `hash` nor `confirm` is rejected with
    /// `ErrorCode::InvalidArgument`: the device has no way to tell which image
    /// to mark, and answers `ImageError::InvalidHash`.
    RequestHandle set_state(const SetStateRequest& request, Callback<ImageState> on_done);

    /// Erases a slot.
    ///
    /// Synchronous on the device and potentially very slow, so it carries
    /// `limits::kEraseTimeout` rather than the client's default deadline. A
    /// slot already marked for the next boot cannot be erased; that arrives as
    /// an ordinary `ErrorCode::ProtocolError`.
    RequestHandle erase(const EraseOptions& options, Callback<void> on_done);

    /// \overload Erases with default options.
    RequestHandle erase(Callback<void> on_done);

    /// Reads slot sizes and upload targets.
    ///
    /// **Optional command**, and everything in the answer is optional too. A
    /// device that does not implement it answers `SmpError::NotSupported`,
    /// which is an ordinary `ProtocolError` here and not a reason to fail an
    /// update.
    RequestHandle get_slot_info(Callback<SlotInfo> on_done);

    /// Uploads an image, one chunk at a time.
    ///
    /// \p source is borrowed until the upload ends and must outlive it, as
    /// must this `ImageManagement` -- the session lives here, not in the
    /// handle (ADR-0008).
    ///
    /// \p on_progress fires on every **confirmed** advance, never on send, so
    /// it cannot move backwards or overstate what the device holds. It may fire
    /// once with everything: given a full `sha`, a device that already holds
    /// this exact image answers the first packet with "complete"
    /// (docs/protocol-notes.md section 6).
    ///
    /// \p on_done fires exactly once, including when the link drops
    /// (`ErrorCode::Disconnected`), when `cancel()` is called
    /// (`ErrorCode::Cancelled`), and when this object is destroyed.
    ///
    /// Only one upload at a time: a second call while one is running is
    /// rejected with `ErrorCode::InvalidState`.
    ///
    /// The returned handle is invalid when the upload could not even be
    /// started -- a rejected argument, an unreadable source, a budget too small
    /// for a chunk. The reason still reaches \p on_done on the next `poll()`,
    /// as it does for `SmpClient::request()`.
    UploadHandle upload(ImageSource& source, const UploadOptions& options,
                        std::function<void(UploadProgress)> on_progress,
                        Callback<UploadResult> on_done);

    /// Restarts a disconnected upload on the current transport.
    ///
    /// A dropped link completes the upload with `ErrorCode::Disconnected` but
    /// keeps the session, so once the application has re-established the link
    /// (`SmpClient::rebind_transport()`) this re-sends the first packet with
    /// the same `sha` and continues from whatever offset the device reports
    /// (docs/protocol-notes.md section 6, rule 6). If the device forgot the
    /// session it answers zero and the upload restarts, bounded by
    /// `UploadOptions::max_restarts`.
    ///
    /// \p on_done is the callback given to `upload()`; it fires again for this
    /// attempt. A stale handle, or a session that ended any other way, is
    /// rejected with `ErrorCode::InvalidState`.
    void resume(const UploadHandle& handle, Callback<UploadResult> on_done);

    /// Abandons an upload. Its callback receives `Cancelled` on the next
    /// `poll()`; a stale or invalid handle is a no-op.
    ///
    /// No cleanup command is sent: the device's stale session is harmless and
    /// is superseded by the next upload's `sha`.
    void cancel(const UploadHandle& handle) noexcept;

    /// Bytes the device has acknowledged, or 0 for a stale handle.
    [[nodiscard]] std::uint64_t transferred(const UploadHandle& handle) const noexcept;

    /// True while \p handle names an upload that has not finished.
    [[nodiscard]] bool uploading(const UploadHandle& handle) const noexcept;

private:
    class Upload;

    SmpClient* client_;
    /// The one piece of mutable state in the group layer (ADR-0008). Held by
    /// pointer so this header does not need the session's definition.
    std::unique_ptr<Upload> upload_;
    /// Never reused, so a handle from a finished upload stays inert.
    std::uint64_t upload_generation_ = 0;
};

} // namespace smply

#endif // SMPLY_GROUPS_IMAGE_HPP
