// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_IMAGE_BUILDER_HPP
#define SMPLY_TESTS_IMAGE_BUILDER_HPP

/// \file
/// Builds MCUboot images byte by byte, for the header parser and the TLV
/// scanner to read.
///
/// Deliberately independent of `src/image/`: every field is written here from
/// the layout table in docs/protocol-notes.md section 7, little-endian by hand,
/// so a parser bug cannot be cancelled out by a matching builder bug. The same
/// discipline as the CBOR builder in test_image_group.cpp.
///
/// It is also deliberately *permissive*. A builder that refused to emit a
/// malformed image would be useless for the cases that matter -- an area that
/// overruns the file, a protected size that disagrees with its own header --
/// so nothing here validates anything.

#include "smply/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace smply::test {

/// Assembles a header, a body and TLV areas.
class ImageBuilder
{
public:
    /// \name Header fields
    /// Each defaults to something plausible; a test overrides only what it is
    /// about.
    /// @{
    ImageBuilder& magic(std::uint32_t value)
    {
        magic_ = value;
        return *this;
    }

    ImageBuilder& header_size(std::uint16_t value)
    {
        header_size_ = value;
        return *this;
    }

    ImageBuilder& flags(std::uint32_t value)
    {
        flags_ = value;
        return *this;
    }

    ImageBuilder& version(std::uint8_t major, std::uint8_t minor, std::uint16_t revision,
                          std::uint32_t build)
    {
        major_ = major;
        minor_ = minor;
        revision_ = revision;
        build_ = build;
        return *this;
    }

    /// Sets the body length. The bytes themselves are filler.
    ImageBuilder& body(std::uint32_t length)
    {
        body_ = length;
        return *this;
    }

    /// @}

    /// Adds one TLV entry to the protected area.
    ImageBuilder& protected_tlv(std::uint16_t type, std::vector<std::byte> value)
    {
        protected_.push_back(Tlv{type, std::move(value)});
        return *this;
    }

    /// Adds one TLV entry to the unprotected area, where the hash TLVs live.
    ImageBuilder& tlv(std::uint16_t type, std::vector<std::byte> value)
    {
        unprotected_.push_back(Tlv{type, std::move(value)});
        return *this;
    }

    /// \name Malformation knobs
    /// Each overrides exactly one field, so a test can express a *specific*
    /// inconsistency. Overriding two fields at once -- which an earlier version
    /// of this builder did -- silently keeps them agreeing, and the test then
    /// passes for a reason it does not name.
    /// @{

    /// Overrides `ih_protect_tlv_size` in the header **only**, leaving the
    /// protected area's own `it_tlv_tot` alone.
    ImageBuilder& header_protected_size(std::uint16_t value)
    {
        header_protected_size_ = value;
        return *this;
    }

    /// Overrides the protected area's own `it_tlv_tot` **only**.
    ImageBuilder& protected_area_total(std::uint16_t value)
    {
        protected_area_total_ = value;
        return *this;
    }

    /// Emits, or suppresses, the protected area regardless of its entries.
    ImageBuilder& emit_protected_area(bool value)
    {
        emit_protected_ = value;
        return *this;
    }

    /// Overrides the magic of the protected area's header.
    ImageBuilder& protected_area_magic(std::uint16_t value)
    {
        protected_area_magic_ = value;
        return *this;
    }

    /// Overrides the `it_tlv_tot` written for the unprotected area, so a test
    /// can claim an area larger than the file.
    ImageBuilder& unprotected_total(std::uint16_t value)
    {
        unprotected_total_ = value;
        return *this;
    }

    /// Overrides the magic of the unprotected area's header.
    ImageBuilder& unprotected_area_magic(std::uint16_t value)
    {
        unprotected_area_magic_ = value;
        return *this;
    }

    /// @}

    /// The protected area's true size: its own four-byte header plus its
    /// entries, which is what `ih_protect_tlv_size` must equal.
    [[nodiscard]] std::uint16_t computed_protected_total() const
    {
        if (protected_.empty() && !emit_protected_) {
            return 0;
        }
        return static_cast<std::uint16_t>(kAreaHeader + payload_size(protected_));
    }

    [[nodiscard]] std::vector<std::byte> build() const
    {
        std::vector<std::byte> out;

        const std::uint16_t true_protected_total = computed_protected_total();
        const std::uint16_t header_protected_total =
            header_protected_size_.has_value() ? *header_protected_size_ : true_protected_total;

        put32(out, magic_);
        put32(out, 0); // ih_load_addr
        put16(out, header_size_);
        put16(out, header_protected_total);
        put32(out, body_);
        put32(out, flags_);
        out.push_back(static_cast<std::byte>(major_));
        out.push_back(static_cast<std::byte>(minor_));
        put16(out, revision_);
        put32(out, build_);
        put32(out, 0); // _pad1

        // Padding out to ih_hdr_size, then the body.
        while (out.size() < header_size_) {
            out.push_back(std::byte{0});
        }
        for (std::uint32_t i = 0; i < body_; ++i) {
            out.push_back(static_cast<std::byte>(i & 0xFFU));
        }

        if (!protected_.empty() || emit_protected_) {
            put16(out, protected_area_magic_.value_or(kProtInfoMagic));
            put16(out, protected_area_total_.value_or(true_protected_total));
            for (const Tlv& entry : protected_) {
                put_tlv(out, entry);
            }
        }

        const std::uint16_t unprotected_total =
            unprotected_total_.has_value()
                ? *unprotected_total_
                : static_cast<std::uint16_t>(kAreaHeader + payload_size(unprotected_));
        put16(out, unprotected_area_magic_.value_or(kInfoMagic));
        put16(out, unprotected_total);
        for (const Tlv& entry : unprotected_) {
            put_tlv(out, entry);
        }

        return out;
    }

private:
    struct Tlv
    {
        std::uint16_t type;
        std::vector<std::byte> value;
    };

    static constexpr std::uint16_t kInfoMagic = 0x6907;
    static constexpr std::uint16_t kProtInfoMagic = 0x6908;
    static constexpr std::size_t kAreaHeader = 4;

    static void put16(std::vector<std::byte>& out, std::uint16_t value)
    {
        out.push_back(static_cast<std::byte>(value & 0xFFU));
        out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    }

    static void put32(std::vector<std::byte>& out, std::uint32_t value)
    {
        out.push_back(static_cast<std::byte>(value & 0xFFU));
        out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
        out.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
    }

    static void put_tlv(std::vector<std::byte>& out, const Tlv& entry)
    {
        put16(out, entry.type);
        put16(out, static_cast<std::uint16_t>(entry.value.size()));
        out.insert(out.end(), entry.value.begin(), entry.value.end());
    }

    [[nodiscard]] static std::size_t payload_size(const std::vector<Tlv>& entries)
    {
        std::size_t total = 0;
        for (const Tlv& entry : entries) {
            total += 4 + entry.value.size();
        }
        return total;
    }

    std::uint32_t magic_ = 0x96F3B83DU;
    std::uint16_t header_size_ = 32;
    std::optional<std::uint16_t> header_protected_size_;
    std::optional<std::uint16_t> protected_area_total_;
    std::optional<std::uint16_t> protected_area_magic_;
    bool emit_protected_ = false;
    std::uint32_t body_ = 64;
    std::uint32_t flags_ = 0;
    std::uint8_t major_ = 1;
    std::uint8_t minor_ = 2;
    std::uint16_t revision_ = 3;
    std::uint32_t build_ = 4;
    std::vector<Tlv> protected_;
    std::vector<Tlv> unprotected_;
    std::optional<std::uint16_t> unprotected_total_;
    std::optional<std::uint16_t> unprotected_area_magic_;
};

/// `count` distinguishable filler bytes, so a misplaced copy is visible.
inline std::vector<std::byte> digest_of(std::size_t count, std::uint8_t first = 0)
{
    std::vector<std::byte> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<std::byte>((first + i) & 0xFFU));
    }
    return out;
}

} // namespace smply::test

#endif // SMPLY_TESTS_IMAGE_BUILDER_HPP
