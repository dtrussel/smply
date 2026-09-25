// SPDX-License-Identifier: Apache-2.0

#include "smply/groups/image.hpp"

#include "cbor/cbor.hpp"
#include "groups/common.hpp"
#include "groups/image/decode.hpp"
#include "groups/image/upload_driver.hpp"
#include "groups/image/upload_session.hpp"
#include "smply/error.hpp"
#include "smply/mcuboot_image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace smply {
namespace {

using groups::command_id;
using groups::decode_slot_info;
using groups::decode_state;
using groups::reject;

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
// why groups::send() reports a failure to encode as Internal.
static_assert(kRequestBufferSize >= limits::kMaxImageHashLength + kLargestRequestEnvelope,
              "the request buffer must fit the largest legal set-state request");

constexpr const char* kBufferTooSmall = "image: request buffer too small";

/// Largest slot number smply will ask a device to erase.
///
/// Zephyr numbers slots globally across images, so a device that fits within
/// smply's own response bounds cannot have one beyond this. The device remains
/// the authority -- it answers `ImageError::InvalidSlot` for a slot it does not
/// have; this only rejects a number no device could mean.
constexpr std::uint32_t kMaxSlotNumber =
    static_cast<std::uint32_t>(limits::kMaxImages * limits::kMaxSlotsPerImage);

} // namespace

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
    Upload(SmpClient& client, ImageSource& source, const upload::UploadConfig& config,
           const UploadOptions& options, ProgressCallback on_progress,
           Callback<UploadResult> on_done, std::uint64_t upload_generation) noexcept
        : driver{client,
                 source,
                 config,
                 options.first_chunk_timeout,
                 options.final_chunk_timeout,
                 options.chunk_timeout,
                 std::move(on_progress),
                 std::move(on_done)},
          generation{upload_generation}
    {}

    upload::UploadDriver driver;
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
                                     ProgressCallback on_progress, Callback<UploadResult> on_done)
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

    upload::UploadConfig config;
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

    const upload::FirstPacketFields fields{.image_size = image_size,
                                           .image = config.image,
                                           .sha = config.sha,
                                           .upgrade_only = config.upgrade_only};
    if (options.chunk_size != 0) {
        config.chunk_size = options.chunk_size;
    } else {
        const upload::ChunkBudget budget{.server_buf_size = options.server_buf_size,
                                         .transport_max_message_size =
                                             client_->transport_max_message_size(),
                                         .configured_max = limits::kUploadChunkMax};
        const auto chunk = upload::compute_chunk_size(budget, fields);
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

UploadHandle ImageManagement::resume(const UploadHandle& handle, Callback<UploadResult> on_done)
{
    if (upload_ == nullptr || handle.generation_ != upload_->generation ||
        !upload_->driver.resumable()) {
        reject(*client_, std::move(on_done),
               Error{ErrorCode::InvalidState, "image: no upload to resume"});
        return UploadHandle{};
    }
    upload_->driver.restart(std::move(on_done));
    return handle;
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
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Read,
                                    .group = Group::Image,
                                    .command = command_id(ImageCommand::State),
                                    .payload = {},
                                    .timeout = {}},
                        groups::encode_empty(MutBytes{buffer}), std::move(on_done), decode_state,
                        kBufferTooSmall);
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

    // The answer is the refreshed slot table, the same shape get_state reads.
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Write,
                                    .group = Group::Image,
                                    .command = command_id(ImageCommand::State),
                                    .payload = {},
                                    .timeout = {}},
                        writer.close_map().finish(), std::move(on_done), decode_state,
                        kBufferTooSmall);
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

    return groups::send(*client_,
                        RequestSpec{.op = Operation::Write,
                                    .group = Group::Image,
                                    .command = command_id(ImageCommand::Erase),
                                    .payload = {},
                                    // Erase is synchronous on the device and can
                                    // take tens of seconds (protocol-notes
                                    // section 9, A12).
                                    .timeout = options.timeout.value_or(limits::kEraseTimeout)},
                        writer.close_map().finish(), std::move(on_done), groups::decode_nothing,
                        kBufferTooSmall);
}

RequestHandle ImageManagement::erase(Callback<void> on_done)
{
    return erase(EraseOptions{}, std::move(on_done));
}

RequestHandle ImageManagement::get_slot_info(Callback<SlotInfo> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    // SmpError::NotSupported arrives as a failure like any other device error;
    // recognising it and falling back is the caller's decision.
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Read,
                                    .group = Group::Image,
                                    .command = command_id(ImageCommand::SlotInfo),
                                    .payload = {},
                                    .timeout = {}},
                        groups::encode_empty(MutBytes{buffer}), std::move(on_done),
                        decode_slot_info, kBufferTooSmall);
}

} // namespace smply
