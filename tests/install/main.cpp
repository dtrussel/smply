// SPDX-License-Identifier: Apache-2.0
//
// What an out-of-tree consumer does: include the public headers, link the
// exported targets, and use them. Built by tools/check_install.sh against an
// installed prefix, never inside smply's own build tree.
//
// It exercises one thing from each exported target, and a decoder, so that a
// broken export shows up as a link error rather than as a header-only success
// that would have passed even if the archive were missing.

#include <smply/error.hpp>
#include <smply/smp/header.hpp>
#include <smply/util/dispatcher.hpp>
#include <smply/version.hpp>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

int main()
{
    // smply::smply -- a real symbol from the archive, not an inline function.
    const smply::Header header{.op = smply::Operation::Read,
                               .version = smply::Version::V1,
                               .flags = 0,
                               .length = 0,
                               .group = smply::Group::Image,
                               .seq = 7,
                               .command = 0};
    const auto encoded = smply::encode(header);
    const smply::Result<smply::Header> decoded = smply::decode_header(smply::ConstBytes{encoded});
    if (!decoded.has_value() || decoded->seq != 7) {
        std::cerr << "install check: header round trip failed\n";
        return EXIT_FAILURE;
    }

    // Also out of the archive: to_string(ErrorCode) is defined in core.cpp.
    if (smply::to_string(smply::ErrorCode::Timeout).empty()) {
        std::cerr << "install check: to_string returned nothing\n";
        return EXIT_FAILURE;
    }

    // smply::util -- the separate target. Dispatcher::post/drain are compiled
    // into libsmply_util.a, so this fails to link if that target was not
    // exported and installed.
    smply::Dispatcher dispatcher;
    int ran = 0;
    dispatcher.post([&ran] { ++ran; });
    if (dispatcher.drain() != 1 || ran != 1) {
        std::cerr << "install check: dispatcher did not run its closure\n";
        return EXIT_FAILURE;
    }

    // The header/library mismatch check version.hpp itself suggests: the macro
    // comes from the installed header, the function from the installed archive.
    // If a prefix ever mixed the two, this is where it shows.
    const std::string linked = smply::version();
    if (linked != SMPLY_VERSION_STRING) {
        std::cerr << "install check: header says " << SMPLY_VERSION_STRING << ", library says "
                  << linked << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "install check OK, smply " << linked << '\n';
    return EXIT_SUCCESS;
}
