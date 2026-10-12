// SPDX-License-Identifier: Apache-2.0

#include "stub_device/demo_image_file.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <system_error>

namespace smply::example {

DemoImageFile::~DemoImageFile()
{
    if (!path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
}

std::optional<std::string> DemoImageFile::write(const std::vector<std::byte>& image)
{
    std::error_code ec;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::nullopt;
    }
    std::random_device entropy;
    const std::string name = tag_ + "_demo_image_" + std::to_string(entropy()) + "_" +
                             std::to_string(entropy()) + ".bin";
    path_ = (directory / name).string();

    std::ofstream out{path_, std::ios::binary | std::ios::trunc};
    if (!out) {
        return std::nullopt;
    }
    // ostream speaks char, and these are bytes this process just built. The
    // marker has to be the last comment line before the code, or it silences
    // the comment instead.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
    return out ? std::optional<std::string>{path_} : std::nullopt;
}

} // namespace smply::example
