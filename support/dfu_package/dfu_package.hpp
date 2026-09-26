// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SUPPORT_DFU_PACKAGE_DFU_PACKAGE_HPP
#define SMPLY_SUPPORT_DFU_PACKAGE_DFU_PACKAGE_HPP

/// \file
/// Reads a multi-image DFU package: a zip of MCUboot-signed images and a
/// `manifest.json` that says which image each file is (ADR-0021).
///
/// The layout is the one nRF Connect SDK's sysbuild writes
/// (`scripts/bootloader/generate_zip.py`, `cmake/sysbuild/zip.cmake`;
/// docs/protocol-notes.md, S38): a **stored** zip, and a manifest whose
/// `files[]` entries carry `file`, `size`, `image_index`, `version_MCUBOOT`,
/// `board` and `soc`. A product with its own container needs none of this and
/// calls `FirmwareUpdater::start()` over its own list of images.
///
/// Support code, not the library: it is **not installed**, and the core stays
/// free of file formats (ADR-0016, ADR-0021). The package is a file somebody
/// handed the application, so it is untrusted throughout: every size is
/// bounded, and the manifest's claims are checked against the images'
/// own MCUboot headers rather than believed.

#include "smply/bytes.hpp"
#include "smply/mcuboot_image.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smply::dfu_package {

/// A package larger than this is refused before it is read.
inline constexpr std::size_t kMaxPackageSize = 64U * 1024U * 1024U;

/// The largest `image_index` accepted: MCUboot's dependency TLV names an image
/// in one byte.
inline constexpr std::uint32_t kMaxImageIndex = 255;

/// An MCUboot dependency TLV (`IMAGE_TLV_DEPENDENCY`): this image needs image
/// `image` at `minimum` or later (docs/protocol-notes.md, S37).
///
/// Reported, never enforced. The device's MCUboot enforces it at boot, which
/// is why every image is staged before the one reset (ADR-0021).
struct ImageDependency
{
    std::uint8_t image = 0;
    ImageVersion minimum;

    [[nodiscard]] friend bool operator==(const ImageDependency&, const ImageDependency&) = default;
};

/// One image of the package.
struct PackageImage
{
    /// The file's name in the zip.
    std::string file;
    /// `image_index`: which of the device's images this is.
    std::uint32_t image = 0;
    /// The signed image: a view into the archive passed to `read_package()`,
    /// valid for as long as that is.
    ConstBytes bytes;
    /// Its MCUboot header. The manifest's version was checked against it.
    McubootImageInfo header;
    /// `version_MCUBOOT`, `board` and `soc`, as the manifest wrote them.
    std::optional<std::string> version;
    std::optional<std::string> board;
    std::optional<std::string> soc;
    /// The image's dependency TLVs, in the order they appear.
    std::vector<ImageDependency> dependencies;
};

/// A whole package.
struct DfuPackage
{
    /// `format-version`, when the manifest has one. Recorded, not checked: the
    /// fields read here have not changed across the versions seen.
    std::optional<std::uint64_t> format_version;
    /// `name`, the optional label the build gave the package.
    std::optional<std::string> name;
    /// Sorted by image index, each index once.
    std::vector<PackageImage> images;
};

/// Reads the package in \p archive.
///
/// A manifest with one file and no `image_index` is read as image 0, the
/// shape of a single-image package. With more files every entry needs one.
///
/// \return `MessageTooLarge` for an archive, manifest or entry count over a
///         bound; `InvalidArgument` for a package smply does not read (a
///         compressed zip, two files for one image as a direct-XIP build
///         writes, an image index over `kMaxImageIndex`, or an image that
///         depends on a newer version of another image than the package
///         carries, compared as MCUboot does by default: major, minor and
///         revision, never the build number); `MalformedMessage`
///         for anything structurally wrong, a manifest that disagrees with
///         the zip or with an image's header, or a broken TLV area.
[[nodiscard]] Result<DfuPackage> read_package(ConstBytes archive);

/// The dependency TLVs of one signed image, whose header is \p header.
///
/// Walks both TLV areas as MCUboot does, bounded against \p image throughout.
[[nodiscard]] Result<std::vector<ImageDependency>>
read_dependencies(ConstBytes image, const McubootImageInfo& header);

} // namespace smply::dfu_package

#endif // SMPLY_SUPPORT_DFU_PACKAGE_DFU_PACKAGE_HPP
