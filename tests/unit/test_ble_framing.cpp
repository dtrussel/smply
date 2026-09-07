// SPDX-License-Identifier: Apache-2.0
//
// The portable half of the BLE transport: fragment sizing, the fragment walk,
// and the close-state bookkeeping.
//
// This is P15's "testable without a radio" logic, and it is tested here rather
// than under transports/winrt_ble/ because none of it is Windows-specific. The
// WinRT adapter (P15b) cannot be built or run on this machine at all, so
// whatever it can be made to rest on is worth resting on it: an off-by-one in
// fragment sizing would otherwise be discoverable only with a device in hand.

#include "common/ble_framing.hpp"
#include "common/link_state.hpp"

#include "smply/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

using smply::ConstBytes;
using smply::transport::fragment_size;
using smply::transport::Fragmenter;
using smply::transport::LinkPhase;
using smply::transport::LinkState;

namespace {

/// A message whose every byte is distinguishable, so a fragment walk that
/// duplicates, drops or reorders shows up as wrong *content* and not merely a
/// wrong count.
[[nodiscard]] std::vector<std::byte> counted(std::size_t size)
{
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(i & 0xFFU);
    }
    return out;
}

/// Reassembles a fragmenter's output the way GATT's ordering guarantee lets a
/// receiver do it: concatenate, no framing (protocol-notes section 8).
[[nodiscard]] std::vector<std::byte> drain(Fragmenter& source)
{
    std::vector<std::byte> out;
    while (!source.done()) {
        const ConstBytes piece = source.next();
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
}

} // namespace

// --- fragment_size ----------------------------------------------------------

TEST_CASE("the minimum ATT MTU yields a 20-byte fragment", "[ble][framing]")
{
    // 23 is the smallest MTU the Bluetooth core specification allows, so 20 is
    // the smallest fragment a conforming link can ever produce. Every other
    // case is bigger; this is the one worth pinning by hand.
    CHECK(fragment_size(smply::transport::kMinAttMtu) == 20);
    CHECK(fragment_size(23) == 20);
}

TEST_CASE("a fragment is the PDU less the three-byte ATT header", "[ble][framing]")
{
    CHECK(fragment_size(27) == 24);   // the classic 4.0 data-length MTU
    CHECK(fragment_size(247) == 244); // what most modern stacks negotiate
    CHECK(fragment_size(185) == 182); // iOS's usual answer
}

TEST_CASE("fragment sizing clamps at both ends", "[ble][framing]")
{
    // Below the floor: a stack that reports a sub-specification PDU, or none at
    // all before the link is up. Without the clamp this is a fragment size of
    // zero and an adapter that sends forever without progressing.
    CHECK(fragment_size(0) == smply::transport::kMinFragment);
    CHECK(fragment_size(1) == smply::transport::kMinFragment);
    CHECK(fragment_size(3) == smply::transport::kMinFragment);  // exactly the header
    CHECK(fragment_size(4) == smply::transport::kMinFragment);  // one usable byte
    CHECK(fragment_size(22) == smply::transport::kMinFragment); // just under the minimum

    // Above the ceiling: a stack may report a large PDU; we do not trust it to
    // accept one, and 512 already costs nothing in round trips.
    CHECK(fragment_size(517) == smply::transport::kMaxFragment);
    CHECK(fragment_size(65535) == smply::transport::kMaxFragment);

    // And the boundary itself is not clamped away.
    CHECK(fragment_size(smply::transport::kMaxFragment + smply::transport::kAttHeaderSize) ==
          smply::transport::kMaxFragment);
}

// --- Fragmenter -------------------------------------------------------------

TEST_CASE("a message shorter than one fragment goes out whole", "[ble][framing]")
{
    const std::vector<std::byte> message = counted(8); // a bare SMP header
    Fragmenter source{ConstBytes{message}, 20};

    REQUIRE_FALSE(source.done());
    CHECK(source.count() == 1);
    const ConstBytes only = source.next();
    CHECK(only.size() == 8);
    CHECK(source.done());
    CHECK(source.remaining() == 0);
}

TEST_CASE("an exact multiple produces no empty final fragment", "[ble][framing]")
{
    // The off-by-one that would put a zero-length GATT write on the air.
    const std::vector<std::byte> message = counted(60);
    Fragmenter source{ConstBytes{message}, 20};

    CHECK(source.count() == 3);
    CHECK(source.next().size() == 20);
    CHECK(source.next().size() == 20);
    CHECK(source.next().size() == 20);
    CHECK(source.done());
}

TEST_CASE("a remainder becomes a short final fragment", "[ble][framing]")
{
    const std::vector<std::byte> message = counted(65);
    Fragmenter source{ConstBytes{message}, 20};

    CHECK(source.count() == 4);
    CHECK(source.next().size() == 20);
    CHECK(source.next().size() == 20);
    CHECK(source.next().size() == 20);
    CHECK(source.next().size() == 5);
    CHECK(source.done());
}

TEST_CASE("fragments reassemble to the original bytes", "[ble][framing]")
{
    // The property that matters: "no additional framing" means concatenating
    // the fragments *is* the message. Checked over sizes that straddle every
    // boundary, at the smallest fragment a link can produce.
    for (const std::size_t size : {1U, 8U, 19U, 20U, 21U, 39U, 40U, 41U, 512U, 8192U}) {
        const std::vector<std::byte> message = counted(size);
        Fragmenter source{ConstBytes{message}, 20};
        const std::vector<std::byte> rebuilt = drain(source);

        INFO("message of " << size << " bytes");
        REQUIRE(rebuilt.size() == message.size());
        CHECK(rebuilt == message);
    }
}

TEST_CASE("remaining counts down to zero", "[ble][framing]")
{
    const std::vector<std::byte> message = counted(50);
    Fragmenter source{ConstBytes{message}, 20};

    CHECK(source.remaining() == 50);
    static_cast<void>(source.next());
    CHECK(source.remaining() == 30);
    static_cast<void>(source.next());
    CHECK(source.remaining() == 10);
    static_cast<void>(source.next());
    CHECK(source.remaining() == 0);
    CHECK(source.done());
}

TEST_CASE("an exhausted fragmenter yields nothing rather than misbehaving", "[ble][framing]")
{
    const std::vector<std::byte> message = counted(4);
    Fragmenter source{ConstBytes{message}, 20};
    static_cast<void>(source.next());

    REQUIRE(source.done());
    CHECK(source.next().empty());
    CHECK(source.next().empty());
    CHECK(source.remaining() == 0);
}

TEST_CASE("an empty message yields no fragments", "[ble][framing]")
{
    // Not reachable from SmpClient -- a message is at least an 8-byte header --
    // so this pins that a caller bug does not become a zero-length GATT write,
    // which would put a packet meaning nothing on the air.
    const std::vector<std::byte> nothing;
    Fragmenter source{ConstBytes{nothing}, 20};

    CHECK(source.done());
    CHECK(source.count() == 0);
    CHECK(source.next().empty());
}

TEST_CASE("a zero fragment size degrades rather than hanging", "[ble][framing]")
{
    // fragment_size() never returns zero, so this is defence against a caller
    // that computed its own. One byte at a time is slow; an infinite loop is a
    // hung adapter.
    const std::vector<std::byte> message = counted(3);
    Fragmenter source{ConstBytes{message}, 0};

    CHECK(source.next().size() == 1);
    CHECK(source.next().size() == 1);
    CHECK(source.next().size() == 1);
    CHECK(source.done());
}

// --- LinkState --------------------------------------------------------------

TEST_CASE("a fresh link is open and usable", "[ble][link]")
{
    const LinkState link;
    CHECK(link.phase() == LinkPhase::Open);
    CHECK(link.is_open());
    CHECK(link.may_send());
    CHECK(link.may_deliver());
    CHECK_FALSE(link.is_closing_or_closed());
}

TEST_CASE("close is idempotent, and says which call did the work", "[ble][link]")
{
    LinkState link;

    REQUIRE(link.begin_close()); // the caller that must tear down
    CHECK(link.phase() == LinkPhase::Closing);

    // Every later attempt is a no-op. This is what makes an adapter's close()
    // idempotent without it tracking a second flag of its own -- and what stops
    // a destructor that calls close() from tearing down twice.
    CHECK_FALSE(link.begin_close());
    link.finish_close();
    CHECK_FALSE(link.begin_close());
    CHECK(link.phase() == LinkPhase::Closed);
}

TEST_CASE("callbacks stop when close begins, not when it ends", "[ble][link]")
{
    // The rule adapters get wrong. At begin_close() there are typically
    // callbacks already queued or in flight; design.md section 9 says none of
    // them may arrive, so the answer has to change here rather than at
    // finish_close().
    LinkState link;
    REQUIRE(link.may_deliver());

    REQUIRE(link.begin_close());
    CHECK_FALSE(link.may_deliver());
    CHECK_FALSE(link.may_send());

    link.finish_close();
    CHECK_FALSE(link.may_deliver());
    CHECK_FALSE(link.may_send());
}

TEST_CASE("a closed link stays closed", "[ble][link]")
{
    // Terminal by design: reopening would let a stale TransportListener come
    // back after SmpClient had detached. Reconnecting means a new transport and
    // rebind_transport(), which is what FakeTransport and cli_dfu both do.
    LinkState link;
    REQUIRE(link.begin_close());
    link.finish_close();

    CHECK(link.phase() == LinkPhase::Closed);
    CHECK(link.is_closing_or_closed());
    link.finish_close(); // harmless
    CHECK(link.phase() == LinkPhase::Closed);
}
