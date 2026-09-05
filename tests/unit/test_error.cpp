// SPDX-License-Identifier: Apache-2.0

#include "smply/error.hpp"
#include "smply/group.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using smply::Error;
using smply::ErrorCode;
using smply::Group;
using smply::MgmtError;

TEST_CASE("a default-constructed Error is not a failure", "[error]")
{
    const Error error;

    REQUIRE(error.code() == ErrorCode::Ok);
    REQUIRE_FALSE(error.failed());
    REQUIRE_FALSE(error.mgmt().has_value());
    REQUIRE(error.reason().empty());
    REQUIRE(error.where() == nullptr);
}

TEST_CASE("an Error carries its code and call site", "[error]")
{
    const Error error{ErrorCode::Timeout, "smp_client"};

    REQUIRE(error.code() == ErrorCode::Timeout);
    REQUIRE(error.failed());
    REQUIRE(std::string_view{error.where()} == "smp_client");
}

TEST_CASE("MgmtError keeps SMP v1 and v2 codes distinguishable", "[error]")
{
    // The whole point of carrying the group and the flag: rc 30 means three
    // different things depending on which shape produced it
    // (docs/protocol-notes.md section 3).
    const auto v1 = MgmtError::smp(30);
    const auto v2_image = MgmtError::scoped(Group::Image, 30);
    const auto v2_os = MgmtError::scoped(Group::Os, 30);

    REQUIRE_FALSE(v1.group_scoped);
    REQUIRE(v2_image.group_scoped);

    REQUIRE_FALSE(v1 == v2_image);
    REQUIRE_FALSE(v2_image == v2_os);
    REQUIRE(v2_image == MgmtError::scoped(Group::Image, 30));
}

TEST_CASE("MgmtError round-trips through an Error", "[error]")
{
    const auto mgmt = MgmtError::scoped(Group::Image, 31);
    const Error error{ErrorCode::ProtocolError, mgmt, "upload"};

    REQUIRE(error.mgmt().has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by the REQUIRE
    // above, which aborts the test on failure. The checker cannot see through
    // Catch2's macros.
    REQUIRE(*error.mgmt() == mgmt);
    REQUIRE(error.mgmt()->group == Group::Image);
    REQUIRE(error.mgmt()->rc == 31);
    REQUIRE(error.mgmt()->group_scoped);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_CASE("MgmtError preserves rc values smply does not recognise", "[error]")
{
    // These enumerations grow with each Zephyr release; an unknown code must
    // pass through numerically rather than being rejected (protocol-notes A2).
    const auto mgmt = MgmtError::scoped(Group::Image, 4095);
    REQUIRE(mgmt.rc == 4095);

    // Likewise an entirely unknown group: Group is open.
    // Group is open by design (see smply/group.hpp); this is the property under
    // test.
    const auto vendor = MgmtError::scoped(static_cast<Group>(9000), 7);
    REQUIRE(smply::to_underlying(vendor.group) == 9000);
    REQUIRE(smply::is_user_defined(vendor.group));
}

TEST_CASE("with_reason attaches device text and chains", "[error]")
{
    SECTION("on an lvalue")
    {
        Error error{ErrorCode::ProtocolError};
        error.with_reason("slot is busy");
        REQUIRE(error.reason() == "slot is busy");
    }

    SECTION("on an rvalue, for use in a return statement")
    {
        const Error error = Error{ErrorCode::ProtocolError, "img"}.with_reason("slot is busy");
        REQUIRE(error.reason() == "slot is busy");
        REQUIRE(std::string_view{error.where()} == "img");
    }
}

TEST_CASE("Errors compare by code, device detail and reason", "[error]")
{
    REQUIRE(Error{ErrorCode::Timeout} == Error{ErrorCode::Timeout});
    REQUIRE_FALSE(Error{ErrorCode::Timeout} == Error{ErrorCode::Cancelled});

    REQUIRE(Error{ErrorCode::ProtocolError, MgmtError::smp(3)} ==
            Error{ErrorCode::ProtocolError, MgmtError::smp(3)});
    REQUIRE_FALSE(Error{ErrorCode::ProtocolError, MgmtError::smp(3)} ==
                  Error{ErrorCode::ProtocolError, MgmtError::smp(4)});

    // An SMP-level rc and a group-scoped rc with the same number differ.
    REQUIRE_FALSE(Error{ErrorCode::ProtocolError, MgmtError::smp(3)} ==
                  Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Os, 3)});

    REQUIRE_FALSE(Error{ErrorCode::ProtocolError}.with_reason("a") ==
                  Error{ErrorCode::ProtocolError}.with_reason("b"));
}

TEST_CASE("the call site is excluded from comparison", "[error]")
{
    // Two failures of the same kind are equal however they were raised; `where`
    // is a logging aid, not part of the error's identity (ADR-0002).
    REQUIRE(Error{ErrorCode::Timeout, "here"} == Error{ErrorCode::Timeout, "elsewhere"});
    REQUIRE(Error{ErrorCode::Timeout, "here"} == Error{ErrorCode::Timeout, nullptr});
}

TEST_CASE("every ErrorCode has a distinct, non-empty name", "[error]")
{
    const std::vector<ErrorCode> all{
        ErrorCode::Ok,
        ErrorCode::InvalidArgument,
        ErrorCode::InvalidState,
        ErrorCode::MalformedMessage,
        ErrorCode::UnsupportedSmpVersion,
        ErrorCode::MessageTooLarge,
        ErrorCode::CborEncode,
        ErrorCode::CborDecode,
        ErrorCode::ProtocolError,
        ErrorCode::UnexpectedResponse,
        ErrorCode::Timeout,
        ErrorCode::Cancelled,
        ErrorCode::TransportError,
        ErrorCode::TransportBusy,
        ErrorCode::Disconnected,
        ErrorCode::ImageMismatch,
        ErrorCode::UpdateFailed,
        ErrorCode::Internal,
    };

    std::vector<std::string_view> names;
    for (const ErrorCode code : all) {
        const std::string_view name = smply::to_string(code);
        REQUIRE_FALSE(name.empty());
        names.push_back(name);
    }

    // Distinct: a duplicated name would make two different failures
    // indistinguishable in a log.
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            INFO("duplicate name: " << names[i]);
            REQUIRE(names[i] != names[j]);
        }
    }
}

TEST_CASE("to_string(ErrorCode) handles an out-of-range value", "[error]")
{
    // Defensive: someone casting a bad value in must not fall off the end.
    REQUIRE_FALSE(smply::to_string(static_cast<ErrorCode>(60000)).empty());
}

TEST_CASE("to_string(Error) renders the category, device detail and context", "[error]")
{
    SECTION("bare code")
    {
        const std::string text = smply::to_string(Error{ErrorCode::Timeout});
        REQUIRE(text == "timed out");
    }

    SECTION("with a call site")
    {
        const std::string text = smply::to_string(Error{ErrorCode::Timeout, "smp"});
        REQUIRE(text.find("timed out") != std::string::npos);
        REQUIRE(text.find("smp") != std::string::npos);
    }

    SECTION("group-scoped device error names the group and code")
    {
        const std::string text =
            smply::to_string(Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Image, 30)});
        REQUIRE(text.find("image") != std::string::npos);
        REQUIRE(text.find("30") != std::string::npos);
    }

    SECTION("flat SMP error is marked as such, so it is not read as group-scoped")
    {
        const std::string text =
            smply::to_string(Error{ErrorCode::ProtocolError, MgmtError::smp(30)});
        REQUIRE(text.find("smp rc=30") != std::string::npos);
        REQUIRE(text.find("image") == std::string::npos);
    }

    SECTION("device reason is included")
    {
        const std::string text =
            smply::to_string(Error{ErrorCode::ProtocolError}.with_reason("secondary slot in use"));
        REQUIRE(text.find("secondary slot in use") != std::string::npos);
    }
}

TEST_CASE("group_name covers the named groups and degrades for unknown ones", "[error]")
{
    REQUIRE(std::string_view{smply::group_name(Group::Os)} == "os");
    REQUIRE(std::string_view{smply::group_name(Group::Image)} == "image");
    REQUIRE(std::string_view{smply::group_name(static_cast<Group>(9000))} == "unknown");
}

TEST_CASE("is_user_defined marks the vendor range", "[error]")
{
    REQUIRE_FALSE(smply::is_user_defined(Group::Os));
    REQUIRE_FALSE(smply::is_user_defined(Group::ZephyrBasic));
    REQUIRE(smply::is_user_defined(Group::PerUser));
    REQUIRE(smply::is_user_defined(static_cast<Group>(200)));
}

TEST_CASE("smp_error reads a flat rc and refuses a group-scoped one", "[error]")
{
    using smply::SmpError;

    // A flat rc is drawn from mcumgr_err_t, so it may be read as an SmpError.
    const Error flat{ErrorCode::ProtocolError, smply::MgmtError::smp(8)};
    REQUIRE(smply::smp_error(flat) == SmpError::NotSupported);

    // A group-scoped rc of the same number means something else entirely.
    // Returning nullopt is what stops a caller comparing the two.
    const Error scoped{ErrorCode::ProtocolError, smply::MgmtError::scoped(Group::Image, 8)};
    REQUIRE_FALSE(smply::smp_error(scoped).has_value());

    // An error with no device report has no SMP code either.
    REQUIRE_FALSE(smply::smp_error(Error{ErrorCode::Timeout}).has_value());
}

TEST_CASE("an unknown SMP code is preserved rather than rejected", "[error]")
{
    // These enumerations grow with each Zephyr release (protocol-notes A2), so
    // a code smply has never heard of must still round-trip.
    const Error unknown{ErrorCode::ProtocolError, smply::MgmtError::smp(4242)};
    const auto code = smply::smp_error(unknown);
    REQUIRE(code.has_value());
    REQUIRE(static_cast<std::uint16_t>(*code) == 4242);
}
