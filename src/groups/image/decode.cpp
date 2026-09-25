// SPDX-License-Identifier: Apache-2.0
//
// The image group's response decoders (docs/protocol-notes.md section 6).
//
// Everything here reads bytes a device chose. Every count, size and string is
// bounded before it is used, and a reader's status() is checked before any
// decoded field is trusted (docs/design.md section 5, rules 1 to 3). The fuzz
// target fuzz_cbor_image_state reaches decode_state() through a real client.

#include "groups/image/decode.hpp"

#include "cbor/cbor.hpp"
#include "detail/narrow.hpp"
#include "groups/common.hpp"
#include "smply/error.hpp"
#include "smply/limits.hpp"
#include "smply/mcuboot_image.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace smply::groups {
namespace {

/// Narrows a decoded CBOR integer, or fails with \p where.
///
/// Every count, size and slot number in this group is a `uint32` on the wire,
/// and every status an `int32`; a value outside that is a device saying
/// something smply will not act on.
template<class To, class From>
[[nodiscard]] Result<To> narrow(From value, const char* where) noexcept
{
    if (const std::optional<To> narrowed = detail::checked_narrow<To>(value)) {
        return *narrowed;
    }
    return fail(Error{ErrorCode::CborDecode, where});
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
    const auto image_number =
        narrow<std::uint32_t>(image.value_or(0), "image: image number out of range");
    if (!image_number.has_value()) {
        return fail(image_number.error());
    }
    const auto slot_number = narrow<std::uint32_t>(*slot, "image: slot number out of range");
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
    const auto slot_number = narrow<std::uint32_t>(*slot, "image: slot number out of range");
    if (!slot_number.has_value()) {
        return fail(slot_number.error());
    }
    descriptor.slot = *slot_number;

    if (size.has_value()) {
        const auto value = narrow<std::uint32_t>(*size, "image: slot size out of range");
        if (!value.has_value()) {
            return fail(value.error());
        }
        descriptor.size = *value;
    }
    if (upload_image_id.has_value()) {
        const auto value =
            narrow<std::uint32_t>(*upload_image_id, "image: upload image id out of range");
        if (!value.has_value()) {
            return fail(value.error());
        }
        descriptor.upload_image_id = *value;
    }
    if (open_error.has_value()) {
        const auto value = narrow<std::int32_t>(*open_error, "image: slot rc out of range");
        if (!value.has_value()) {
            return fail(value.error());
        }
        descriptor.open_error = *value;
    }
    return descriptor;
}

/// Decodes one entry of a slot-info response's "images" array: an image's
/// size limit and its slots.
[[nodiscard]] Result<ImageSlotsInfo> decode_slots_image(cbor::Reader& element)
{
    const std::optional<std::uint64_t> image = element.uint("image");
    const std::optional<std::uint64_t> max_image_size = element.uint("max_image_size");
    if (const auto status = element.status(); !status.has_value()) {
        return fail(status.error());
    }

    ImageSlotsInfo entry;
    const auto image_number =
        narrow<std::uint32_t>(image.value_or(0), "image: image number out of range");
    if (!image_number.has_value()) {
        return fail(image_number.error());
    }
    entry.image = *image_number;
    if (max_image_size.has_value()) {
        const auto value =
            narrow<std::uint32_t>(*max_image_size, "image: max image size out of range");
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
    return entry;
}

} // namespace

/// Decodes an image-state response. Shared by get_state and set_state, which
/// answer with the same shape.
Result<ImageState> decode_state(ConstBytes payload)
{
    cbor::Reader reader{payload};
    if (const auto entered = groups::enter_response(reader); !entered.has_value()) {
        return fail(entered.error());
    }

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
        const auto value = narrow<std::int32_t>(*split, "image: splitStatus out of range");
        if (!value.has_value()) {
            return fail(value.error());
        }
        state.split_status = *value;
    }
    return state;
}

/// Decodes a slot-info response.
Result<SlotInfo> decode_slot_info(ConstBytes payload)
{
    cbor::Reader reader{payload};
    if (const auto entered = groups::enter_response(reader); !entered.has_value()) {
        return fail(entered.error());
    }

    SlotInfo info;
    const auto walked = reader.for_each_map_in_array(
        "images", limits::kMaxImages, [&info](cbor::Reader& element) -> Result<void> {
            auto entry = decode_slots_image(element);
            if (!entry.has_value()) {
                return fail(entry.error());
            }
            info.images.push_back(*std::move(entry));
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

} // namespace smply::groups
