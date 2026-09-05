// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_CBOR_CBOR_HPP
#define SMPLY_SRC_CBOR_CBOR_HPP

/// \file
/// The narrow CBOR façade (ADR-0007).
///
/// Internal to smply. QCBOR appears here and in `src/cbor/*.cpp` and nowhere
/// else: no public header may name it, and the API-discipline gate enforces
/// that. Replacing the backend means rewriting these two translation units,
/// and their test suite doubles as the conformance suite for any replacement.
///
/// Two properties shape the design, both consequences of the input being
/// untrusted:
///
/// * **Nothing allocates.** `Writer` encodes into a buffer the caller owns;
///   `Reader` returns views into the caller's input. A device cannot induce an
///   allocation by claiming a size.
/// * **Absent is not an error.** MCUmgr omits fields rather than sending false
///   or zero (docs/protocol-notes.md section 6), so a missing key yields
///   `std::nullopt` while a genuine decode failure sets a sticky status checked
///   once at the end. Mixing those two would force every call site to handle an
///   error that is really just an absent optional field.

#include "smply/bytes.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"

#include <qcbor/qcbor_decode.h>
#include <qcbor/qcbor_encode.h>
#include <qcbor/qcbor_spiffy_decode.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace smply::cbor {

/// Longest map key smply will encode or look up. MCUmgr's longest is
/// "boot_mode", at nine characters.
inline constexpr std::size_t kMaxKeyLength = 31;

/// Encodes a CBOR map into a caller-owned buffer.
///
/// Errors are sticky: an individual `put_*` never reports failure, and
/// `finish()` reports the first thing that went wrong. That keeps request
/// construction free of per-field error handling without losing the error --
/// running out of buffer is the case that matters, and it is reported once.
class Writer
{
public:
    /// \param out         Destination. Must outlive the Writer and the span
    ///                    that `finish()` returns.
    /// \param max_nesting Bound on map/array depth.
    explicit Writer(MutBytes out, unsigned max_nesting = limits::kMaxCborNesting) noexcept;

    Writer& open_map() noexcept;
    Writer& close_map() noexcept;

    /// Named rather than overloaded on purpose. CBOR distinguishes unsigned
    /// from negative integers on the wire, and MCUmgr fields have specific
    /// types, so letting overload resolution pick could silently produce a
    /// different encoding than the protocol asks for.
    Writer& put_uint(std::string_view key, std::uint64_t value) noexcept;
    Writer& put_int(std::string_view key, std::int64_t value) noexcept;
    Writer& put_bool(std::string_view key, bool value) noexcept;
    Writer& put_text(std::string_view key, std::string_view value) noexcept;
    Writer& put_bytes(std::string_view key, ConstBytes value) noexcept;

    /// Completes the document.
    ///
    /// \return The encoded bytes, as a view into the caller's buffer, or the
    ///         first error encountered -- most often `CborEncode` because the
    ///         buffer was too small.
    [[nodiscard]] Result<ConstBytes> finish() noexcept;

    /// True once something has gone wrong. Further writes are ignored.
    [[nodiscard]] bool failed() const noexcept
    {
        return failed_;
    }

private:
    /// Copies `key` into a null-terminated buffer, since QCBOR's map API takes
    /// a C string. Returns nullptr and sets the sticky error if it is too long.
    [[nodiscard]] const char* label(std::string_view key) noexcept;

    QCBOREncodeContext context_{};
    unsigned max_nesting_;
    unsigned depth_ = 0;
    bool failed_ = false;
    // NOLINTNEXTLINE(*-avoid-c-arrays): a fixed C string for QCBOR's label API.
    char label_[kMaxKeyLength + 1]{};
};

/// Decodes a CBOR map by key.
///
/// Non-copyable and non-movable: QCBOR's decode context holds interior
/// pointers, so the object must stay put once decoding has begun.
class Reader
{
public:
    /// \param input       The document. Must outlive the Reader and every view
    ///                    it returns.
    /// \param max_nesting Bound on map/array depth entered through this Reader.
    explicit Reader(ConstBytes input, unsigned max_nesting = limits::kMaxCborNesting) noexcept;

    Reader(const Reader&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader& operator=(Reader&&) = delete;
    ~Reader() = default;

    /// Enters the map at the current position. Required before any getter.
    [[nodiscard]] Result<void> enter_map() noexcept;
    /// Leaves the innermost map.
    [[nodiscard]] Result<void> leave_map() noexcept;

    /// Enters a map held under \p key, for nested structures.
    [[nodiscard]] Result<void> enter_map(std::string_view key) noexcept;

    /// \name Getters
    /// Each returns `std::nullopt` when the key is absent, which the protocol
    /// uses to mean "false" or "default". A key present with the wrong type is
    /// a genuine decode failure and sets the sticky status instead.
    /// @{
    [[nodiscard]] std::optional<std::uint64_t> uint(std::string_view key) noexcept;
    [[nodiscard]] std::optional<std::int64_t> integer(std::string_view key) noexcept;
    [[nodiscard]] std::optional<bool> boolean(std::string_view key) noexcept;
    /// Views into the input buffer; valid only while it lives.
    [[nodiscard]] std::optional<std::string_view> text(std::string_view key) noexcept;
    [[nodiscard]] std::optional<ConstBytes> bytes(std::string_view key) noexcept;
    /// @}

    /// Visits each map element of the array held under \p key.
    ///
    /// The callback runs with this Reader positioned inside one element, so it
    /// uses the same getters. Returning an error from the callback stops the
    /// iteration and propagates.
    ///
    /// \param max_elements Hard cap. A device cannot make smply iterate --
    ///                     or make the caller accumulate -- without bound.
    ///
    /// An absent key is not an error: the array is simply empty, which is how
    /// MCUmgr reports "no valid images".
    [[nodiscard]] Result<void>
    for_each_map_in_array(std::string_view key, std::size_t max_elements,
                          const std::function<Result<void>(Reader&)>& visit) noexcept;

    /// The first decode failure, if any. Absent keys are not failures.
    [[nodiscard]] Result<void> status() const noexcept;

    /// True when the document is a well-formed CBOR map and nothing has failed.
    [[nodiscard]] bool ok() const noexcept
    {
        return status().has_value();
    }

private:
    [[nodiscard]] const char* label(std::string_view key) noexcept;

    /// Maps QCBOR's error state onto ours, treating a missing label as absence
    /// rather than failure.
    ///
    /// \return true when the value was present and well-typed.
    [[nodiscard]] bool consume_lookup() noexcept;

    /// Records \p error as the first failure if none has been recorded yet, and
    /// returns it as a failed Result.
    ///
    /// Returning it rather than storing and separately re-reading it keeps the
    /// engagement of `error_` obvious at every call site -- the earlier
    /// store-then-dereference form relied on a postcondition a reader (and the
    /// static analyser) had to take on trust.
    [[nodiscard]] unexpected<Error> record(Error error) noexcept;

    QCBORDecodeContext context_{};
    std::optional<Error> error_;
    unsigned max_nesting_;
    unsigned depth_ = 0;
    // NOLINTNEXTLINE(*-avoid-c-arrays): a fixed C string for QCBOR's label API.
    char label_[kMaxKeyLength + 1]{};
};

} // namespace smply::cbor

#endif // SMPLY_SRC_CBOR_CBOR_HPP
