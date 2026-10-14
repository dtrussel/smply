// SPDX-License-Identifier: Apache-2.0
//
// The boot model, rule by rule, through its interface only: a slot table in,
// an operation, a listing, a swap type or an ImageError out. No SMP is encoded
// anywhere here, so a failure is a rule bug and not a CBOR one.
//
// Each case names the docs/protocol-notes.md entry it checks. The expected
// values are the tables and sentences there, not a rerun of the model's code.

#include "boot_model/boot_model.hpp"
#include "image_builder.hpp"

#include "smply/bytes.hpp"
#include "smply/groups/image.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using smply::ConstBytes;
using smply::ImageError;
using smply::boot_model::ApplyOutcome;
using smply::boot_model::BootConfig;
using smply::boot_model::BootModel;
using smply::boot_model::CommitOutcome;
using smply::boot_model::DeviceCommit;
using smply::boot_model::SlotEntry;
using smply::boot_model::SwapType;
using smply::test::digest_of;
using smply::test::ImageBuilder;

namespace Catch {
template<>
struct StringMaker<ImageError>
{
    static std::string convert(ImageError code)
    {
        return "ImageError(" + std::to_string(static_cast<unsigned>(code)) + ")";
    }
};

template<>
struct StringMaker<SwapType>
{
    static std::string convert(SwapType swap)
    {
        switch (swap) {
        case SwapType::None:
            return "None";
        case SwapType::Test:
            return "Test";
        case SwapType::Perm:
            return "Perm";
        case SwapType::Revert:
            return "Revert";
        }
        return "?";
    }
};
} // namespace Catch

namespace {

constexpr std::uint16_t kTlvSha256 = 0x10;

/// A valid image at version `major.0.0` whose SHA-256 TLV is 32 bytes
/// starting at \p seed, so each image's hash is distinct.
[[nodiscard]] std::vector<std::byte> image(std::uint8_t major, std::uint8_t seed)
{
    return ImageBuilder{}.version(major, 0, 0, 0).tlv(kTlvSha256, digest_of(32, seed)).build();
}

[[nodiscard]] std::vector<std::byte> hash(std::uint8_t seed)
{
    return digest_of(32, seed);
}

/// The flags of one listing entry, so an expectation reads like the S14 table.
struct Flags
{
    bool pending = false;
    bool confirmed = false;
    bool active = false;
    bool permanent = false;

    friend bool operator==(const Flags&, const Flags&) = default;
};

[[nodiscard]] Flags flags_of(const SlotEntry& entry)
{
    return Flags{entry.pending, entry.confirmed, entry.active, entry.permanent};
}

/// The listing entry for \p image's \p slot, or nullopt when it is not listed.
[[nodiscard]] std::optional<SlotEntry> entry_for(const std::vector<SlotEntry>& listing,
                                                 std::uint32_t image, std::size_t slot)
{
    for (const SlotEntry& entry : listing) {
        if (entry.image == image && entry.slot == slot) {
            return entry;
        }
    }
    return std::nullopt;
}

/// The version string a listed slot reports, or "" when it is not listed.
[[nodiscard]] std::string version_in(const BootModel& model, std::uint32_t image, std::size_t slot)
{
    const std::optional<SlotEntry> entry = entry_for(model.listing(), image, slot);
    return entry.has_value() ? entry->content.version : std::string{};
}

/// One image: v1 running in slot 0 (hash seed 1), v2 uploaded to slot 1
/// (hash seed 2).
[[nodiscard]] BootModel one_image(BootConfig config = {})
{
    BootModel model{std::move(config)};
    model.load_slot(0, image(1, 1));
    model.load_slot(1, image(2, 2));
    return model;
}

} // namespace

namespace Catch {
template<>
struct StringMaker<Flags>
{
    static std::string convert(const Flags& flags)
    {
        return std::string{"{"} + (flags.pending ? "pending " : "") +
               (flags.confirmed ? "confirmed " : "") + (flags.active ? "active " : "") +
               (flags.permanent ? "permanent" : "") + "}";
    }
};
} // namespace Catch

// --- The flag table (S14) ----------------------------------------------------

TEST_CASE("each swap type reports the flags of protocol-notes' S14 table", "[boot-model][flags]")
{
    struct Row
    {
        const char* name;
        SwapType swap;
        Flags active;
        Flags other;
    };

    // protocol-notes section 7, "Slot flags are derived, not stored".
    const Row row = GENERATE(
        Row{"None", SwapType::None, {false, true, true, false}, {false, false, false, false}},
        Row{"Test", SwapType::Test, {false, true, true, false}, {true, false, false, false}},
        Row{"Perm", SwapType::Perm, {false, true, true, false}, {true, false, false, true}},
        Row{"Revert", SwapType::Revert, {false, false, true, false}, {false, true, false, false}});
    INFO(row.name);

    BootModel model = one_image(BootConfig{.allow_confirm_non_active_slot = true});
    switch (row.swap) {
    case SwapType::None:
        break;
    case SwapType::Test:
        REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
        break;
    case SwapType::Perm:
        REQUIRE(model.set_state(ConstBytes{hash(2)}, true) == ImageError::Ok);
        break;
    case SwapType::Revert:
        REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
        model.reboot();
        break;
    }
    REQUIRE(model.swap_type() == row.swap);

    const std::vector<SlotEntry> listing = model.listing();
    REQUIRE(listing.size() == 2);
    CHECK(flags_of(listing[0]) == row.active);
    CHECK(flags_of(listing[1]) == row.other);
}

// --- Reboot transitions (protocol-notes section 7) ---------------------------

TEST_CASE("a reboot with nothing scheduled changes nothing", "[boot-model][reboot]")
{
    BootModel model = one_image();
    model.reboot();
    CHECK(model.swap_type() == SwapType::None);
    CHECK(version_in(model, 0, 0) == "1.0.0");
    CHECK(version_in(model, 0, 1) == "2.0.0");
}

TEST_CASE("a reboot after a test swaps and leaves the trial to revert", "[boot-model][reboot]")
{
    BootModel model = one_image();
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
    model.reboot();
    CHECK(model.swap_type() == SwapType::Revert);
    CHECK(version_in(model, 0, 0) == "2.0.0");
    CHECK(version_in(model, 0, 1) == "1.0.0");

    SECTION("an unconfirmed trial is swapped back at the next reboot")
    {
        model.reboot();
        CHECK(model.swap_type() == SwapType::None);
        CHECK(version_in(model, 0, 0) == "1.0.0");
        CHECK(version_in(model, 0, 1) == "2.0.0");
    }
    SECTION("a confirmed trial stays")
    {
        REQUIRE(model.set_state(std::nullopt, true) == ImageError::Ok);
        CHECK(model.swap_type() == SwapType::None);
        model.reboot();
        CHECK(version_in(model, 0, 0) == "2.0.0");
    }
}

TEST_CASE("a reboot after a permanent mark swaps for good", "[boot-model][reboot]")
{
    BootModel model = one_image(BootConfig{.allow_confirm_non_active_slot = true});
    REQUIRE(model.set_state(ConstBytes{hash(2)}, true) == ImageError::Ok);
    model.reboot();
    CHECK(model.swap_type() == SwapType::None);
    CHECK(version_in(model, 0, 0) == "2.0.0");
    model.reboot();
    CHECK(version_in(model, 0, 0) == "2.0.0");
}

TEST_CASE("every image's swap runs at the same boot", "[boot-model][reboot][multi-image]")
{
    BootModel model{BootConfig{.image_count = 2}};
    model.load_slot(0, image(1, 1));
    model.load_slot(1, image(2, 2));
    model.load_slot(2, image(3, 3));
    model.load_slot(3, image(4, 4));
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
    REQUIRE(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    model.reboot();
    CHECK(model.swap_type(0) == SwapType::Revert);
    CHECK(model.swap_type(1) == SwapType::Revert);
    CHECK(version_in(model, 0, 0) == "2.0.0");
    CHECK(version_in(model, 1, 0) == "4.0.0");
}

// --- Set-state (S14, S35) ----------------------------------------------------

TEST_CASE("set-state refuses what img_mgmt_set_next_boot_slot() refuses", "[boot-model][set-state]")
{
    BootModel model = one_image();

    SECTION("a hash that names no slot is not found")
    {
        CHECK(model.set_state(ConstBytes{hash(99)}, false) == ImageError::HashNotFound);
    }
    SECTION("a test with no hash names nothing")
    {
        CHECK(model.set_state(std::nullopt, false) == ImageError::InvalidHash);
    }
    SECTION("marking the running slot for test is denied")
    {
        CHECK(model.set_state(ConstBytes{hash(1)}, false) ==
              ImageError::ImageSettingTestToActiveDenied);
    }
    SECTION("confirming a slot that is not the running one is denied by default")
    {
        CHECK(model.set_state(ConstBytes{hash(2)}, true) == ImageError::ImageConfirmationDenied);
        CHECK(model.swap_type() == SwapType::None);
    }
    SECTION("confirming the running slot with nothing scheduled succeeds and changes nothing")
    {
        CHECK(model.set_state(std::nullopt, true) == ImageError::Ok);
        CHECK(model.set_state(ConstBytes{hash(1)}, true) == ImageError::Ok);
        CHECK(model.swap_type() == SwapType::None);
    }
}

TEST_CASE("while a swap is scheduled only the same request succeeds", "[boot-model][set-state]")
{
    BootModel model = one_image();
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);

    SECTION("re-requesting the scheduled test is an idempotent success")
    {
        CHECK(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
        CHECK(model.swap_type() == SwapType::Test);
    }
    SECTION("confirming the running slot while a test is scheduled is already pending")
    {
        CHECK(model.set_state(std::nullopt, true) == ImageError::ImageAlreadyPending);
        CHECK(model.swap_type() == SwapType::Test);
    }
}

TEST_CASE("during a trial a test is already pending and a confirm ends the trial",
          "[boot-model][set-state]")
{
    BootModel model = one_image();
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
    model.reboot();
    REQUIRE(model.swap_type() == SwapType::Revert);

    // The fallback image now sits in slot 1 and carries hash 1.
    CHECK(model.set_state(ConstBytes{hash(1)}, false) == ImageError::ImageAlreadyPending);
    CHECK(model.set_state(ConstBytes{hash(2)}, true) == ImageError::Ok);
    CHECK(model.swap_type() == SwapType::None);
}

TEST_CASE("a hash is found in any image's slots", "[boot-model][set-state][multi-image]")
{
    BootModel model{BootConfig{.image_count = 2}};
    model.load_slot(0, image(1, 1));
    model.load_slot(2, image(3, 3));
    model.load_slot(3, image(4, 4));

    CHECK(model.find_by_hash(ConstBytes{hash(4)}) == std::optional<std::size_t>{3});
    CHECK(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    CHECK(model.swap_type(1) == SwapType::Test);
    CHECK(model.swap_type(0) == SwapType::None);
}

TEST_CASE("a hashless confirm names image 0 and never image 1",
          "[boot-model][set-state][multi-image]")
{
    BootModel model{BootConfig{.image_count = 2}};
    model.load_slot(0, image(1, 1));
    model.load_slot(1, image(2, 2));
    model.load_slot(2, image(3, 3));
    model.load_slot(3, image(4, 4));
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
    REQUIRE(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    model.reboot();

    CHECK(model.set_state(std::nullopt, true) == ImageError::Ok);
    CHECK(model.swap_type(0) == SwapType::None);
    CHECK(model.swap_type(1) == SwapType::Revert);
}

// --- A27: confirming an image that is not running -----------------------------

TEST_CASE("a confirm of image 1 is denied unless a Kconfig allows it, each relaxing only its rule",
          "[boot-model][set-state][multi-image][A27]")
{
    struct Row
    {
        const char* name;
        BootConfig config;
        ImageError primary;   ///< Confirming image 1's active slot (slot 0).
        ImageError secondary; ///< Confirming image 1's other slot (slot 1).
    };

    const Row row = GENERATE(
        Row{"defaults", BootConfig{.image_count = 2}, ImageError::ImageConfirmationDenied,
            ImageError::ImageConfirmationDenied},
        Row{"NON_ACTIVE_IMAGE_ANY",
            BootConfig{.image_count = 2, .allow_confirm_non_active_image_any = true},
            ImageError::Ok, ImageError::ImageConfirmationDenied},
        Row{"NON_ACTIVE_IMAGE_SECONDARY",
            BootConfig{.image_count = 2, .allow_confirm_non_active_image_secondary = true},
            ImageError::ImageConfirmationDenied, ImageError::ImageConfirmationDenied},
        Row{"NON_ACTIVE_SLOT", BootConfig{.image_count = 2, .allow_confirm_non_active_slot = true},
            ImageError::ImageConfirmationDenied, ImageError::ImageConfirmationDenied},
        Row{"NON_ACTIVE_IMAGE_SECONDARY and NON_ACTIVE_SLOT",
            BootConfig{.image_count = 2,
                       .allow_confirm_non_active_image_secondary = true,
                       .allow_confirm_non_active_slot = true},
            ImageError::ImageConfirmationDenied, ImageError::Ok},
        Row{"NON_ACTIVE_IMAGE_ANY and NON_ACTIVE_SLOT",
            BootConfig{.image_count = 2,
                       .allow_confirm_non_active_image_any = true,
                       .allow_confirm_non_active_slot = true},
            ImageError::Ok, ImageError::Ok});
    INFO(row.name);

    BootModel model{row.config};
    model.load_slot(0, image(1, 1));
    model.load_slot(2, image(3, 3));
    model.load_slot(3, image(4, 4));

    CHECK(model.set_state(ConstBytes{hash(3)}, true) == row.primary);
    CHECK(model.set_state(ConstBytes{hash(4)}, true) == row.secondary);
}

TEST_CASE("A27 is about confirms: a test of image 1's active slot meets the test rule",
          "[boot-model][set-state][A27]")
{
    // The image rule applies to a confirm only (S35); a test of any image's
    // active slot is refused by the test-to-active rule that follows it.
    BootModel model{BootConfig{.image_count = 2}};
    model.load_slot(0, image(1, 1));
    model.load_slot(2, image(3, 3));
    CHECK(model.set_state(ConstBytes{hash(3)}, false) ==
          ImageError::ImageSettingTestToActiveDenied);
}

// --- The upload's flash (S36, protocol-notes section 6) -----------------------

TEST_CASE("an upload of image n goes to its secondary, and a missing image has no slot",
          "[boot-model][upload][multi-image]")
{
    const BootModel model{BootConfig{.image_count = 2}};
    CHECK(model.upload_target(0) == std::optional<std::size_t>{1});
    CHECK(model.upload_target(1) == std::optional<std::size_t>{3});
    CHECK_FALSE(model.upload_target(2).has_value());
    CHECK_FALSE(model.upload_target(UINT64_MAX).has_value());
}

TEST_CASE("starting an upload erases the target, and writes land where they are put",
          "[boot-model][upload]")
{
    BootModel model = one_image();
    const std::vector<std::byte> incoming = image(3, 3);

    model.begin_upload(1, incoming.size());
    // The erase took the old image: erased flash is 0xFF and lists nothing.
    const ConstBytes erased = model.slot_content(1);
    REQUIRE(erased.size() == incoming.size());
    CHECK(std::all_of(erased.begin(), erased.end(),
                      [](std::byte b) { return b == std::byte{0xFF}; }));
    CHECK_FALSE(entry_for(model.listing(), 0, 1).has_value());

    const ConstBytes all{incoming};
    const std::size_t half = incoming.size() / 2;
    CHECK(model.write(1, 0, all.first(half)));
    CHECK(model.write(1, half, all.subspan(half)));
    CHECK(version_in(model, 0, 1) == "3.0.0");
    CHECK(model.find_by_hash(ConstBytes{hash(3)}) == std::optional<std::size_t>{1});
}

TEST_CASE("a write past the erased area is refused and writes nothing", "[boot-model][upload]")
{
    BootModel model = one_image();
    model.begin_upload(1, 8);
    const std::vector<std::byte> data(4, std::byte{0x11});
    CHECK_FALSE(model.write(1, 6, ConstBytes{data}));
    CHECK_FALSE(model.write(1, SIZE_MAX, ConstBytes{data}));
    const ConstBytes slot = model.slot_content(1);
    CHECK(std::all_of(slot.begin(), slot.end(), [](std::byte b) { return b == std::byte{0xFF}; }));
    CHECK(model.write(1, 4, ConstBytes{data}));
}

// --- Erase (protocol-notes section 6, S10) ------------------------------------

TEST_CASE("erase empties a slot, refuses one marked for the next boot, and one that does not exist",
          "[boot-model][erase]")
{
    BootModel model = one_image();

    SECTION("a slot that does not exist")
    {
        CHECK(model.erase(2) == ImageError::InvalidSlot);
        CHECK(model.erase(UINT64_MAX) == ImageError::InvalidSlot);
    }
    SECTION("an unmarked secondary")
    {
        CHECK(model.erase(1) == ImageError::Ok);
        CHECK(model.slot_content(1).empty());
        CHECK(model.listing().size() == 1);
    }
    SECTION("a secondary marked for test")
    {
        REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
        CHECK(model.erase(1) == ImageError::NoFreeSlot);
        CHECK(version_in(model, 0, 1) == "2.0.0");
    }
    SECTION("the fallback of a trial, which the revert needs")
    {
        REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
        model.reboot();
        CHECK(model.erase(1) == ImageError::NoFreeSlot);
    }
}

// --- The image reader (S14) ---------------------------------------------------

TEST_CASE("the listing skips invalid content and reports what a header says",
          "[boot-model][listing]")
{
    BootModel model;

    SECTION("an erased or garbage slot is not listed")
    {
        model.load_slot(0, image(1, 1));
        model.load_slot(1, std::vector<std::byte>(64, std::byte{0xFF}));
        const std::vector<SlotEntry> listing = model.listing();
        REQUIRE(listing.size() == 1);
        CHECK(listing[0].slot == 0);

        model.load_slot(0, {});
        CHECK(model.listing().empty());
    }
    SECTION("the version carries the build number only when it is not zero")
    {
        model.load_slot(0, ImageBuilder{}.version(1, 2, 3, 0).build());
        model.load_slot(1, ImageBuilder{}.version(1, 2, 3, 4).build());
        CHECK(version_in(model, 0, 0) == "1.2.3");
        CHECK(version_in(model, 0, 1) == "1.2.3.4");
    }
    SECTION("a non-bootable header is reported not bootable")
    {
        constexpr std::uint32_t kNonBootable = 0x10;
        model.load_slot(0, ImageBuilder{}.flags(kNonBootable).build());
        REQUIRE(model.listing().size() == 1);
        CHECK_FALSE(model.listing()[0].content.bootable);
    }
    SECTION("the hash is the unprotected area's SHA-256, after a protected area")
    {
        model.load_slot(0, ImageBuilder{}
                               .protected_tlv(kTlvSha256, digest_of(32, 50))
                               .tlv(kTlvSha256, digest_of(32, 60))
                               .build());
        REQUIRE(model.listing().size() == 1);
        CHECK(model.listing()[0].content.hash == digest_of(32, 60));
    }
}

// --- Device-committed images (ADR-0022, S39) ----------------------------------

namespace {

/// Two images, image 1 committed by the device: image 0 runs v1 (hash 1),
/// image 1 runs v3 (hash 3) and has v4 (hash 4) staged.
[[nodiscard]] BootModel dual_mcu(const DeviceCommit& how, bool image0_staged = false)
{
    BootModel model{BootConfig{.image_count = 2, .device_commits = {std::nullopt, how}}};
    model.load_slot(0, image(1, 1));
    if (image0_staged) {
        model.load_slot(1, image(2, 2));
    }
    model.load_slot(2, image(3, 3));
    model.load_slot(3, image(4, 4));
    return model;
}

/// Reads the state \p count times, as a client polling would.
void read_times(BootModel& model, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        static_cast<void>(model.read_state());
    }
}

} // namespace

TEST_CASE("a device-committed image is applied over state reads, not swapped at the reboot",
          "[boot-model][device-commit]")
{
    BootModel model = dual_mcu(DeviceCommit{.apply_reads = 2});
    REQUIRE(model.device_committed(1));
    REQUIRE_FALSE(model.device_committed(0));
    REQUIRE(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    model.reboot();

    // Still the old image in slot 0, the new one pending in slot 1.
    CHECK(model.swap_type(1) == SwapType::Test);
    CHECK(version_in(model, 1, 0) == "3.0.0");

    SECTION("the plain listing never advances the apply")
    {
        for (int i = 0; i < 5; ++i) {
            CHECK(model.listing().size() == 3);
        }
        CHECK(model.swap_type(1) == SwapType::Test);
    }
    SECTION("apply_reads reads report it in progress, and the next one reports it applied")
    {
        read_times(model, 2);
        CHECK(model.swap_type(1) == SwapType::Test);
        const std::vector<SlotEntry> listing = model.read_state();
        CHECK(model.swap_type(1) == SwapType::Revert);
        const std::optional<SlotEntry> running = entry_for(listing, 1, 0);
        REQUIRE(running.has_value());
        CHECK(running->content.version == "4.0.0");
        CHECK(flags_of(*running) == Flags{.active = true});
    }
}

TEST_CASE("with image 0 not on trial the device commits at once", "[boot-model][device-commit]")
{
    BootModel model = dual_mcu(DeviceCommit{.apply_reads = 1, .commit_reads = 1});
    REQUIRE(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    model.reboot();

    read_times(model, 2); // applied, and the commit starts at the same read
    REQUIRE(model.swap_type(1) == SwapType::Revert);
    read_times(model, 1);
    CHECK(model.swap_type(1) == SwapType::Revert);
    read_times(model, 1);
    CHECK(model.swap_type(1) == SwapType::None);
    CHECK(version_in(model, 1, 0) == "4.0.0");
}

TEST_CASE("with image 0 on trial the device commits only after image 0 is confirmed",
          "[boot-model][device-commit]")
{
    BootModel model = dual_mcu(DeviceCommit{.apply_reads = 1, .commit_reads = 1}, true);
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
    REQUIRE(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    model.reboot();
    REQUIRE(model.swap_type(0) == SwapType::Revert);

    read_times(model, 10);
    CHECK(model.swap_type(1) == SwapType::Revert);

    // The confirm hook of image 0 starts the commit (S39).
    REQUIRE(model.set_state(std::nullopt, true) == ImageError::Ok);
    read_times(model, 1);
    CHECK(model.swap_type(1) == SwapType::Revert);
    read_times(model, 1);
    CHECK(model.swap_type(1) == SwapType::None);
}

TEST_CASE("a failed apply leaves the old image running and nothing pending",
          "[boot-model][device-commit]")
{
    BootModel model = dual_mcu(DeviceCommit{.outcome = ApplyOutcome::Failed, .apply_reads = 1});
    REQUIRE(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    model.reboot();
    read_times(model, 2);

    CHECK(model.swap_type(1) == SwapType::None);
    const std::vector<SlotEntry> listing = model.listing();
    const std::optional<SlotEntry> running = entry_for(listing, 1, 0);
    const std::optional<SlotEntry> staged = entry_for(listing, 1, 1);
    REQUIRE(running.has_value());
    REQUIRE(staged.has_value());
    CHECK(running->content.version == "3.0.0");
    CHECK(flags_of(*staged) == Flags{});
}

TEST_CASE("a device that never commits keeps the image on trial, and a reboot reverts it",
          "[boot-model][device-commit]")
{
    BootModel model{BootConfig{.image_count = 2}};
    model.load_slot(0, image(1, 1));
    model.load_slot(2, image(3, 3));
    model.load_slot(3, image(4, 4));
    // Made device-committed after construction, as ServerSimulator does.
    model.device_commits(1, DeviceCommit{.apply_reads = 1, .commit = CommitOutcome::Never});
    REQUIRE(model.device_committed(1));
    REQUIRE(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    model.reboot();

    read_times(model, 10);
    CHECK(model.swap_type(1) == SwapType::Revert);
    CHECK(version_in(model, 1, 0) == "4.0.0");

    model.reboot();
    CHECK(model.swap_type(1) == SwapType::None);
    CHECK(version_in(model, 1, 0) == "3.0.0");
}

// --- The bridge to overwrite and downgrade prevention ----------------------------

TEST_CASE("a swap ended without a trial installs or drops the secondary and erases it",
          "[boot-model][bridge]")
{
    BootModel model = one_image();
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);

    SECTION("installed, as an overwrite does")
    {
        model.end_swap_without_trial(0, true);
        CHECK(version_in(model, 0, 0) == "2.0.0");
    }
    SECTION("dropped, as a refused downgrade does (S41)")
    {
        model.end_swap_without_trial(0, false);
        CHECK(version_in(model, 0, 0) == "1.0.0");
    }
    CHECK(model.swap_type() == SwapType::None);
    CHECK(model.slot_content(1).empty());
    REQUIRE(model.listing().size() == 1);
    CHECK(flags_of(model.listing()[0]) == Flags{.confirmed = true, .active = true});
}
