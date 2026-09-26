// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_ZIP_BUILDER_HPP
#define SMPLY_TESTS_ZIP_BUILDER_HPP

/// \file
/// Writes a zip archive byte by byte, for the DFU package reader to read.
///
/// Independent of `support/dfu_package/` for the same reason `ImageBuilder` is
/// independent of `src/image/`: every record is laid out here from APPNOTE.TXT,
/// so a reader bug cannot be cancelled out by a matching writer bug. The one
/// shared piece is the CRC, which is a checksum rather than a layout; a test
/// that needs a wrong one says so with `Entry::crc`.
///
/// Permissive, like `ImageBuilder`: each entry can lie about its method, its
/// sizes, its CRC or its flags, because those are the cases worth testing.

#include "dfu_package/stored_zip.hpp"

#include "smply/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smply::test {

class ZipBuilder
{
public:
    struct Entry
    {
        std::string name;
        std::vector<std::byte> data;
        std::uint16_t method = 0; ///< 0 stored, 8 deflated
        std::uint16_t flags = 0;
        /// Overrides the CRC written to both headers.
        std::optional<std::uint32_t> crc = std::nullopt;
        /// Overrides the central directory's uncompressed size.
        std::optional<std::uint32_t> uncompressed = std::nullopt;
        /// Written in the local header instead of `name`.
        std::optional<std::string> local_name = std::nullopt;
    };

    ZipBuilder& add(Entry entry)
    {
        entries_.push_back(std::move(entry));
        return *this;
    }

    ZipBuilder& add(std::string name, std::vector<std::byte> data)
    {
        return add(Entry{.name = std::move(name), .data = std::move(data)});
    }

    ZipBuilder& add(std::string name, std::string_view text)
    {
        std::vector<std::byte> data;
        for (const char c : text) {
            data.push_back(static_cast<std::byte>(c));
        }
        return add(std::move(name), std::move(data));
    }

    /// The end record's comment.
    ZipBuilder& comment(std::string text)
    {
        comment_ = std::move(text);
        return *this;
    }

    /// Overrides the end record's entry counts.
    ZipBuilder& entry_count(std::uint16_t value)
    {
        entry_count_ = value;
        return *this;
    }

    [[nodiscard]] std::vector<std::byte> build() const
    {
        std::vector<std::byte> out;
        std::vector<std::uint32_t> offsets;
        for (const Entry& entry : entries_) {
            offsets.push_back(static_cast<std::uint32_t>(out.size()));
            const std::string& local = entry.local_name.value_or(entry.name);
            u32(out, 0x04034B50U);
            u16(out, 20); // version needed
            u16(out, entry.flags);
            u16(out, entry.method);
            u16(out, 0);    // time
            u16(out, 0x21); // date
            u32(out, crc_of(entry));
            u32(out, size_of(entry.data)); // compressed
            u32(out, size_of(entry.data)); // uncompressed
            u16(out, static_cast<std::uint16_t>(local.size()));
            u16(out, 0); // extra
            text(out, local);
            out.insert(out.end(), entry.data.begin(), entry.data.end());
        }

        const auto directory = static_cast<std::uint32_t>(out.size());
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            const Entry& entry = entries_[index];
            u32(out, 0x02014B50U);
            u16(out, 20); // version made by
            u16(out, 20); // version needed
            u16(out, entry.flags);
            u16(out, entry.method);
            u16(out, 0);
            u16(out, 0x21);
            u32(out, crc_of(entry));
            u32(out, size_of(entry.data));
            u32(out, entry.uncompressed.value_or(size_of(entry.data)));
            u16(out, static_cast<std::uint16_t>(entry.name.size()));
            u16(out, 0); // extra
            u16(out, 0); // comment
            u16(out, 0); // disk
            u16(out, 0); // internal attributes
            u32(out, 0); // external attributes
            u32(out, offsets[index]);
            text(out, entry.name);
        }
        const auto directory_size = static_cast<std::uint32_t>(out.size()) - directory;

        const std::uint16_t count =
            entry_count_.value_or(static_cast<std::uint16_t>(entries_.size()));
        u32(out, 0x06054B50U);
        u16(out, 0);
        u16(out, 0);
        u16(out, count);
        u16(out, count);
        u32(out, directory_size);
        u32(out, directory);
        u16(out, static_cast<std::uint16_t>(comment_.size()));
        text(out, comment_);
        return out;
    }

private:
    static void u16(std::vector<std::byte>& out, std::uint16_t value)
    {
        out.push_back(static_cast<std::byte>(value & 0xFFU));
        out.push_back(static_cast<std::byte>(value >> 8U));
    }

    static void u32(std::vector<std::byte>& out, std::uint32_t value)
    {
        u16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
        u16(out, static_cast<std::uint16_t>(value >> 16U));
    }

    static void text(std::vector<std::byte>& out, std::string_view value)
    {
        for (const char c : value) {
            out.push_back(static_cast<std::byte>(c));
        }
    }

    [[nodiscard]] static std::uint32_t size_of(const std::vector<std::byte>& data)
    {
        return static_cast<std::uint32_t>(data.size());
    }

    [[nodiscard]] static std::uint32_t crc_of(const Entry& entry)
    {
        return entry.crc.value_or(dfu_package::crc32(ConstBytes{entry.data}));
    }

    std::vector<Entry> entries_;
    std::string comment_;
    std::optional<std::uint16_t> entry_count_;
};

} // namespace smply::test

#endif // SMPLY_TESTS_ZIP_BUILDER_HPP
