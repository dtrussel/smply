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
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using smply::ConstBytes;
using smply::ImageError;
using smply::boot_model::ApplyOutcome;
using smply::boot_model::behaviour_for_mode;
using smply::boot_model::BootBehaviour;
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

// --- A slot the device does not have ------------------------------------------

TEST_CASE("a slot the device does not have is refused in every build", "[boot-model][flash]")
{
    // Not an assert: a release build must refuse it too.
    BootModel model{BootConfig{.image_count = 2}};
    CHECK_THROWS_AS(model.load_slot(4, {}), std::out_of_range);
    CHECK_THROWS_AS(model.slot_content(4), std::out_of_range);
    CHECK_THROWS_AS(model.begin_upload(4, 32), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(model.write(4, 0, ConstBytes{})), std::out_of_range);
    CHECK_THROWS_AS(model.boot_from_slot(2), std::out_of_range);
    CHECK_NOTHROW(model.slot_content(3));
}

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
    model.set_device_commit(1, DeviceCommit{.apply_reads = 1, .commit = CommitOutcome::Never});
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

// --- The boot behaviour (protocol-notes section 7, "MCUboot modes") ----------

TEST_CASE("a reported mode number picks the behaviour, anything unmodelled swapping with revert",
          "[boot-model][behaviour]")
{
    struct Row
    {
        std::optional<std::int64_t> reported;
        BootBehaviour behaviour;
    };

    // S56's enum: 2 is upgrade-only, 4 and 5 are direct-XIP without and with
    // revert. Every swap mode (1, 3, 9), a mode the model does not run (0, 6,
    // 7, 8), Zephyr's -1, a number past the enum and no report at all are
    // modelled as swap with revert.
    const Row row = GENERATE(Row{std::int64_t{2}, BootBehaviour::Overwrite},
                             Row{std::int64_t{4}, BootBehaviour::DirectXip},
                             Row{std::int64_t{5}, BootBehaviour::DirectXipWithRevert},
                             Row{std::int64_t{1}, BootBehaviour::SwapWithRevert},
                             Row{std::int64_t{3}, BootBehaviour::SwapWithRevert},
                             Row{std::int64_t{9}, BootBehaviour::SwapWithRevert},
                             Row{std::int64_t{0}, BootBehaviour::SwapWithRevert},
                             Row{std::int64_t{6}, BootBehaviour::SwapWithRevert},
                             Row{std::int64_t{7}, BootBehaviour::SwapWithRevert},
                             Row{std::int64_t{8}, BootBehaviour::SwapWithRevert},
                             Row{std::int64_t{-1}, BootBehaviour::SwapWithRevert},
                             Row{std::int64_t{10}, BootBehaviour::SwapWithRevert},
                             Row{std::nullopt, BootBehaviour::SwapWithRevert});
    INFO(row.reported.value_or(-99));
    CHECK(behaviour_for_mode(row.reported) == row.behaviour);
}

TEST_CASE("set-state exists in every behaviour but plain direct-XIP", "[boot-model][behaviour]")
{
    // S10: the write handler is NULL under DIRECT_XIP; the with-revert
    // variant keeps it (S14).
    CHECK(BootModel{BootConfig{.behaviour = BootBehaviour::SwapWithRevert}}.supports_set_state());
    CHECK(BootModel{BootConfig{.behaviour = BootBehaviour::Overwrite}}.supports_set_state());
    CHECK(BootModel{BootConfig{.behaviour = BootBehaviour::DirectXipWithRevert}}
              .supports_set_state());
    CHECK_FALSE(BootModel{BootConfig{.behaviour = BootBehaviour::DirectXip}}.supports_set_state());
}

// --- Overwrite (protocol-notes section 7, "MCUboot modes") --------------------

TEST_CASE("an overwrite installs a scheduled image for good, with no trial and nothing to revert",
          "[boot-model][overwrite]")
{
    // Mode 2, upgrade-only: set-state works, but a test is a permanent copy.
    BootModel model = one_image(
        BootConfig{.behaviour = BootBehaviour::Overwrite, .allow_confirm_non_active_slot = true});
    const bool confirm = GENERATE(false, true);
    INFO((confirm ? "marked permanent" : "marked for test"));
    REQUIRE(model.set_state(ConstBytes{hash(2)}, confirm) == ImageError::Ok);

    model.reboot();
    CHECK(model.swap_type() == SwapType::None);
    CHECK(version_in(model, 0, 0) == "2.0.0");
    // The secondary is consumed: erased, so it lists nothing.
    CHECK(model.slot_content(1).empty());
    const std::vector<SlotEntry> listing = model.listing();
    REQUIRE(listing.size() == 1);
    CHECK(flags_of(listing[0]) == Flags{.confirmed = true, .active = true});

    // Nothing reverts at the next boot.
    model.reboot();
    CHECK(version_in(model, 0, 0) == "2.0.0");
}

TEST_CASE("an overwrite device still applies an image it commits itself", "[boot-model][overwrite]")
{
    // A device-committed image is not this MCU's to boot (ADR-0022).
    BootModel model{BootConfig{.behaviour = BootBehaviour::Overwrite, .image_count = 2}};
    model.load_slot(0, image(1, 1));
    model.load_slot(2, image(3, 3));
    model.load_slot(3, image(4, 4));
    model.set_device_commit(1, DeviceCommit{});
    REQUIRE(model.set_state(ConstBytes{hash(4)}, false) == ImageError::Ok);
    model.reboot();
    CHECK(model.swap_type(1) == SwapType::Test);
    CHECK(version_in(model, 1, 0) == "3.0.0");
    CHECK(version_in(model, 1, 1) == "4.0.0");
}

// --- Downgrade prevention (S41, protocol-notes section 7) -------------------

TEST_CASE("with no_downgrade an older scheduled image is erased and the running one stays",
          "[boot-model][downgrade]")
{
    const BootBehaviour behaviour =
        GENERATE(BootBehaviour::SwapWithRevert, BootBehaviour::Overwrite);
    INFO((behaviour == BootBehaviour::Overwrite ? "overwrite" : "swap"));
    BootModel model{BootConfig{.behaviour = behaviour, .no_downgrade = true}};
    model.load_slot(0, image(2, 2));
    model.load_slot(1, image(1, 1));
    REQUIRE(model.set_state(ConstBytes{hash(1)}, false) == ImageError::Ok);

    model.reboot();
    CHECK(model.swap_type() == SwapType::None);
    CHECK(version_in(model, 0, 0) == "2.0.0");
    CHECK(model.slot_content(1).empty());
}

TEST_CASE("downgrade prevention compares major, minor and revision only, and accepts an equal one",
          "[boot-model][downgrade]")
{
    // boot_compare_version() (S41): the build number does not count, so 1.2.3+9
    // over 1.2.3+1 is equal, and an equal version is accepted.
    BootModel model{BootConfig{.no_downgrade = true}};
    model.load_slot(0,
                    ImageBuilder{}.version(1, 2, 3, 9).tlv(kTlvSha256, digest_of(32, 1)).build());
    model.load_slot(1,
                    ImageBuilder{}.version(1, 2, 3, 1).tlv(kTlvSha256, digest_of(32, 2)).build());
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);

    model.reboot();
    CHECK(model.swap_type() == SwapType::Revert);
    CHECK(version_in(model, 0, 0) == "1.2.3.1");
}

TEST_CASE("without no_downgrade an older image swaps in like any other", "[boot-model][downgrade]")
{
    BootModel model;
    model.load_slot(0, image(2, 2));
    model.load_slot(1, image(1, 1));
    REQUIRE(model.set_state(ConstBytes{hash(1)}, false) == ImageError::Ok);
    model.reboot();
    CHECK(model.swap_type() == SwapType::Revert);
    CHECK(version_in(model, 0, 0) == "1.0.0");
}

// --- Direct-XIP (S10, S14, protocol-notes section 7) -------------------------

namespace {

/// A direct-XIP device running from slot 0, with \p first in slot 0 and
/// \p second in slot 1.
[[nodiscard]] BootModel xip(BootBehaviour behaviour, std::vector<std::byte> first,
                            std::vector<std::byte> second, BootConfig config = {})
{
    config.behaviour = behaviour;
    BootModel model{std::move(config)};
    model.load_slot(0, std::move(first));
    model.load_slot(1, std::move(second));
    return model;
}

/// The flags image 0's \p slot reports, or nullopt when it is not listed.
[[nodiscard]] std::optional<Flags> flags_at(const BootModel& model, std::size_t slot)
{
    const std::optional<SlotEntry> entry = entry_for(model.listing(), 0, slot);
    return entry.has_value() ? std::optional<Flags>{flags_of(*entry)} : std::nullopt;
}

constexpr std::uint32_t kImageFlagNonBootable = 0x10;

} // namespace

TEST_CASE("plain direct-XIP boots the newer slot, reported pending and permanent, never confirmed",
          "[boot-model][direct-xip]")
{
    BootModel model = xip(BootBehaviour::DirectXip, image(1, 1), image(2, 2));
    CHECK(model.active_slot() == 0);
    // Without revert the running slot is only active (S14).
    CHECK(flags_at(model, 0) == Flags{.active = true});
    CHECK(flags_at(model, 1) == Flags{.pending = true, .permanent = true});

    model.reboot();
    CHECK(model.active_slot() == 1);
    CHECK(flags_at(model, 1) == Flags{.active = true});
    // The older image in slot 0 is held but never booted.
    CHECK(flags_at(model, 0) == Flags{});
    model.reboot();
    CHECK(model.active_slot() == 1);
}

TEST_CASE("direct-XIP breaks a version tie towards the lower slot", "[boot-model][direct-xip]")
{
    SECTION("running the higher slot, an equal image in the lower one boots")
    {
        BootModel model = xip(BootBehaviour::DirectXip, image(2, 1), image(2, 2));
        model.boot_from_slot(1);
        CHECK(flags_at(model, 0) == Flags{.pending = true, .permanent = true});
        model.reboot();
        CHECK(model.active_slot() == 0);
    }
    SECTION("running the lower slot, an equal image in the higher one does not")
    {
        BootModel model = xip(BootBehaviour::DirectXip, image(2, 1), image(2, 2));
        CHECK(flags_at(model, 1) == Flags{});
        model.reboot();
        CHECK(model.active_slot() == 0);
    }
}

TEST_CASE("direct-XIP never boots an older, invalid or non-bootable image",
          "[boot-model][direct-xip]")
{
    struct Row
    {
        const char* name;
        std::vector<std::byte> other;
    };

    const Row row =
        GENERATE(Row{"older", image(0, 2)}, Row{"erased", {}},
                 Row{"not an image", ImageBuilder{}.magic(0x12345678).version(9, 0, 0, 0).build()},
                 Row{"non-bootable", ImageBuilder{}
                                         .version(9, 0, 0, 0)
                                         .flags(kImageFlagNonBootable)
                                         .tlv(kTlvSha256, digest_of(32, 2))
                                         .build()});
    INFO(row.name);
    BootModel model = xip(BootBehaviour::DirectXip, image(1, 1), row.other);
    const std::optional<Flags> other = flags_at(model, 1);
    CHECK((!other.has_value() || !other->pending));
    model.reboot();
    CHECK(model.active_slot() == 0);
}

TEST_CASE("direct-XIP leaves a running slot that holds no valid image for a valid one",
          "[boot-model][direct-xip]")
{
    BootModel model = xip(BootBehaviour::DirectXip, {}, image(1, 1));
    CHECK(flags_at(model, 1) == Flags{.pending = true, .permanent = true});
    model.reboot();
    CHECK(model.active_slot() == 1);
}

TEST_CASE("a direct-XIP upload goes to the slot not running", "[boot-model][direct-xip][upload]")
{
    // S10: the slot opposite the active one.
    const BootBehaviour behaviour =
        GENERATE(BootBehaviour::DirectXip, BootBehaviour::DirectXipWithRevert);
    BootModel model = xip(behaviour, image(1, 1), image(2, 2));
    CHECK(model.upload_target(0) == std::optional<std::size_t>{1});
    model.boot_from_slot(1);
    CHECK(model.active_slot() == 1);
    CHECK(model.upload_target(0) == std::optional<std::size_t>{0});
    CHECK_FALSE(model.upload_target(1).has_value());
}

TEST_CASE("a direct-XIP device started in a slot runs it confirmed", "[boot-model][direct-xip]")
{
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(2, 1), image(2, 2));
    model.boot_from_slot(1);
    CHECK(model.active_slot() == 1);
    CHECK(flags_at(model, 1) == Flags{.confirmed = true, .active = true});
    // A confirmed running slot is not on trial: the reboot keeps it, even
    // though the equal image in the lower slot would win were it marked.
    model.reboot();
    CHECK(model.active_slot() == 1);
}

TEST_CASE("direct-XIP with revert boots a newer slot only once it is marked",
          "[boot-model][direct-xip][revert]")
{
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(1, 1), image(2, 2));
    // Unmarked: held, not pending, and not booted (S14).
    CHECK(flags_at(model, 0) == Flags{.confirmed = true, .active = true});
    CHECK(flags_at(model, 1) == Flags{});
    model.reboot();
    CHECK(model.active_slot() == 0);
}

TEST_CASE("direct-XIP with revert does not boot a marked image that is not newer",
          "[boot-model][direct-xip][revert]")
{
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(2, 1), image(1, 2));
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
    CHECK(flags_at(model, 1) == Flags{});
    model.reboot();
    CHECK(model.active_slot() == 0);
}

TEST_CASE("an unconfirmed direct-XIP trial is reverted and its slot erased",
          "[boot-model][direct-xip][revert]")
{
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(1, 1), image(2, 2));
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
    // Marked for one boot: the ordinary table's Test row.
    CHECK(flags_at(model, 1) == Flags{.pending = true});

    model.reboot();
    CHECK(model.active_slot() == 1);
    // On trial: the ordinary table's Revert row.
    CHECK(flags_at(model, 1) == Flags{.active = true});
    CHECK(flags_at(model, 0) == Flags{.confirmed = true});
    // Any test during a trial is already pending.
    CHECK(model.set_state(ConstBytes{hash(1)}, false) == ImageError::ImageAlreadyPending);

    model.reboot();
    CHECK(model.active_slot() == 0);
    CHECK(model.slot_content(1).empty());
    CHECK(flags_at(model, 0) == Flags{.confirmed = true, .active = true});
}

TEST_CASE("a confirmed direct-XIP trial stays", "[boot-model][direct-xip][revert]")
{
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(1, 1), image(2, 2));
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
    model.reboot();
    REQUIRE(model.active_slot() == 1);

    // A hashless confirm names the slot running, not slot 0.
    REQUIRE(model.set_state(std::nullopt, true) == ImageError::Ok);
    CHECK(flags_at(model, 1) == Flags{.confirmed = true, .active = true});
    CHECK(flags_at(model, 0) == Flags{});
    model.reboot();
    CHECK(model.active_slot() == 1);
    CHECK(version_in(model, 0, 1) == "2.0.0");
}

TEST_CASE("a direct-XIP slot confirmed before it runs boots for good",
          "[boot-model][direct-xip][revert]")
{
    // Confirming the other slot needs the Kconfig, as anywhere (A27).
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(1, 1), image(2, 2),
                          BootConfig{.allow_confirm_non_active_slot = true});
    REQUIRE(model.set_state(ConstBytes{hash(2)}, true) == ImageError::Ok);
    // The ordinary table's Perm row.
    CHECK(flags_at(model, 1) == Flags{.pending = true, .permanent = true});
    model.reboot();
    CHECK(model.active_slot() == 1);
    CHECK(flags_at(model, 1) == Flags{.confirmed = true, .active = true});
    model.reboot();
    CHECK(model.active_slot() == 1);
}

TEST_CASE("direct-XIP's set-state refuses what img_mgmt_set_next_boot_slot() refuses",
          "[boot-model][direct-xip][set-state]")
{
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(1, 1), image(2, 2));
    model.boot_from_slot(1);

    // Confirming the slot not running (S14, A27).
    CHECK(model.set_state(ConstBytes{hash(1)}, true) == ImageError::ImageConfirmationDenied);
    // Testing the slot running (S14).
    CHECK(model.set_state(ConstBytes{hash(2)}, false) ==
          ImageError::ImageSettingTestToActiveDenied);
    // A hash nowhere on the device.
    CHECK(model.set_state(ConstBytes{hash(9)}, false) == ImageError::HashNotFound);
    // A test with no hash names no image.
    CHECK(model.set_state(std::nullopt, false) == ImageError::InvalidHash);
    // Confirming the slot running is fine, and changes nothing.
    CHECK(model.set_state(std::nullopt, true) == ImageError::Ok);
    CHECK(model.active_slot() == 1);
}

TEST_CASE("a direct-XIP mark repeats, but a confirmed slot cannot be marked for one boot",
          "[boot-model][direct-xip][set-state]")
{
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(1, 1), image(2, 2),
                          BootConfig{.allow_confirm_non_active_slot = true});
    SECTION("marking twice succeeds and does nothing more")
    {
        CHECK(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
        CHECK(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);
        CHECK(flags_at(model, 1) == Flags{.pending = true});
    }
    SECTION("a confirmed slot is already pending for a test")
    {
        REQUIRE(model.set_state(ConstBytes{hash(2)}, true) == ImageError::Ok);
        CHECK(model.set_state(ConstBytes{hash(2)}, false) == ImageError::ImageAlreadyPending);
    }
}

TEST_CASE("plain direct-XIP has no set-state, so a request changes nothing",
          "[boot-model][direct-xip][set-state]")
{
    BootModel model = xip(BootBehaviour::DirectXip, image(1, 1), image(2, 2));
    REQUIRE_FALSE(model.supports_set_state());
    CHECK(model.set_state(ConstBytes{hash(1)}, true) == ImageError::Unknown);
    CHECK(flags_at(model, 0) == Flags{.active = true});
}

TEST_CASE("starting a direct-XIP upload erases the slot's mark with it",
          "[boot-model][direct-xip][upload]")
{
    BootModel model = xip(BootBehaviour::DirectXipWithRevert, image(1, 1), image(2, 2));
    REQUIRE(model.set_state(ConstBytes{hash(2)}, false) == ImageError::Ok);

    // The same image again: only the erase of the trailer can unmark it.
    const std::vector<std::byte> again = image(2, 2);
    model.begin_upload(1, again.size());
    REQUIRE(model.write(1, 0, ConstBytes{again}));
    CHECK(flags_at(model, 1) == Flags{});
    model.reboot();
    CHECK(model.active_slot() == 0);
}

TEST_CASE("downgrade prevention does not apply under direct-XIP", "[boot-model][direct-xip]")
{
    // MCUboot's downgrade prevention depends on !BOOT_DIRECT_XIP (S56), and a
    // direct-XIP device only ever boots the newer slot anyway.
    BootModel model =
        xip(BootBehaviour::DirectXip, image(1, 1), image(2, 2), BootConfig{.no_downgrade = true});
    model.reboot();
    CHECK(model.active_slot() == 1);
    CHECK(version_in(model, 0, 0) == "1.0.0");
}
