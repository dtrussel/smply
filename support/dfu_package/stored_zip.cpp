// SPDX-License-Identifier: Apache-2.0

#include "dfu_package/stored_zip.hpp"

#include "smply/error.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace smply::dfu_package {
namespace {

// The record signatures and fixed sizes of APPNOTE.TXT sections 4.3.7, 4.3.12
// and 4.3.16.
constexpr std::uint32_t kLocalHeaderSignature = 0x04034B50U;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014B50U;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054B50U;
constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::size_t kCentralHeaderSize = 46;
constexpr std::size_t kEndOfCentralDirectorySize = 22;
/// The end record's comment is at most this long, which bounds the search.
constexpr std::size_t kMaxCommentLength = 0xFFFF;

constexpr std::uint16_t kFlagEncrypted = 0x0001U;
constexpr std::uint16_t kFlagStrongEncryption = 0x0040U;
constexpr std::uint16_t kMethodStored = 0;
constexpr std::uint16_t kMethodDeflated = 8;

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

[[nodiscard]] unexpected<Error> unsupported(const char* what) noexcept
{
    return fail(ErrorCode::InvalidArgument, what);
}

/// The CRC-32 lookup table, one entry per byte value.
[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0 ? (value >> 1U) ^ 0xEDB88320U : value >> 1U;
        }
        table[index] = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

/// The end of central directory record: where the directory is, and how big.
struct Directory
{
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t entries = 0;
};

/// Finds the end record, searching back from the end of the archive.
///
/// A candidate counts only if its comment length reaches exactly to the end of
/// the archive, so a signature that happens to sit inside a comment is not
/// mistaken for the record.
[[nodiscard]] Result<Directory> find_directory(ConstBytes archive)
{
    if (archive.size() < kEndOfCentralDirectorySize) {
        return malformed("zip: shorter than an end of central directory record");
    }
    const std::size_t last = archive.size() - kEndOfCentralDirectorySize;
    const std::size_t first = last > kMaxCommentLength ? last - kMaxCommentLength : 0;
    for (std::size_t at = last + 1; at-- > first;) {
        if (le32(archive, at) != kEndOfCentralDirectorySignature ||
            le16(archive, at + 20) != last - at) {
            continue;
        }
        const std::uint16_t disk = le16(archive, at + 4);
        const std::uint16_t directory_disk = le16(archive, at + 6);
        const std::uint16_t on_disk = le16(archive, at + 8);
        const std::uint16_t total = le16(archive, at + 10);
        const std::uint32_t size = le32(archive, at + 12);
        const std::uint32_t offset = le32(archive, at + 16);
        if (total == 0xFFFFU || size == 0xFFFFFFFFU || offset == 0xFFFFFFFFU) {
            return unsupported("zip: zip64 archives are not supported");
        }
        if (disk != 0 || directory_disk != 0 || on_disk != total) {
            return unsupported("zip: archives spanning several disks are not supported");
        }
        if (total > kMaxZipEntries) {
            return fail(ErrorCode::MessageTooLarge, "zip: too many entries");
        }
        if (offset > at || size > at - offset) {
            return malformed("zip: central directory outside the archive");
        }
        return Directory{.offset = offset, .size = size, .entries = total};
    }
    return malformed("zip: no end of central directory record");
}

/// One entry, from its central directory header and its local header.
[[nodiscard]] Result<ZipEntry> read_entry(ConstBytes archive, const Directory& directory,
                                          std::size_t& cursor)
{
    const std::size_t end = directory.offset + directory.size;
    if (kCentralHeaderSize > end - cursor || le32(archive, cursor) != kCentralHeaderSignature) {
        return malformed("zip: bad central directory header");
    }
    const ConstBytes header = archive.subspan(cursor, kCentralHeaderSize);
    const std::uint16_t flags = le16(header, 8);
    const std::uint16_t method = le16(header, 10);
    const std::uint32_t crc = le32(header, 16);
    const std::uint32_t compressed = le32(header, 20);
    const std::uint32_t uncompressed = le32(header, 24);
    const std::size_t name_length = le16(header, 28);
    const std::size_t variable = name_length + le16(header, 30) + le16(header, 32);
    const std::uint32_t local = le32(header, 42);

    if (variable > end - cursor - kCentralHeaderSize) {
        return malformed("zip: central directory header overruns the directory");
    }
    if (name_length == 0 || name_length > kMaxZipNameLength) {
        return malformed("zip: entry name empty or too long");
    }
    const auto* name_bytes = archive.subspan(cursor + kCentralHeaderSize, name_length).data();
    std::string name(name_length, '\0');
    std::transform(name_bytes, name_bytes + name_length, name.begin(),
                   [](std::byte b) { return static_cast<char>(std::to_integer<unsigned>(b)); });
    cursor += kCentralHeaderSize + variable;

    if ((flags & (kFlagEncrypted | kFlagStrongEncryption)) != 0) {
        return unsupported("zip: encrypted entries are not supported");
    }
    if (method == kMethodDeflated) {
        return unsupported("zip: deflated entries are not supported; the package must be stored");
    }
    if (method != kMethodStored) {
        return unsupported("zip: compressed entries are not supported");
    }
    if (compressed != uncompressed) {
        return malformed("zip: stored entry with two different sizes");
    }

    // The local header, and the data after it, must lie before the directory.
    if (local > directory.offset || kLocalHeaderSize > directory.offset - local ||
        le32(archive, local) != kLocalHeaderSignature) {
        return malformed("zip: bad local header");
    }
    const std::size_t local_name = le16(archive, local + 26);
    const std::size_t local_extra = le16(archive, local + 28);
    const std::size_t room = directory.offset - local - kLocalHeaderSize;
    if (local_name != name_length || local_name + local_extra > room ||
        compressed > room - local_name - local_extra) {
        return malformed("zip: entry overruns the archive");
    }
    const ConstBytes local_name_bytes = archive.subspan(local + kLocalHeaderSize, local_name);
    if (!std::equal(local_name_bytes.begin(), local_name_bytes.end(), name_bytes)) {
        return malformed("zip: local and central names disagree");
    }

    const ConstBytes data =
        archive.subspan(local + kLocalHeaderSize + local_name + local_extra, compressed);
    if (crc32(data) != crc) {
        return malformed("zip: CRC-32 mismatch");
    }
    return ZipEntry{.name = std::move(name), .data = data};
}

} // namespace

std::uint32_t crc32(ConstBytes data) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte byte : data) {
        crc = kCrcTable[(crc ^ std::to_integer<std::uint32_t>(byte)) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

Result<std::vector<ZipEntry>> read_stored_zip(ConstBytes archive)
{
    const Result<Directory> directory = find_directory(archive);
    if (!directory.has_value()) {
        return fail(directory.error());
    }

    std::vector<ZipEntry> entries;
    entries.reserve(directory->entries);
    std::size_t cursor = directory->offset;
    for (std::size_t index = 0; index < directory->entries; ++index) {
        Result<ZipEntry> entry = read_entry(archive, *directory, cursor);
        if (!entry.has_value()) {
            return fail(entry.error());
        }
        if (entry->name.back() == '/') {
            continue; // A directory: nothing to read.
        }
        const bool seen = std::ranges::any_of(
            entries, [&entry](const ZipEntry& other) { return other.name == entry->name; });
        if (seen) {
            return malformed("zip: a name appears twice");
        }
        entries.push_back(std::move(*entry));
    }
    return entries;
}

} // namespace smply::dfu_package
