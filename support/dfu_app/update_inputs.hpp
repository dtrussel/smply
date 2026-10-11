// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_UPDATE_INPUTS_HPP
#define SMPLY_DFU_APP_UPDATE_INPUTS_HPP

/// \file
/// What an update sends, turned into the image list `FirmwareUpdater::start()`
/// takes: one file, a build per slot (ADR-0025), or a multi-image package
/// (ADR-0021).
///
/// Shared by every example tool, for the reason `FileImageSource` is: the
/// alternative is three copies that drift. It owns the files or the package's
/// bytes, the sources over each image and the target list, so a caller keeps
/// one object alive for the length of the update and nothing else, and always
/// calls the image-list `start()`.
///
/// **Who commits which image is the application's call**, not the package's:
/// nothing in a manifest says so. The default here is the coordinator layout
/// of docs/multi-image.md -- image 0 committed by smply, every other image,
/// one per target, by the device -- and `set_commit()` overrides it per image.

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

/// Parses a `--mode` value naming one of the library's `UpdateMode`s:
/// `test-then-confirm`, `confirm-immediately` or `upload-only`. A tool with
/// modes of its own (`winrt_ble_dfu`'s `test-only` and `confirm-only`) checks
/// for those first.
[[nodiscard]] std::optional<UpdateMode> parse_update_mode(std::string_view text) noexcept;

/// The images of one update, with a target per image.
///
/// Non-copyable and non-movable: the targets point at sources it owns, and a
/// package's sources view the bytes it owns. The factories hand one out on the
/// heap.
class UpdateInputs
{
public:
    /// Image 0 from a file, or from two: with \p secondary, \p primary is the
    /// build for the image's primary slot and \p secondary the build for its
    /// secondary slot, and the updater sends the one for the slot the device is
    /// not running (`ImageTarget::secondary_source`).
    ///
    /// \return `InvalidArgument` for no \p primary (a second build goes with a
    ///         first), or whatever `FileImageSource::open()` refuses.
    [[nodiscard]] static Result<std::unique_ptr<UpdateInputs>>
    from_files(const std::string& primary, const std::string& secondary = {});

    /// Reads the package file at \p path.
    ///
    /// \return `InvalidArgument` if it cannot be opened or read,
    ///         `MessageTooLarge` over `dfu_package::kMaxPackageSize` (checked
    ///         before anything is allocated), or whatever `read_package()`
    ///         refuses.
    [[nodiscard]] static Result<std::unique_ptr<UpdateInputs>>
    from_package(const std::string& path);

    /// Takes a package already in memory.
    [[nodiscard]] static Result<std::unique_ptr<UpdateInputs>>
    from_package_bytes(std::vector<std::byte> bytes);

    UpdateInputs(const UpdateInputs&) = delete;
    UpdateInputs(UpdateInputs&&) = delete;
    UpdateInputs& operator=(const UpdateInputs&) = delete;
    UpdateInputs& operator=(UpdateInputs&&) = delete;
    ~UpdateInputs() = default;

    /// Overrides who commits \p image.
    ///
    /// \return `InvalidArgument` if the inputs hold no such image.
    [[nodiscard]] Result<void> set_commit(std::uint32_t image, CommitBy commit);

    /// The list to pass to `FirmwareUpdater::start()`, sorted by image.
    [[nodiscard]] std::span<const ImageTarget> targets() const noexcept
    {
        return targets_;
    }

private:
    /// Only the factories construct one; the token keeps the constructor
    /// public enough for `std::make_unique` and no further.
    struct Token
    {};

public:
    explicit UpdateInputs(Token /*unused*/) noexcept {}

private:
    /// A package's bytes, which its sources view. Empty for files.
    std::vector<std::byte> bytes_;
    std::vector<std::unique_ptr<ImageSource>> sources_;
    std::vector<ImageTarget> targets_;
};

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_UPDATE_INPUTS_HPP
