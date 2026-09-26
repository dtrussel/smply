// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_PACKAGE_UPDATE_HPP
#define SMPLY_DFU_APP_PACKAGE_UPDATE_HPP

/// \file
/// A multi-image DFU package, turned into the image list
/// `FirmwareUpdater::start()` takes (ADR-0021).
///
/// Shared by both portable examples, for the reason `FileImageSource` is: the
/// alternative is two copies that drift. It owns the package's bytes, the
/// sources over each image and the target list, so a caller keeps one object
/// alive for the length of the update and nothing else.
///
/// **Who commits which image is the application's call**, not the package's:
/// nothing in a manifest says so. The default here is the coordinating-MCU
/// product of docs/multi-image.md -- image 0 committed by smply, every other
/// image by the device -- and `set_commit()` overrides it per image.

#include "dfu_package/dfu_package.hpp"
#include "smply/bytes.hpp"
#include "smply/dfu/firmware_updater.hpp"
#include "smply/image_source.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smply::dfu_app {

/// Parses a `--commit` value, `N=client` or `N=device`: who commits image N.
[[nodiscard]] std::optional<std::pair<std::uint32_t, CommitBy>> parse_commit(std::string_view text);

/// A package read into memory, with an update target per image.
///
/// Non-copyable and non-movable: the targets point at sources that view the
/// bytes it owns. `load()` and `from_bytes()` hand one out on the heap.
class PackageUpdate
{
public:
    /// Reads the package file at \p path.
    ///
    /// \return `InvalidArgument` if it cannot be opened or read,
    ///         `MessageTooLarge` over `dfu_package::kMaxPackageSize` (checked
    ///         before anything is allocated), or whatever `read_package()`
    ///         refuses.
    [[nodiscard]] static Result<std::unique_ptr<PackageUpdate>> load(const std::string& path);

    /// Takes a package already in memory.
    [[nodiscard]] static Result<std::unique_ptr<PackageUpdate>>
    from_bytes(std::vector<std::byte> bytes);

    PackageUpdate(const PackageUpdate&) = delete;
    PackageUpdate(PackageUpdate&&) = delete;
    PackageUpdate& operator=(const PackageUpdate&) = delete;
    PackageUpdate& operator=(PackageUpdate&&) = delete;
    ~PackageUpdate() = default;

    /// Overrides who commits \p image.
    ///
    /// \return `InvalidArgument` if the package holds no such image.
    [[nodiscard]] Result<void> set_commit(std::uint32_t image, CommitBy commit);

    /// The list to pass to `FirmwareUpdater::start()`, sorted by image.
    [[nodiscard]] std::span<const ImageTarget> targets() const noexcept
    {
        return targets_;
    }

    [[nodiscard]] const dfu_package::DfuPackage& package() const noexcept
    {
        return package_;
    }

private:
    /// Only `from_bytes()` constructs one; the token keeps the constructor
    /// public enough for `std::make_unique` and no further.
    struct Token
    {};

public:
    PackageUpdate(Token /*unused*/, std::vector<std::byte> bytes) noexcept
        : bytes_{std::move(bytes)}
    {}

private:
    std::vector<std::byte> bytes_;
    dfu_package::DfuPackage package_;
    std::vector<std::unique_ptr<MemoryImageSource>> sources_;
    std::vector<ImageTarget> targets_;
};

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_PACKAGE_UPDATE_HPP
