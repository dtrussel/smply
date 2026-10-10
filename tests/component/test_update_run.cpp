// SPDX-License-Identifier: Apache-2.0
//
// The update run (support/dfu_app/update_run.hpp), driven into a simulated
// device under ManualClock through the SimulatedWait adapter.
//
// These cases are about the run itself: the reconnect episode and its delays,
// the two ways of giving up, the approve hook's stop, the overall deadline,
// and the rule that no hook runs inside a library callback. The update's own
// sequence is test_firmware_update.cpp's, which drives every case through the
// same run.

#include "harness.hpp"
#include "simulated_wait.hpp"

#include "dfu_app/reconnect_policy.hpp"
#include "dfu_app/update_run.hpp"

#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

using smply::ConstBytes;
using smply::Error;
using smply::ErrorCode;
using smply::MemoryImageSource;
using smply::UpdateEvent;
using smply::UpdatePlan;
using smply::UpdateState;
using smply::dfu_app::Approval;
using smply::dfu_app::LinkAttempt;
using smply::dfu_app::ReconnectAttempt;
using smply::dfu_app::ReconnectSettings;
using smply::dfu_app::RunEnd;
using smply::dfu_app::UpdateRun;
using smply::dfu_app::UpdateRunHooks;
using smply::dfu_app::UpdateRunOutcome;
using smply::dfu_app::UpdateRunSettings;
using smply::test::FakeTransport;
using smply::test::Fixture;
using smply::test::make_firmware;
using smply::test::ServerConfig;
using smply::test::SimulatedWait;
using smply::test::SwapType;

using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kBodySize = 600;

/// The two images every case updates between, declared before the fixture:
/// the source views these bytes.
struct Images
{
    std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);
    MemoryImageSource source{ConstBytes{update}};
};

/// What the hooks saw.
struct Seen
{
    std::vector<UpdateEvent> events;
    /// When each open-a-link call happened, by the fixture's ManualClock.
    std::vector<smply::TimePoint> attempts;
    std::vector<ReconnectAttempt> attempt_args;
    int approvals = 0;

    [[nodiscard]] int count_disconnects() const
    {
        int n = 0;
        for (const UpdateEvent& event : events) {
            n += std::holds_alternative<smply::DisconnectExpected>(event) ? 1 : 0;
        }
        return n;
    }
};

[[nodiscard]] UpdateRunSettings brisk(unsigned attempts = 3)
{
    UpdateRunSettings settings;
    settings.reconnect = ReconnectSettings{
        .first_delay = 100ms,
        .max_delay = 1000ms,
        .max_attempts = attempts,
    };
    settings.overall_timeout = 60s;
    return settings;
}

/// A device, an application's hooks scripted by knobs, and the run over them,
/// declared in the order the lifetime rules require: the links before the
/// fixture, and the run -- whose hooks capture all of it -- last.
struct Rig
{
    explicit Rig(const ServerConfig& config = {}, UpdateRunSettings settings = brisk())
        : fixture{config}, run{fixture.client, fixture.updater, wait, std::move(settings), hooks()}
    {
        fixture.simulator.load_slot(0, images.running);
        // The device's side of a reset: once the updater says the link will
        // drop, the device reboots and the link goes -- between turns, in the
        // wait, never inside a callback.
        wait.between_turns([this, served = 0]() mutable {
            if (seen.count_disconnects() > served) {
                ++served;
                fixture.simulator.reboot();
                current.back()->disconnect();
            }
        });
    }

    /// The handler to start the update with: the run's, wrapped so the hooks
    /// can tell whether they are being called from inside it.
    [[nodiscard]] smply::UpdateEventCallback handler()
    {
        return [this, inner = run.event_handler()](const UpdateEvent& event) {
            in_callback = true;
            inner(event);
            in_callback = false;
        };
    }

    [[nodiscard]] UpdateRunOutcome start_and_run(const UpdatePlan& plan = {})
    {
        REQUIRE(fixture.updater.start(images.source, plan, handler()).has_value());
        return run.run();
    }

    Images images;
    Seen seen;
    std::array<FakeTransport, 4> spares;
    Fixture fixture;
    std::vector<FakeTransport*> current{&fixture.transport};
    std::size_t next_spare = 0;
    SimulatedWait wait{fixture};

    /// Attempts refused at the start of every episode.
    unsigned refusals_per_episode = 0;
    /// The reason a refused attempt gives, if any.
    std::optional<Error> retry_error;
    /// Give up on the first attempt, with this.
    std::optional<Error> give_up_with;
    Approval approval = Approval::Confirm;

    bool in_callback = false;
    int hooks_inside_callback = 0;
    unsigned refused = 0;

    UpdateRun run;

private:
    [[nodiscard]] UpdateRunHooks hooks()
    {
        return UpdateRunHooks{
            .open_link =
                [this](const ReconnectAttempt& attempt) {
                    hooks_inside_callback += in_callback ? 1 : 0;
                    seen.attempts.push_back(fixture.clock.now());
                    seen.attempt_args.push_back(attempt);
                    if (attempt.number == 1) {
                        refused = 0;
                    }
                    if (give_up_with.has_value()) {
                        return LinkAttempt::give_up(*give_up_with);
                    }
                    if (refused < refusals_per_episode) {
                        ++refused;
                        return retry_error.has_value() ? LinkAttempt::retry(*retry_error)
                                                       : LinkAttempt::retry();
                    }
                    REQUIRE(next_spare < spares.size());
                    FakeTransport& link = spares.at(next_spare++);
                    fixture.simulator.rebind_transport(link);
                    current.push_back(&link);
                    return LinkAttempt::opened(link);
                },
            .approve =
                [this] {
                    hooks_inside_callback += in_callback ? 1 : 0;
                    ++seen.approvals;
                    return approval;
                },
            .observe =
                [this](const UpdateEvent& event) {
                    hooks_inside_callback += in_callback ? 1 : 0;
                    seen.events.push_back(event);
                },
        };
    }
};

/// The gaps between consecutive attempts, read off the ManualClock.
[[nodiscard]] std::vector<smply::Duration> gaps(smply::TimePoint from,
                                                const std::vector<smply::TimePoint>& attempts)
{
    std::vector<smply::Duration> out;
    for (const smply::TimePoint at : attempts) {
        out.push_back(std::chrono::duration_cast<smply::Duration>(at - from));
        from = at;
    }
    return out;
}

} // namespace

TEST_CASE("the update run completes a clean update through its hooks", "[dfu][run]")
{
    Rig rig;
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(outcome.end == RunEnd::Finished);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->final_state == UpdateState::Completed);
    CHECK_FALSE(outcome.gave_up_reconnecting);
    CHECK(rig.seen.approvals == 1);
    REQUIRE(rig.seen.attempt_args.size() == 1);
    CHECK(rig.seen.attempt_args[0].number == 1);
    CHECK(rig.seen.attempt_args[0].max_attempts == 3);
    CHECK(rig.seen.attempt_args[0].waited == 100ms);
    // The last event observed is the finish, and nothing follows it.
    REQUIRE_FALSE(rig.seen.events.empty());
    CHECK(std::holds_alternative<smply::UpdateFinished>(rig.seen.events.back()));
    CHECK(rig.fixture.simulator.swap_type() == SwapType::None);
}

TEST_CASE("no hook is called from inside a library callback", "[dfu][run]")
{
    Rig rig;
    std::size_t raised = 0;
    const smply::UpdateEventCallback counted = [&, inner = rig.handler()](const UpdateEvent& e) {
        ++raised;
        inner(e);
    };
    REQUIRE(rig.fixture.updater.start(rig.images.source, UpdatePlan{}, counted).has_value());
    const UpdateRunOutcome outcome = rig.run.run();

    REQUIRE(outcome.end == RunEnd::Finished);
    CHECK(rig.hooks_inside_callback == 0);
    // Every event raised was observed, in a later step of the run.
    CHECK(rig.seen.events.size() == raised);
    CHECK(rig.seen.approvals == 1);
    CHECK(rig.seen.attempts.size() == 1);
}

TEST_CASE("refused attempts back off by the policy's exact delays", "[dfu][run][reconnect]")
{
    Rig rig{ServerConfig{}, brisk(5)};
    rig.refusals_per_episode = 3;
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(outcome.end == RunEnd::Finished);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->final_state == UpdateState::Completed);
    // 100, 200, 400 ms refused, then 800 ms and the link: nothing but the
    // delays moves the clock between attempts.
    REQUIRE(rig.seen.attempts.size() == 4);
    CHECK(rig.seen.attempt_args.front().waited == 100ms);
    const std::vector<smply::Duration> waited =
        gaps(rig.seen.attempts.front(), {rig.seen.attempts.begin() + 1, rig.seen.attempts.end()});
    CHECK(waited == std::vector<smply::Duration>{200ms, 400ms, 800ms});
    CHECK(rig.wait.sleeps() == std::vector<smply::Duration>{100ms, 200ms, 400ms, 800ms});
    CHECK(rig.seen.attempt_args.back().number == 4);
    CHECK_FALSE(outcome.gave_up_reconnecting);
}

TEST_CASE("an exhausted policy tells the updater, which ends the update", "[dfu][run][reconnect]")
{
    Rig rig;
    rig.refusals_per_episode = 100;
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(outcome.end == RunEnd::Finished);
    REQUIRE_FALSE(outcome.result.has_value());
    // The settings' error: no attempt said why.
    CHECK(outcome.result.error().code() == ErrorCode::Disconnected);
    CHECK(outcome.gave_up_reconnecting);
    CHECK(rig.seen.attempts.size() == 3);
    CHECK(rig.wait.sleeps() == std::vector<smply::Duration>{100ms, 200ms, 400ms});
    CHECK(rig.fixture.updater.report().final_state == UpdateState::Failed);
    CHECK(rig.fixture.updater.report().revert_pending);
}

TEST_CASE("an exhausted policy passes on the last refusal's reason", "[dfu][run][reconnect]")
{
    // serial_dfu's case: a port that is absent is retried, and if it never
    // comes back, the updater hears why.
    Rig rig;
    rig.refusals_per_episode = 100;
    rig.retry_error = Error{ErrorCode::TransportError, "port absent"};
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(outcome.end == RunEnd::Finished);
    REQUIRE_FALSE(outcome.result.has_value());
    CHECK(outcome.result.error().code() == ErrorCode::TransportError);
    CHECK(rig.seen.attempts.size() == 3);
}

TEST_CASE("the open hook's give-up ends the episode at once, with its error",
          "[dfu][run][reconnect]")
{
    Rig rig;
    rig.give_up_with = Error{ErrorCode::InvalidArgument, "port refused"};
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(outcome.end == RunEnd::Finished);
    REQUIRE_FALSE(outcome.result.has_value());
    CHECK(outcome.result.error().code() == ErrorCode::InvalidArgument);
    CHECK(outcome.gave_up_reconnecting);
    // One attempt, though the policy allowed three.
    CHECK(rig.seen.attempts.size() == 1);
    REQUIRE(rig.fixture.updater.report().cause.has_value());
    CHECK(rig.fixture.updater.report().cause->code() == ErrorCode::InvalidArgument);
}

TEST_CASE("approve answering stop ends the run with the update still waiting", "[dfu][run]")
{
    Rig rig;
    rig.approval = Approval::Stop;
    const UpdateRunOutcome stopped = rig.start_and_run();

    REQUIRE(stopped.end == RunEnd::StoppedBeforeConfirm);
    REQUIRE_FALSE(stopped.result.has_value());
    CHECK(rig.seen.approvals == 1);
    CHECK(rig.fixture.updater.state() == UpdateState::AwaitingConfirmation);
    // Installed, running on trial: the next reset reverts it.
    CHECK(rig.fixture.simulator.swap_type() == SwapType::Revert);

    // The application may approve later and run again; it is not asked twice.
    REQUIRE(rig.fixture.updater.confirm().has_value());
    const UpdateRunOutcome finished = rig.run.run();
    REQUIRE(finished.end == RunEnd::Finished);
    REQUIRE(finished.result.has_value());
    CHECK(finished.result->final_state == UpdateState::Completed);
    CHECK(rig.seen.approvals == 1);
    CHECK(rig.fixture.simulator.swap_type() == SwapType::None);
}

TEST_CASE("the overall deadline ends a stalled run", "[dfu][run][deadline]")
{
    // A device that takes an hour to answer: the client's own timeout (5 s)
    // is later than the run's.
    ServerConfig slow;
    slow.response_delay = std::chrono::hours{1};
    UpdateRunSettings settings = brisk();
    settings.overall_timeout = 2s;
    Rig rig{slow, settings};
    const smply::TimePoint began = rig.fixture.clock.now();
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(outcome.end == RunEnd::TimedOut);
    REQUIRE_FALSE(outcome.result.has_value());
    CHECK(outcome.result.error().code() == ErrorCode::Timeout);
    // Reached to within one simulation step, and the update is still running.
    CHECK(rig.fixture.clock.now() >= began + 2s);
    CHECK(rig.fixture.clock.now() <= began + 2s + 10ms);
    CHECK(rig.fixture.updater.state() != UpdateState::Failed);
    CHECK(rig.fixture.updater.state() != UpdateState::Completed);
}

TEST_CASE("the overall deadline ends a reconnect episode that would outlast it",
          "[dfu][run][deadline]")
{
    UpdateRunSettings settings = brisk(1000);
    settings.overall_timeout = 5s;
    Rig rig{ServerConfig{}, settings};
    rig.refusals_per_episode = 1000;
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(outcome.end == RunEnd::TimedOut);
    CHECK_FALSE(outcome.gave_up_reconnecting);
    // Nobody told the updater: it is still waiting for the link.
    CHECK(rig.fixture.updater.state() == UpdateState::AwaitingReconnect);
}

TEST_CASE("a reconnect delay is cut short at the overall deadline, not overrun",
          "[dfu][run][deadline]")
{
    // 100, 200, 400, 800 ms, then 1 s at a time: one of those seconds
    // straddles the 5 s deadline, and the run must not sleep past it and then
    // open a link anyway.
    UpdateRunSettings settings = brisk(1000);
    settings.overall_timeout = 5s;
    Rig rig{ServerConfig{}, settings};
    rig.refusals_per_episode = 1000;
    const smply::TimePoint began = rig.fixture.clock.now();
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(outcome.end == RunEnd::TimedOut);
    CHECK(outcome.result.error().code() == ErrorCode::Timeout);
    // Exactly at the deadline: the last sleep is the time that was left.
    CHECK(rig.fixture.clock.now() == began + 5s);
    REQUIRE_FALSE(rig.wait.sleeps().empty());
    CHECK(rig.wait.sleeps().back() > 0ms);
    CHECK(rig.wait.sleeps().back() < 1000ms);
    // No attempt was made at or after the deadline.
    REQUIRE_FALSE(rig.seen.attempts.empty());
    CHECK(rig.seen.attempts.back() < began + 5s);
    // The cut-short sleep is the one with no attempt after it.
    CHECK(rig.wait.sleeps().size() == rig.seen.attempts.size() + 1);
}

TEST_CASE("a run that timed out while reconnecting reconnects when run again",
          "[dfu][run][deadline][reconnect]")
{
    UpdateRunSettings settings = brisk(1000);
    settings.overall_timeout = 5s;
    Rig rig{ServerConfig{}, settings};
    rig.refusals_per_episode = 1000;
    const UpdateRunOutcome timed_out = rig.start_and_run();
    REQUIRE(timed_out.end == RunEnd::TimedOut);
    REQUIRE(rig.fixture.updater.state() == UpdateState::AwaitingReconnect);
    const std::size_t attempts_before = rig.seen.attempt_args.size();

    // The device is reachable now. The reconnect is still owed, and the next
    // run starts a fresh episode for it: first attempt, first delay.
    rig.refusals_per_episode = 0;
    const UpdateRunOutcome finished = rig.run.run();

    REQUIRE(finished.end == RunEnd::Finished);
    REQUIRE(finished.result.has_value());
    CHECK(finished.result->final_state == UpdateState::Completed);
    CHECK_FALSE(finished.gave_up_reconnecting);
    REQUIRE(rig.seen.attempt_args.size() == attempts_before + 1);
    CHECK(rig.seen.attempt_args.back().number == 1);
    CHECK(rig.seen.attempt_args.back().waited == 100ms);
    CHECK(rig.fixture.simulator.swap_type() == SwapType::None);
}

TEST_CASE("a second reboot in one update runs a fresh episode", "[dfu][run][reconnect]")
{
    // An upload interrupted by a dropped link, and then the reset: two
    // episodes, each refused once, each starting from the first delay.
    Rig rig;
    rig.refusals_per_episode = 1;
    rig.wait.when(
        [&] {
            return std::any_of(
                rig.seen.events.begin(), rig.seen.events.end(), [](const UpdateEvent& event) {
                    const auto* progress = std::get_if<smply::UploadProgress>(&event);
                    return progress != nullptr && progress->transferred > 0;
                });
        },
        [&] { rig.fixture.transport.disconnect(); });
    const UpdateRunOutcome outcome = rig.start_and_run();

    REQUIRE(rig.wait.all_fired());
    REQUIRE(outcome.end == RunEnd::Finished);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->final_state == UpdateState::Completed);
    CHECK(rig.wait.sleeps() == std::vector<smply::Duration>{100ms, 200ms, 100ms, 200ms});
    REQUIRE(rig.seen.attempt_args.size() == 4);
    CHECK(rig.seen.attempt_args[2].number == 1);
    CHECK(rig.seen.attempt_args[3].number == 2);
    const ConstBytes primary = rig.fixture.simulator.slot_content(0);
    CHECK(std::equal(primary.begin(), primary.end(), rig.images.update.begin(),
                     rig.images.update.end()));
}
