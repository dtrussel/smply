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
/// commits itself is applied and committed. A fake device is a wire adapter
/// over it: `tests/support/server_simulator.*` decodes SMP, runs the upload
/// session and injects faults, and asks this model every boot question.
///
/// Every rule is traced to docs/protocol-notes.md (section 7 for one image,
/// section 6's "Several images on one device" for more) and cites its source
/// there (S14, S35, S36, S39, A27, ADR-0022).
///
/// It reads flash with its own image reader (`image_reader.hpp`), never with
/// smply's parser, and links only smply's public headers, so an example may
/// use it. It is single-threaded and holds no lock: whoever owns a model owns
/// it on one thread.
///
/// Slots are numbered globally, as Zephyr does: image `n` owns slots `2n`
/// (primary, the one that runs) and `2n + 1` (secondary, the upload target).

#include "boot_model/image_reader.hpp"

#include "smply/bytes.hpp"
#include "smply/groups/image.hpp"

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

/// How the device is built: a description of the firmware, never a script.
struct BootConfig
{
    /// `CONFIG_MCUMGR_GRP_IMG_UPDATABLE_IMAGE_NUMBER`: how many image pairs.
    /// Zero is taken as one. Image 0 is the running application.
    std::uint32_t image_count = 1;

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

    // --- Flash ---------------------------------------------------------------

    /// Puts \p content into global \p slot, as though it had been flashed
    /// there. An empty vector is an erased slot. \p slot must exist.
    void load_slot(std::size_t slot, std::vector<std::byte> content);

    /// What global \p slot holds. \p slot must exist.
    [[nodiscard]] ConstBytes slot_content(std::size_t slot) const;

    /// The global slot an upload of \p image writes to: that image's
    /// secondary, `img_mgmt_get_unused_slot_area_id()` (S36). Nullopt when the
    /// device has no such image, which a server answers `NO_FREE_SLOT`.
    [[nodiscard]] std::optional<std::size_t> upload_target(std::uint64_t image) const;

    /// Starts an upload into global \p slot: the implicit erase of the target
    /// (protocol-notes section 6, rule 12), leaving \p size bytes of erased
    /// flash (`0xFF`) to write into. The caller bounds \p size by the slot's
    /// capacity first; the model never sizes flash on an unchecked number.
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
    /// A device-committed image marked for test is not swapped: the device
    /// starts applying it (ADR-0022).
    void reboot();

    /// Makes \p image device-committed after construction, as
    /// `BootConfig::device_commits` does at it.
    void device_commits(std::uint32_t image, const DeviceCommit& how);

    [[nodiscard]] bool device_committed(std::uint32_t image) const;

    // --- Listing -------------------------------------------------------------

    /// An image-state read: advances every device-committed apply or commit in
    /// progress by one read, then lists (docs/multi-image.md, ADR-0022).
    [[nodiscard]] std::vector<SlotEntry> read_state();

    /// The listing as it stands, as a set-state answer carries it. Invalid
    /// slot content is left out (S14).
    [[nodiscard]] std::vector<SlotEntry> listing() const;

    /// What \p image will do on the next boot. \p image must exist.
    [[nodiscard]] SwapType swap_type(std::uint32_t image = 0) const;

    // --- Bridge to the modes the simulator still runs ------------------------
    //
    // Overwrite (upgrade-only) and downgrade prevention are decided by
    // ServerSimulator until they move in here (docs/boot-model-plan.md); this
    // is the one operation they need from the model.

    /// Ends \p image's scheduled swap at a boot without a trial. With
    /// \p install the secondary's image is copied over the primary (an
    /// overwrite); without, it is dropped (a refused downgrade, S41). Either
    /// way the secondary is erased and nothing stays scheduled.
    void end_swap_without_trial(std::uint32_t image, bool install);

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

    [[nodiscard]] ImageError set_next_boot_slot(std::size_t global_slot, bool confirm);
    [[nodiscard]] std::size_t next_boot_slot(std::uint32_t image) const;
    void start_device_commits();
    void advance_applies();

    BootConfig config_;
    std::vector<ImagePair> images_;
};

} // namespace smply::boot_model

#endif // SMPLY_SUPPORT_BOOT_MODEL_BOOT_MODEL_HPP
