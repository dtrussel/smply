// SPDX-License-Identifier: Apache-2.0

#include "boot_model/boot_model.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace smply::boot_model {

BootModel::BootModel(BootConfig config)
    : config_{std::move(config)}, images_(config_.image_count == 0 ? 1U : config_.image_count)
{
    for (std::size_t image = 0; image < images_.size() && image < config_.device_commits.size();
         ++image) {
        images_[image].device = config_.device_commits[image];
    }
}

std::uint32_t BootModel::image_count() const noexcept
{
    return static_cast<std::uint32_t>(images_.size());
}

// --- Flash -------------------------------------------------------------------

void BootModel::load_slot(std::size_t slot, std::vector<std::byte> content)
{
    assert(slot / 2 < images_.size());
    images_[slot / 2].slots[slot % 2] = std::move(content);
}

ConstBytes BootModel::slot_content(std::size_t slot) const
{
    assert(slot / 2 < images_.size());
    return ConstBytes{images_[slot / 2].slots[slot % 2]};
}

std::optional<std::size_t> BootModel::upload_target(std::uint64_t image) const
{
    // img_mgmt_upload_inspect() picks img_mgmt_get_unused_slot_area_id(image)
    // on a first packet (S36): the image's secondary. An image the device
    // does not have has no slot to upload to.
    if (image >= images_.size()) {
        return std::nullopt;
    }
    return (static_cast<std::size_t>(image) * 2) + 1;
}

void BootModel::begin_upload(std::size_t slot, std::size_t size)
{
    // The implicit erase of the target slot (protocol-notes section 6,
    // rule 12).
    assert(slot / 2 < images_.size());
    images_[slot / 2].slots[slot % 2].assign(size, std::byte{0xFF});
}

bool BootModel::write(std::size_t slot, std::size_t offset, ConstBytes data)
{
    assert(slot / 2 < images_.size());
    std::vector<std::byte>& flash = images_[slot / 2].slots[slot % 2];
    if (offset > flash.size() || data.size() > flash.size() - offset) {
        return false;
    }
    std::copy(data.begin(), data.end(), flash.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

ImageError BootModel::erase(std::uint64_t slot)
{
    // img_mgmt_erase() (S10): slots are global, as in the listing.
    if (slot >= images_.size() * 2) {
        return ImageError::InvalidSlot;
    }
    const auto image = static_cast<std::uint32_t>(slot / 2);
    const std::size_t within = slot % 2 == 0 ? 0U : 1U;
    if (within == next_boot_slot(image) && within != 0) {
        // A slot already marked for the next boot cannot be erased.
        return ImageError::NoFreeSlot;
    }
    images_[image].slots[within].clear();
    return ImageError::Ok;
}

// --- Boot state --------------------------------------------------------------

std::optional<std::size_t> BootModel::find_by_hash(ConstBytes hash) const
{
    // img_mgmt_find_by_hash() searches every image's slots (S35).
    for (std::size_t global = 0; global < images_.size() * 2; ++global) {
        const std::optional<SlotImage> described = describe(slot_content(global));
        if (described.has_value() &&
            std::equal(described->hash.begin(), described->hash.end(), hash.begin(), hash.end())) {
            return global;
        }
    }
    return std::nullopt;
}

ImageError BootModel::set_state(std::optional<ConstBytes> hash, bool confirm)
{
    // img_mgmt_state_write() (S14, S35). A hashless confirm names the RUNNING
    // image's active slot, never any other image's: global slot 0.
    std::size_t slot = 0;
    if (!hash.has_value()) {
        if (!confirm) {
            // A test with no hash names no image, and is refused (S14).
            return ImageError::InvalidHash;
        }
    } else {
        const std::optional<std::size_t> found = find_by_hash(*hash);
        if (!found.has_value()) {
            return ImageError::HashNotFound;
        }
        slot = *found;
    }
    return set_next_boot_slot(slot, confirm);
}

std::size_t BootModel::next_boot_slot(std::uint32_t image) const
{
    return images_.at(image).swap == SwapType::None ? 0U : 1U;
}

ImageError BootModel::set_next_boot_slot(std::size_t global_slot, bool confirm)
{
    const auto image = static_cast<std::uint32_t>(global_slot / 2);
    const std::size_t slot = global_slot % 2;
    const std::size_t active = 0;
    const std::size_t next = next_boot_slot(image);
    ImagePair& pair = images_[image];

    // img_mgmt_set_next_boot_slot() (S35), in its own order. First: confirming
    // an image that is not the running one is denied unless a Kconfig allows
    // it -- for any slot, or only for the secondary (A27).
    const bool running_image = image == 0;
    if (confirm && !running_image && !config_.allow_confirm_non_active_image_any &&
        (!config_.allow_confirm_non_active_image_secondary || slot == active)) {
        return ImageError::ImageConfirmationDenied;
    }
    // Then, for every image: confirming a slot that is not the active one
    // (S14, A27).
    if (confirm && slot != active && !config_.allow_confirm_non_active_slot) {
        return ImageError::ImageConfirmationDenied;
    }
    // Marking the running slot for test (S14).
    if (!confirm && slot == active) {
        return ImageError::ImageSettingTestToActiveDenied;
    }

    // While a swap is scheduled, changing it is refused; re-requesting exactly
    // what is scheduled succeeds and does nothing (S14).
    switch (pair.swap) {
    case SwapType::Test:
        if (!confirm && slot == next) {
            return ImageError::Ok;
        }
        return ImageError::ImageAlreadyPending;

    case SwapType::None:
    case SwapType::Perm:
        if (confirm && slot == next) {
            return ImageError::Ok;
        }
        if ((slot == active && active != next) || (!confirm && slot != active && slot == next)) {
            return ImageError::ImageAlreadyPending;
        }
        break;

    case SwapType::Revert:
        if (!confirm) {
            return ImageError::ImageAlreadyPending;
        }
        break;
    }

    // boot_set_next(): confirming the running slot clears a pending revert;
    // marking the other slot schedules a test, or a permanent swap when it is
    // confirmed at the same time.
    if (slot == active) {
        if (pair.swap == SwapType::Revert) {
            pair.swap = SwapType::None;
            pair.applying.reset();
            pair.committing.reset();
            if (image == 0) {
                // The coordinating MCU's confirm hook: now commit the images
                // it applied to the other MCU (S39, ADR-0022).
                start_device_commits();
            }
        }
    } else if (pair.swap == SwapType::None) {
        pair.swap = confirm ? SwapType::Perm : SwapType::Test;
    }
    return ImageError::Ok;
}

void BootModel::reboot()
{
    // MCUboot runs every image's swap at the same boot (protocol-notes
    // section 7, S36).
    for (ImagePair& pair : images_) {
        switch (pair.swap) {
        case SwapType::None:
            break;
        case SwapType::Test:
            if (pair.device.has_value()) {
                // Not this MCU's image: the coordinating MCU starts applying it
                // to the other one, and slot 0 keeps reporting what the other
                // MCU runs -- still the old image, with slot 1 pending
                // (docs/multi-image.md, ADR-0022).
                pair.applying = pair.device->apply_reads;
                break;
            }
            std::swap(pair.slots[0], pair.slots[1]);
            pair.swap = SwapType::Revert;
            break;
        case SwapType::Perm:
        case SwapType::Revert:
            // Two different intentions with the same effect: `Perm` swaps the
            // new image in for good, `Revert` swaps the old one back after an
            // unconfirmed trial. Either way the contents exchange and nothing
            // is left scheduled. For a device-committed image this is the
            // other MCU reverting its unconfirmed trial on a reset.
            std::swap(pair.slots[0], pair.slots[1]);
            pair.swap = SwapType::None;
            pair.applying.reset();
            pair.committing.reset();
            break;
        }
    }
}

void BootModel::device_commits(std::uint32_t image, const DeviceCommit& how)
{
    images_.at(image).device = how;
}

bool BootModel::device_committed(std::uint32_t image) const
{
    return images_.at(image).device.has_value();
}

void BootModel::start_device_commits()
{
    // Every device-committed image on trial starts its commit, as the
    // coordinating MCU does from its confirm hook (S39, ADR-0022).
    for (ImagePair& pair : images_) {
        const bool on_trial =
            pair.device.has_value() && pair.swap == SwapType::Revert && !pair.applying.has_value();
        if (on_trial && pair.device->commit == CommitOutcome::Commits &&
            !pair.committing.has_value()) {
            pair.committing = pair.device->commit_reads;
        }
    }
}

void BootModel::advance_applies()
{
    // The device-committed image's MCU finishes its update in the background;
    // each state read is one step of "a while" (docs/multi-image.md,
    // ADR-0022).
    for (ImagePair& pair : images_) {
        if (pair.committing.has_value()) {
            if (*pair.committing > 0) {
                --*pair.committing;
                continue;
            }
            // Committed: the other MCU's trial becomes permanent.
            pair.committing.reset();
            pair.swap = SwapType::None;
            continue;
        }
        if (!pair.applying.has_value()) {
            continue;
        }
        if (*pair.applying > 0) {
            --*pair.applying;
            continue;
        }
        pair.applying.reset();
        // `applying` is set only on a device-committed image.
        if (pair.device.has_value() && pair.device->outcome == ApplyOutcome::Applied) {
            // The other MCU runs it, on trial: slot 0 new and unconfirmed,
            // nothing pending.
            std::swap(pair.slots[0], pair.slots[1]);
            pair.swap = SwapType::Revert;
        } else {
            // Failed: the old image stays in slot 0, and the new one sits in
            // slot 1 with nothing pending -- the contract's failure.
            pair.swap = SwapType::None;
        }
    }
    // Nothing will confirm image 0 if it is not on trial, so the device
    // commits at once (ADR-0022).
    if (images_[0].swap != SwapType::Revert) {
        start_device_commits();
    }
}

// --- Listing -----------------------------------------------------------------

std::vector<SlotEntry> BootModel::read_state()
{
    advance_applies();
    return listing();
}

std::vector<SlotEntry> BootModel::listing() const
{
    std::vector<SlotEntry> entries;
    for (std::uint32_t image = 0; image < images_.size(); ++image) {
        // The flag table from img_mgmt_state_read() (S14, S35), which derives
        // every flag from the swap type rather than storing it per slot, and
        // flags each image's primary slot active.
        const ImagePair& pair = images_[image];
        const bool active_confirmed = pair.swap != SwapType::Revert;
        bool other_pending = false;
        bool other_permanent = false;
        bool other_confirmed = false;
        switch (pair.swap) {
        case SwapType::Perm:
            other_pending = true;
            other_permanent = true;
            break;
        case SwapType::Revert:
            other_confirmed = true;
            break;
        case SwapType::Test:
            other_pending = true;
            break;
        case SwapType::None:
            break;
        }

        for (std::size_t slot = 0; slot < pair.slots.size(); ++slot) {
            // A slot whose content is not a valid image is skipped silently,
            // which is why an empty images array is normal rather than an
            // error (S14).
            std::optional<SlotImage> described = describe(ConstBytes{pair.slots[slot]});
            if (!described.has_value()) {
                continue;
            }
            const bool is_active = slot == 0;
            entries.push_back(SlotEntry{.image = image,
                                        .slot = slot,
                                        .content = std::move(*described),
                                        .pending = !is_active && other_pending,
                                        .confirmed = is_active ? active_confirmed : other_confirmed,
                                        .active = is_active,
                                        .permanent = !is_active && other_permanent});
        }
    }
    return entries;
}

SwapType BootModel::swap_type(std::uint32_t image) const
{
    return images_.at(image).swap;
}

// --- Bridge ------------------------------------------------------------------

void BootModel::end_swap_without_trial(std::uint32_t image, bool install)
{
    ImagePair& pair = images_.at(image);
    if (install) {
        // Overwrite: the new image is copied over the old one for good
        // (protocol-notes section 7).
        pair.slots[0] = std::move(pair.slots[1]);
    }
    // Either way the secondary is erased: an overwrite consumes it, and
    // MCUboot's check_downgrade_prevention() erases a refused image (S41).
    pair.slots[1].clear();
    pair.swap = SwapType::None;
}

} // namespace smply::boot_model
