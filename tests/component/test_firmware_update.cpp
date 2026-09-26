// SPDX-License-Identifier: Apache-2.0
//
// The whole update, driven into a simulated device.
//
// These tests are about the *sequence*: the commands a real server accepts, in
// the order a real client issues them, across a reboot the application has to
// take part in. A case that is really about one decision belongs in
// tests/unit/test_update_state_machine.cpp, where it needs no device at all.

#include "fake_image_source.hpp"
#include "harness.hpp"

#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/mcuboot_image.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

using smply::CommitBy;
using smply::ConstBytes;
using smply::ErrorCode;
using smply::ImageError;
using smply::ImageTarget;
using smply::MemoryImageSource;
using smply::SmpClientConfig;
using smply::UpdateEvent;
using smply::UpdateMode;
using smply::UpdatePlan;
using smply::UpdateReport;
using smply::UpdateState;
using smply::Version;
using smply::test::ApplyOutcome;
using smply::test::CommitOutcome;
using smply::test::FakeTransport;
using smply::test::Fixture;
using smply::test::make_firmware;
using smply::test::ServerConfig;
using smply::test::SwapType;

namespace Catch {
template<>
struct StringMaker<smply::UpdateState>
{
    static std::string convert(smply::UpdateState state)
    {
        return std::string{smply::to_string(state)};
    }
};

template<>
struct StringMaker<smply::ErrorCode>
{
    static std::string convert(smply::ErrorCode code)
    {
        return std::string{smply::to_string(code)};
    }
};
} // namespace Catch

namespace {

constexpr std::uint32_t kBodySize = 600;

/// Everything the application saw.
///
/// Declared by a test **before** its fixture: the updater, the groups and the
/// client all complete outstanding work in their destructors, and this is what
/// their callbacks touch.
struct UpdateOutcome
{
    std::vector<UpdateState> visited;
    std::size_t events = 0;
    std::optional<UpdateReport> report;
    std::optional<ErrorCode> code;
    int finishes = 0;
    /// Counted, not latched: an update can go round the disconnect/reconnect
    /// loop more than once -- an interrupted upload does it twice.
    int disconnects = 0;
    int reconnects = 0;
    int confirmations = 0;
    std::uint64_t last_progress = 0;

    [[nodiscard]] auto handler()
    {
        return [this](const UpdateEvent& event) {
            ++events;
            std::visit(smply::overloaded{
                           [this](const smply::UpdateStateChanged& changed) {
                               visited.push_back(changed.to);
                           },
                           [this](const smply::UploadProgress& progress) {
                               last_progress = progress.transferred;
                           },
                           [this](const smply::DisconnectExpected&) { ++disconnects; },
                           [this](const smply::ReconnectRequired&) { ++reconnects; },
                           [this](const smply::ConfirmationRequired&) { ++confirmations; },
                           [this](const smply::UpdateFinished& finished) {
                               ++finishes;
                               if (finished.result.has_value()) {
                                   report = *finished.result;
                               } else {
                                   // A failed update has no report *value*; the
                                   // updater keeps one, and a test reads it from there.
                                   code = finished.result.error().code();
                               }
                           },
                       },
                       event);
        };
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return finishes > 0;
    }

    [[nodiscard]] bool reached(UpdateState state) const
    {
        return std::find(visited.begin(), visited.end(), state) != visited.end();
    }
};

/// Drives an update to its end, playing the part of the application.
///
/// The application's two jobs are the ones the updater deliberately refuses to
/// do: reconnect after the reset, and decide whether the new image is good.
struct Application
{
    /// Reboot the device when the reset drops the link. False models a device
    /// that comes back running the *old* image.
    bool reboot_on_disconnect = true;
    /// Reboot a second time before reconnecting: an unconfirmed trial boot that
    /// the device resets out of, which is MCUboot reverting.
    bool reboot_twice = false;
    /// Answer `ConfirmationRequired`. False leaves the update waiting.
    bool confirm = true;
    /// Refuse to reconnect at all.
    bool fail_reconnect = false;

    int disconnects_served = 0;
    int reconnects_served = 0;
    int confirmations_served = 0;
    std::size_t next_spare = 0;

    /// Runs turns until the update finishes or the budget runs out.
    ///
    /// \param spares Fresh links, used one per reconnect. A `FakeTransport` is
    ///               terminally disconnected once dropped, exactly as a real
    ///               one is, so each cycle needs its own.
    bool run(Fixture& fixture, std::vector<FakeTransport*> spares, UpdateOutcome& outcome,
             int budget = 6000, smply::Duration step = std::chrono::milliseconds{10})
    {
        for (int i = 0; i < budget && !outcome.finished(); ++i) {
            if (outcome.disconnects > disconnects_served) {
                ++disconnects_served;
                if (reboot_on_disconnect) {
                    fixture.simulator.reboot();
                    if (reboot_twice) {
                        fixture.simulator.reboot();
                    }
                }
                current(fixture, spares).disconnect();
            }
            if (outcome.reconnects > reconnects_served) {
                ++reconnects_served;
                if (fail_reconnect) {
                    fixture.updater.reconnect_failed(smply::Error{ErrorCode::Disconnected});
                } else {
                    REQUIRE(next_spare < spares.size());
                    FakeTransport& link = *spares[next_spare++];
                    fixture.client.rebind_transport(link);
                    fixture.simulator.rebind_transport(link);
                    REQUIRE(fixture.updater.resume_after_reconnect().has_value());
                }
            }
            if (outcome.confirmations > confirmations_served && confirm) {
                ++confirmations_served;
                REQUIRE(fixture.updater.confirm().has_value());
            }
            fixture.step(step);
        }
        return outcome.finished();
    }

private:
    /// The link currently in use: the fixture's own until a spare replaces it.
    [[nodiscard]] FakeTransport& current(Fixture& fixture,
                                         const std::vector<FakeTransport*>& spares) const
    {
        return next_spare == 0 ? fixture.transport : *spares[next_spare - 1];
    }
};

/// One request, identified the way the protocol identifies it.
///
/// An aggregate rather than a `std::pair`: MSVC's `/w14242` rejects
/// `pair<uint16_t, uint8_t>{0, 6}`, because the `int` literals narrow through
/// pair's constructor template. Aggregate initialisation from constant
/// expressions that fit is not narrowing, so this compiles everywhere -- and it
/// reads better than `.first` and `.second`.
struct Command
{
    std::uint16_t group = 0;
    std::uint8_t command = 0;

    [[nodiscard]] friend bool operator==(const Command&, const Command&) = default;
};

/// Every request a run issued, in order.
[[nodiscard]] std::vector<Command> commands(const Fixture& fixture)
{
    std::vector<Command> out;
    for (const smply::Header& header : fixture.simulator.requests()) {
        out.push_back(Command{static_cast<std::uint16_t>(header.group), header.command});
    }
    return out;
}

constexpr Command kParams{0, 6};
constexpr Command kState{1, 0};
constexpr Command kUpload{1, 1};
constexpr Command kReset{0, 5};

[[nodiscard]] bool issued(const Fixture& fixture, Command command)
{
    const std::vector<Command> all = commands(fixture);
    return std::find(all.begin(), all.end(), command) != all.end();
}

} // namespace

TEST_CASE("a clean update runs upload, test, reset, verify and confirm", "[dfu][update]")
{
    // The acceptance path, in both SMP versions.
    const Version version = GENERATE(Version::V1, Version::V2);

    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    SmpClientConfig client_config;
    client_config.smp_version = version;
    Fixture fixture{ServerConfig{}, client_config};
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    // Nothing is emitted from inside start(); the first event arrives on a poll.
    CHECK(outcome.events == 0);

    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    CHECK(outcome.finishes == 1);
    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK_FALSE(outcome.report->rolled_back);
    CHECK_FALSE(outcome.report->revert_pending);
    CHECK(outcome.report->bytes_transferred == update.size());

    // The sequence, and the application's part in it.
    CHECK(issued(fixture, kParams));
    CHECK(issued(fixture, kState));
    CHECK(issued(fixture, kUpload));
    CHECK(issued(fixture, kReset));
    CHECK(outcome.reached(UpdateState::Uploading));
    CHECK(outcome.reached(UpdateState::MarkingForTest));
    CHECK(outcome.reached(UpdateState::AwaitingConfirmation));
    CHECK(outcome.reached(UpdateState::VerifyingConfirmed));

    // The device is running the new image, confirmed and permanent.
    CHECK(fixture.simulator.swap_type() == SwapType::None);
    const ConstBytes primary = fixture.simulator.slot_content(0);
    REQUIRE(primary.size() == update.size());
    CHECK(std::equal(primary.begin(), primary.end(), update.begin()));
}

TEST_CASE("ConfirmImmediately never asks the application", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;
    REQUIRE(fixture.updater.start(source, plan, outcome.handler()).has_value());

    Application application;
    application.confirm = false; // Nothing should be waiting on us.
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(outcome.confirmations == 0);
    CHECK_FALSE(outcome.reached(UpdateState::AwaitingConfirmation));
    CHECK(fixture.simulator.swap_type() == SwapType::None);
}

TEST_CASE("UploadOnly stops once the device holds the image", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    UpdatePlan plan;
    plan.mode = UpdateMode::UploadOnly;
    REQUIRE(fixture.updater.start(source, plan, outcome.handler()).has_value());
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK_FALSE(issued(fixture, kReset));
    CHECK(fixture.simulator.swap_type() == SwapType::None);

    const ConstBytes secondary = fixture.simulator.slot_content(1);
    REQUIRE(secondary.size() == update.size());
    CHECK(std::equal(secondary.begin(), secondary.end(), update.begin()));
}

TEST_CASE("an image already in the secondary slot is not uploaded again", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(outcome.report->upload_skipped);
    CHECK(outcome.report->bytes_transferred == 0);
    CHECK_FALSE(issued(fixture, kUpload));
    CHECK_FALSE(outcome.reached(UpdateState::Uploading));
}

TEST_CASE("the server's own already-present check is reported as a skip too", "[dfu][update]")
{
    // An easy case to get wrong. With the pre-flight check turned
    // off the updater has no idea the device already holds the image, so it
    // starts an upload -- and the *server* ends it on the first packet with its
    // own check (rule 9a). The device wrote nothing, so a report claiming a
    // transfer is a lie, and it is the one a user reads.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update);
    MemoryImageSource source{ConstBytes{update}};

    UpdatePlan plan;
    plan.skip_if_already_present = false;

    REQUIRE(fixture.updater.start(source, plan, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);

    // The upload really was attempted -- that is the difference from the test
    // above, and what makes this the server's answer rather than the client's.
    CHECK(outcome.reached(UpdateState::Uploading));
    CHECK(issued(fixture, kUpload));
    CHECK(fixture.simulator.bytes_written() == 0);

    CHECK(outcome.report->upload_skipped);
}

TEST_CASE("an image already running and confirmed finishes immediately", "[dfu][update]")
{
    const std::vector<std::byte> current = make_firmware(kBodySize, 3, 0, 0, 3);

    UpdateOutcome outcome;
    Fixture fixture;
    fixture.simulator.load_slot(0, current);
    MemoryImageSource source{ConstBytes{current}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(outcome.report->upload_skipped);
    CHECK_FALSE(issued(fixture, kReset));
}

TEST_CASE("an upload interrupted by a disconnect is resumed", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    // Two links: one for the upload's own reconnect, one for the reset's.
    FakeTransport resumed_link;
    FakeTransport rebooted_link;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());

    // Let the transfer start, then drop the link under it.
    REQUIRE(fixture.run_until([&] { return outcome.last_progress > 0; }));
    fixture.transport.disconnect();

    Application application;
    REQUIRE(application.run(fixture, {&resumed_link, &rebooted_link}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    // Two round trips through the application: the dropped upload, and then the
    // reset. The updater asks for a reconnect each time and does neither itself.
    CHECK(outcome.reconnects == 2);
    CHECK(outcome.report->bytes_transferred == update.size());

    const ConstBytes primary = fixture.simulator.slot_content(0);
    REQUIRE(primary.size() == update.size());
    CHECK(std::equal(primary.begin(), primary.end(), update.begin()));
}

TEST_CASE("a busy reset is retried with force", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.reset_busy_once();
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(fixture.simulator.last_reset_force() == true);
}

TEST_CASE("a lost reset response is not a failure", "[dfu][update]")
{
    // The device may reset before its answer goes out (protocol-notes section
    // 9, A3). Giving up here would abandon a device that is already swapping.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());

    // Swallow the answer to the reset, once the update gets that far.
    REQUIRE(fixture.run_until([&] { return fixture.updater.state() == UpdateState::Resetting; },
                              6000, std::chrono::milliseconds{10}));
    fixture.simulator.drop_next_response();

    Application application;
    REQUIRE(
        application.run(fixture, {&reconnected}, outcome, 6000, std::chrono::milliseconds{100}));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(fixture.simulator.dropped() == 1);
}

TEST_CASE("a device that reverts is reported as a rollback", "[dfu][update]")
{
    // The rule the flags exist to protect: a trial boot reports the running image as
    // active-but-unconfirmed, so a revert cannot be recognised from a flag
    // alone. Here the device resets a second time before reconnecting, which is
    // MCUboot undoing the swap.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    Application application;
    application.reboot_twice = true;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    CHECK(outcome.code == ErrorCode::UpdateFailed);
    const UpdateReport& report = fixture.updater.report();
    CHECK(report.final_state == UpdateState::Failed);
    CHECK(report.rolled_back);
    CHECK_FALSE(report.revert_pending);

    // The device really is back on the old image.
    const ConstBytes primary = fixture.simulator.slot_content(0);
    CHECK(std::equal(primary.begin(), primary.end(), running.begin()));
}

TEST_CASE("a refused confirm is terminal and warns that a revert is coming", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());

    // Armed for the *write* specifically: the group's read and write share
    // command 0, and failing the get-state that precedes the confirm would let
    // this test pass without ever reaching the path it is named after.
    Application application;
    application.confirm = false;
    static_cast<void>(application.run(fixture, {&reconnected}, outcome, 2000));
    REQUIRE(outcome.confirmations == 1);
    fixture.simulator.fail_next(ImageError::ImageConfirmationDenied, smply::Operation::Write);
    REQUIRE(fixture.updater.confirm().has_value());
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    CHECK(outcome.code == ErrorCode::ProtocolError);
    const UpdateReport& report = fixture.updater.report();
    CHECK(report.final_state == UpdateState::Failed);
    CHECK(report.revert_pending);
    CHECK_FALSE(report.rolled_back);
}

TEST_CASE("declining to confirm ends the update with a revert pending", "[dfu][update]")
{
    // Not the same as "nothing happened": the device is running the new image
    // and will undo that on its next reset.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());

    Application application;
    application.confirm = false;
    // The update parks in AwaitingConfirmation rather than finishing.
    static_cast<void>(application.run(fixture, {&reconnected}, outcome, 400));
    REQUIRE(outcome.confirmations == 1);
    CHECK_FALSE(outcome.finished());
    CHECK(fixture.updater.state() == UpdateState::AwaitingConfirmation);

    fixture.updater.cancel();
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    CHECK(outcome.code == ErrorCode::Cancelled);
    const UpdateReport& report = fixture.updater.report();
    CHECK(report.final_state == UpdateState::Cancelled);
    CHECK(report.revert_pending);
    CHECK(fixture.simulator.swap_type() == SwapType::Revert);
}

TEST_CASE("an application that cannot reconnect fails the update", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    Application application;
    application.fail_reconnect = true;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    CHECK(outcome.code == ErrorCode::Disconnected);
    const UpdateReport& report = fixture.updater.report();
    CHECK(report.final_state == UpdateState::Failed);
    // The device is mid-swap and nobody confirmed it.
    CHECK(report.revert_pending);
}

TEST_CASE("an image that is not MCUboot firmware is refused before anything is sent",
          "[dfu][update]")
{
    UpdateOutcome outcome;
    Fixture fixture;
    std::vector<std::byte> junk(256, std::byte{0x00});
    MemoryImageSource source{ConstBytes{junk}};

    const auto started = fixture.updater.start(source, UpdatePlan{}, outcome.handler());
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().code() == ErrorCode::InvalidArgument);
    CHECK(fixture.simulator.requests().empty());
}

TEST_CASE("a device without mcumgr parameters still updates", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture{ServerConfig{.supports_mcumgr_params = false}};
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
}

TEST_CASE("cancelling mid-update completes the callback exactly once", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    REQUIRE(fixture.run_until([&] { return outcome.last_progress > 0; }));

    fixture.updater.cancel();
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    CHECK(outcome.finishes == 1);
    CHECK(outcome.code == ErrorCode::Cancelled);
    // Nothing was scheduled on the device, so nothing will revert.
    CHECK_FALSE(fixture.updater.report().revert_pending);
}

TEST_CASE("destroying the updater mid-update completes the callback once", "[dfu][update]")
{
    // The lifetime rule, now three deep: the outcome must outlive the updater,
    // the groups and the client, all of which finish outstanding work as they
    // are destroyed.
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    {
        Fixture fixture;
        fixture.simulator.load_slot(0, make_firmware(kBodySize, 1, 0, 0, 1));
        MemoryImageSource source{ConstBytes{update}};
        REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
        REQUIRE(fixture.run_until([&] { return outcome.last_progress > 0; }));
        REQUIRE(outcome.finishes == 0);
    }

    CHECK(outcome.finishes == 1);
    CHECK(outcome.code == ErrorCode::Cancelled);
}

TEST_CASE("a second update cannot start while one is running", "[dfu][update]")
{
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    UpdateOutcome second;
    Fixture fixture;
    fixture.simulator.load_slot(0, make_firmware(kBodySize, 1, 0, 0, 1));
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    const auto again = fixture.updater.start(source, UpdatePlan{}, second.handler());
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code() == ErrorCode::InvalidState);

    // And the operations that need a particular state say so rather than
    // silently doing nothing.
    CHECK(fixture.updater.confirm().error().code() == ErrorCode::InvalidState);
    CHECK(fixture.updater.resume_after_reconnect().error().code() == ErrorCode::InvalidState);
}

TEST_CASE("an update refuses arguments it cannot honour", "[dfu][update]")
{
    UpdateOutcome outcome;
    Fixture fixture;
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);
    MemoryImageSource source{ConstBytes{update}};

    // No callback: there would be nowhere to report the outcome.
    const auto no_handler = fixture.updater.start(source, UpdatePlan{}, {});
    REQUIRE_FALSE(no_handler.has_value());
    CHECK(no_handler.error().code() == ErrorCode::InvalidArgument);

    // Shorter than an image header.
    std::vector<std::byte> stub(8, std::byte{0x00});
    MemoryImageSource tiny{ConstBytes{stub}};
    const auto too_short = fixture.updater.start(tiny, UpdatePlan{}, outcome.handler());
    REQUIRE_FALSE(too_short.has_value());
    CHECK(too_short.error().code() == ErrorCode::InvalidArgument);

    // A well-formed image carrying no hash TLV: nothing could recognise it in
    // the device's slot table afterwards, so every later check would be
    // guesswork.
    smply::test::ImageBuilder builder;
    builder.version(4, 0, 0, 0).body(kBodySize);
    const std::vector<std::byte> unsigned_image = builder.build();
    MemoryImageSource without_hash{ConstBytes{unsigned_image}};
    const auto no_hash = fixture.updater.start(without_hash, UpdatePlan{}, outcome.handler());
    REQUIRE_FALSE(no_hash.has_value());
    CHECK(no_hash.error().code() == ErrorCode::InvalidArgument);

    CHECK(fixture.simulator.requests().empty());
}

TEST_CASE("an idle updater ignores the operations that need a running update", "[dfu][update]")
{
    Fixture fixture;
    // Neither does anything, and neither is an error the caller has to handle:
    // an application tidying up after a finished update should not have to
    // remember whether it already stopped.
    fixture.updater.cancel();
    fixture.updater.reconnect_failed(smply::Error{ErrorCode::Disconnected});
    CHECK(fixture.updater.state() == UpdateState::Idle);
    CHECK(fixture.updater.next_deadline() == std::nullopt);
}

TEST_CASE("cancelling before the transfer starts schedules nothing", "[dfu][update]")
{
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    Fixture fixture;
    fixture.simulator.load_slot(0, make_firmware(kBodySize, 1, 0, 0, 1));
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    fixture.updater.cancel(); // Before a single request has gone out.
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    CHECK(outcome.finishes == 1);
    CHECK(outcome.code == ErrorCode::Cancelled);
    CHECK(fixture.simulator.swap_type() == SwapType::None);
}

TEST_CASE("a device that never drops the link is given up on after the grace period",
          "[dfu][update]")
{
    // A reset response is acceptance, not completion, and a link that stays up
    // is not proof the device ignored it. The verify after the reboot is the
    // real check, so the updater carries on rather than failing here.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    UpdatePlan plan;
    plan.disconnect_grace = std::chrono::seconds{2};
    REQUIRE(fixture.updater.start(source, plan, outcome.handler()).has_value());

    // Wait for the reset to be accepted, then reboot the device *without*
    // dropping the link, so only the grace timer can move things on.
    REQUIRE(fixture.run_until([&] { return outcome.disconnects > 0; }, 6000,
                              std::chrono::milliseconds{10}));
    CHECK(fixture.updater.next_deadline().has_value());
    fixture.simulator.reboot();

    Application application;
    application.reboot_on_disconnect = false;
    application.disconnects_served = outcome.disconnects; // The link stays up.
    REQUIRE(
        application.run(fixture, {&reconnected}, outcome, 6000, std::chrono::milliseconds{100}));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
}

TEST_CASE("a device that refuses to mark the image is a clean failure", "[dfu][update]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update); // Present, so the mark is the first write.
    MemoryImageSource source{ConstBytes{update}};

    // Again for the write only, so the refusal lands on set-state and not on
    // the get-state that reads the slot table first.
    fixture.simulator.fail_next(ImageError::ImageSettingTestToActiveDenied,
                                smply::Operation::Write);
    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    CHECK(outcome.code == ErrorCode::ProtocolError);
    const UpdateReport& report = fixture.updater.report();
    CHECK(report.final_state == UpdateState::Failed);
    // Nothing was scheduled, so nothing will revert.
    CHECK_FALSE(report.revert_pending);
    CHECK(fixture.simulator.swap_type() == SwapType::None);
}

TEST_CASE("a lost mark-for-test is recovered over SMP v1", "[dfu][update]")
{
    // A24, end to end. The device is a default one, which means
    // `translate_v1_errors` is on -- it is built as the bench peer is, with
    // CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL -- and the client speaks v1,
    // which is smply's default. So the refusal arrives as a flat `rc = 6` with
    // no group at all, and `image_error()` on it is `nullopt`.
    //
    // A recovery that branched on the group-scoped code alone would fail
    // here. It is the shape a real device produces, and a test that injects
    // only the group-scoped shape by hand is blind to it.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update); // Present, so the mark is the first write.
    MemoryImageSource source{ConstBytes{update}};

    // Write only, so the refusal lands on set-state rather than on the
    // get-state that reads the slot table first.
    fixture.simulator.fail_next(ImageError::ImageAlreadyPending, smply::Operation::Write);
    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    // The recovery is the second visit to InspectingImages: the machine went
    // back and re-planned rather than failing.
    CHECK(std::count(outcome.visited.begin(), outcome.visited.end(),
                     UpdateState::InspectingImages) == 2);
}

TEST_CASE("the same refusal in its group-scoped shape still recovers", "[dfu][update]")
{
    // The control for the case above, and the reason it is not a regression.
    // With the v1 translation off the server sends `err: {group, rc}` even to a
    // v1 client, so `image_error()` answers and the original branch is the one
    // that fires. Both shapes must reach the same place.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture{ServerConfig{.translate_v1_errors = false}};
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update);
    MemoryImageSource source{ConstBytes{update}};

    fixture.simulator.fail_next(ImageError::ImageAlreadyPending, smply::Operation::Write);
    REQUIRE(fixture.updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(std::count(outcome.visited.begin(), outcome.visited.end(),
                     UpdateState::InspectingImages) == 2);
}

TEST_CASE("a caller-supplied buffer size is used as given", "[dfu][update]")
{
    // The plan wins over what the device reports: an application that already
    // knows the budget should not have it silently overwritten.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture{ServerConfig{.buf_size = 512}};
    fixture.simulator.load_slot(0, running);
    MemoryImageSource source{ConstBytes{update}};

    UpdatePlan plan;
    plan.upload.server_buf_size = 128;
    REQUIRE(fixture.updater.start(source, plan, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    // A 128-byte budget means more, smaller chunks than the device's own 512.
    const auto all = commands(fixture);
    const auto uploads = static_cast<std::size_t>(std::count(all.begin(), all.end(), kUpload));
    CHECK(uploads > 6);
}

TEST_CASE("a callback that outlives the updater does nothing", "[dfu][update][lifetime]")
{
    // The updater is destroyed with a request still in flight and the client
    // still alive, so that request's callback fires afterwards. It must find an
    // expired guard and return, not a dangling `this`. Clang's ASan is the only
    // thing that catches getting this wrong, which is why it has its own test
    // rather than riding along on another.
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    UpdateOutcome outcome;
    FakeTransport transport;
    smply::test::ManualClock clock;
    smply::test::ServerSimulator simulator{transport};
    smply::SmpClient client{transport, clock};
    smply::ImageManagement image{client};
    smply::OsManagement os{client};
    MemoryImageSource source{ConstBytes{update}};

    {
        smply::FirmwareUpdater updater{client, image, os};
        REQUIRE(updater.start(source, UpdatePlan{}, outcome.handler()).has_value());
        // One turn: the first request is out and unanswered.
        client.poll(clock.now());
        clock.advance(std::chrono::milliseconds{1});
        CHECK(updater.state() != UpdateState::Idle);
    }

    // The device answers a request whose updater no longer exists.
    for (int i = 0; i < 5; ++i) {
        simulator.pump(clock.now());
        client.poll(clock.now());
        clock.advance(std::chrono::milliseconds{1});
    }
    CHECK(outcome.finishes == 1);
    CHECK(outcome.code == ErrorCode::Cancelled);
}

TEST_CASE("a source that cannot be read is refused", "[dfu][update]")
{
    UpdateOutcome outcome;
    Fixture fixture;
    smply::test::FailingImageSource source{1024};

    const auto started = fixture.updater.start(source, UpdatePlan{}, outcome.handler());
    REQUIRE_FALSE(started.has_value());
    CHECK(fixture.simulator.requests().empty());
}

TEST_CASE("an image with a broken TLV area is refused", "[dfu][update]")
{
    // The scan fails rather than returning "no hash", and the difference
    // matters: one is a file that was never signed, the other is a file that
    // has been damaged.
    UpdateOutcome outcome;
    Fixture fixture;

    smply::test::ImageBuilder builder;
    builder.version(5, 0, 0, 0).body(kBodySize).tlv(0x10, std::vector<std::byte>(32));
    builder.unprotected_total(0xFFFF); // Overruns the file.
    const std::vector<std::byte> damaged = builder.build();
    MemoryImageSource source{ConstBytes{damaged}};

    const auto started = fixture.updater.start(source, UpdatePlan{}, outcome.handler());
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().code() == ErrorCode::MalformedMessage);
    CHECK(fixture.simulator.requests().empty());
}

// --- Image >= 1 (O5, ADR-0021) ----------------------------------------------

namespace {

/// A two-image device: image 0 runs v1.0.0, image 1 runs v5.0.0. The knob
/// decides whether a confirm may reach image 1 (protocol-notes A27).
[[nodiscard]] ServerConfig two_image_device(bool allow_confirm_image_1)
{
    ServerConfig config;
    config.image_count = 2;
    config.allow_confirm_non_active_image_any = allow_confirm_image_1;
    return config;
}

[[nodiscard]] SmpClientConfig v2_client()
{
    SmpClientConfig config;
    config.smp_version = Version::V2; // keeps the group-scoped code (A16)
    return config;
}

[[nodiscard]] UpdatePlan plan_for_image(std::uint32_t image)
{
    UpdatePlan plan;
    plan.upload.image = image;
    return plan;
}

[[nodiscard]] bool same_bytes(ConstBytes actual, const std::vector<std::byte>& expected)
{
    return actual.size() == expected.size() &&
           std::equal(actual.begin(), actual.end(), expected.begin());
}

} // namespace

TEST_CASE("image 1 is updated and image 0 is left exactly as it was", "[dfu][update][multi]")
{
    const std::vector<std::byte> app = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> other_running = make_firmware(kBodySize, 5, 0, 0, 5);
    const std::vector<std::byte> other_update = make_firmware(kBodySize, 6, 0, 0, 6);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture{two_image_device(true), v2_client()};
    fixture.simulator.load_slot(0, app);
    fixture.simulator.load_slot(2, other_running);
    MemoryImageSource source{ConstBytes{other_update}};

    REQUIRE(fixture.updater.start(source, plan_for_image(1), outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK_FALSE(outcome.report->rolled_back);

    // Image 1 now runs the update, confirmed; image 0 was never touched.
    CHECK(same_bytes(fixture.simulator.slot_content(2), other_update));
    CHECK(fixture.simulator.swap_type(1) == SwapType::None);
    CHECK(same_bytes(fixture.simulator.slot_content(0), app));
    CHECK(fixture.simulator.slot_content(1).empty());
    CHECK(fixture.simulator.swap_type(0) == SwapType::None);
}

TEST_CASE("an interrupted image-1 upload resumes on image 1", "[dfu][update][multi]")
{
    const std::vector<std::byte> app = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> other_running = make_firmware(kBodySize, 5, 0, 0, 5);
    const std::vector<std::byte> other_update = make_firmware(kBodySize, 6, 0, 0, 6);

    UpdateOutcome outcome;
    FakeTransport resumed_link;
    FakeTransport rebooted_link;
    Fixture fixture{two_image_device(true), v2_client()};
    fixture.simulator.load_slot(0, app);
    fixture.simulator.load_slot(2, other_running);
    MemoryImageSource source{ConstBytes{other_update}};

    REQUIRE(fixture.updater.start(source, plan_for_image(1), outcome.handler()).has_value());
    REQUIRE(fixture.run_until([&] { return outcome.last_progress > 0; }));
    fixture.transport.disconnect();

    Application application;
    REQUIRE(application.run(fixture, {&resumed_link, &rebooted_link}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(outcome.reconnects == 2);
    CHECK(same_bytes(fixture.simulator.slot_content(2), other_update));
    CHECK(fixture.simulator.slot_content(1).empty()); // nothing strayed into image 0
}

TEST_CASE("a revert of image 1 is reported as a rollback", "[dfu][update][multi]")
{
    const std::vector<std::byte> app = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> other_running = make_firmware(kBodySize, 5, 0, 0, 5);
    const std::vector<std::byte> other_update = make_firmware(kBodySize, 6, 0, 0, 6);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture{two_image_device(true), v2_client()};
    fixture.simulator.load_slot(0, app);
    fixture.simulator.load_slot(2, other_running);
    MemoryImageSource source{ConstBytes{other_update}};

    REQUIRE(fixture.updater.start(source, plan_for_image(1), outcome.handler()).has_value());
    Application application;
    application.reboot_twice = true;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    CHECK(outcome.code == ErrorCode::UpdateFailed);
    CHECK(fixture.updater.report().rolled_back);
    CHECK(same_bytes(fixture.simulator.slot_content(2), other_running));
}

TEST_CASE("a plan for an image the device does not have fails cleanly", "[dfu][update][multi]")
{
    // A device lists nothing for an image it has never been flashed with, so
    // the updater cannot tell "absent" from "empty" before trying; the server's
    // first-packet answer is the authority (protocol-notes section 6).
    const std::vector<std::byte> app = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 6, 0, 0, 6);

    UpdateOutcome outcome;
    Fixture fixture{two_image_device(true), v2_client()};
    fixture.simulator.load_slot(0, app);
    MemoryImageSource source{ConstBytes{update}};

    REQUIRE(fixture.updater.start(source, plan_for_image(2), outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {}, outcome));

    CHECK(outcome.code == ErrorCode::ProtocolError);
    const UpdateReport& report = fixture.updater.report();
    REQUIRE(report.cause.has_value());
    CHECK(smply::image_error(*report.cause) == ImageError::NoFreeSlot);
    CHECK_FALSE(outcome.reached(UpdateState::Resetting));
    CHECK(same_bytes(fixture.simulator.slot_content(0), app));
}

TEST_CASE("a default build refuses to confirm image 1, and the update says so",
          "[dfu][update][multi]")
{
    // A27: the confirm names image 1 by hash -- a hashless one would confirm
    // image 0 -- and Zephyr denies it without the Kconfig. The update fails
    // with the device's own reason and warns that the trial will revert.
    const std::vector<std::byte> app = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> other_running = make_firmware(kBodySize, 5, 0, 0, 5);
    const std::vector<std::byte> other_update = make_firmware(kBodySize, 6, 0, 0, 6);

    UpdateOutcome outcome;
    FakeTransport reconnected;
    Fixture fixture{two_image_device(false), v2_client()};
    fixture.simulator.load_slot(0, app);
    fixture.simulator.load_slot(2, other_running);
    MemoryImageSource source{ConstBytes{other_update}};

    REQUIRE(fixture.updater.start(source, plan_for_image(1), outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    CHECK(outcome.code == ErrorCode::ProtocolError);
    const UpdateReport& report = fixture.updater.report();
    REQUIRE(report.cause.has_value());
    CHECK(smply::image_error(*report.cause) == ImageError::ImageConfirmationDenied);
    CHECK(report.revert_pending);
    // Image 0 stayed confirmed: the confirm did not land on it instead.
    CHECK(fixture.simulator.swap_type(0) == SwapType::None);
    CHECK(fixture.simulator.swap_type(1) == SwapType::Revert);
}

// --- Several images in one update (ADR-0021) --------------------------------

namespace {

/// The coordinating-MCU product of docs/multi-image.md: image 0 is the host's
/// application, committed by smply; image 1 is the second MCU's, committed by
/// the device once it has applied it.
///
/// Declared before the fixture, like `UpdateOutcome`: the sources view these
/// bytes and must outlive the update.
struct AppAndRadio
{
    std::vector<std::byte> app_running = make_firmware(kBodySize, 1, 0, 0, 1);
    std::vector<std::byte> app_update = make_firmware(kBodySize, 2, 0, 0, 2);
    std::vector<std::byte> radio_running = make_firmware(kBodySize, 5, 0, 0, 5);
    std::vector<std::byte> radio_update = make_firmware(kBodySize, 6, 0, 0, 6);
    MemoryImageSource app{ConstBytes{app_update}};
    MemoryImageSource radio{ConstBytes{radio_update}};
    std::array<ImageTarget, 2> targets{
        ImageTarget{.image = 0, .source = &app, .commit = CommitBy::Client},
        ImageTarget{.image = 1, .source = &radio, .commit = CommitBy::Device}};

    /// Loads the running images, and makes image 1 device-committed.
    void install(Fixture& fixture, ApplyOutcome outcome, unsigned reads,
                 CommitOutcome commit = CommitOutcome::Commits, unsigned commit_reads = 1) const
    {
        fixture.simulator.load_slot(0, app_running);
        fixture.simulator.load_slot(2, radio_running);
        fixture.simulator.device_commits(1, outcome, reads, commit, commit_reads);
    }
};

/// Plays the application, one turn at a time, until \p done.
template<class Predicate>
[[nodiscard]] bool drive_until(Application& application, Fixture& fixture,
                               const std::vector<FakeTransport*>& spares, UpdateOutcome& outcome,
                               Predicate done)
{
    for (int i = 0; i < 6000 && !done(); ++i) {
        static_cast<void>(application.run(fixture, spares, outcome, 1));
    }
    return done();
}

[[nodiscard]] std::size_t resets(const Fixture& fixture)
{
    const std::vector<Command> all = commands(fixture);
    return static_cast<std::size_t>(std::count(all.begin(), all.end(), kReset));
}

} // namespace

TEST_CASE("two images, one reset, and image 0 confirmed only after the device applied image 1",
          "[dfu][update][multi]")
{
    UpdateOutcome outcome;
    FakeTransport reconnected;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 3);

    REQUIRE(fixture.updater.start(images.targets, UpdatePlan{}, outcome.handler()).has_value());

    // While waiting, the poll timer is what the application must wake for.
    Application application;
    REQUIRE(drive_until(application, fixture, {&reconnected}, outcome, [&] {
        return fixture.updater.state() == UpdateState::AwaitingDeviceApply;
    }));
    CHECK(fixture.updater.next_deadline().has_value());
    CHECK(outcome.confirmations == 0);

    REQUIRE(application.run(fixture, {&reconnected}, outcome));
    REQUIRE(outcome.report.has_value());
    const UpdateReport& report = *outcome.report;
    CHECK(report.final_state == UpdateState::Completed);
    CHECK_FALSE(report.revert_pending);
    CHECK(report.bytes_transferred == images.app_update.size() + images.radio_update.size());
    REQUIRE(report.images.size() == 2);
    CHECK(report.images[1].applied);
    CHECK(report.images[1].committed);
    CHECK(report.images[1].commit == CommitBy::Device);
    // The device committed image 1 only after image 0 was confirmed (ADR-0022).
    CHECK(outcome.reached(UpdateState::AwaitingDeviceCommit));

    CHECK(resets(fixture) == 1);
    CHECK(outcome.confirmations == 1);
    // Both images run their update, both committed -- image 1 by the device,
    // which a default build requires (A27).
    CHECK(same_bytes(fixture.simulator.slot_content(0), images.app_update));
    CHECK(fixture.simulator.swap_type(0) == SwapType::None);
    CHECK(same_bytes(fixture.simulator.slot_content(2), images.radio_update));
    CHECK(fixture.simulator.swap_type(1) == SwapType::None);
}

TEST_CASE("a failed device apply leaves image 0 unconfirmed", "[dfu][update][multi]")
{
    UpdateOutcome outcome;
    FakeTransport reconnected;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Failed, 2);

    REQUIRE(fixture.updater.start(images.targets, UpdatePlan{}, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    CHECK(outcome.code == ErrorCode::UpdateFailed);
    const UpdateReport& report = fixture.updater.report();
    CHECK(report.revert_pending);
    REQUIRE(report.images.size() == 2);
    CHECK_FALSE(report.images[1].applied);
    CHECK(outcome.confirmations == 0);
    // Image 0 is still on trial: the next reset takes it back.
    CHECK(fixture.simulator.swap_type(0) == SwapType::Revert);
    CHECK(same_bytes(fixture.simulator.slot_content(2), images.radio_running));
}

TEST_CASE("a device that never finishes applying times out", "[dfu][update][multi]")
{
    UpdateOutcome outcome;
    FakeTransport reconnected;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 1'000'000);

    UpdatePlan plan;
    plan.apply_timeout = std::chrono::seconds{2};
    plan.apply_poll_interval = std::chrono::milliseconds{200};
    REQUIRE(fixture.updater.start(images.targets, plan, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    CHECK(outcome.code == ErrorCode::Timeout);
    CHECK(fixture.updater.report().revert_pending);
    CHECK(outcome.confirmations == 0);
    CHECK(fixture.simulator.swap_type(0) == SwapType::Revert);
}

TEST_CASE("an update resumed while the device applies waits, and does not reset again",
          "[dfu][update][multi]")
{
    UpdateOutcome first;
    UpdateOutcome second;
    FakeTransport reconnected;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 20);

    // The first process gets as far as the wait, then goes away.
    REQUIRE(fixture.updater.start(images.targets, UpdatePlan{}, first.handler()).has_value());
    Application application;
    REQUIRE(drive_until(application, fixture, {&reconnected}, first,
                        [&] { return first.reached(UpdateState::AwaitingDeviceApply); }));
    fixture.updater.cancel();
    REQUIRE(fixture.run_until([&] { return first.finished(); }));

    // A second one picks it up from the slot table alone.
    REQUIRE(fixture.updater.start(images.targets, UpdatePlan{}, second.handler()).has_value());
    Application again;
    REQUIRE(again.run(fixture, {}, second));

    REQUIRE(second.report.has_value());
    CHECK(second.report->final_state == UpdateState::Completed);
    CHECK(second.report->upload_skipped);
    CHECK(second.reached(UpdateState::AwaitingDeviceApply));
    CHECK_FALSE(second.reached(UpdateState::Resetting));
    CHECK(resets(fixture) == 1);
    CHECK(fixture.simulator.swap_type(0) == SwapType::None);
    CHECK(fixture.simulator.swap_type(1) == SwapType::None);
}

TEST_CASE("an update resumed after the device applied goes to the confirmation window",
          "[dfu][update][multi]")
{
    UpdateOutcome first;
    UpdateOutcome second;
    FakeTransport reconnected;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 1);

    // The first process reaches the window and never answers it.
    REQUIRE(fixture.updater.start(images.targets, UpdatePlan{}, first.handler()).has_value());
    Application undecided;
    undecided.confirm = false;
    REQUIRE(drive_until(undecided, fixture, {&reconnected}, first,
                        [&] { return first.confirmations > 0; }));
    fixture.updater.cancel();
    REQUIRE(fixture.run_until([&] { return first.finished(); }));
    CHECK(fixture.updater.report().revert_pending);

    REQUIRE(fixture.updater.start(images.targets, UpdatePlan{}, second.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {}, second));

    REQUIRE(second.report.has_value());
    CHECK(second.report->final_state == UpdateState::Completed);
    CHECK(second.confirmations == 1);
    CHECK_FALSE(second.reached(UpdateState::Resetting));
    CHECK(resets(fixture) == 1);
    CHECK(fixture.simulator.swap_type(0) == SwapType::None);
}

TEST_CASE("an image-1 upload interrupted after image 0 finished resumes on image 1",
          "[dfu][update][multi]")
{
    UpdateOutcome outcome;
    FakeTransport resumed_link;
    FakeTransport rebooted_link;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 1);

    REQUIRE(fixture.updater.start(images.targets, UpdatePlan{}, outcome.handler()).has_value());
    // The second upload has started and is part-way through.
    REQUIRE(fixture.run_until([&] {
        return std::count(outcome.visited.begin(), outcome.visited.end(), UpdateState::Uploading) ==
                   2 &&
               outcome.last_progress > 0 && outcome.last_progress < images.radio_update.size();
    }));
    fixture.transport.disconnect();

    Application application;
    REQUIRE(application.run(fixture, {&resumed_link, &rebooted_link}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(outcome.reconnects == 2);
    CHECK(resets(fixture) == 1);
    CHECK(same_bytes(fixture.simulator.slot_content(0), images.app_update));
    CHECK(same_bytes(fixture.simulator.slot_content(2), images.radio_update));
}

TEST_CASE("two client images are each confirmed by their own hash", "[dfu][update][multi]")
{
    // A device built to allow it (A27) lets smply commit image 1 too.
    UpdateOutcome outcome;
    FakeTransport reconnected;
    AppAndRadio images;
    images.targets[1].commit = CommitBy::Client;
    Fixture fixture{two_image_device(true), v2_client()};
    fixture.simulator.load_slot(0, images.app_running);
    fixture.simulator.load_slot(2, images.radio_running);

    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;
    REQUIRE(fixture.updater.start(images.targets, plan, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK_FALSE(outcome.reached(UpdateState::AwaitingDeviceApply));
    CHECK(fixture.simulator.swap_type(0) == SwapType::None);
    CHECK(fixture.simulator.swap_type(1) == SwapType::None);
    CHECK(same_bytes(fixture.simulator.slot_content(2), images.radio_update));
}

TEST_CASE("an image list start() cannot honour is refused before anything is sent",
          "[dfu][update][multi]")
{
    UpdateOutcome outcome;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};

    const auto refused = [&](std::span<const ImageTarget> targets, const UpdatePlan& plan) {
        const auto started = fixture.updater.start(targets, plan, outcome.handler());
        return !started.has_value() && started.error().code() == ErrorCode::InvalidArgument;
    };

    CHECK(refused({}, UpdatePlan{}));

    std::array<ImageTarget, 2> no_source = images.targets;
    no_source[1].source = nullptr;
    CHECK(refused(no_source, UpdatePlan{}));

    std::array<ImageTarget, 2> twice = images.targets;
    twice[1].image = 0;
    CHECK(refused(twice, UpdatePlan{}));

    UpdatePlan with_sha;
    const smply::Result<smply::Hash> sha = smply::sha256(images.app);
    REQUIRE(sha.has_value());
    with_sha.upload.sha = *sha;
    CHECK(refused(images.targets, with_sha));

    UpdatePlan no_interval;
    no_interval.apply_poll_interval = smply::Duration::zero();
    CHECK(refused(images.targets, no_interval));

    CHECK(fixture.simulator.requests().empty());
    CHECK(outcome.events == 0);
}

// --- ADR-0022: commit after the confirm, and a link that drops mid-apply -----

TEST_CASE("a commit that never comes fails the update, with nothing left to revert",
          "[dfu][update][multi]")
{
    UpdateOutcome outcome;
    FakeTransport reconnected;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 1, CommitOutcome::Never);

    UpdatePlan plan;
    plan.apply_timeout = std::chrono::seconds{2};
    plan.apply_poll_interval = std::chrono::milliseconds{200};
    REQUIRE(fixture.updater.start(images.targets, plan, outcome.handler()).has_value());
    Application application;
    REQUIRE(application.run(fixture, {&reconnected}, outcome));

    CHECK(outcome.code == ErrorCode::Timeout);
    const UpdateReport& report = fixture.updater.report();
    CHECK(report.final_state == UpdateState::Failed);
    // Image 0 is confirmed, so nothing reverts; the device's boot-time logic
    // owns the rest (multi-image.md).
    CHECK_FALSE(report.revert_pending);
    CHECK(fixture.simulator.swap_type(0) == SwapType::None);
    REQUIRE(report.images.size() == 2);
    CHECK(report.images[1].applied);
    CHECK_FALSE(report.images[1].committed);
}

TEST_CASE("the link dropping while the device applies its image is a reconnect, not a failure",
          "[dfu][update][multi]")
{
    // On the product the BLE link runs through the MCU being updated.
    UpdateOutcome outcome;
    FakeTransport after_reset;
    FakeTransport after_apply;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 20);

    UpdatePlan plan;
    plan.apply_poll_interval = std::chrono::milliseconds{50};
    REQUIRE(fixture.updater.start(images.targets, plan, outcome.handler()).has_value());
    Application application;
    const std::vector<FakeTransport*> spares{&after_reset, &after_apply};
    REQUIRE(drive_until(application, fixture, spares, outcome, [&] {
        return fixture.updater.state() == UpdateState::AwaitingDeviceApply;
    }));
    after_reset.disconnect();

    REQUIRE(application.run(fixture, spares, outcome));
    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(outcome.reconnects == 2);
    CHECK(resets(fixture) == 1);
    CHECK(outcome.report->images[1].committed);
}

TEST_CASE("a read lost while the device applies its image is asked again", "[dfu][update][multi]")
{
    UpdateOutcome outcome;
    FakeTransport reconnected;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 5);

    UpdatePlan plan;
    plan.apply_poll_interval = std::chrono::milliseconds{50};
    REQUIRE(fixture.updater.start(images.targets, plan, outcome.handler()).has_value());
    Application application;
    REQUIRE(drive_until(application, fixture, {&reconnected}, outcome, [&] {
        return fixture.updater.state() == UpdateState::AwaitingDeviceApply;
    }));
    fixture.simulator.drop_next_response();

    REQUIRE(application.run(fixture, {&reconnected}, outcome));
    REQUIRE(outcome.report.has_value());
    CHECK(outcome.report->final_state == UpdateState::Completed);
    CHECK(fixture.simulator.dropped() == 1);
    CHECK(outcome.reconnects == 1);
}

TEST_CASE("an update resumed after the confirm waits for the device's commit",
          "[dfu][update][multi]")
{
    UpdateOutcome first;
    UpdateOutcome second;
    FakeTransport reconnected;
    AppAndRadio images;
    Fixture fixture{two_image_device(false), v2_client()};
    images.install(fixture, ApplyOutcome::Applied, 1, CommitOutcome::Commits, 40);
    UpdatePlan plan;
    plan.apply_poll_interval = std::chrono::milliseconds{50};

    // The first process confirms image 0, then goes away while the device is
    // still committing image 1.
    REQUIRE(fixture.updater.start(images.targets, plan, first.handler()).has_value());
    Application application;
    REQUIRE(drive_until(application, fixture, {&reconnected}, first,
                        [&] { return first.reached(UpdateState::AwaitingDeviceCommit); }));
    fixture.updater.cancel();
    REQUIRE(fixture.run_until([&] { return first.finished(); }));
    CHECK_FALSE(fixture.updater.report().revert_pending);

    REQUIRE(fixture.updater.start(images.targets, plan, second.handler()).has_value());
    Application again;
    REQUIRE(again.run(fixture, {}, second));

    REQUIRE(second.report.has_value());
    CHECK(second.report->final_state == UpdateState::Completed);
    CHECK(second.reached(UpdateState::AwaitingDeviceCommit));
    CHECK_FALSE(second.reached(UpdateState::Resetting));
    CHECK(second.confirmations == 0);
    CHECK(second.report->images[1].committed);
    CHECK(resets(fixture) == 1);
}
