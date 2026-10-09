// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_BOOTLOADER_MODE_HPP
#define SMPLY_DFU_APP_BOOTLOADER_MODE_HPP

/// \file
/// The MCUboot mode on a DFU tool's command line, and in its report.
///
/// `--fallback-mode` names a mode the way `to_string(McubootMode)` does, and
/// every example tool prints where the mode an update ran under came from
/// (ADR-0025). Three tools need the same two functions, so they are here rather
/// than copied three times.

#include "smply/dfu/firmware_updater.hpp"
#include "smply/groups/os.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace smply::dfu_app {

/// Every mode a caller can name, in MCUboot's order. `Unknown` is not one:
/// supplying "unknown" means supplying nothing.
inline constexpr std::array<McubootMode, 10> kNamedModes{
    McubootMode::SingleSlot,     McubootMode::SwapUsingScratch, McubootMode::UpgradeOnly,
    McubootMode::SwapUsingMove,  McubootMode::DirectXip,        McubootMode::DirectXipWithRevert,
    McubootMode::RamLoad,        McubootMode::FirmwareLoader,   McubootMode::SingleSlotRamLoad,
    McubootMode::SwapUsingOffset};

/// The mode \p text names, such as `"swap-using-move"`, or nothing.
[[nodiscard]] inline std::optional<McubootMode> parse_mcuboot_mode(std::string_view text) noexcept
{
    for (const McubootMode mode : kNamedModes) {
        if (to_string(mode) == text) {
            return mode;
        }
    }
    return std::nullopt;
}

/// Where the report's mode came from, in a few words.
[[nodiscard]] inline std::string_view describe(ModeSource source) noexcept
{
    switch (source) {
    case ModeSource::Reported:
        return "reported by the device";
    case ModeSource::Supplied:
        return "not reported; the fallback was used";
    case ModeSource::Assumed:
        return "not reported; assumed to swap with revert";
    }
    return "unknown"; // LCOV_EXCL_LINE -- every enumerator is handled above
}

/// One line for a tool's output: `"bootloader: swap-using-move (reported by
/// the device)"`.
[[nodiscard]] inline std::string describe_mode(const UpdateReport& report)
{
    std::string line{"bootloader: "};
    line += to_string(report.bootloader_mode);
    line += " (";
    line += describe(report.mode_source);
    line += ')';
    return line;
}

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_BOOTLOADER_MODE_HPP
