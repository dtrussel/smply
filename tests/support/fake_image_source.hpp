// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_FAKE_IMAGE_SOURCE_HPP
#define SMPLY_TESTS_FAKE_IMAGE_SOURCE_HPP

/// \file
/// Image sources that misbehave in the two ways the contract forbids.
///
/// `MemoryImageSource` cannot fail and cannot read short, so without these the
/// error paths in `src/image/` would be unreachable from a test -- and an error
/// path no test can reach is indistinguishable from one that does not work.

#include "smply/error.hpp"
#include "smply/image_source.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>

namespace smply::test {

/// Fails every read. Stands in for a file that was deleted mid-operation, or a
/// download that dropped.
class FailingImageSource final : public ImageSource
{
public:
    explicit FailingImageSource(std::uint64_t size,
                                ErrorCode code = ErrorCode::TransportError) noexcept
        : size_{size}, code_{code}
    {}

    [[nodiscard]] std::uint64_t size() const noexcept override
    {
        return size_;
    }

    [[nodiscard]] Result<std::size_t> read(std::uint64_t, MutBytes) override
    {
        return fail(Error{code_, "fake image source"});
    }

private:
    std::uint64_t size_;
    ErrorCode code_;
};

/// Returns one byte per call, however many were asked for.
///
/// Legal only at the end of the image, so away from the end this is a broken
/// source. smply must refuse rather than loop: looping would turn a 16 MiB hash
/// into sixteen million calls.
class ShortReadingImageSource final : public ImageSource
{
public:
    explicit ShortReadingImageSource(std::uint64_t size) noexcept : size_{size} {}

    [[nodiscard]] std::uint64_t size() const noexcept override
    {
        return size_;
    }

    [[nodiscard]] Result<std::size_t> read(std::uint64_t, MutBytes out) override
    {
        if (out.empty()) {
            return std::size_t{0};
        }
        out[0] = std::byte{0};
        return std::size_t{1};
    }

private:
    std::uint64_t size_;
};

} // namespace smply::test

#endif // SMPLY_TESTS_FAKE_IMAGE_SOURCE_HPP
