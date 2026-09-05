// SPDX-License-Identifier: Apache-2.0
//
// The four shapes a device may use to report success or failure
// (docs/protocol-notes.md section 3). Getting this wrong is not a cosmetic bug:
// mistaking an error for a success would let a firmware update proceed past a
// device that refused a step.

#include "cbor/mgmt_error.hpp"

#include "message_builder.hpp"
#include "smply/group.hpp"
#include "smply/limits.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::Group;
using smply::cbor::extract_mgmt_error;
using smply::test::bytes_of;

// ---------------------------------------------------------------------------
// Success
// ---------------------------------------------------------------------------

TEST_CASE("an empty map is success", "[mgmt-error]")
{
    // The ordinary successful response: MCUmgr returns rc/err only on failure,
    // so an empty map means the command worked.
    const auto payload = bytes_of({0xA0});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->failed());
    REQUIRE(outcome->reason.empty());
}

TEST_CASE("a map with unrelated keys is success", "[mgmt-error]")
{
    // {"off": 512} -- a successful upload response carries data, not a code.
    const auto payload = bytes_of({0xA1, 0x63, 0x6F, 0x66, 0x66, 0x19, 0x02, 0x00});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->failed());
}

TEST_CASE("an explicit rc of zero is success", "[mgmt-error]")
{
    // {"rc": 0}. Some configurations send it (Zephyr's legacy-rc behaviour),
    // and zero is MGMT_ERR_EOK -- treating it as a failure would break those
    // devices entirely.
    const auto payload = bytes_of({0xA1, 0x62, 0x72, 0x63, 0x00});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->failed());
}

TEST_CASE("a group-scoped rc of zero is success", "[mgmt-error]")
{
    // {"err": {"group": 1, "rc": 0}}
    const auto payload = bytes_of({0xA1, 0x63, 0x65, 0x72, 0x72, 0xA2, 0x65, 0x67, 0x72, 0x6F, 0x75,
                                   0x70, 0x01, 0x62, 0x72, 0x63, 0x00});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->failed());
}

// ---------------------------------------------------------------------------
// SMP v1: a flat rc
// ---------------------------------------------------------------------------

TEST_CASE("a flat rc is an SMP-level error", "[mgmt-error]")
{
    // {"rc": 3} -- MGMT_ERR_EINVAL.
    const auto payload = bytes_of({0xA1, 0x62, 0x72, 0x63, 0x03});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->failed());
    REQUIRE(outcome->error->rc == 3);
    // Not group-scoped: the code is drawn from mcumgr_err_t, and reading it
    // against a group would be a category error.
    REQUIRE_FALSE(outcome->error->group_scoped);
}

TEST_CASE("a large flat rc is preserved", "[mgmt-error]")
{
    // {"rc": 256} -- MGMT_ERR_EPERUSER, the base of the user-defined range.
    const auto payload = bytes_of({0xA1, 0x62, 0x72, 0x63, 0x19, 0x01, 0x00});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->failed());
    REQUIRE(outcome->error->rc == 256);
}

// ---------------------------------------------------------------------------
// SMP v2: a group-scoped err map
// ---------------------------------------------------------------------------

TEST_CASE("a group-scoped error carries its group", "[mgmt-error]")
{
    // {"err": {"group": 1, "rc": 30}} -- image group,
    // IMG_MGMT_ERR_INVALID_IMAGE_TOO_LARGE.
    const auto payload = bytes_of({0xA1, 0x63, 0x65, 0x72, 0x72, 0xA2, 0x65, 0x67, 0x72, 0x6F, 0x75,
                                   0x70, 0x01, 0x62, 0x72, 0x63, 0x18, 0x1E});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->failed());
    REQUIRE(outcome->error->group == Group::Image);
    REQUIRE(outcome->error->rc == 30);
    REQUIRE(outcome->error->group_scoped);
}

TEST_CASE("the same rc in different groups produces different errors", "[mgmt-error]")
{
    // This is the whole reason MgmtError carries the group. Image rc 3 is
    // IMG_MGMT_ERR_NO_IMAGE; OS rc 3 is OS_MGMT_ERR_QUERY_YIELDS_NO_ANSWER;
    // and a flat rc 3 is MGMT_ERR_EINVAL. Three unrelated failures.
    const auto image = bytes_of({0xA1, 0x63, 0x65, 0x72, 0x72, 0xA2, 0x65, 0x67, 0x72, 0x6F, 0x75,
                                 0x70, 0x01, 0x62, 0x72, 0x63, 0x03});
    const auto os = bytes_of({0xA1, 0x63, 0x65, 0x72, 0x72, 0xA2, 0x65, 0x67, 0x72, 0x6F, 0x75,
                              0x70, 0x00, 0x62, 0x72, 0x63, 0x03});
    const auto flat = bytes_of({0xA1, 0x62, 0x72, 0x63, 0x03});

    const auto image_outcome = extract_mgmt_error(ConstBytes{image});
    const auto os_outcome = extract_mgmt_error(ConstBytes{os});
    const auto flat_outcome = extract_mgmt_error(ConstBytes{flat});

    REQUIRE(image_outcome.has_value());
    REQUIRE(os_outcome.has_value());
    REQUIRE(flat_outcome.has_value());

    REQUIRE_FALSE(*image_outcome->error == *os_outcome->error);
    REQUIRE_FALSE(*image_outcome->error == *flat_outcome->error);
    REQUIRE_FALSE(*os_outcome->error == *flat_outcome->error);
}

TEST_CASE("an unknown group is preserved rather than rejected", "[mgmt-error]")
{
    // {"err": {"group": 9000, "rc": 7}} -- a vendor group. These enumerations
    // grow, and a code we cannot name is still worth reporting numerically.
    const auto payload = bytes_of({0xA1, 0x63, 0x65, 0x72, 0x72, 0xA2, 0x65, 0x67, 0x72, 0x6F, 0x75,
                                   0x70, 0x19, 0x23, 0x28, 0x62, 0x72, 0x63, 0x07});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->failed());
    REQUIRE(smply::to_underlying(outcome->error->group) == 9000);
    REQUIRE(outcome->error->rc == 7);
}

TEST_CASE("an err map without an rc reports no error", "[mgmt-error]")
{
    // {"err": {"group": 1}} -- says nothing usable. Inventing a code would be
    // worse than reporting success, since the response carried no failure.
    const auto payload =
        bytes_of({0xA1, 0x63, 0x65, 0x72, 0x72, 0xA1, 0x65, 0x67, 0x72, 0x6F, 0x75, 0x70, 0x01});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->failed());
}

// ---------------------------------------------------------------------------
// The fourth shape: a v2 device reporting an SMP-level error as a flat rc.
// ---------------------------------------------------------------------------

TEST_CASE("a v2 client still understands a flat rc", "[mgmt-error]")
{
    // The specification is explicit: under SMP v2, errors relating to SMP
    // itself are still returned as a flat rc, and a v2 client must handle both.
    // So the flat shape is decoded regardless of which version was requested.
    const auto payload = bytes_of({0xA1, 0x62, 0x72, 0x63, 0x08}); // MGMT_ERR_ENOTSUP

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->failed());
    REQUIRE(outcome->error->rc == 8);
    REQUIRE_FALSE(outcome->error->group_scoped);
}

TEST_CASE("when both shapes appear the group-scoped one wins", "[mgmt-error]")
{
    // {"err": {"group": 1, "rc": 5}, "rc": 3}. They should not co-occur, but
    // preferring the one that names its group loses less information.
    const auto payload = bytes_of({0xA2, 0x63, 0x65, 0x72, 0x72, 0xA2, 0x65, 0x67, 0x72, 0x6F, 0x75,
                                   0x70, 0x01, 0x62, 0x72, 0x63, 0x05, 0x62, 0x72, 0x63, 0x03});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->failed());
    REQUIRE(outcome->error->group_scoped);
    REQUIRE(outcome->error->rc == 5);
}

// ---------------------------------------------------------------------------
// The rsn string
// ---------------------------------------------------------------------------

TEST_CASE("the device's reason text is carried through", "[mgmt-error]")
{
    // {"rc": 6, "rsn": "busy"}
    const auto payload = bytes_of(
        {0xA2, 0x62, 0x72, 0x63, 0x06, 0x63, 0x72, 0x73, 0x6E, 0x64, 0x62, 0x75, 0x73, 0x79});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->failed());
    REQUIRE(outcome->reason == "busy");
}

TEST_CASE("an over-long reason is capped, not rejected", "[mgmt-error][hostile]")
{
    // rsn is attacker-controlled text. It must not be able to grow the host's
    // memory or a log line without bound -- but a long one is not a reason to
    // discard an otherwise valid error report.
    const std::string long_reason(1000, 'x');

    std::vector<std::byte> payload = bytes_of(
        {0xA2, 0x62, 0x72, 0x63, 0x06, 0x63, 0x72, 0x73, 0x6E, 0x79, 0x03, 0xE8}); // text(1000)
    for (const char character : long_reason) {
        payload.push_back(static_cast<std::byte>(character));
    }

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE(outcome->failed());
    REQUIRE(outcome->reason.size() == smply::limits::kMaxReasonLength);
}

TEST_CASE("a reason without an error is still reported as success", "[mgmt-error]")
{
    // {"rsn": "note"} with no rc. Odd, but not a failure.
    const auto payload = bytes_of({0xA1, 0x63, 0x72, 0x73, 0x6E, 0x64, 0x6E, 0x6F, 0x74, 0x65});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->failed());
    REQUIRE(outcome->reason == "note");
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST_CASE("a payload that is not a map is a decode error", "[mgmt-error]")
{
    const auto payload = bytes_of({0x01});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code() == ErrorCode::CborDecode);
}

TEST_CASE("an empty payload is a decode error", "[mgmt-error]")
{
    // Distinct from an empty CBOR map: no payload at all is malformed, since
    // every MCUmgr response must carry at least an empty map.
    const auto outcome = extract_mgmt_error(ConstBytes{});

    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code() == ErrorCode::CborDecode);
}

TEST_CASE("an rc of the wrong type is a decode error, not a silent success",
          "[mgmt-error][hostile]")
{
    // {"rc": "three"}. Treating this as absent would turn a malformed response
    // into an apparent success, which is exactly the failure that must not
    // happen here.
    const auto payload = bytes_of({0xA1, 0x62, 0x72, 0x63, 0x65, 0x74, 0x68, 0x72, 0x65, 0x65});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code() == ErrorCode::CborDecode);
}

TEST_CASE("a truncated payload is a decode error", "[mgmt-error][hostile]")
{
    // A complete {"rc": 30} minus its last byte.
    const auto payload = bytes_of({0xA1, 0x62, 0x72, 0x63, 0x18});

    const auto outcome = extract_mgmt_error(ConstBytes{payload});

    REQUIRE_FALSE(outcome.has_value());
}
