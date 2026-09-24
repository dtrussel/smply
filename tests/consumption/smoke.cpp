// SPDX-License-Identifier: Apache-2.0
//
// What an out-of-tree consumer does: include the public headers, link the
// exported targets, and use them.
//
// **One program, three consumption modes.** find_package/, add_subdirectory/
// and fetchcontent/ each compile this same file (tools/check_install.sh drives
// all three). Sharing it is the point: three smoke programs would drift, and
// the mode that drifted would be the one nobody noticed had stopped checking.
//
// It exercises one thing from each installed target, and a decoder, so that a
// broken export shows up as a link error rather than as a header-only success
// that would have passed even if the archive were missing.

#include "common/ble_framing.hpp"
#include "common/send_queue.hpp"
#include "serial/serial_framing.hpp"

#include <smply/async/future.hpp>
#include <smply/async/task.hpp>
#include <smply/error.hpp>
#include <smply/smp/header.hpp>
#include <smply/util/dispatcher.hpp>
#include <smply/version.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace {

/// A coroutine over an operation that completes inline, so no client is needed
/// to drive it.
smply::async::Task<int> answer()
{
    const smply::Result<int> value =
        co_await smply::async::await_result<int>([](auto done) { done(42); });
    co_return value.value_or(0);
}

} // namespace

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
        std::cerr << "consumption check: header round trip failed\n";
        return EXIT_FAILURE;
    }

    // Also out of the archive: to_string(ErrorCode) is defined in core.cpp.
    if (smply::to_string(smply::ErrorCode::Timeout).empty()) {
        std::cerr << "consumption check: to_string returned nothing\n";
        return EXIT_FAILURE;
    }

    // smply::util -- the separate target. Dispatcher::post/drain are compiled
    // into libsmply_util.a, so this fails to link if that target was not
    // exported and installed.
    smply::Dispatcher dispatcher;
    int ran = 0;
    dispatcher.post([&ran] { ++ran; });
    if (dispatcher.drain() != 1 || ran != 1) {
        std::cerr << "consumption check: dispatcher did not run its closure\n";
        return EXIT_FAILURE;
    }

    // smply::asyncutil -- header-only, and linked through the export like the
    // others. A coroutine that completes inline, and a future fulfilled when
    // this thread drains the dispatcher (never get() on the pump thread before
    // that: see the header).
    smply::async::Task<int> task = answer();
    std::future<smply::Result<int>> future =
        smply::async::post_for_future<int>(dispatcher, [](auto done) { done(7); });
    dispatcher.drain();
    if (!task.done() || task.result() != 42 || future.get().value_or(0) != 7) {
        std::cerr << "consumption check: asyncutil did not deliver\n";
        return EXIT_FAILURE;
    }

    // smply::transport_common -- header-only, so this proves the *include root*
    // rather than an archive: an adapter out of the install tree must be able
    // to write "common/..." exactly as an in-tree one does.
    if (smply::transport::fragment_size(247) != 244) {
        std::cerr << "consumption check: fragment_size is wrong\n";
        return EXIT_FAILURE;
    }
    smply::transport::SendQueue queue;
    if (queue.offer(std::vector<std::byte>{std::byte{0}}) !=
        smply::transport::Admission::StartWriter) {
        std::cerr << "consumption check: first send was not admitted\n";
        return EXIT_FAILURE;
    }

    // serial/ ships under that same target and from the same include root
    // (ADR-0017), so a prefix that installed common/ and forgot serial/ fails
    // here and nowhere else. Round-tripping one packet exercises both halves.
    const std::vector<std::byte> packet{encoded.begin(), encoded.end()};
    smply::transport::SerialFramer framer{smply::ConstBytes{packet}};
    smply::transport::SerialDeframer deframer;
    smply::transport::LineSplitter splitter;
    std::array<std::byte, smply::transport::kMaxFrame> frame{};
    bool arrived = false;
    while (!framer.done()) {
        smply::ConstBytes rest{frame.data(), framer.next_frame(frame)};
        while (const auto line = splitter.next_line(rest)) {
            arrived =
                deframer.feed_line(*line) == smply::transport::SerialDeframer::Outcome::Packet;
        }
    }
    if (!arrived || deframer.packet().size() != packet.size()) {
        std::cerr << "consumption check: serial round trip failed\n";
        return EXIT_FAILURE;
    }

    // The header/library mismatch check version.hpp itself suggests: the macro
    // comes from the installed header, the function from the installed archive.
    // If a prefix ever mixed the two, this is where it shows.
    const std::string linked = smply::version();
    if (linked != SMPLY_VERSION_STRING) {
        std::cerr << "consumption check: header says " << SMPLY_VERSION_STRING << ", library says "
                  << linked << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "consumption check OK, smply " << linked << '\n';
    return EXIT_SUCCESS;
}
