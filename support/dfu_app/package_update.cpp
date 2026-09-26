// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/package_update.hpp"

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

Result<std::unique_ptr<PackageUpdate>> PackageUpdate::load(const std::string& path)
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
    return from_bytes(std::move(bytes));
}

Result<std::unique_ptr<PackageUpdate>> PackageUpdate::from_bytes(std::vector<std::byte> bytes)
{
    auto update = std::make_unique<PackageUpdate>(Token{}, std::move(bytes));
    // Read from the object's own copy, so every view in the package points into
    // bytes that live as long as it does.
    Result<dfu_package::DfuPackage> package = dfu_package::read_package(ConstBytes{update->bytes_});
    if (!package.has_value()) {
        return fail(package.error());
    }
    update->package_ = std::move(*package);

    for (const dfu_package::PackageImage& image : update->package_.images) {
        update->sources_.push_back(std::make_unique<MemoryImageSource>(image.bytes));
        // The coordinating-MCU default (docs/multi-image.md): the device's own
        // application is smply's to confirm, every other image the device's.
        update->targets_.push_back(
            ImageTarget{.image = image.image,
                        .source = update->sources_.back().get(),
                        .commit = image.image == 0 ? CommitBy::Client : CommitBy::Device});
    }
    return update;
}

Result<void> PackageUpdate::set_commit(std::uint32_t image, CommitBy commit)
{
    const auto found = std::ranges::find_if(
        targets_, [image](const ImageTarget& target) { return target.image == image; });
    if (found == targets_.end()) {
        return fail(ErrorCode::InvalidArgument, "package: no such image");
    }
    found->commit = commit;
    return {};
}

} // namespace smply::dfu_app
