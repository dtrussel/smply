// SPDX-License-Identifier: Apache-2.0

#include "dfu_package/dfu_package.hpp"

#include "dfu_package/json.hpp"
#include "dfu_package/stored_zip.hpp"
#include "smply/error.hpp"
#include "smply/limits.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace smply::dfu_package {
namespace {

/// `IMAGE_TLV_INFO_MAGIC` and `IMAGE_TLV_PROT_INFO_MAGIC`.
constexpr std::uint16_t kTlvInfoMagic = 0x6907;
constexpr std::uint16_t kTlvProtInfoMagic = 0x6908;
constexpr std::size_t kTlvHeaderSize = 4;
/// `IMAGE_TLV_DEPENDENCY`, and `sizeof(struct image_dependency)`: an image id,
/// three bytes of padding, and an eight-byte `image_version` (S37).
constexpr std::uint16_t kTlvDependency = 0x40;
constexpr std::size_t kDependencySize = 12;

[[nodiscard]] std::uint16_t le16(ConstBytes bytes, std::size_t at) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[at]) |
                                      (std::to_integer<unsigned>(bytes[at + 1]) << 8U));
}

[[nodiscard]] std::uint32_t le32(ConstBytes bytes, std::size_t at) noexcept
{
    return static_cast<std::uint32_t>(le16(bytes, at)) |
           (static_cast<std::uint32_t>(le16(bytes, at + 2)) << 16U);
}

[[nodiscard]] unexpected<Error> malformed(const char* what) noexcept
{
    return fail(ErrorCode::MalformedMessage, what);
}

/// A string member, which may be absent but may not be anything else.
[[nodiscard]] Result<std::optional<std::string>> optional_text(const JsonValue& object,
                                                               std::string_view key)
{
    const JsonValue* value = object.find(key);
    if (value == nullptr) {
        return std::optional<std::string>{};
    }
    if (value->kind != JsonValue::Kind::String) {
        return malformed("package: a manifest field is not a string");
    }
    return std::optional<std::string>{value->text};
}

/// `image_index`, which `generate_zip.py` writes as a string ("1") because
/// every `key=value` it is given is a string unless it starts with 0x. A
/// number is accepted too.
[[nodiscard]] Result<std::optional<std::uint32_t>> image_index(const JsonValue& entry)
{
    const JsonValue* value = entry.find("image_index");
    if (value == nullptr) {
        return std::optional<std::uint32_t>{};
    }
    std::optional<std::uint64_t> number;
    if (value->kind == JsonValue::Kind::String) {
        JsonValue as_number;
        as_number.kind = JsonValue::Kind::Number;
        as_number.text = value->text;
        number = as_number.as_uint();
    } else {
        number = value->as_uint();
    }
    if (!number.has_value()) {
        return malformed("package: image_index is not a non-negative integer");
    }
    if (*number > kMaxImageIndex) {
        return fail(ErrorCode::InvalidArgument, "package: image_index out of range");
    }
    return std::optional<std::uint32_t>{static_cast<std::uint32_t>(*number)};
}

/// One `files[]` entry, checked against the zip and against the image itself.
[[nodiscard]] Result<PackageImage> read_image(const JsonValue& entry,
                                              const std::vector<ZipEntry>& zip, bool single)
{
    if (entry.kind != JsonValue::Kind::Object) {
        return malformed("package: a files[] entry is not an object");
    }
    PackageImage image;

    const Result<std::optional<std::string>> file = optional_text(entry, "file");
    if (!file.has_value()) {
        return fail(file.error());
    }
    // Bound once, so the engaged check is visible where the value is read.
    const std::optional<std::string>& name = *file;
    if (!name.has_value()) {
        return malformed("package: a files[] entry has no file name");
    }
    image.file = *name;
    const auto found = std::ranges::find_if(
        zip, [&image](const ZipEntry& candidate) { return candidate.name == image.file; });
    if (found == zip.end()) {
        return malformed("package: the manifest names a file the zip does not hold");
    }
    image.bytes = found->data;

    if (const JsonValue* size = entry.find("size"); size != nullptr) {
        const std::optional<std::uint64_t> stated = size->as_uint();
        if (!stated.has_value() || *stated != image.bytes.size()) {
            return malformed("package: a file's size disagrees with the manifest");
        }
    }

    const Result<std::optional<std::uint32_t>> index = image_index(entry);
    if (!index.has_value()) {
        return fail(index.error());
    }
    if (!index->has_value() && !single) {
        return malformed("package: a files[] entry has no image_index");
    }
    image.image = index->value_or(0);

    for (const auto& [key, into] :
         {std::pair{"version_MCUBOOT", &image.version}, std::pair{"board", &image.board},
          std::pair{"soc", &image.soc}}) {
        Result<std::optional<std::string>> text = optional_text(entry, key);
        if (!text.has_value()) {
            return fail(text.error());
        }
        *into = std::move(*text);
    }

    if (image.bytes.size() < kMcubootHeaderSize) {
        return malformed("package: a file is too short to be an MCUboot image");
    }
    const Result<McubootImageInfo> header =
        parse_mcuboot_header(image.bytes.first(kMcubootHeaderSize));
    if (!header.has_value()) {
        return fail(header.error());
    }
    image.header = *header;

    if (image.version.has_value()) {
        // The manifest is a claim; the header is what MCUboot will read.
        const Result<ImageVersion> stated = ImageVersion::parse(*image.version);
        if (!stated.has_value() || *stated != image.header.version) {
            return malformed("package: version_MCUBOOT disagrees with the image header");
        }
    }

    Result<std::vector<ImageDependency>> dependencies = read_dependencies(image.bytes, *header);
    if (!dependencies.has_value()) {
        return fail(dependencies.error());
    }
    image.dependencies = std::move(*dependencies);
    return image;
}

/// Whether \p version satisfies \p minimum, as MCUboot decides it by default:
/// major, minor and revision, never the build number, which only counts under
/// `MCUBOOT_VERSION_CMP_USE_BUILD_NUMBER` (protocol-notes S41).
[[nodiscard]] bool satisfies(const ImageVersion& version, const ImageVersion& minimum) noexcept
{
    if (version.major != minimum.major) {
        return version.major > minimum.major;
    }
    if (version.minor != minimum.minor) {
        return version.minor > minimum.minor;
    }
    return version.revision >= minimum.revision;
}

/// Refuses a package whose own images disagree: an image that depends on
/// another image the package carries, at a version the package does not
/// carry. MCUboot would refuse to boot it anyway (S37); this says so before
/// anything is sent. A dependency on an image outside the package is left to
/// the device, which alone knows what it runs (ADR-0022).
[[nodiscard]] Result<void> check_dependencies(const DfuPackage& package)
{
    for (const PackageImage& image : package.images) {
        for (const ImageDependency& dependency : image.dependencies) {
            const auto partner =
                std::ranges::find_if(package.images, [&dependency](const PackageImage& other) {
                    return other.image == dependency.image;
                });
            if (partner != package.images.end() &&
                !satisfies(partner->header.version, dependency.minimum)) {
                return fail(ErrorCode::InvalidArgument,
                            "package: an image needs a newer version of another image than the "
                            "package carries");
            }
        }
    }
    return {};
}

} // namespace

Result<std::vector<ImageDependency>> read_dependencies(ConstBytes image,
                                                       const McubootImageInfo& header)
{
    const std::size_t base =
        static_cast<std::size_t>(header.header_size) + static_cast<std::size_t>(header.image_size);
    const auto area =
        [&image](std::size_t at) -> std::optional<std::pair<std::uint16_t, std::size_t>> {
        if (at > image.size() || kTlvHeaderSize > image.size() - at) {
            return std::nullopt;
        }
        return std::pair{le16(image, at), static_cast<std::size_t>(le16(image, at + 2))};
    };

    // The protected area, if there is one, then the unprotected one: contiguous,
    // and walked as one run (docs/protocol-notes.md section 7).
    const auto first = area(base);
    if (!first.has_value()) {
        return malformed("package: an image has no TLV area");
    }
    std::size_t protected_end = base;
    std::size_t unprotected = 0;
    if (first->first == kTlvProtInfoMagic) {
        if (first->second != header.protected_tlv_size || first->second < kTlvHeaderSize) {
            return malformed("package: an image's protected TLV size disagrees with its header");
        }
        protected_end = base + first->second;
        const auto second = area(protected_end);
        if (!second.has_value() || second->first != kTlvInfoMagic) {
            return malformed("package: an image has no unprotected TLV area");
        }
        unprotected = second->second;
    } else {
        if (first->first != kTlvInfoMagic || header.protected_tlv_size != 0) {
            return malformed("package: an image has no TLV area");
        }
        unprotected = first->second;
    }
    if (unprotected < kTlvHeaderSize || unprotected > image.size() - protected_end) {
        return malformed("package: an image's TLV area extends past its end");
    }
    const std::size_t end = protected_end + unprotected;

    std::vector<ImageDependency> dependencies;
    std::size_t offset = base + kTlvHeaderSize;
    for (std::size_t visited = 0; offset < end; ++visited) {
        if (visited == limits::kMaxImageTlvs) {
            return malformed("package: an image has too many TLV entries");
        }
        if (header.protected_tlv_size != 0 && offset == protected_end) {
            offset += kTlvHeaderSize; // the unprotected area's own header
            continue;
        }
        if (kTlvHeaderSize > end - offset) {
            return malformed("package: a TLV entry overruns its area");
        }
        const std::uint16_t type = le16(image, offset);
        const std::size_t length = le16(image, offset + 2);
        if (length > end - offset - kTlvHeaderSize) {
            return malformed("package: a TLV entry overruns its area");
        }
        if (type == kTlvDependency) {
            if (length != kDependencySize) {
                // MCUboot refuses the image for this too.
                return malformed("package: a dependency TLV has the wrong length");
            }
            const ConstBytes value = image.subspan(offset + kTlvHeaderSize, kDependencySize);
            dependencies.push_back(ImageDependency{
                .image = static_cast<std::uint8_t>(std::to_integer<unsigned>(value[0])),
                .minimum = ImageVersion{
                    .major = static_cast<std::uint8_t>(std::to_integer<unsigned>(value[4])),
                    .minor = static_cast<std::uint8_t>(std::to_integer<unsigned>(value[5])),
                    .revision = le16(value, 6),
                    .build = le32(value, 8)}});
        }
        offset += kTlvHeaderSize + length;
    }
    return dependencies;
}

Result<DfuPackage> read_package(ConstBytes archive)
{
    if (archive.size() > kMaxPackageSize) {
        return fail(ErrorCode::MessageTooLarge, "package: larger than kMaxPackageSize");
    }
    const Result<std::vector<ZipEntry>> zip = read_stored_zip(archive);
    if (!zip.has_value()) {
        return fail(zip.error());
    }

    const auto manifest_entry = std::ranges::find_if(
        *zip, [](const ZipEntry& entry) { return entry.name == "manifest.json"; });
    if (manifest_entry == zip->end()) {
        return malformed("package: no manifest.json");
    }
    const ConstBytes manifest_bytes = manifest_entry->data;
    std::string manifest_text(manifest_bytes.size(), '\0');
    if (manifest_bytes.size() > kMaxJsonSize) {
        return fail(ErrorCode::MessageTooLarge, "package: manifest.json too large");
    }
    std::ranges::transform(manifest_bytes, manifest_text.begin(), [](std::byte b) {
        return static_cast<char>(std::to_integer<unsigned>(b));
    });
    const Result<JsonValue> manifest = parse_json(manifest_text);
    if (!manifest.has_value()) {
        return fail(manifest.error());
    }
    if (manifest->kind != JsonValue::Kind::Object) {
        return malformed("package: manifest.json is not an object");
    }

    DfuPackage package;
    if (const JsonValue* version = manifest->find("format-version"); version != nullptr) {
        package.format_version = version->as_uint();
        if (!package.format_version.has_value()) {
            return malformed("package: format-version is not a non-negative integer");
        }
    }
    Result<std::optional<std::string>> name = optional_text(*manifest, "name");
    if (!name.has_value()) {
        return fail(name.error());
    }
    package.name = std::move(*name);

    const JsonValue* files = manifest->find("files");
    if (files == nullptr || files->kind != JsonValue::Kind::Array || files->items.empty()) {
        return malformed("package: the manifest lists no files");
    }
    if (files->items.size() > kMaxZipEntries) {
        return fail(ErrorCode::MessageTooLarge, "package: the manifest lists too many files");
    }

    const bool single = files->items.size() == 1;
    for (const JsonValue& entry : files->items) {
        Result<PackageImage> image = read_image(entry, *zip, single);
        if (!image.has_value()) {
            return fail(image.error());
        }
        const bool taken = std::ranges::any_of(package.images, [&image](const PackageImage& other) {
            return other.image == image->image;
        });
        if (taken) {
            // A direct-XIP build writes one file per slot of the same image;
            // those are alternatives, not a set to send (docs/protocol-notes.md).
            return fail(ErrorCode::InvalidArgument, "package: two files for one image");
        }
        package.images.push_back(std::move(*image));
    }
    std::ranges::sort(package.images, {}, &PackageImage::image);
    if (Result<void> consistent = check_dependencies(package); !consistent.has_value()) {
        return fail(consistent.error());
    }
    return package;
}

} // namespace smply::dfu_package
