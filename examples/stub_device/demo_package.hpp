// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_DEMO_PACKAGE_HPP
#define SMPLY_EXAMPLE_DEMO_PACKAGE_HPP

/// \file
/// Builds a multi-image DFU package, so `--demo-package` runs with no files.
///
/// Scaffolding, like `demo_image.hpp`. The layout is the one nRF Connect SDK's
/// sysbuild writes (docs/protocol-notes.md, S38): a zip whose entries are
/// **stored**, and a `manifest.json` whose `files[]` carry `file`, `size`,
/// `image_index` -- as a string, the way `generate_zip.py` writes it --
/// `version_MCUBOOT` and `board`, the last as S60 describes it.
/// `support/dfu_package/` is what reads it back. The zip is
/// written from APPNOTE.TXT here rather than shared with that reader, so the
/// demo exercises the reader rather than agreeing with it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smply::example {

/// One file of the package.
struct DemoPackageFile
{
    std::string name;
    std::uint32_t image_index = 0;
    /// The dotted version the image's header carries.
    std::string version;
    std::vector<std::byte> content;
    /// A direct-XIP build's global slot. With it the entry is written as
    /// zip.cmake writes a direct-XIP file (S58): `slot`, and the version under
    /// `version_MCUBOOT+XIP`.
    std::optional<std::uint32_t> slot = std::nullopt;
    /// The manifest's `board`: a board name or a board target
    /// (docs/protocol-notes.md section 5). Without it the entry has none.
    std::optional<std::string> board = std::nullopt;
};

/// The board name the demo packages are built for. Invented: no Zephyr board
/// has it, so a demo package is never mistaken for one built for real hardware.
inline constexpr const char* kDemoBoard = "demo_board";

/// A stored zip holding \p files and a manifest describing them.
[[nodiscard]] std::vector<std::byte> build_demo_package(const std::vector<DemoPackageFile>& files);

/// The demo's two-image package, the coordinating-MCU update of
/// docs/multi-image.md: `app.bin` (image 0, 2.0.0) and `radio.bin` (image 1,
/// 6.0.0), for a device running app 1.0.0 and radio `kDemoRadioRunning`.
/// As nRF Connect SDK writes them (S60), image 0's `board` is the board name
/// `kDemoBoard` and image 1's a board target on the same board,
/// `demo_board/demo_soc/cpunet`.
[[nodiscard]] std::vector<std::byte> build_demo_two_image_package();

/// The demo's direct-XIP package: image 0, version 2.0.0, linked for slot 0
/// (`app.signed.bin`) and for slot 1 (`app_slot1_variant.signed.bin`), both
/// for the board name `kDemoBoard`.
[[nodiscard]] std::vector<std::byte> build_demo_xip_package();

/// The radio version the demo device runs before the update.
inline constexpr std::uint8_t kDemoRadioRunning = 5;

} // namespace smply::example

#endif // SMPLY_EXAMPLE_DEMO_PACKAGE_HPP
