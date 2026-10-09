// SPDX-License-Identifier: Apache-2.0
//
// The tools' `--fallback-mode` names and their report line (ADR-0025). Every
// mode MCUboot defines must be nameable, under exactly the name smply prints
// for it, or a tool would print a mode that cannot be typed back in.

#include "dfu_app/bootloader_mode.hpp"

#include "smply/dfu/firmware_updater.hpp"
#include "smply/groups/os.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

using smply::McubootMode;
using smply::ModeSource;
using smply::UpdateReport;
using smply::dfu_app::describe_mode;
using smply::dfu_app::kNamedModes;
using smply::dfu_app::parse_mcuboot_mode;

TEST_CASE("every MCUboot mode round-trips through its printed name", "[dfu_app][mode]")
{
    std::int64_t expected = 0;
    for (const McubootMode mode : kNamedModes) {
        // In MCUboot's order, so the list cannot silently skip one.
        CHECK(static_cast<std::int64_t>(mode) == expected);
        ++expected;
        CHECK(parse_mcuboot_mode(smply::to_string(mode)) == mode);
    }
    CHECK(expected == 10);
}

TEST_CASE("unknown and unrecognised names are not a mode", "[dfu_app][mode]")
{
    CHECK_FALSE(parse_mcuboot_mode("unknown").has_value());
    CHECK_FALSE(parse_mcuboot_mode("").has_value());
    CHECK_FALSE(parse_mcuboot_mode("Swap-Using-Move").has_value());
    CHECK_FALSE(parse_mcuboot_mode("3").has_value());
}

TEST_CASE("the report line says where the mode came from", "[dfu_app][mode]")
{
    UpdateReport report;
    CHECK(describe_mode(report) ==
          std::string{"bootloader: unknown (not reported; assumed to swap with revert)"});

    report.bootloader_mode = McubootMode::SwapUsingMove;
    report.mode_source = ModeSource::Reported;
    CHECK(describe_mode(report) ==
          std::string{"bootloader: swap-using-move (reported by the device)"});

    report.bootloader_mode = McubootMode::UpgradeOnly;
    report.mode_source = ModeSource::Supplied;
    CHECK(describe_mode(report) ==
          std::string{"bootloader: upgrade-only (not reported; the fallback was used)"});
}
