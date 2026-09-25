// SPDX-License-Identifier: Apache-2.0
//
// The multi-image DFU package reader over an arbitrary file (ADR-0021).
//
// A package is a file somebody handed the application, and the reader walks
// three formats the file controls -- a zip's directory, a JSON manifest and
// each image's MCUboot TLV areas -- every one of them by offsets and lengths
// the file supplies. The seed corpus includes a package written by nRF Connect
// SDK's own generate_zip.py, so mutation starts from the real layout.
//
// Properties beyond "no crash": a package that reads has images sorted by
// index, each index once and in range, each image a view inside the input,
// and each dependency list within the TLV bound. The JSON reader is run over
// the same bytes on its own as well, since the manifest reaches it only
// inside a valid zip.

#include "fuzz_support.hpp"

#include "dfu_package/dfu_package.hpp"
#include "dfu_package/json.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > smply::fuzz::kMaxUsefulInput) {
        return 0;
    }
    const smply::ConstBytes input = smply::fuzz::view(data, size);

    const smply::Result<smply::dfu_package::DfuPackage> package =
        smply::dfu_package::read_package(input);
    if (package.has_value()) {
        const auto* begin = input.data();
        const auto* end = input.data() + input.size();
        for (std::size_t index = 0; index < package->images.size(); ++index) {
            const smply::dfu_package::PackageImage& image = package->images[index];
            assert(image.image <= smply::dfu_package::kMaxImageIndex);
            assert(index == 0 || package->images[index - 1].image < image.image);
            assert(image.bytes.data() >= begin);
            assert(image.bytes.size() <= static_cast<std::size_t>(end - image.bytes.data()));
            assert(image.dependencies.size() <= smply::limits::kMaxImageTlvs);
        }
    }

    // The reader is only reached through the zip above; this reaches it
    // directly, with the fuzzer's bytes as the whole document.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    static_cast<void>(smply::dfu_package::parse_json(text));
    return 0;
}
