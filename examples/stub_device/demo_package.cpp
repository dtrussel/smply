// SPDX-License-Identifier: Apache-2.0

#include "stub_device/demo_package.hpp"

#include "stub_device/demo_image.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace smply::example {
namespace {

/// CRC-32 as zip uses it (APPNOTE.TXT section 4.4.7): reflected 0xEDB88320.
[[nodiscard]] std::uint32_t crc32(const std::vector<std::byte>& data)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte byte : data) {
        crc ^= std::to_integer<std::uint32_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

void u16(std::vector<std::byte>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void u32(std::vector<std::byte>& out, std::uint32_t value)
{
    u16(out, value & 0xFFFFU);
    u16(out, value >> 16U);
}

void text(std::vector<std::byte>& out, std::string_view value)
{
    for (const char c : value) {
        out.push_back(static_cast<std::byte>(c));
    }
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view value)
{
    std::vector<std::byte> out;
    text(out, value);
    return out;
}

/// The manifest, in generate_zip.py's shape: sysbuild's values are strings.
[[nodiscard]] std::string manifest_for(const std::vector<DemoPackageFile>& files)
{
    std::string out = R"({"format-version": 1, "time": 0, "name": "smply demo", "files": [)";
    for (std::size_t index = 0; index < files.size(); ++index) {
        const DemoPackageFile& file = files[index];
        out += index == 0 ? "" : ", ";
        out += R"({"image_index": ")" + std::to_string(file.image_index) +
               R"(", "version_MCUBOOT": ")" + file.version + R"(", "size": )" +
               std::to_string(file.content.size()) + R"(, "file": ")" + file.name + R"("})";
    }
    return out + "]}";
}

} // namespace

std::vector<std::byte> build_demo_package(const std::vector<DemoPackageFile>& files)
{
    std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
    entries.reserve(files.size() + 1);
    for (const DemoPackageFile& file : files) {
        entries.emplace_back(file.name, file.content);
    }
    entries.emplace_back("manifest.json", bytes_of(manifest_for(files)));

    // Local headers and data, then the central directory, then its end record
    // (APPNOTE.TXT sections 4.3.7, 4.3.12 and 4.3.16). Method 0: stored.
    std::vector<std::byte> out;
    std::vector<std::uint32_t> offsets;
    offsets.reserve(entries.size());
    for (const auto& [name, data] : entries) {
        offsets.push_back(static_cast<std::uint32_t>(out.size()));
        u32(out, 0x04034B50U);
        u16(out, 20);   // version needed
        u16(out, 0);    // flags
        u16(out, 0);    // stored
        u16(out, 0);    // time
        u16(out, 0x21); // date
        u32(out, crc32(data));
        u32(out, static_cast<std::uint32_t>(data.size()));
        u32(out, static_cast<std::uint32_t>(data.size()));
        u16(out, static_cast<std::uint32_t>(name.size()));
        u16(out, 0); // extra
        text(out, name);
        out.insert(out.end(), data.begin(), data.end());
    }

    const auto directory = static_cast<std::uint32_t>(out.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& [name, data] = entries[index];
        u32(out, 0x02014B50U);
        u16(out, 20); // version made by
        u16(out, 20); // version needed
        u16(out, 0);
        u16(out, 0);
        u16(out, 0);
        u16(out, 0x21);
        u32(out, crc32(data));
        u32(out, static_cast<std::uint32_t>(data.size()));
        u32(out, static_cast<std::uint32_t>(data.size()));
        u16(out, static_cast<std::uint32_t>(name.size()));
        u16(out, 0); // extra
        u16(out, 0); // comment
        u16(out, 0); // disk
        u16(out, 0); // internal attributes
        u32(out, 0); // external attributes
        u32(out, offsets[index]);
        text(out, name);
    }
    const auto directory_size = static_cast<std::uint32_t>(out.size()) - directory;

    u32(out, 0x06054B50U);
    u16(out, 0);
    u16(out, 0);
    u16(out, static_cast<std::uint32_t>(entries.size()));
    u16(out, static_cast<std::uint32_t>(entries.size()));
    u32(out, directory_size);
    u32(out, directory);
    u16(out, 0); // comment
    return out;
}

std::vector<std::byte> build_demo_two_image_package()
{
    return build_demo_package({
        DemoPackageFile{.name = "app.bin",
                        .image_index = 0,
                        .version = "2.0.0",
                        .content = build_demo_image(DemoVersion{.major = 2})},
        DemoPackageFile{.name = "radio.bin",
                        .image_index = 1,
                        .version = "6.0.0",
                        .content = build_demo_image(DemoVersion{.major = 6})},
    });
}

} // namespace smply::example
