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

BootBehaviour behaviour_for_mode(std::optional<std::int64_t> reported) noexcept
{
    // enum mcuboot_mode (S56), protocol-notes section 7, "MCUboot modes".
    constexpr std::int64_t kUpgradeOnly = 2;
    constexpr std::int64_t kDirectXip = 4;
    constexpr std::int64_t kDirectXipWithRevert = 5;
    if (reported == kUpgradeOnly) {
        return BootBehaviour::Overwrite;
    }
    if (reported == kDirectXip) {
        return BootBehaviour::DirectXip;
    }
    if (reported == kDirectXipWithRevert) {
        return BootBehaviour::DirectXipWithRevert;
    }
    return BootBehaviour::SwapWithRevert;
}

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

BootBehaviour BootModel::behaviour() const noexcept
{
    return config_.behaviour;
}

bool BootModel::supports_set_state() const noexcept
{
    // The write handler is NULL under DIRECT_XIP (S10); with revert it stays.
    return config_.behaviour != BootBehaviour::DirectXip;
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
    if (direct_xip() && image == 0) {
        // Direct-XIP writes the slot that is not running (S10).
        return 1 - xip_active_;
    }
    return (static_cast<std::size_t>(image) * 2) + 1;
}

void BootModel::begin_upload(std::size_t slot, std::size_t size)
{
    // The implicit erase of the target slot (protocol-notes section 6,
    // rule 12).
    assert(slot / 2 < images_.size());
    images_[slot / 2].slots[slot % 2].assign(size, std::byte{0xFF});
    if (direct_xip() && slot < xip_state_.size()) {
        // The erase took the slot's trailer, and its boot state with it.
        xip_state_[slot] = XipState::Unset;
    }
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
    if (!supports_set_state()) {
        // The write handler is NULL under DIRECT_XIP (S10): a server answers
        // ENOTSUP before it would get here.
        return ImageError::Unknown;
    }
    // img_mgmt_state_write() (S14, S35). A hashless confirm names the RUNNING
    // image's active slot, never any other image's: global slot 0, or under
    // direct-XIP whichever of image 0's slots runs.
    std::size_t slot = direct_xip() ? xip_active_ : 0;
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
    if (direct_xip()) {
        return xip_set_next(slot, confirm);
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
    if (direct_xip()) {
        // Image 0 runs in place; the image group supports no other image
        // under direct-XIP (S10), so nothing else is swapped.
        xip_reboot();
        return;
    }
    // Downgrade prevention and overwrite end a scheduled swap without a
    // trial. Neither applies to an image the device commits itself, which is
    // not this MCU's to boot (ADR-0022).
    for (std::uint32_t image = 0; image < images_.size(); ++image) {
        const ImagePair& pair = images_[image];
        if ((pair.swap != SwapType::Test && pair.swap != SwapType::Perm) ||
            pair.device.has_value()) {
            continue;
        }
        if (refuses_downgrade(image)) {
            // MCUboot's check_downgrade_prevention(): the new image is erased,
            // and the old one boots as though nothing had been scheduled (S41).
            end_swap_without_trial(image, false);
        } else if (config_.behaviour == BootBehaviour::Overwrite) {
            // Upgrade-only: the new image is copied over the old one for good,
            // and the secondary is erased. No trial, nothing to revert
            // (protocol-notes section 7, "MCUboot modes").
            end_swap_without_trial(image, true);
        }
    }

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
            SlotEntry entry{.image = image,
                            .slot = slot,
                            .content = std::move(*described),
                            .pending = !is_active && other_pending,
                            .confirmed = is_active ? active_confirmed : other_confirmed,
                            .active = is_active,
                            .permanent = !is_active && other_permanent};
            if (direct_xip() && image == 0) {
                entry = xip_flags(std::move(entry));
            }
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

SwapType BootModel::swap_type(std::uint32_t image) const
{
    return images_.at(image).swap;
}

// --- Ending a swap without a trial ------------------------------------------

bool BootModel::refuses_downgrade(std::uint32_t image) const
{
    if (!config_.no_downgrade) {
        return false;
    }
    // major, minor and revision, as boot_compare_version() reads them (S41).
    // An equal version is accepted.
    const std::optional<HeaderVersion> running =
        header_version(slot_content(std::size_t{image} * 2));
    const std::optional<HeaderVersion> scheduled =
        header_version(slot_content((std::size_t{image} * 2) + 1));
    return running.has_value() && scheduled.has_value() && *scheduled < *running;
}

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

// --- Direct-XIP --------------------------------------------------------------
//
// Image 0 runs in place from whichever slot MCUboot chose. The rules are
// Zephyr's predictions of MCUboot's choice in img_mgmt_state.c (S14), which is
// what a client sees: the newest valid slot boots, a tie going to the lower
// slot; with revert, only a slot marked for one boot or for good qualifies, a
// one-boot trial reverts unless confirmed, and the flags follow the standard
// table. Without revert there is no set-state (S10), nothing is ever
// "confirmed", and the preferred slot is reported pending and permanent.

bool BootModel::direct_xip() const noexcept
{
    return config_.behaviour == BootBehaviour::DirectXip ||
           config_.behaviour == BootBehaviour::DirectXipWithRevert;
}

std::size_t BootModel::active_slot() const noexcept
{
    return xip_active_;
}

void BootModel::boot_from_slot(std::size_t slot)
{
    xip_active_ = slot % 2;
    xip_state_ = {XipState::Unset, XipState::Unset};
    xip_state_[xip_active_] = XipState::Forever;
    xip_trial_ = false;
}

bool BootModel::xip_prefers_other() const
{
    const std::size_t other = 1 - xip_active_;
    const ConstBytes theirs = slot_content(other);
    const ConstBytes ours = slot_content(xip_active_);
    const std::optional<SlotImage> candidate = describe(theirs);
    if (!candidate.has_value() || !candidate->bootable) {
        return false;
    }
    if (!describe(ours).has_value()) {
        return true;
    }
    // Both are valid images, so both have a whole header to compare.
    const HeaderVersion mine = header_version(ours).value_or(HeaderVersion{});
    const HeaderVersion other_version = header_version(theirs).value_or(HeaderVersion{});
    return mine < other_version || (mine == other_version && other < xip_active_);
}

std::optional<bool> BootModel::xip_next_is_trial() const
{
    if (!xip_prefers_other()) {
        return std::nullopt;
    }
    if (config_.behaviour == BootBehaviour::DirectXip) {
        return false;
    }
    switch (xip_state_[1 - xip_active_]) {
    case XipState::Once:
        return true;
    case XipState::Forever:
        return false;
    case XipState::Unset:
        return std::nullopt;
    }
    return std::nullopt;
}

SlotEntry BootModel::xip_flags(SlotEntry entry) const
{
    const bool active = entry.slot == xip_active_;
    const std::optional<bool> next_trial = xip_next_is_trial();
    entry.active = active;
    if (config_.behaviour == BootBehaviour::DirectXip) {
        // Never confirmed; the slot MCUboot would boot is pending and
        // permanent (S14).
        entry.pending = !active && next_trial.has_value();
        entry.confirmed = false;
        entry.permanent = !active && next_trial.has_value();
        return entry;
    }
    if (xip_trial_) {
        // Swap type REVERT: the running image is unconfirmed, the other is
        // what the device goes back to.
        entry.pending = false;
        entry.confirmed = !active;
        entry.permanent = false;
        return entry;
    }
    if (active) {
        entry.pending = false;
        entry.confirmed = true;
        entry.permanent = false;
        return entry;
    }
    entry.pending = next_trial.has_value();
    entry.confirmed = false;
    entry.permanent = next_trial.has_value() && !*next_trial;
    return entry;
}

void BootModel::xip_reboot()
{
    const std::size_t other = 1 - xip_active_;
    if (config_.behaviour == BootBehaviour::DirectXipWithRevert && xip_trial_) {
        // An unconfirmed trial: MCUboot drops the image and boots the other.
        images_[0].slots[xip_active_].clear();
        xip_state_[xip_active_] = XipState::Unset;
        xip_active_ = other;
        xip_trial_ = false;
        return;
    }
    const std::optional<bool> trial = xip_next_is_trial();
    if (!trial.has_value()) {
        return;
    }
    xip_active_ = other;
    xip_trial_ = *trial;
}

ImageError BootModel::xip_set_next(std::size_t global_slot, bool confirm)
{
    if (global_slot >= xip_state_.size()) {
        // Only image 0 is supported under direct-XIP (S10).
        return ImageError::HashNotFound;
    }
    const bool active = global_slot == xip_active_;
    // img_mgmt_set_next_boot_slot()'s denials, in its order (S14, A27).
    if (confirm && !active && !config_.allow_confirm_non_active_slot) {
        return ImageError::ImageConfirmationDenied;
    }
    if (!confirm && active) {
        return ImageError::ImageSettingTestToActiveDenied;
    }
    if (confirm) {
        xip_state_[global_slot] = XipState::Forever;
        if (active) {
            xip_trial_ = false;
        }
        return ImageError::Ok;
    }
    if (xip_trial_ || xip_state_[global_slot] == XipState::Forever) {
        return ImageError::ImageAlreadyPending;
    }
    xip_state_[global_slot] = XipState::Once; // already Once: nothing to do
    return ImageError::Ok;
}

} // namespace smply::boot_model
