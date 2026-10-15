// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SUPPORT_BOOT_MODEL_BOOT_MODEL_HPP
#define SMPLY_SUPPORT_BOOT_MODEL_BOOT_MODEL_HPP

/// \file
/// The boot model: a device's slot table, and what MCUboot and Zephyr's image
/// group do to it. No wire, no thread, no clock.
///
/// It is the part of a pretend device that decides: the swap type each image
/// has, the flags image-state derives from it, the set-state and erase rules,
/// where an upload goes, what a reboot does, and how an image the device
/// commits itself is applied and committed -- for each of the four boot
/// behaviours it runs (swap with revert, overwrite, direct-XIP with and
/// without revert) and with or without downgrade prevention. A fake device is
/// a wire adapter over it: `tests/support/server_simulator.*` decodes SMP,
/// runs the upload session and injects faults, and asks this model every boot
/// question.
///
/// Every rule is traced to docs/protocol-notes.md (section 7 for one image,
/// section 6's "Several images on one device" for more) and cites its source
/// there (S10, S14, S35, S36, S39, S41, S56, A27, ADR-0022).
///
/// It reads flash with its own image reader (`image_reader.hpp`), never with
/// smply's parser: this model is the oracle smply's client is tested against,
/// and an oracle that shared the parser under test could not tell a correct
/// image from one both halves misread. It links only smply's public headers,
/// so an example may use it. It is single-threaded and holds no lock: whoever
/// owns a model owns it on one thread.
///
/// Slots are numbered globally, as Zephyr does: image `n` owns slots `2n`
/// (primary, the one that runs) and `2n + 1` (secondary, the upload target).

#include "boot_model/image_reader.hpp"

#include "smply/bytes.hpp"
#include "smply/groups/image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace smply::boot_model {

/// What MCUboot will do with an image on the next boot (protocol-notes
/// section 7).
///
/// A client never sees this value; it sees the slot flags derived from it,
/// which is exactly the indirection that makes a trial boot hard to recognise.
enum class SwapType : std::uint8_t
{
    None,   ///< Boot the active slot again.
    Test,   ///< Swap on the next boot, then require a confirm.
    Perm,   ///< Swap on the next boot and keep it.
    Revert, ///< A trial boot is running unconfirmed; the next boot undoes it.
};

/// How a device-committed image's apply ends (docs/multi-image.md, ADR-0022).
enum class ApplyOutcome : std::uint8_t
{
    Applied, ///< The target MCU runs it on trial: slot 0 new, not confirmed.
    Failed,  ///< It could not: slot 0 still the old image, nothing pending.
};

/// Whether the device commits an applied image when it should.
enum class CommitOutcome : std::uint8_t
{
    Commits, ///< After image 0 is confirmed, or at once if image 0 is not on trial.
    Never,   ///< It stays on trial: a commit that never happens.
};

/// How the device applies and commits an image it commits itself, as the
/// coordinating MCU of a dual-MCU device does with the other MCU's image
/// (docs/multi-image.md, ADR-0022). Each count is in image-state reads: one
/// read is one step of "a while".
struct DeviceCommit
{
    ApplyOutcome outcome = ApplyOutcome::Applied;
    /// Reads, after the first, until the apply ends with `outcome`.
    unsigned apply_reads = 1;
    CommitOutcome commit = CommitOutcome::Commits;
    /// Reads, after the first, until a commit in progress ends.
    unsigned commit_reads = 1;
};

/// What the bootloader does with the slots: the MCUboot modes the model runs
/// (protocol-notes section 7, "MCUboot modes", S56).
enum class BootBehaviour : std::uint8_t
{
    /// Swap with revert: scratch, move and offset alike. A test boots once on
    /// trial and reverts unless confirmed.
    SwapWithRevert,
    /// Upgrade-only: a scheduled image is copied over the primary for good at
    /// the next boot. No trial, nothing to revert.
    Overwrite,
    /// Direct-XIP: image 0 runs in place from either slot, the newest valid
    /// one booting, and set-state does not exist (S10, S14).
    DirectXip,
    /// Direct-XIP with revert: as above, but the other slot boots only once
    /// marked, and a one-boot trial reverts unless confirmed (S14).
    DirectXipWithRevert,
};

/// The behaviour a device runs when it reports \p reported as its MCUboot
/// mode: the number the bootloader-information `mode` query answers
/// (protocol-notes section 5), or nullopt when it answers nothing.
///
/// 2 is `Overwrite`, 4 `DirectXip` and 5 `DirectXipWithRevert` (S56). Every
/// swap mode, every mode the model does not run (single slot, RAM load, the
/// firmware loader), `-1`, any number outside the enum and no report at all
/// are `SwapWithRevert`. The reported number stays the fake's to send as
/// given; this only picks the nearest behaviour the model has.
[[nodiscard]] BootBehaviour behaviour_for_mode(std::optional<std::int64_t> reported) noexcept;

/// How the device is built: a description of the firmware, never a script.
struct BootConfig
{
    /// What the bootloader does with the slots.
    BootBehaviour behaviour = BootBehaviour::SwapWithRevert;

    /// `CONFIG_MCUMGR_GRP_IMG_UPDATABLE_IMAGE_NUMBER`: how many image pairs.
    /// Zero is taken as one. Image 0 is the running application.
    std::uint32_t image_count = 1;

    /// `CONFIG_MCUBOOT_BOOTLOADER_NO_DOWNGRADE`, mirroring MCUboot's
    /// `MCUBOOT_DOWNGRADE_PREVENTION`: a reboot erases a scheduled image older
    /// than the running one instead of booting it (S41, S57). Direct-XIP
    /// cannot enable it (S56), so it has no effect there.
    bool no_downgrade = false;

    /// The three Kconfigs that let a confirm reach something other than the
    /// running image's active slot (protocol-notes section 6, A27). All off,
    /// as in Zephyr.
    bool allow_confirm_non_active_image_secondary = false;
    bool allow_confirm_non_active_image_any = false;
    bool allow_confirm_non_active_slot = false;

    /// Per image, by index: whether and how the device commits it. An image
    /// beyond the end, or without a value, is committed by the client.
    std::vector<std::optional<DeviceCommit>> device_commits{};
};

/// One entry of the image-state listing: what `img_mgmt_state_read()` reports
/// for a slot holding a valid image (S14, S35).
struct SlotEntry
{
    std::uint32_t image = 0;
    /// Within the image: 0 or 1.
    std::size_t slot = 0;
    SlotImage content;
    bool pending = false;
    bool confirmed = false;
    bool active = false;
    bool permanent = false;
};

/// A device's images and slots, and the boot rules over them.
class BootModel
{
public:
    explicit BootModel(BootConfig config = {});

    /// How many image pairs the device has.
    [[nodiscard]] std::uint32_t image_count() const noexcept;

    /// Whether set-state exists: false only under plain direct-XIP, where
    /// Zephyr's write handler is `NULL` and a server answers `ENOTSUP` (S10).
    [[nodiscard]] bool supports_set_state() const noexcept;

    // --- Flash ---------------------------------------------------------------

    /// Puts \p content into global \p slot, as though it had been flashed
    /// there. An empty vector is an erased slot. A \p slot the device does
    /// not have throws `std::out_of_range`, as every operation taking a global
    /// slot or an image number does, in a release build too.
    void load_slot(std::size_t slot, std::vector<std::byte> content);

    /// What global \p slot holds.
    [[nodiscard]] ConstBytes slot_content(std::size_t slot) const;

    /// The global slot an upload of \p image writes to: that image's
    /// secondary, `img_mgmt_get_unused_slot_area_id()` (S36), or under
    /// direct-XIP image 0's slot that is not running (S10). Nullopt when the
    /// device has no such image, which a server answers `NO_FREE_SLOT`.
    [[nodiscard]] std::optional<std::size_t> upload_target(std::uint64_t image) const;

    /// Starts an upload into global \p slot: the implicit erase of the target
    /// (protocol-notes section 6, rule 12), leaving \p size bytes of erased
    /// flash (`0xFF`) to write into. The caller bounds \p size by the slot's
    /// capacity first; the model never sizes flash on an unchecked number.
    /// Under direct-XIP the erase takes the slot's trailer too, so a mark for
    /// the next boot it carried is gone (S14).
    void begin_upload(std::size_t slot, std::size_t size);

    /// Writes \p data at \p offset into global \p slot. False, and nothing
    /// written, when it would run past what `begin_upload()` erased.
    [[nodiscard]] bool write(std::size_t slot, std::size_t offset, ConstBytes data);

    /// Erases global \p slot (the image-erase command, protocol-notes
    /// section 6). `InvalidSlot` for a slot the device does not have,
    /// `NoFreeSlot` for one marked for the next boot.
    [[nodiscard]] ImageError erase(std::uint64_t slot);

    // --- Boot state ----------------------------------------------------------

    /// `img_mgmt_find_by_hash()`: the global slot whose image carries \p hash,
    /// searching every image's slots (S35).
    [[nodiscard]] std::optional<std::size_t> find_by_hash(ConstBytes hash) const;

    /// Set-state (`img_mgmt_state_write()`, S14, S35). With \p hash, the slot
    /// carrying it in any image; without, the running image's active slot,
    /// which only a confirm may name. The answer is Zephyr's: `Ok`,
    /// `HashNotFound`, or one of `img_mgmt_set_next_boot_slot()`'s denials, in
    /// its order. Checking the hash's encoding is the wire's job.
    ///
    /// Under direct-XIP with revert the rules are the same, applied to image
    /// 0's two slots and their boot state (S14): a hashless confirm names the
    /// slot running. Ask `supports_set_state()` first: under plain direct-XIP
    /// set-state does not exist, so this changes nothing and answers `Unknown`.
    [[nodiscard]] ImageError set_state(std::optional<ConstBytes> hash, bool confirm);

    /// Reboots: MCUboot runs every image's swap at the same boot
    /// (protocol-notes section 7).
    ///
    /// | before | after              |
    /// | ------ | ------------------ |
    /// | None   | unchanged          |
    /// | Test   | swapped, Revert    |
    /// | Perm   | swapped, None      |
    /// | Revert | swapped back, None |
    ///
    /// Under overwrite a Test or Perm instead copies the secondary over the
    /// primary and erases it, leaving None. With `no_downgrade`, a scheduled
    /// image older than the running one is erased and nothing is swapped
    /// (S41). A device-committed image marked for test is none of these: the
    /// device starts applying it (ADR-0022).
    ///
    /// Under direct-XIP, image 0 boots the slot MCUboot would choose: the
    /// newest valid one, a tie going to the lower slot, and with revert only a
    /// slot marked or confirmed. An unconfirmed trial is reverted and its slot
    /// erased (S14).
    void reboot();

    /// Makes \p image device-committed after construction, as
    /// `BootConfig::device_commits` does at it.
    void set_device_commit(std::uint32_t image, const DeviceCommit& how);

    [[nodiscard]] bool device_committed(std::uint32_t image) const;

    // --- Listing -------------------------------------------------------------

    /// An image-state read: advances every device-committed apply or commit in
    /// progress by one read, then lists (docs/multi-image.md, ADR-0022).
    [[nodiscard]] std::vector<SlotEntry> read_state();

    /// The listing as it stands, as a set-state answer carries it. Invalid
    /// slot content is left out (S14). Under direct-XIP, image 0's flags are
    /// `img_mgmt_state_flags()`'s for a device running in place (S14):
    /// without revert the running slot is never confirmed, and the slot
    /// MCUboot would boot is pending and permanent.
    [[nodiscard]] std::vector<SlotEntry> listing() const;

    /// What \p image will do on the next boot. Under
    /// direct-XIP, image 0's is always `None`: its boot state is per slot
    /// (`active_slot()`, and the listing's flags).
    [[nodiscard]] SwapType swap_type(std::uint32_t image = 0) const;

    // --- Direct-XIP ----------------------------------------------------------
    //
    // Image 0 runs in place from whichever slot MCUboot chose, and an upload
    // goes to the other (S10). Zephyr's image group does not support several
    // images under direct-XIP (S10): only image 0 runs in place, set-state
    // finds no other image's hash, and a reboot leaves the others alone.

    /// The slot image 0 runs from under direct-XIP: 0 at start.
    [[nodiscard]] std::size_t active_slot() const noexcept;

    /// Starts the device running image 0 from \p slot (0 or 1) instead, as
    /// though an earlier update had moved it there. The slot runs confirmed,
    /// and the other carries no boot state. Any other \p slot throws
    /// `std::out_of_range`.
    void boot_from_slot(std::size_t slot);

private:
    /// One image pair: its two slots and what the next boot does with them.
    struct ImagePair
    {
        std::vector<std::vector<std::byte>> slots{2};
        SwapType swap = SwapType::None;
        std::optional<DeviceCommit> device;
        /// Reads left before an apply in progress finishes; nullopt when none
        /// is in progress.
        std::optional<unsigned> applying;
        /// Reads left before a commit in progress finishes.
        std::optional<unsigned> committing;
    };

    /// Direct-XIP's boot state for one of image 0's slots, as Zephyr's
    /// read_boot_swap_state() names it: unset, marked for one boot, or
    /// confirmed for good (S14). Only the with-revert variant writes it.
    enum class XipState : std::uint8_t
    {
        Unset,
        Once,
        Forever,
    };

    /// Global \p slot's flash; `std::out_of_range` for a slot the device
    /// does not have.
    [[nodiscard]] std::vector<std::byte>& flash(std::size_t slot);
    [[nodiscard]] const std::vector<std::byte>& flash(std::size_t slot) const;

    /// Ends \p image's scheduled swap at a boot without a trial. With
    /// \p install the secondary's image is copied over the primary (an
    /// overwrite); without, it is dropped (a refused downgrade, S41). Either
    /// way the secondary is erased and nothing stays scheduled.
    void end_swap_without_trial(std::uint32_t image, bool install);
    /// With `no_downgrade`, whether MCUboot would erase \p image's scheduled
    /// image instead of booting it: it is older than the running one (S41).
    [[nodiscard]] bool refuses_downgrade(std::uint32_t image) const;
    [[nodiscard]] ImageError set_next_boot_slot(std::size_t global_slot, bool confirm);
    [[nodiscard]] std::size_t next_boot_slot(std::uint32_t image) const;
    void start_device_commits();
    void advance_applies();

    [[nodiscard]] bool direct_xip() const noexcept;
    /// MCUboot would prefer image 0's slot that is not running: it holds a
    /// valid, bootable image with a higher version, or the same version in a
    /// lower slot.
    [[nodiscard]] bool xip_prefers_other() const;
    /// What image 0's next boot runs under direct-XIP.
    enum class XipNext : std::uint8_t
    {
        Stay,         ///< The running slot boots again.
        OtherOnTrial, ///< The other slot boots, as a one-boot trial.
        OtherForGood, ///< The other slot boots, and keeps booting.
    };
    [[nodiscard]] XipNext xip_next() const;
    /// The flags image-state reports for image 0's \p slot under direct-XIP.
    [[nodiscard]] SlotEntry xip_flags(SlotEntry entry) const;
    [[nodiscard]] ImageError xip_set_next(std::size_t global_slot, bool confirm);
    void xip_reboot();

    BootConfig config_;
    std::vector<ImagePair> images_;

    /// Direct-XIP's state for image 0: the running slot, each slot's boot
    /// state, and whether the running slot is a one-boot trial nobody has
    /// confirmed, which the next reboot reverts.
    std::size_t xip_active_ = 0;
    std::array<XipState, 2> xip_state_{XipState::Forever, XipState::Unset};
    bool xip_trial_ = false;
};

} // namespace smply::boot_model

#endif // SMPLY_SUPPORT_BOOT_MODEL_BOOT_MODEL_HPP
