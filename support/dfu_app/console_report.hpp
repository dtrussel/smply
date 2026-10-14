// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_CONSOLE_REPORT_HPP
#define SMPLY_DFU_APP_CONSOLE_REPORT_HPP

/// \file
/// An update's report, as the example tools print it.
///
/// A failed update has still changed the device: some images may be staged,
/// one may be running on trial, and the next reset may revert it. Every tool
/// says so the same way, so the lines are here rather than copied, where a
/// copy that left one out would tell its user nothing had changed.

#include "smply/dfu/firmware_updater.hpp"

#include <ostream>
#include <string_view>

namespace smply::dfu_app {

/// How far the device took an image it commits itself (ADR-0022):
/// `"committed"`, `"applied, not committed"` or `"not applied"`.
[[nodiscard]] std::string_view device_image_state(const ImageReport& image) noexcept;

/// One line per image of a multi-image update, each indented by two spaces.
/// Nothing for an update of one image, whose report says it all.
void print_images(const UpdateReport& report, std::ostream& out);

/// What a failed update left behind: the bootloader mode, each image's line,
/// and a warning when a swap is scheduled but unconfirmed, which the next
/// reset reverts. Printed after the failure itself, on the same stream, so
/// the lines keep their order.
void print_failure_details(const UpdateReport& report, std::ostream& out);

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_CONSOLE_REPORT_HPP
