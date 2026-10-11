// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/update_inputs.hpp"

#include "dfu_app/file_image_source.hpp"
#include "dfu_package/dfu_package.hpp"
#include "smply/bytes.hpp"
#include "smply/error.hpp"

#include <algorithm>
#include <fstream>
#include <ios>
#include <utility>

namespace smply::dfu_app {

std::optional<std::pair<std::uint32_t, CommitBy>> parse_commit(std::string_view text)
{
    const std::size_t equals = text.find('=');
    // One to three digits: an image index fits in a byte (dfu_package::kMaxImageIndex).
    if (equals == 0 || equals == std::string_view::npos || equals > 3 ||
        text.find_first_not_of("0123456789") != equals) {
        return std::nullopt;
    }
    std::uint32_t image = 0;
    for (const char digit : text.substr(0, equals)) {
        image = image * 10U + static_cast<std::uint32_t>(digit - '0');
    }
    const std::string_view who = text.substr(equals + 1);
    if (who == "client") {
        return std::pair{image, CommitBy::Client};
    }
    if (who == "device") {
        return std::pair{image, CommitBy::Device};
    }
    return std::nullopt;
}

std::optional<UpdateMode> parse_update_mode(std::string_view text) noexcept
{
    if (text == "test-then-confirm") {
        return UpdateMode::TestThenConfirm;
    }
    if (text == "confirm-immediately") {
        return UpdateMode::ConfirmImmediately;
    }
    if (text == "upload-only") {
        return UpdateMode::UploadOnly;
    }
    return std::nullopt;
}

Result<std::unique_ptr<UpdateInputs>> UpdateInputs::from_files(const std::string& primary,
                                                               const std::string& secondary)
{
    if (primary.empty()) {
        return fail(ErrorCode::InvalidArgument, secondary.empty()
                                                    ? "inputs: no image file"
                                                    : "inputs: a second build needs the first");
    }
    auto inputs = std::make_unique<UpdateInputs>(Token{});
    Result<FileImageSource> first = FileImageSource::open(primary);
    if (!first.has_value()) {
        return fail(first.error());
    }
    inputs->sources_.push_back(std::make_unique<FileImageSource>(std::move(*first)));
    ImageSource* source = inputs->sources_.back().get();

    ImageSource* second_build = nullptr;
    if (!secondary.empty()) {
        Result<FileImageSource> second = FileImageSource::open(secondary);
        if (!second.has_value()) {
            return fail(second.error());
        }
        inputs->sources_.push_back(std::make_unique<FileImageSource>(std::move(*second)));
        second_build = inputs->sources_.back().get();
    }
    inputs->targets_.push_back(ImageTarget{.image = 0,
                                           .source = source,
                                           .commit = CommitBy::Client,
                                           .secondary_source = second_build});
    return inputs;
}

Result<std::unique_ptr<UpdateInputs>> UpdateInputs::from_package(const std::string& path)
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return fail(ErrorCode::InvalidArgument, "package: cannot open the file");
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return fail(ErrorCode::InvalidArgument, "package: cannot read the file");
    }
    // Bounded before anything is allocated: the size is the file's claim.
    if (static_cast<std::uint64_t>(size) > dfu_package::kMaxPackageSize) {
        return fail(ErrorCode::MessageTooLarge, "package: larger than kMaxPackageSize");
    }
    std::vector<char> raw(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(raw.data(), size)) {
        return fail(ErrorCode::InvalidArgument, "package: cannot read the file");
    }
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) {
        return static_cast<std::byte>(static_cast<unsigned char>(c));
    });
    return from_package_bytes(std::move(bytes));
}

Result<std::unique_ptr<UpdateInputs>> UpdateInputs::from_package_bytes(std::vector<std::byte> bytes)
{
    auto update = std::make_unique<UpdateInputs>(Token{});
    update->bytes_ = std::move(bytes);
    // Read from the object's own copy, so every view in the package points into
    // bytes that live as long as it does.
    const Result<dfu_package::DfuPackage> package =
        dfu_package::read_package(ConstBytes{update->bytes_});
    if (!package.has_value()) {
        return fail(package.error());
    }

    for (const dfu_package::PackageImage& image : package->images) {
        update->sources_.push_back(std::make_unique<MemoryImageSource>(image.bytes));
        ImageSource* source = update->sources_.back().get();
        // A direct-XIP image's second build, for its secondary slot: the
        // updater sends whichever matches the slot the device is not running
        // (ADR-0025).
        ImageSource* secondary = nullptr;
        if (image.secondary.has_value()) {
            update->sources_.push_back(std::make_unique<MemoryImageSource>(image.secondary->bytes));
            secondary = update->sources_.back().get();
        }
        // The coordinating-MCU default (docs/multi-image.md): the device's own
        // application is smply's to confirm, every other image the device's.
        update->targets_.push_back(
            ImageTarget{.image = image.image,
                        .source = source,
                        .commit = image.image == 0 ? CommitBy::Client : CommitBy::Device,
                        .secondary_source = secondary});
    }
    return update;
}

Result<void> UpdateInputs::set_commit(std::uint32_t image, CommitBy commit)
{
    const auto found = std::ranges::find_if(
        targets_, [image](const ImageTarget& target) { return target.image == image; });
    if (found == targets_.end()) {
        return fail(ErrorCode::InvalidArgument, "inputs: no such image");
    }
    found->commit = commit;
    return {};
}

} // namespace smply::dfu_app
