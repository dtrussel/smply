// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_DEMO_PACKAGE_HPP
#define SMPLY_EXAMPLE_DEMO_PACKAGE_HPP

/// \file
/// Builds a multi-image DFU package, so `--demo-package` runs with no files.
///
/// Scaffolding, like `demo_image.hpp`. The layout is the one nRF Connect SDK's
/// sysbuild writes (docs/protocol-notes.md, S38): a zip whose entries are
/// **stored**, and a `manifest.json` whose `files[]` carry `file`, `size`,
/// `image_index` -- as a string, the way `generate_zip.py` writes it -- and
/// `version_MCUBOOT`. `support/dfu_package/` is what reads it back. The zip is
/// written from APPNOTE.TXT here rather than shared with that reader, so the
/// demo exercises the reader rather than agreeing with it.

#include <cstddef>
#include <cstdint>
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
};

/// A stored zip holding \p files and a manifest describing them.
[[nodiscard]] std::vector<std::byte> build_demo_package(const std::vector<DemoPackageFile>& files);

/// The demo's two-image package, the coordinating-MCU update of
/// docs/multi-image.md: `app.bin` (image 0, 2.0.0) and `radio.bin` (image 1,
/// 6.0.0), for a device running app 1.0.0 and radio `kDemoRadioRunning`.
[[nodiscard]] std::vector<std::byte> build_demo_two_image_package();

/// The radio version the demo device runs before the update.
inline constexpr std::uint8_t kDemoRadioRunning = 5;

} // namespace smply::example

#endif // SMPLY_EXAMPLE_DEMO_PACKAGE_HPP
