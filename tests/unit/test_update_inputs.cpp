// SPDX-License-Identifier: Apache-2.0
//
// `smply::dfu_app::UpdateInputs`, `parse_commit()` and `parse_update_mode()`:
// what an update sends -- one file, a build per slot, or a multi-image package
// (ADR-0021, ADR-0025) -- turned into the image list `FirmwareUpdater::start()`
// takes, as every example tool does. The package format itself is
// test_dfu_package.cpp's, and reading a file test_file_image_source.cpp's;
// these check which targets come out, the default commit rule and its override,
// and what is refused.

#include "image_builder.hpp"
#include "zip_builder.hpp"

#include "dfu_app/update_inputs.hpp"
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
#include <string_view>
#include <system_error>
#include <vector>

using smply::CommitBy;
using smply::ConstBytes;
using smply::ErrorCode;
using smply::UpdateMode;
using smply::dfu_app::parse_commit;
using smply::dfu_app::parse_update_mode;
using smply::dfu_app::UpdateInputs;
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
    explicit TempFile(std::string_view extension = ".zip") : path_{unique_path(extension)} {}

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
    [[nodiscard]] static std::string unique_path(std::string_view extension)
    {
        std::random_device entropy;
        return (std::filesystem::temp_directory_path() /
                ("smply_update_inputs_" + std::to_string(entropy()) + std::string{extension}))
            .string();
    }

    std::string path_;
};

} // namespace

TEST_CASE("a package becomes one target per image, image 0 committed by the client",
          "[dfu_app][inputs][package]")
{
    const auto update = UpdateInputs::from_package_bytes(three_images());
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
}

TEST_CASE("who commits an image can be overridden, for an image the package has",
          "[dfu_app][inputs][package]")
{
    const auto update = UpdateInputs::from_package_bytes(three_images());
    REQUIRE(update.has_value());
    REQUIRE((*update)->set_commit(2, CommitBy::Client).has_value());
    REQUIRE((*update)->set_commit(0, CommitBy::Device).has_value());
    CHECK((*update)->targets()[2].commit == CommitBy::Client);
    CHECK((*update)->targets()[0].commit == CommitBy::Device);

    const auto missing = (*update)->set_commit(7, CommitBy::Client);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a package file is read whole, and a bad one refused", "[dfu_app][inputs][package]")
{
    const TempFile file;
    file.write(three_images());
    const auto update = UpdateInputs::from_package(file.path());
    REQUIRE(update.has_value());
    CHECK((*update)->targets().size() == 3);

    const auto missing = UpdateInputs::from_package(file.path() + ".absent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code() == ErrorCode::InvalidArgument);

    const TempFile garbage;
    garbage.write(std::vector<std::byte>(64, std::byte{0x41}));
    const auto refused = UpdateInputs::from_package(garbage.path());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("parse_commit reads N=client and N=device", "[dfu_app][inputs]")
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

TEST_CASE("a direct-XIP package becomes one target with a build per slot",
          "[dfu_app][inputs][package][xip]")
{
    // Two files for image 0, each with its slot (protocol-notes S58). The
    // target's source is slot 0's build and its secondary_source slot 1's.
    const std::vector<std::byte> slot0 =
        ImageBuilder{}.version(2, 0, 0, 0).body(64).tlv(0x10, std::vector<std::byte>(32)).build();
    const std::vector<std::byte> slot1 =
        ImageBuilder{}.version(2, 0, 0, 0).body(80).tlv(0x10, std::vector<std::byte>(32)).build();
    std::vector<std::byte> archive = ZipBuilder{}
                                         .add("app_slot1_variant.signed.bin", slot1)
                                         .add("app.signed.bin", slot0)
                                         .add("manifest.json", std::string_view{R"({"files": [
                     {"file": "app_slot1_variant.signed.bin", "image_index": "0", "slot": "1"},
                     {"file": "app.signed.bin", "image_index": "0", "slot": "0"}]})"})
                                         .build();

    auto update = UpdateInputs::from_package_bytes(std::move(archive));
    REQUIRE(update.has_value());
    const auto targets = (*update)->targets();
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].image == 0);
    CHECK(targets[0].commit == CommitBy::Client);
    REQUIRE(targets[0].source != nullptr);
    REQUIRE(targets[0].secondary_source != nullptr);
    CHECK(targets[0].source->size() == slot0.size());
    CHECK(targets[0].secondary_source->size() == slot1.size());
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

TEST_CASE("one file is one target: image 0, committed by the client", "[dfu_app][inputs][file]")
{
    const TempFile file{".bin"};
    file.write(image(2));

    const auto inputs = UpdateInputs::from_files(file.path());

    REQUIRE(inputs.has_value());
    const auto targets = (*inputs)->targets();
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].image == 0);
    CHECK(targets[0].commit == CommitBy::Client);
    REQUIRE(targets[0].source != nullptr);
    CHECK(targets[0].source->size() == image(2).size());
    CHECK(targets[0].secondary_source == nullptr);
}

TEST_CASE("two files are one target with a build per slot", "[dfu_app][inputs][file][xip]")
{
    // The first file is the build for slot 0, the second the build for slot 1
    // (ADR-0025): the updater sends whichever the device is not running.
    const std::vector<std::byte> slot0 =
        ImageBuilder{}.version(2, 0, 0, 0).body(64).tlv(0x10, std::vector<std::byte>(32)).build();
    const std::vector<std::byte> slot1 =
        ImageBuilder{}.version(2, 0, 0, 0).body(80).tlv(0x10, std::vector<std::byte>(32)).build();
    const TempFile primary{".bin"};
    primary.write(slot0);
    const TempFile secondary{".bin"};
    secondary.write(slot1);

    const auto inputs = UpdateInputs::from_files(primary.path(), secondary.path());

    REQUIRE(inputs.has_value());
    const auto targets = (*inputs)->targets();
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].image == 0);
    CHECK(targets[0].commit == CommitBy::Client);
    REQUIRE(targets[0].source != nullptr);
    REQUIRE(targets[0].secondary_source != nullptr);
    CHECK(targets[0].source->size() == slot0.size());
    CHECK(targets[0].secondary_source->size() == slot1.size());
}

TEST_CASE("files that cannot make an update are refused", "[dfu_app][inputs][file]")
{
    const TempFile file{".bin"};
    file.write(image(2));

    SECTION("no file at all")
    {
        const auto inputs = UpdateInputs::from_files("");
        REQUIRE_FALSE(inputs.has_value());
        CHECK(inputs.error().code() == ErrorCode::InvalidArgument);
    }
    SECTION("a second build without a first")
    {
        const auto inputs = UpdateInputs::from_files("", file.path());
        REQUIRE_FALSE(inputs.has_value());
        CHECK(inputs.error().code() == ErrorCode::InvalidArgument);
    }
    SECTION("a first build that cannot be opened")
    {
        const auto inputs = UpdateInputs::from_files(file.path() + ".absent");
        REQUIRE_FALSE(inputs.has_value());
        CHECK(inputs.error().code() == ErrorCode::InvalidArgument);
    }
    SECTION("a second build that cannot be opened")
    {
        const auto inputs = UpdateInputs::from_files(file.path(), file.path() + ".absent");
        REQUIRE_FALSE(inputs.has_value());
        CHECK(inputs.error().code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("who commits a file's image can be set, for image 0 only", "[dfu_app][inputs][file]")
{
    const TempFile file{".bin"};
    file.write(image(2));
    const auto inputs = UpdateInputs::from_files(file.path());
    REQUIRE(inputs.has_value());

    REQUIRE((*inputs)->set_commit(0, CommitBy::Device).has_value());
    CHECK((*inputs)->targets()[0].commit == CommitBy::Device);

    const auto missing = (*inputs)->set_commit(1, CommitBy::Device);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("parse_update_mode names the three modes of one update", "[dfu_app][inputs]")
{
    CHECK(parse_update_mode("test-then-confirm") == UpdateMode::TestThenConfirm);
    CHECK(parse_update_mode("confirm-immediately") == UpdateMode::ConfirmImmediately);
    CHECK(parse_update_mode("upload-only") == UpdateMode::UploadOnly);
    for (const char* bad :
         {"", "test", "Upload-only", "upload-only ", "test-only", "confirm-only"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_update_mode(bad).has_value());
    }
}
