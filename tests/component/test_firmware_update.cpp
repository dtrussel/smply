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

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::ImageError;
using smply::MemoryImageSource;
using smply::SmpClientConfig;
using smply::UpdateEvent;
using smply::UpdateMode;
using smply::UpdatePlan;
using smply::UpdateReport;
using smply::UpdateState;
using smply::Version;
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
    std::vector<UpdateEvent::Kind> kinds;
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
            kinds.push_back(event.kind);
            switch (event.kind) {
            case UpdateEvent::Kind::StateChanged:
                visited.push_back(event.to);
                break;
            case UpdateEvent::Kind::Progress:
                last_progress = event.progress.transferred;
                break;
            case UpdateEvent::Kind::DisconnectExpected:
                ++disconnects;
                break;
            case UpdateEvent::Kind::ReconnectRequired:
                ++reconnects;
                break;
            case UpdateEvent::Kind::ConfirmationRequired:
                ++confirmations;
                break;
            case UpdateEvent::Kind::Finished:
                ++finishes;
                if (event.result->has_value()) {
                    report = **event.result;
                } else {
                    // A failed update has no report *value*; the updater keeps
                    // one, and a test reads it from there.
                    code = (*event.result).error().code();
                }
                break;
            }
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

/// The commands a run issued, as (group, command) pairs.
[[nodiscard]] std::vector<std::pair<std::uint16_t, std::uint8_t>> commands(const Fixture& fixture)
{
    std::vector<std::pair<std::uint16_t, std::uint8_t>> out;
    for (const smply::Header& header : fixture.simulator.requests()) {
        out.emplace_back(static_cast<std::uint16_t>(header.group), header.command);
    }
    return out;
}

constexpr std::pair<std::uint16_t, std::uint8_t> kParams{0, 6};
constexpr std::pair<std::uint16_t, std::uint8_t> kState{1, 0};
constexpr std::pair<std::uint16_t, std::uint8_t> kUpload{1, 1};
constexpr std::pair<std::uint16_t, std::uint8_t> kReset{0, 5};

[[nodiscard]] bool issued(const Fixture& fixture, std::pair<std::uint16_t, std::uint8_t> command)
{
    const auto all = commands(fixture);
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
    CHECK(outcome.kinds.empty());

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
    // The rule P11 exists to protect: a trial boot reports the running image as
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
