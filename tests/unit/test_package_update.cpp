// SPDX-License-Identifier: Apache-2.0
//
// `smply::dfu_app::PackageUpdate` and `parse_commit()`: the few lines between a
// package file and `FirmwareUpdater::start()` that both examples share
// (ADR-0021). The package format itself is test_dfu_package.cpp's; these check
// the file handling, the default commit rule and its override.

#include "image_builder.hpp"
#include "zip_builder.hpp"

#include "dfu_app/package_update.hpp"
#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

using smply::CommitBy;
using smply::ConstBytes;
using smply::ErrorCode;
using smply::dfu_app::PackageUpdate;
using smply::dfu_app::parse_commit;
using smply::test::ImageBuilder;
using smply::test::ZipBuilder;

namespace {

[[nodiscard]] std::vector<std::byte> image(std::uint8_t major)
{
    return ImageBuilder{}
        .version(major, 0, 0, 0)
        .body(64)
        .tlv(0x10, std::vector<std::byte>(32))
        .build();
}

/// Three images, listed out of order, as the manifest may.
[[nodiscard]] std::vector<std::byte> three_images()
{
    return ZipBuilder{}
        .add("a.bin", image(1))
        .add("b.bin", image(2))
        .add("c.bin", image(3))
        .add("manifest.json", std::string_view{R"({"files": [{"file": "c.bin", "image_index": "2"},
                                            {"file": "a.bin", "image_index": "0"},
                                            {"file": "b.bin", "image_index": "1"}]})"})
        .build();
}

/// A uniquely named temporary file, removed at the end of the test.
class TempFile
{
public:
    TempFile() : path_{unique_path()} {}

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    ~TempFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    void write(const std::vector<std::byte>& bytes) const
    {
        std::ofstream out{path_, std::ios::binary | std::ios::trunc};
        for (const std::byte b : bytes) {
            out.put(static_cast<char>(std::to_integer<unsigned char>(b)));
        }
    }

    [[nodiscard]] const std::string& path() const noexcept
    {
        return path_;
    }

private:
    [[nodiscard]] static std::string unique_path()
    {
        std::random_device entropy;
        return (std::filesystem::temp_directory_path() /
                ("smply_package_update_" + std::to_string(entropy()) + ".zip"))
            .string();
    }

    std::string path_;
};

} // namespace

TEST_CASE("a package becomes one target per image, image 0 committed by the client",
          "[dfu_app][package]")
{
    const auto update = PackageUpdate::from_bytes(three_images());
    REQUIRE(update.has_value());
    const auto targets = (*update)->targets();
    REQUIRE(targets.size() == 3);
    for (std::uint32_t index = 0; index < targets.size(); ++index) {
        CHECK(targets[index].image == index);
        REQUIRE(targets[index].source != nullptr);
        CHECK(targets[index].source->size() == image(1).size());
    }
    CHECK(targets[0].commit == CommitBy::Client);
    CHECK(targets[1].commit == CommitBy::Device);
    CHECK(targets[2].commit == CommitBy::Device);
    CHECK((*update)->package().images.size() == 3);
}

TEST_CASE("who commits an image can be overridden, for an image the package has",
          "[dfu_app][package]")
{
    const auto update = PackageUpdate::from_bytes(three_images());
    REQUIRE(update.has_value());
    REQUIRE((*update)->set_commit(2, CommitBy::Client).has_value());
    REQUIRE((*update)->set_commit(0, CommitBy::Device).has_value());
    CHECK((*update)->targets()[2].commit == CommitBy::Client);
    CHECK((*update)->targets()[0].commit == CommitBy::Device);

    const auto missing = (*update)->set_commit(7, CommitBy::Client);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a package file is read whole, and a bad one refused", "[dfu_app][package]")
{
    const TempFile file;
    file.write(three_images());
    const auto update = PackageUpdate::load(file.path());
    REQUIRE(update.has_value());
    CHECK((*update)->targets().size() == 3);

    const auto missing = PackageUpdate::load(file.path() + ".absent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code() == ErrorCode::InvalidArgument);

    const TempFile garbage;
    garbage.write(std::vector<std::byte>(64, std::byte{0x41}));
    const auto refused = PackageUpdate::load(garbage.path());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("parse_commit reads N=client and N=device", "[dfu_app][package]")
{
    CHECK(parse_commit("0=client") == std::pair{0U, CommitBy::Client});
    CHECK(parse_commit("12=device") == std::pair{12U, CommitBy::Device});
    CHECK(parse_commit("255=client") == std::pair{255U, CommitBy::Client});
    for (const char* bad : {"", "=client", "1", "1=", "1=Client", "x=client", "1x=device",
                            "1000=client", "-1=device", "1=client ", " 1=client"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_commit(bad).has_value());
    }
}
