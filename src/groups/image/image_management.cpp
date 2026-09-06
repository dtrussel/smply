// SPDX-License-Identifier: Apache-2.0

#include "smply/groups/image.hpp"

#include "cbor/cbor.hpp"
#include "groups/image/upload_driver.hpp"
#include "groups/image/upload_session.hpp"
#include "smply/error.hpp"
#include "smply/mcuboot_image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace smply {
namespace {

/// Image-group command IDs (docs/protocol-notes.md section 6, S6).
enum class ImageCommand : std::uint8_t
{
    State = 0,
    Erase = 5,
    SlotInfo = 6,
};

/// Every request in this group is a small, flat map. The largest is
/// set-state's: a map header, the key "hash" and a byte string of at most
/// `kMaxImageHashLength`, then the key "confirm" and a boolean.
constexpr std::size_t kLargestRequestEnvelope = 1 + 5 + 2 + 8 + 1;
constexpr std::size_t kRequestBufferSize = limits::kMaxImageHashLength + 32;

// Sized from the same constant that bounds the only variable-length field any
// request in this group carries, so encoding cannot run out of room -- which is
// why the encode guards below report Internal rather than an ordinary failure.
static_assert(kRequestBufferSize >= limits::kMaxImageHashLength + kLargestRequestEnvelope,
              "the request buffer must fit the largest legal set-state request");

/// Largest slot number smply will ask a device to erase.
///
/// Zephyr numbers slots globally across images, so a device that fits within
/// smply's own response bounds cannot have one beyond this. The device remains
/// the authority -- it answers `ImageError::InvalidSlot` for a slot it does not
/// have; this only rejects a number no device could mean.
constexpr std::uint32_t kMaxSlotNumber =
    static_cast<std::uint32_t>(limits::kMaxImages * limits::kMaxSlotsPerImage);

/// The empty CBOR map, `{}`, the body of every request with no fields.
[[nodiscard]] Result<ConstBytes> encode_empty(MutBytes buffer) noexcept
{
    cbor::Writer writer{buffer};
    return writer.open_map().close_map().finish();
}

[[nodiscard]] std::uint8_t command_id(ImageCommand command) noexcept
{
    return static_cast<std::uint8_t>(command);
}

/// Reports \p error to \p on_done on the next poll(), never inside this call.
template<class T>
RequestHandle reject(SmpClient& client, Callback<T> on_done, Error error)
{
    if (on_done) {
        client.defer([callback = std::move(on_done), failure = std::move(error)]() mutable {
            callback(fail(std::move(failure)));
        });
    }
    return {};
}

/// Narrows a decoded CBOR unsigned to 32 bits, or fails.
///
/// Every count, size and slot number in this group is a `uint32` on the wire;
/// a larger value is a device saying something smply will not act on.
[[nodiscard]] Result<std::uint32_t> narrow(std::uint64_t value, const char* where) noexcept
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return fail(Error{ErrorCode::CborDecode, where});
    }
    return static_cast<std::uint32_t>(value);
}

/// Decodes one entry of the image-state "images" array.
[[nodiscard]] Result<ImageSlot> decode_slot(cbor::Reader& element)
{
    const std::optional<std::uint64_t> image = element.uint("image");
    const std::optional<std::uint64_t> slot = element.uint("slot");
    const std::optional<std::string_view> version = element.text("version");
    const std::optional<ConstBytes> hash = element.bytes("hash");
    const std::optional<bool> bootable = element.boolean("bootable");
    const std::optional<bool> pending = element.boolean("pending");
    const std::optional<bool> confirmed = element.boolean("confirmed");
    const std::optional<bool> active = element.boolean("active");
    const std::optional<bool> permanent = element.boolean("permanent");

    // Checked before any of the above is trusted: a wrong-typed field poisons
    // the reader and leaves every other field looking merely absent, so a
    // malformed entry would otherwise decode into a plausible one full of
    // defaults (docs/design.md section 5, rule 2).
    if (const auto status = element.status(); !status.has_value()) {
        return fail(status.error());
    }

    // "slot" and "version" are the two fields the specification does not mark
    // optional, and the server always encodes both.
    if (!slot.has_value()) {
        return fail(Error{ErrorCode::CborDecode, "image: state entry has no slot"});
    }
    if (!version.has_value()) {
        return fail(Error{ErrorCode::CborDecode, "image: state entry has no version"});
    }
    if (version->size() > limits::kMaxVersionStringLength) {
        // Bounded before the copy: a device cannot make smply allocate on a
        // size it chose.
        return fail(Error{ErrorCode::CborDecode, "image: version string too long"});
    }

    ImageSlot decoded;
    // An absent "image" means zero -- single-image devices omit it entirely
    // (docs/protocol-notes.md section 9, A9).
    const auto image_number = narrow(image.value_or(0), "image: image number out of range");
    if (!image_number.has_value()) {
        return fail(image_number.error());
    }
    const auto slot_number = narrow(*slot, "image: slot number out of range");
    if (!slot_number.has_value()) {
        return fail(slot_number.error());
    }
    decoded.image = *image_number;
    decoded.slot = *slot_number;
    // The view points into the assembler's buffer, valid only for this
    // callback; the copy is what the caller keeps.
    decoded.version = std::string{*version};

    if (hash.has_value()) {
        auto value = ImageHash::from(*hash);
        if (!value.has_value()) {
            return fail(value.error());
        }
        decoded.hash = *std::move(value);
    }

    // An absent flag means false. A Zephyr server encodes the flags explicitly
    // unless it was built with CONFIG_MCUMGR_GRP_IMG_FRUGAL_LIST, so a present
    // `false` is just as ordinary (docs/protocol-notes.md section 6).
    decoded.bootable = bootable.value_or(false);
    decoded.pending = pending.value_or(false);
    decoded.confirmed = confirmed.value_or(false);
    decoded.active = active.value_or(false);
    decoded.permanent = permanent.value_or(false);
    return decoded;
}

/// Decodes an image-state response. Shared by get_state and set_state, which
/// answer with the same shape.
[[nodiscard]] Result<ImageState> decode_state(ConstBytes payload)
{
    cbor::Reader reader{payload};
    // LCOV_EXCL_START -- unreachable guard, and the whole block is: marking
    // only the `if` leaves its body counted against the branch denominator,
    // which is what docs/quality-gates.md section 6 excludes it for.
    if (const auto entered = reader.enter_map(); !entered.has_value()) {
        // Unreachable today: SmpClient::interpret() has already run
        // extract_mgmt_error() over this payload, which fails unless it is a
        // map. Checked anyway -- a decoder that assumes its input was validated
        // elsewhere is one refactor away from trusting a device.
        return fail(entered.error());
    }
    // LCOV_EXCL_STOP

    ImageState state;
    // An absent or empty array is a successful, empty answer: the device
    // reports only slots holding an image it considers valid, so an erased
    // secondary slot simply is not there (docs/protocol-notes.md section 6).
    const auto walked = reader.for_each_map_in_array(
        "images", limits::kMaxImages, [&state](cbor::Reader& element) -> Result<void> {
            auto slot = decode_slot(element);
            if (!slot.has_value()) {
                return fail(slot.error());
            }
            state.slots.push_back(*std::move(slot));
            return {};
        });
    if (!walked.has_value()) {
        return fail(walked.error());
    }

    const std::optional<std::int64_t> split = reader.integer("splitStatus");
    static_cast<void>(reader.leave_map());

    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (split.has_value()) {
        if (*split < std::numeric_limits<std::int32_t>::min() ||
            *split > std::numeric_limits<std::int32_t>::max()) {
            return fail(Error{ErrorCode::CborDecode, "image: splitStatus out of range"});
        }
        state.split_status = static_cast<std::int32_t>(*split);
    }
    return state;
}

/// Decodes one entry of a slot-info image's "slots" array.
[[nodiscard]] Result<SlotDescriptor> decode_slot_descriptor(cbor::Reader& element)
{
    const std::optional<std::uint64_t> slot = element.uint("slot");
    const std::optional<std::uint64_t> size = element.uint("size");
    const std::optional<std::uint64_t> upload_image_id = element.uint("upload_image_id");
    // Not in the specification: the server emits this in place of "size" when
    // it cannot open the slot's flash area (docs/protocol-notes.md section 6).
    // It is nested inside the slot map, so it is never the message-level "rc".
    const std::optional<std::int64_t> open_error = element.integer("rc");

    if (const auto status = element.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (!slot.has_value()) {
        return fail(Error{ErrorCode::CborDecode, "image: slot-info entry has no slot"});
    }

    SlotDescriptor descriptor;
    const auto slot_number = narrow(*slot, "image: slot number out of range");
    if (!slot_number.has_value()) {
        return fail(slot_number.error());
    }
    descriptor.slot = *slot_number;

    if (size.has_value()) {
        const auto value = narrow(*size, "image: slot size out of range");
        if (!value.has_value()) {
            return fail(value.error());
        }
        descriptor.size = *value;
    }
    if (upload_image_id.has_value()) {
        const auto value = narrow(*upload_image_id, "image: upload image id out of range");
        if (!value.has_value()) {
            return fail(value.error());
        }
        descriptor.upload_image_id = *value;
    }
    if (open_error.has_value()) {
        if (*open_error < std::numeric_limits<std::int32_t>::min() ||
            *open_error > std::numeric_limits<std::int32_t>::max()) {
            return fail(Error{ErrorCode::CborDecode, "image: slot rc out of range"});
        }
        descriptor.open_error = static_cast<std::int32_t>(*open_error);
    }
    return descriptor;
}

/// Decodes a slot-info response.
[[nodiscard]] Result<SlotInfo> decode_slot_info(ConstBytes payload)
{
    cbor::Reader reader{payload};
    // LCOV_EXCL_START -- unreachable guard; see the note above the first
    // one for why the whole block and not just the condition.
    if (const auto entered = reader.enter_map(); !entered.has_value()) {
        // Unreachable today: see decode_state().
        return fail(entered.error());
    }
    // LCOV_EXCL_STOP

    SlotInfo info;
    const auto walked = reader.for_each_map_in_array(
        "images", limits::kMaxImages, [&info](cbor::Reader& element) -> Result<void> {
            const std::optional<std::uint64_t> image = element.uint("image");
            const std::optional<std::uint64_t> max_image_size = element.uint("max_image_size");
            if (const auto status = element.status(); !status.has_value()) {
                return fail(status.error());
            }

            ImageSlotsInfo entry;
            const auto image_number = narrow(image.value_or(0), "image: image number out of range");
            if (!image_number.has_value()) {
                return fail(image_number.error());
            }
            entry.image = *image_number;
            if (max_image_size.has_value()) {
                const auto value = narrow(*max_image_size, "image: max image size out of range");
                if (!value.has_value()) {
                    return fail(value.error());
                }
                entry.max_image_size = *value;
            }

            const auto slots = element.for_each_map_in_array(
                "slots", limits::kMaxSlotsPerImage, [&entry](cbor::Reader& slot) -> Result<void> {
                    auto descriptor = decode_slot_descriptor(slot);
                    if (!descriptor.has_value()) {
                        return fail(descriptor.error());
                    }
                    entry.slots.push_back(*descriptor);
                    return {};
                });
            if (!slots.has_value()) {
                return fail(slots.error());
            }

            info.images.push_back(std::move(entry));
            return {};
        });
    if (!walked.has_value()) {
        return fail(walked.error());
    }

    static_cast<void>(reader.leave_map());
    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    return info;
}

/// Completes \p callback with the result of \p decode, or with the failure that
/// arrived instead of a response.
template<class T, class Decode>
void complete(Callback<T>& callback, Result<RawResponse>& response, Decode decode)
{
    if (!callback) {
        return;
    }
    if (!response.has_value()) {
        callback(fail(response.error()));
        return;
    }
    auto decoded = decode(response->payload);
    if (!decoded.has_value()) {
        callback(fail(decoded.error()));
        return;
    }
    callback(*std::move(decoded));
}

} // namespace

Result<ImageHash> ImageHash::from(ConstBytes bytes) noexcept
{
    if (bytes.empty()) {
        // An absent hash decodes to std::nullopt; a present but empty one is a
        // device saying something that cannot be true.
        return fail(Error{ErrorCode::CborDecode, "image: empty hash"});
    }
    if (bytes.size() > limits::kMaxImageHashLength) {
        return fail(Error{ErrorCode::CborDecode, "image: hash too long"});
    }
    ImageHash hash;
    std::copy(bytes.begin(), bytes.end(), hash.data_.begin());
    hash.size_ = bytes.size();
    return hash;
}

ImageHash ImageHash::from(const Hash& hash) noexcept
{
    ImageHash value;
    std::copy(hash.begin(), hash.end(), value.data_.begin());
    value.size_ = hash.size();
    return value;
}

Result<ImageVersion> ImageVersion::parse(std::string_view text)
{
    if (text.empty() || text.size() > limits::kMaxVersionStringLength) {
        return fail(Error{ErrorCode::InvalidArgument, "image: version string not parseable"});
    }

    // Hand-written rather than delegating to a stream or strtoul: both accept
    // leading signs, whitespace and other spellings this grammar does not have,
    // and neither reports trailing junk without extra care.
    std::size_t at = 0;
    const auto number = [&text, &at](std::uint64_t limit) -> Result<std::uint64_t> {
        const std::size_t start = at;
        std::uint64_t value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            value = (value * 10) + static_cast<std::uint64_t>(text[at] - '0');
            if (value > limit) {
                return fail(
                    Error{ErrorCode::InvalidArgument, "image: version component too large"});
            }
            ++at;
        }
        if (at == start) {
            return fail(Error{ErrorCode::InvalidArgument, "image: version component missing"});
        }
        return value;
    };
    const auto separator = [&text, &at](char expected) {
        if (at < text.size() && text[at] == expected) {
            ++at;
            return true;
        }
        return false;
    };

    ImageVersion version;
    const auto major = number(std::numeric_limits<std::uint8_t>::max());
    if (!major.has_value()) {
        return fail(major.error());
    }
    if (!separator('.')) {
        return fail(Error{ErrorCode::InvalidArgument, "image: version needs major.minor.revision"});
    }
    const auto minor = number(std::numeric_limits<std::uint8_t>::max());
    if (!minor.has_value()) {
        return fail(minor.error());
    }
    if (!separator('.')) {
        return fail(Error{ErrorCode::InvalidArgument, "image: version needs major.minor.revision"});
    }
    const auto revision = number(std::numeric_limits<std::uint16_t>::max());
    if (!revision.has_value()) {
        return fail(revision.error());
    }

    version.major = static_cast<std::uint8_t>(*major);
    version.minor = static_cast<std::uint8_t>(*minor);
    version.revision = static_cast<std::uint16_t>(*revision);

    // The device writes ".build" and imgtool takes "+build"; both are accepted
    // so a string can round-trip either way (docs/protocol-notes.md section 6).
    if (separator('.') || separator('+')) {
        const auto build = number(std::numeric_limits<std::uint32_t>::max());
        if (!build.has_value()) {
            return fail(build.error());
        }
        version.build = static_cast<std::uint32_t>(*build);
    }

    if (at != text.size()) {
        return fail(Error{ErrorCode::InvalidArgument, "image: trailing text after version"});
    }
    return version;
}

std::string ImageVersion::to_string() const
{
    std::string text =
        std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(revision);
    if (build != 0) {
        text += '.' + std::to_string(build);
    }
    return text;
}

const ImageSlot* ImageState::active_slot(std::uint32_t image) const noexcept
{
    for (const ImageSlot& slot : slots) {
        if (slot.image == image && slot.active) {
            return &slot;
        }
    }
    return nullptr;
}

const ImageSlot* ImageState::secondary(std::uint32_t image) const noexcept
{
    for (const ImageSlot& slot : slots) {
        if (slot.image == image && !slot.active) {
            return &slot;
        }
    }
    return nullptr;
}

const ImageSlot* ImageState::find_by_hash(const ImageHash& hash) const noexcept
{
    for (const ImageSlot& slot : slots) {
        if (slot.hash.has_value() && *slot.hash == hash) {
            return &slot;
        }
    }
    return nullptr;
}

std::optional<ImageError> image_error(const Error& error) noexcept
{
    // Bound once rather than reaching through mgmt() repeatedly, so the engaged
    // state is visible where rc is read.
    const std::optional<MgmtError>& mgmt = error.mgmt();
    if (!mgmt.has_value() || !mgmt->group_scoped || mgmt->group != Group::Image) {
        return std::nullopt;
    }
    return static_cast<ImageError>(mgmt->rc);
}

/// One upload, plus the generation that makes a stale handle inert.
class ImageManagement::Upload
{
public:
    Upload(SmpClient& client, ImageSource& source, const image::UploadConfig& config,
           const UploadOptions& options, std::function<void(UploadProgress)> on_progress,
           Callback<UploadResult> on_done, std::uint64_t upload_generation) noexcept
        : driver{client,
                 source,
                 config,
                 options.first_chunk_timeout,
                 options.chunk_timeout,
                 std::move(on_progress),
                 std::move(on_done)},
          generation{upload_generation}
    {}

    image::UploadDriver driver;
    std::uint64_t generation;
};

ImageManagement::ImageManagement(SmpClient& client) noexcept : client_{&client} {}

ImageManagement::~ImageManagement()
{
    if (upload_) {
        // Completes the callback inline: there is no later poll() to defer to.
        upload_->driver.abandon();
    }
}

UploadHandle ImageManagement::upload(ImageSource& source, const UploadOptions& options,
                                     std::function<void(UploadProgress)> on_progress,
                                     Callback<UploadResult> on_done)
{
    const auto refuse = [this, &on_done](Error error) {
        return reject(*client_, std::move(on_done), std::move(error)), UploadHandle{};
    };

    if (upload_ && upload_->driver.active()) {
        return refuse(Error{ErrorCode::InvalidState, "image: an upload is already running"});
    }

    const std::uint64_t image_size = source.size();
    if (image_size == 0) {
        // The server rejects a first chunk that does not carry the 32-byte
        // MCUboot header, so an empty source can never succeed.
        return refuse(Error{ErrorCode::InvalidArgument, "image: empty image source"});
    }
    if (image_size > limits::kMaxImageSize) {
        return refuse(Error{ErrorCode::InvalidArgument, "image: image implausibly large"});
    }
    if (options.chunk_size != 0 && (options.chunk_size < limits::kUploadChunkMin ||
                                    options.chunk_size > limits::kUploadChunkMax)) {
        return refuse(Error{ErrorCode::InvalidArgument, "image: chunk size out of range"});
    }

    image::UploadConfig config;
    config.image_size = image_size;
    config.image = options.image;
    config.sha = options.sha;
    config.upgrade_only = options.upgrade_only;
    config.max_chunk_retries = options.max_chunk_retries;
    config.max_restarts = options.max_restarts;
    config.max_no_progress = options.max_no_progress;

    if (!config.sha.has_value()) {
        // Worth the extra pass over the source: the full hash is what lets the
        // device resume, skip an image it already holds, and verify what it
        // flashed (docs/protocol-notes.md section 6).
        auto digest = sha256(source);
        if (!digest.has_value()) {
            return refuse(digest.error());
        }
        config.sha = *digest;
    }

    const image::FirstPacketFields fields{.image_size = image_size,
                                          .image = config.image,
                                          .sha = config.sha,
                                          .upgrade_only = config.upgrade_only};
    if (options.chunk_size != 0) {
        config.chunk_size = options.chunk_size;
    } else {
        const image::ChunkBudget budget{.server_buf_size = options.server_buf_size,
                                        .transport_max_message_size =
                                            client_->transport_max_message_size(),
                                        .configured_max = limits::kUploadChunkMax};
        const auto chunk = image::compute_chunk_size(budget, fields);
        if (!chunk.has_value()) {
            return refuse(chunk.error());
        }
        config.chunk_size = *chunk;
    }

    ++upload_generation_;
    upload_ = std::make_unique<Upload>(*client_, source, config, options, std::move(on_progress),
                                       std::move(on_done), upload_generation_);
    upload_->driver.start();
    if (!upload_->driver.active()) {
        // It failed before a byte went out -- an unreadable source, say. The
        // reason is already queued for the next poll(); the handle is invalid,
        // exactly as SmpClient::request() does for a request it could not even
        // attempt.
        return {};
    }
    return UploadHandle{upload_generation_};
}

void ImageManagement::resume(const UploadHandle& handle, Callback<UploadResult> on_done)
{
    if (upload_ == nullptr || handle.generation_ != upload_->generation ||
        !upload_->driver.resumable()) {
        reject(*client_, std::move(on_done),
               Error{ErrorCode::InvalidState, "image: no upload to resume"});
        return;
    }
    upload_->driver.restart(std::move(on_done));
}

void ImageManagement::cancel(const UploadHandle& handle) noexcept
{
    if (upload_ != nullptr && handle.generation_ == upload_->generation) {
        upload_->driver.cancel();
    }
}

std::uint64_t ImageManagement::transferred(const UploadHandle& handle) const noexcept
{
    if (upload_ == nullptr || handle.generation_ != upload_->generation) {
        return 0;
    }
    return upload_->driver.transferred();
}

bool ImageManagement::uploading(const UploadHandle& handle) const noexcept
{
    return upload_ != nullptr && handle.generation_ == upload_->generation &&
           upload_->driver.active();
}

RequestHandle ImageManagement::get_state(Callback<ImageState> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    const auto payload = encode_empty(MutBytes{buffer});
    // LCOV_EXCL_START -- unreachable guard; see the note above the first
    // one for why the whole block and not just the condition.
    if (!payload.has_value()) {
        // Unreachable: see the static_assert on kRequestBufferSize. Kept as a
        // guard rather than deleted, so a future change to the sizing fails
        // loudly instead of sending a truncated request.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::Internal, "image: request buffer too small"});
    }
    // LCOV_EXCL_STOP

    const RequestSpec spec{.op = Operation::Read,
                           .group = Group::Image,
                           .command = command_id(ImageCommand::State),
                           .payload = *payload,
                           .timeout = {}};

    return client_->request(spec,
                            [callback = std::move(on_done)](Result<RawResponse> response) mutable {
                                complete(callback, response, decode_state);
                            });
}

RequestHandle ImageManagement::set_state(const SetStateRequest& request,
                                         Callback<ImageState> on_done)
{
    if (!request.confirm && !request.hash.has_value()) {
        // The device cannot tell which image to mark and answers
        // ImageError::InvalidHash; saying so here is clearer and cheaper.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, "image: test needs a hash"});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    writer.open_map();
    if (request.hash.has_value()) {
        writer.put_bytes("hash", request.hash->bytes());
    }
    // Encoded even when false: the specification does not mark it optional, and
    // a confirm that silently became a test would be the worse failure.
    writer.put_bool("confirm", request.confirm);
    const auto payload = writer.close_map().finish();
    // LCOV_EXCL_START -- unreachable guard; see the note above the first
    // one for why the whole block and not just the condition.
    if (!payload.has_value()) {
        // Unreachable: see the static_assert on kRequestBufferSize.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::Internal, "image: request buffer too small"});
    }
    // LCOV_EXCL_STOP

    const RequestSpec spec{.op = Operation::Write,
                           .group = Group::Image,
                           .command = command_id(ImageCommand::State),
                           .payload = *payload,
                           .timeout = {}};

    return client_->request(spec,
                            [callback = std::move(on_done)](Result<RawResponse> response) mutable {
                                complete(callback, response, decode_state);
                            });
}

RequestHandle ImageManagement::erase(const EraseOptions& options, Callback<void> on_done)
{
    if (options.slot.has_value() && *options.slot > kMaxSlotNumber) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, "image: slot number out of range"});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    writer.open_map();
    if (options.slot.has_value()) {
        // Omitted when the caller did not choose, which leaves the device to
        // erase the slot opposite the running one -- the upload target.
        writer.put_uint("slot", *options.slot);
    }
    const auto payload = writer.close_map().finish();
    // LCOV_EXCL_START -- unreachable guard; see the note above the first
    // one for why the whole block and not just the condition.
    if (!payload.has_value()) {
        // Unreachable: see the static_assert on kRequestBufferSize.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::Internal, "image: request buffer too small"});
    }
    // LCOV_EXCL_STOP

    const RequestSpec spec{.op = Operation::Write,
                           .group = Group::Image,
                           .command = command_id(ImageCommand::Erase),
                           .payload = *payload,
                           // Erase is synchronous on the device and can take
                           // tens of seconds (protocol-notes section 9, A12).
                           .timeout = options.timeout.value_or(limits::kEraseTimeout)};

    return client_->request(spec, [callback = std::move(on_done)](Result<RawResponse> response) {
        if (!callback) {
            return;
        }
        if (!response.has_value()) {
            callback(fail(response.error()));
            return;
        }
        // Success carries an empty map -- or `{"rc": 0}` from a server built
        // for the legacy result-code behaviour, which SmpClient has already
        // read as success. Either way there is nothing to decode.
        callback({});
    });
}

RequestHandle ImageManagement::erase(Callback<void> on_done)
{
    return erase(EraseOptions{}, std::move(on_done));
}

RequestHandle ImageManagement::get_slot_info(Callback<SlotInfo> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    const auto payload = encode_empty(MutBytes{buffer});
    // LCOV_EXCL_START -- unreachable guard; see the note above the first
    // one for why the whole block and not just the condition.
    if (!payload.has_value()) {
        // Unreachable: see the static_assert on kRequestBufferSize.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::Internal, "image: request buffer too small"});
    }
    // LCOV_EXCL_STOP

    const RequestSpec spec{.op = Operation::Read,
                           .group = Group::Image,
                           .command = command_id(ImageCommand::SlotInfo),
                           .payload = *payload,
                           .timeout = {}};

    return client_->request(spec,
                            [callback = std::move(on_done)](Result<RawResponse> response) mutable {
                                // SmpError::NotSupported arrives here like any other device error;
                                // recognising it and falling back is the caller's decision.
                                complete(callback, response, decode_slot_info);
                            });
}

} // namespace smply
