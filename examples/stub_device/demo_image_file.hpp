// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_DEMO_IMAGE_FILE_HPP
#define SMPLY_EXAMPLE_DEMO_IMAGE_FILE_HPP

/// \file
/// A generated image, as a file: the examples read their firmware through a
/// real `FileImageSource` even when they invent it.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smply::example {

/// A temporary file holding a generated image, removed when this object goes,
/// however the run ends.
///
/// Into the temporary directory rather than the working one: the examples run
/// as `ctest` tests, and a test that drops files into the build tree makes the
/// next build's diff noisy.
///
/// **The name is unique per run.** Several runs go at once under `ctest -j`,
/// and with one shared name each truncated the file another was reading. The
/// failure was a short read, which also let a `WILL_FAIL` test pass for the
/// wrong reason.
class DemoImageFile
{
public:
    /// \param tag Starts the file's name, so a leftover file says whose it is.
    explicit DemoImageFile(std::string_view tag) : tag_{tag} {}

    DemoImageFile(const DemoImageFile&) = delete;
    DemoImageFile& operator=(const DemoImageFile&) = delete;
    DemoImageFile(DemoImageFile&&) = delete;
    DemoImageFile& operator=(DemoImageFile&&) = delete;
    ~DemoImageFile();

    /// Writes \p image to a new file and returns its path, or nothing.
    [[nodiscard]] std::optional<std::string> write(const std::vector<std::byte>& image);

private:
    std::string tag_;
    std::string path_;
};

} // namespace smply::example

#endif // SMPLY_EXAMPLE_DEMO_IMAGE_FILE_HPP
