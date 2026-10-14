// SPDX-License-Identifier: Apache-2.0
//
// `smply::dfu_app`'s update command: the flags every example tool shares, the
// one set of rules they are checked by, and the console output -- the event
// lines, the escaped errors and the report. The tools' own ctests run it end
// to end; these pin each rule and line on its own.

#include "dfu_app/update_command.hpp"

#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using smply::CommitBy;
using smply::Error;
using smply::ErrorCode;
using smply::McubootMode;
using smply::UpdateMode;
using smply::UpdateReport;
using smply::UpdateState;
using smply::dfu_app::FlagResult;
using smply::dfu_app::parse_update_flag;
using smply::dfu_app::UpdateCommand;
using smply::dfu_app::UpdateConsole;
using smply::dfu_app::UpdateFlags;
using smply::dfu_app::valid;

namespace {

/// Parses \p args the way a tool with no flags of its own would. False on the
/// first flag that is not taken.
[[nodiscard]] bool parse(const std::vector<std::string>& args, UpdateCommand& out,
                         const UpdateFlags& offered = UpdateFlags{})
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (parse_update_flag(args, i, offered, out) != FlagResult::Taken) {
            return false;
        }
    }
    return true;
}

/// \p args parsed, then checked.
[[nodiscard]] bool accepted(const std::vector<std::string>& args, bool needs_named_input = false,
                            unsigned generated_packages = 0)
{
    UpdateCommand command;
    command.generated_packages = generated_packages;
    return parse(args, command) && valid(command, needs_named_input);
}

} // namespace

TEST_CASE("the shared flags are parsed into the command and its plan", "[dfu_app][command]")
{
    UpdateCommand command;
    REQUIRE(parse({"--image", "a.bin", "--image-secondary", "b.bin", "--mode", "upload-only",
                   "--fallback-mode", "swap-using-move", "--allow-no-revert",
                   "--no-downgrade-check", "--quiet", "--commit", "1=device", "--board",
                   "nrf54l15dk/nrf54l15/cpuapp", "--package", "p.zip"},
                  command));
    CHECK(command.image_path == "a.bin");
    CHECK(command.image_secondary_path == "b.bin");
    CHECK(command.package_path == "p.zip");
    CHECK(command.quiet);
    REQUIRE(command.commits.size() == 1);
    CHECK(command.commits[0] == std::pair{std::uint32_t{1}, CommitBy::Device});
    REQUIRE(command.board.has_value());

    const smply::UpdatePlan plan = command.plan();
    CHECK(plan.mode == UpdateMode::UploadOnly);
    CHECK(plan.fallback_mode == McubootMode::SwapUsingMove);
    CHECK(plan.allow_no_revert);
    CHECK_FALSE(plan.check_downgrade);
}

TEST_CASE("a shared flag says whether it was taken, unknown or invalid", "[dfu_app][command]")
{
    const std::vector<std::string> args{"--image", "a.bin", "--stub", "--mode"};
    UpdateCommand command;
    std::size_t i = 0;
    CHECK(parse_update_flag(args, i, UpdateFlags{}, command) == FlagResult::Taken);
    CHECK(i == 1); // left on the value it consumed
    i = 2;
    CHECK(parse_update_flag(args, i, UpdateFlags{}, command) == FlagResult::Unknown);
    CHECK(i == 2);
    i = 3;
    // A flag that needs a value and has none.
    CHECK(parse_update_flag(args, i, UpdateFlags{}, command) == FlagResult::Invalid);

    for (const std::vector<std::string>& bad :
         {std::vector<std::string>{"--mode", "test-only"},
          std::vector<std::string>{"--fallback-mode", "unknown"},
          std::vector<std::string>{"--commit", "1=nobody"},
          std::vector<std::string>{"--board", "/cpuapp"}}) {
        CAPTURE(bad[0], bad[1]);
        UpdateCommand out;
        std::size_t at = 0;
        CHECK(parse_update_flag(bad, at, UpdateFlags{}, out) == FlagResult::Invalid);
    }
}

TEST_CASE("a tool without packages does not know the package flags", "[dfu_app][command]")
{
    for (const char* flag : {"--package", "--commit", "--board"}) {
        CAPTURE(flag);
        const std::vector<std::string> args{flag, "x"};
        UpdateCommand command;
        std::size_t i = 0;
        CHECK(parse_update_flag(args, i, UpdateFlags{.packages = false}, command) ==
              FlagResult::Unknown);
    }
}

TEST_CASE("one thing to install at most", "[dfu_app][command]")
{
    CHECK(accepted({}));
    CHECK(accepted({"--image", "a.bin"}));
    CHECK(accepted({"--package", "p.zip"}));
    CHECK(accepted({}, false, 1));
    CHECK_FALSE(accepted({"--image", "a.bin", "--package", "p.zip"}));
    CHECK_FALSE(accepted({"--image", "a.bin"}, false, 1));
    CHECK_FALSE(accepted({"--package", "p.zip"}, false, 1));
    CHECK_FALSE(accepted({}, false, 2));
}

TEST_CASE("only a package takes --commit and --board", "[dfu_app][command]")
{
    CHECK(accepted({"--package", "p.zip", "--commit", "1=device", "--board", "b"}));
    CHECK(accepted({"--commit", "1=device", "--board", "b"}, false, 1));
    CHECK_FALSE(accepted({"--commit", "1=device"}));
    CHECK_FALSE(accepted({"--image", "a.bin", "--commit", "1=device"}));
    CHECK_FALSE(accepted({"--board", "b"}));
    CHECK_FALSE(accepted({"--image", "a.bin", "--board", "b"}));
}

TEST_CASE("a second build goes with a named first build, never with a package",
          "[dfu_app][command]")
{
    CHECK(accepted({"--image", "a.bin", "--image-secondary", "b.bin"}));
    CHECK_FALSE(accepted({"--image-secondary", "b.bin"}));
    CHECK_FALSE(accepted({"--package", "p.zip", "--image-secondary", "b.bin"}));
    CHECK_FALSE(accepted({"--image-secondary", "b.bin"}, false, 1));
}

TEST_CASE("real hardware needs a named image or package", "[dfu_app][command]")
{
    CHECK(accepted({"--image", "a.bin"}, true));
    CHECK(accepted({"--package", "p.zip"}, true));
    CHECK_FALSE(accepted({}, true));
    CHECK_FALSE(accepted({}, true, 1));
}

TEST_CASE("an error is printed escaped, with the tool's name", "[dfu_app][command]")
{
    std::ostringstream out;
    std::ostringstream err;
    const UpdateConsole console{"tool", false, out, err};
    // A device's rsn is the device's to choose (docs/security.md T12).
    console.error(Error{ErrorCode::ProtocolError, "smp"}.with_reason("\x1b[2Jgone"));
    console.error("--commit 1", Error{ErrorCode::InvalidArgument}.with_reason("bell\a"));
    const std::string printed = err.str();
    CHECK(printed.find('\x1b') == std::string::npos);
    CHECK(printed.find('\a') == std::string::npos);
    CHECK(printed.find("tool: ") == 0);
    CHECK(printed.find("\\x1B[2Jgone") != std::string::npos);
    CHECK(printed.find("tool: --commit 1: ") != std::string::npos);
    CHECK(out.str().empty());
}

TEST_CASE("events are progress lines, and quiet prints none", "[dfu_app][command]")
{
    std::ostringstream out;
    std::ostringstream err;
    const UpdateConsole loud{"tool", false, out, err};
    loud.event(smply::UpdateStateChanged{.from = UpdateState::Idle,
                                         .to = UpdateState::QueryingParameters});
    loud.event(smply::UploadProgress{.transferred = 10, .total = 10});
    loud.event(smply::DisconnectExpected{});
    loud.confirming();
    CHECK(out.str() == "  QueryingParameters\n"
                       "\r  uploading 10/10 bytes\n"
                       "  the device is about to reboot\n"
                       "  the new image is running unconfirmed; confirming\n");

    std::ostringstream silent;
    const UpdateConsole quiet{"tool", true, silent, err};
    quiet.event(smply::UpdateStateChanged{.from = UpdateState::Idle,
                                          .to = UpdateState::QueryingParameters});
    quiet.confirming();
    CHECK(silent.str().empty());
    CHECK(err.str().empty());
}

TEST_CASE("the report: a completed update to the output, a failure to the error stream",
          "[dfu_app][command]")
{
    UpdateReport report;
    report.final_state = UpdateState::Completed;
    report.bytes_transferred = 4096;

    std::ostringstream out;
    std::ostringstream err;
    // Quiet or not: the report is the outcome.
    const UpdateConsole console{"tool", true, out, err};
    CHECK(console.finish(report, report));
    CHECK(out.str().find("update Completed\n  bytes transferred: 4096\n") == 0);
    CHECK(err.str().empty());

    std::ostringstream failed_out;
    std::ostringstream failed_err;
    const UpdateConsole failing{"tool", true, failed_out, failed_err};
    report.final_state = UpdateState::Failed;
    report.revert_pending = true;
    CHECK_FALSE(failing.finish(smply::fail(Error{ErrorCode::UpdateFailed, "dfu: \x07"}), report));
    CHECK(failed_out.str().empty());
    const std::string printed = failed_err.str();
    CHECK(printed.find("tool: update failed: ") == 0);
    CHECK(printed.find('\x07') == std::string::npos);
    CHECK(printed.find("will revert on the next reset") != std::string::npos);
}
