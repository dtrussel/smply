// SPDX-License-Identifier: Apache-2.0

/// \file
/// The hardware interoperability cases (docs/testing.md section 6, roadmap P17b).
///
/// Every case starts from whatever the device is running and works out its own
/// target: the image (A or B) that is *not* active. That is what lets the
/// supervisor reflash the baseline between groups rather than between cases,
/// and lets a person run one case by hand against a device in either state.
///
/// Each case prints its timeline and `HIL-METRIC` lines on stdout; the
/// supervisor keeps them as evidence and turns the metrics into a table. The
/// assertions are on structured outcomes -- states, flags, hashes, counters --
/// never on the timeline text (testing.md section 7).

#include "support/bench.hpp"
#include "support/rig.hpp"

#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/image_source.hpp"
#include "smply/mcuboot_image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace smply;      // NOLINT(google-build-using-namespace) -- a test
using namespace smply::hil; // NOLINT(google-build-using-namespace)

namespace {

/// The two firmware images, and which one the device is running.
struct Images
{
    std::vector<std::byte> a;
    std::vector<std::byte> b;
    ImageHash hash_a;
    ImageHash hash_b;

    /// The image that is not active: the one an update should install.
    [[nodiscard]] const std::vector<std::byte>& other_than(const ImageHash& running) const
    {
        return running == hash_a ? b : a;
    }

    [[nodiscard]] const ImageHash& hash_other_than(const ImageHash& running) const
    {
        return running == hash_a ? hash_b : hash_a;
    }
};

Images load_images(const Bench& bench)
{
    Images images;
    images.a = *read_file(bench.image_a);
    images.b = *read_file(bench.image_b);
    MemoryImageSource a{ConstBytes{images.a}};
    MemoryImageSource b{ConstBytes{images.b}};
    const auto ha = image_hash_of(a);
    const auto hb = image_hash_of(b);
    REQUIRE(ha.has_value());
    REQUIRE(hb.has_value());
    images.hash_a = *ha;
    images.hash_b = *hb;
    REQUIRE_FALSE(images.hash_a == images.hash_b);
    return images;
}

/// The slot the device is booting from, which every case begins by reading.
const ImageSlot& active_slot(const ImageState& state)
{
    const auto it = std::find_if(state.slots.begin(), state.slots.end(),
                                 [](const ImageSlot& slot) { return slot.active; });
    REQUIRE(it != state.slots.end());
    return *it;
}

struct Session
{
    Bench bench = require_bench();
    Images images = load_images(bench);
    Rig rig{bench};
    ImageHash running;

    // Uploads hold their source by reference, and a resume reads from it again
    // long after upload() returned -- so the source must outlive the whole
    // Session, not the upload() call (handoff.md "Lifetime"). Kept here as owned
    // pointers; MemoryImageSource is a view over the image bytes, which live in
    // `images` above and so also outlive it.
    std::vector<std::unique_ptr<MemoryImageSource>> sources;

    Session()
    {
        // A constructor that throws never runs its destructor, so the timeline
        // below -- which is where the *reason* a connect failed is recorded
        // (Rig::connect notes the error) -- would be lost precisely in the case
        // worth diagnosing. P17b spent a bench run discovering that: a forced
        // discovery failure produced a bare 'REQUIRE(rig.connect())' and no
        // error string anywhere in the evidence bundle. Handled here rather
        // than in a function-try-block, whose handler may not touch members.
        try {
            REQUIRE(rig.connect().has_value());
            const auto state = rig.read_state();
            REQUIRE(state.has_value());
            const ImageSlot& slot = active_slot(*state);
            REQUIRE(slot.hash.has_value());
            running = *slot.hash;
            REQUIRE((running == images.hash_a || running == images.hash_b));
            rig.timeline().note(std::string{"running "} + (running == images.hash_a ? "A" : "B"));
        } catch (...) {
            std::cout << rig.timeline().dump() << std::flush;
            throw;
        }
    }

    ~Session()
    {
        rig.record_send_counters();
        std::cout << rig.timeline().dump() << std::flush;
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// A full update to the other image, with the given hooks.
    Result<UpdateReport> update_to_other(UpdateMode mode, const UpdateHooks& hooks = {})
    {
        MemoryImageSource source{ConstBytes{images.other_than(running)}};
        UpdatePlan plan;
        plan.mode = mode;
        return rig.update(source, plan, hooks);
    }

    /// A bare upload of \p bytes into the secondary slot, with a full sha.
    ///
    /// The source is owned by the Session, not this call, because a resume of
    /// the returned handle reads from it after upload() has returned.
    UploadOutcome upload(const std::vector<std::byte>& bytes, UploadInterrupt interrupt = {})
    {
        auto& source =
            *sources.emplace_back(std::make_unique<MemoryImageSource>(ConstBytes{bytes}));
        UploadOptions options;
        const auto digest = sha256(source);
        REQUIRE(digest.has_value());
        options.sha = *digest;
        return rig.upload(source, options, interrupt);
    }

    /// Reads the state and returns the active slot's hash.
    ImageHash running_now()
    {
        const auto state = rig.read_state();
        REQUIRE(state.has_value());
        const ImageSlot& slot = active_slot(*state);
        REQUIRE(slot.hash.has_value());
        return *slot.hash;
    }

    /// Reset, wait for the link to go, and come back on a patient policy.
    void reboot()
    {
        REQUIRE(rig.reset().has_value());
        rig.timeline().note("reset accepted");
        // The device reboots 250 ms after answering; how soon the central
        // notices depends on the connection parameters in force (PN §9, A20),
        // and the swap itself takes seconds. Wait for the drop, then reconnect
        // with a policy that outlasts the longest reboot seen on the bench.
        static_cast<void>(rig.wait_disconnected(std::chrono::seconds{15}));
        rig.drop_link();
        dfu_app::ReconnectSettings patient;
        patient.first_delay = std::chrono::milliseconds{500};
        patient.max_delay = std::chrono::seconds{4};
        patient.max_attempts = 12;
        REQUIRE(rig.reconnect(patient).has_value());
    }
};

void print_slots(Rig& rig, const ImageState& state, const char* when)
{
    std::string line = std::string{"slots "} + when + ":";
    for (const ImageSlot& slot : state.slots) {
        line += " [slot " + std::to_string(slot.slot) + " v" + slot.version +
                (slot.active ? " active" : "") + (slot.confirmed ? " confirmed" : "") +
                (slot.pending ? " pending" : "") + (slot.permanent ? " permanent" : "") + "]";
    }
    rig.timeline().note(line);
}

} // namespace

// ---------------------------------------------------------------------------
// Presence of the optional commands
// ---------------------------------------------------------------------------

TEST_CASE("hil: the bench answers params and slot info and echo", "[hil]")
{
    Session s;

    const auto params = s.rig.params();
    REQUIRE(params.has_value());
    CHECK(params->buf_size > 0);
    CHECK(params->buf_count > 0);
    s.rig.timeline().metric("buf_size", params->buf_size);
    // Both halves, not just the width: whether the server can hold more than one
    // request at a time is the input to O3 (raise `max_in_flight`? -- ADR-0010),
    // and a bundle that records only `buf_size` cannot answer it.
    s.rig.timeline().metric("buf_count", params->buf_count);

    const auto info = s.rig.slot_info();
    REQUIRE(info.has_value());
    REQUIRE(info->images.size() == 1);
    REQUIRE(info->images.front().slots.size() == 2);
    CHECK(info->images.front().slots[0].size.has_value());
    CHECK(info->images.front().slots[1].size.has_value());

    const auto echoed = s.rig.echo("smply-p17");
    REQUIRE(echoed.has_value());
    CHECK(*echoed == "smply-p17");
    CHECK(s.rig.stats().timeouts == 0);
}

// ---------------------------------------------------------------------------
// Whole updates
// ---------------------------------------------------------------------------

TEST_CASE("hil: a clean update installs the other image and confirms it", "[hil][update]")
{
    Session s;
    const ImageHash target = s.images.hash_other_than(s.running);

    const auto report = s.update_to_other(UpdateMode::TestThenConfirm);
    REQUIRE(report.has_value());
    CHECK(report->final_state == UpdateState::Completed);
    CHECK_FALSE(report->upload_skipped);
    CHECK_FALSE(report->rolled_back);
    CHECK_FALSE(report->revert_pending);
    CHECK(report->bytes_transferred == s.images.other_than(s.running).size());
    CHECK(s.rig.stats().timeouts == 0);

    const auto state = s.rig.read_state();
    REQUIRE(state.has_value());
    print_slots(s.rig, *state, "after the update");
    const ImageSlot& slot = active_slot(*state);
    REQUIRE(slot.hash.has_value());
    CHECK(*slot.hash == target);
    CHECK(slot.confirmed);
    CHECK_FALSE(slot.pending);
}

TEST_CASE("hil: confirm-immediately installs without the confirmation pause", "[hil][update]")
{
    Session s;
    const ImageHash target = s.images.hash_other_than(s.running);

    const auto report = s.update_to_other(UpdateMode::ConfirmImmediately);
    REQUIRE(report.has_value());
    CHECK(report->final_state == UpdateState::Completed);
    CHECK(s.running_now() == target);
    for (const UpdateState state : s.rig.states()) {
        CHECK(state != UpdateState::AwaitingConfirmation);
    }
}

TEST_CASE("hil: an image the device already runs is not uploaded again", "[hil][update]")
{
    Session s;
    MemoryImageSource source{ConstBytes{s.running == s.images.hash_a ? s.images.a : s.images.b}};
    UpdatePlan plan;
    const auto before = s.rig.stats().sent;

    const auto report = s.rig.update(source, plan, {});
    REQUIRE(report.has_value());
    CHECK(report->final_state == UpdateState::Completed);
    CHECK(report->upload_skipped);
    CHECK(report->bytes_transferred == 0);
    CHECK(s.rig.stats().sent - before < 10); // a state read or two, no upload
    CHECK(s.running_now() == s.running);
}

// ---------------------------------------------------------------------------
// The upload alone
// ---------------------------------------------------------------------------

TEST_CASE("hil: the server's own already-present check completes a re-upload on the first packet",
          "[hil][upload]")
{
    // Rule 9a: with a full sha, the server checks the target slot before
    // writing anything. The first upload transfers; the second is answered
    // complete on its first packet.
    Session s;
    const auto& bytes = s.images.other_than(s.running);

    const UploadOutcome first = s.upload(bytes);
    REQUIRE(first.result.has_value());
    CHECK_FALSE(first.result->already_present);
    CHECK(first.result->match == true);
    CHECK(first.progress.size() > 2);

    const UploadOutcome second = s.upload(bytes);
    REQUIRE(second.result.has_value());
    CHECK(second.result->already_present);
    CHECK(second.result->transferred == bytes.size());
    CHECK(second.progress.size() <= 1);
    CHECK(s.rig.stats().timeouts == 0);

    // Leave the secondary slot as the next case expects to find it.
    CHECK(s.rig.erase(std::nullopt).has_value());
}

TEST_CASE("hil: an interrupted upload completes after a reconnect and resume", "[hil][upload]")
{
    // The application drops the link half-way; a fresh link and resume() finish
    // the image. **Where** the server resumes is its call, and this device
    // makes a call the simulator does not: it resets its upload session on the
    // BLE disconnect, so the resume's first packet is answered near offset zero
    // and the whole image is sent again (protocol-notes section 9, A21). The
    // contract smply keeps is only that the resume completes with the whole
    // image and a matching hash -- never that it continues from the drop point,
    // because rule 5/6 make the server's offset authoritative in every
    // direction. The restart pair below shows the other outcome: a session the
    // server still holds *is* continued.
    Session s;
    const auto& bytes = s.images.other_than(s.running);

    UploadOutcome outcome = s.upload(bytes, {UploadInterrupt::Action::DropLink, 0.5});
    REQUIRE_FALSE(outcome.result.has_value());
    CHECK(outcome.result.error().code() == ErrorCode::Disconnected);
    s.rig.timeline().metric("close_ms", s.rig.last_close_duration().count());
    REQUIRE_FALSE(outcome.progress.empty());
    const std::uint64_t reached = outcome.progress.back().transferred;
    CHECK(reached >= bytes.size() / 4); // the device really did store some of it

    REQUIRE(s.rig.reconnect(dfu_app::ReconnectSettings{}).has_value());
    const auto resumed = s.rig.resume(outcome);
    REQUIRE(resumed.has_value());
    CHECK(resumed->transferred == bytes.size());
    CHECK(resumed->match == true);
    CHECK_FALSE(resumed->already_present);
    REQUIRE_FALSE(outcome.progress.empty());
    // Recorded, not asserted to be near `reached`: this device restarts (A21).
    s.rig.timeline().metric("resumed_from",
                            static_cast<std::int64_t>(outcome.progress.front().transferred));

    CHECK(s.rig.erase(std::nullopt).has_value());
}

TEST_CASE("hil: part 1 -- an upload is abandoned by a process that exits", "[hil][upload][restart]")
{
    // Half the image goes over, then this process simply stops. Part 2, run
    // afterwards by the supervisor without a reflash in between, must find the
    // device holding the session.
    Session s;
    const auto& bytes = s.images.other_than(s.running);

    UploadOutcome outcome = s.upload(bytes, {UploadInterrupt::Action::Cancel, 0.5});
    REQUIRE_FALSE(outcome.result.has_value());
    CHECK(outcome.result.error().code() == ErrorCode::Cancelled);
    REQUIRE_FALSE(outcome.progress.empty());
    CHECK(outcome.progress.back().transferred < bytes.size()); // stopped before the end
    s.rig.timeline().metric("abandoned_at",
                            static_cast<std::int64_t>(outcome.progress.back().transferred));
}

TEST_CASE("hil: part 2 -- a new process resumes the abandoned upload by sha",
          "[hil][upload][restart]")
{
    Session s;
    const auto& bytes = s.images.other_than(s.running);

    const UploadOutcome outcome = s.upload(bytes);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->transferred == bytes.size());
    CHECK(outcome.result->match == true);
    REQUIRE_FALSE(outcome.progress.empty());
    // The server answered the first packet with the offset the previous process
    // reached, so the first progress report is far from zero.
    s.rig.timeline().metric("resumed_from",
                            static_cast<std::int64_t>(outcome.progress.front().transferred));
    CHECK(outcome.progress.front().transferred >= bytes.size() / 4);
    CHECK_FALSE(outcome.result->already_present);

    CHECK(s.rig.erase(std::nullopt).has_value());
}

// ---------------------------------------------------------------------------
// Refusals and rollback
// ---------------------------------------------------------------------------

TEST_CASE("hil: a corrupted image is refused and the device keeps running what it had",
          "[hil][update][corrupt]")
{
    // One body byte flipped after signing: the upload sha still matches what
    // we send, so the refusal has to come from the image's own hash TLV --
    // where exactly is the finding this case exists to record.
    Session s;
    std::vector<std::byte> corrupt = s.images.other_than(s.running);
    REQUIRE(corrupt.size() > 0x300);
    corrupt[0x240] ^= std::byte{0xFF};
    MemoryImageSource source{ConstBytes{corrupt}};
    UpdatePlan plan;

    const auto report = s.rig.update(source, plan, {});

    // The point is that MCUboot refused the image and the device kept running
    // what it had. The updater surfaces that refusal one of two ways, and P17b
    // recorded which: a body byte flipped after signing fails the image's own
    // IMAGE_TLV_SHA256, so the bootloader reverts *before* the client can
    // reconnect. VerifyingBooted then finds the old image running and the
    // update ends unsuccessfully -- observed here as a **failure Result**, not
    // a report with `final_state == Failed`. Either is acceptable; a Completed
    // report is not.
    if (report.has_value()) {
        s.rig.timeline().note(std::string{"corrupt image: final state "} +
                              std::string{to_string(report->final_state)});
        CHECK(report->final_state != UpdateState::Completed);
    } else {
        s.rig.timeline().note("corrupt image: the update failed with " +
                              std::string{to_string(report.error().code())});
    }
    // The device is running the image it began with, whichever way it was told.
    CHECK(s.running_now() == s.running);
}

TEST_CASE("hil: a trial boot that nobody confirms is reverted on the next reset",
          "[hil][boot][rollback]")
{
    Session s;
    const auto& bytes = s.images.other_than(s.running);
    const ImageHash target = s.images.hash_other_than(s.running);

    const UploadOutcome uploaded = s.upload(bytes);
    REQUIRE(uploaded.result.has_value());

    SetStateRequest test;
    test.hash = target;
    test.confirm = false;
    const auto marked = s.rig.set_state(test);
    REQUIRE(marked.has_value());
    print_slots(s.rig, *marked, "after marking for test");

    s.reboot();
    const auto trial = s.rig.read_state();
    REQUIRE(trial.has_value());
    print_slots(s.rig, *trial, "during the trial boot");
    const ImageSlot& running_trial = active_slot(*trial);
    REQUIRE(running_trial.hash.has_value());
    CHECK(*running_trial.hash == target);
    CHECK_FALSE(running_trial.confirmed);
    s.rig.timeline().metric("slots_listed_during_trial",
                            static_cast<std::int64_t>(trial->slots.size()));

    s.reboot();
    const auto reverted = s.rig.read_state();
    REQUIRE(reverted.has_value());
    print_slots(s.rig, *reverted, "after the second reset");
    const ImageSlot& running_after = active_slot(*reverted);
    REQUIRE(running_after.hash.has_value());
    CHECK(*running_after.hash == s.running);
    CHECK(running_after.confirmed);
}

TEST_CASE("hil: a reset drops the link and the device comes back", "[hil][boot]")
{
    Session s;
    REQUIRE(s.rig.echo("before").has_value());
    const auto t0 = s.rig.timeline().elapsed_ms();

    s.reboot();
    s.rig.timeline().metric("reboot_total_ms", s.rig.timeline().elapsed_ms() - t0);

    const auto echoed = s.rig.echo("after");
    REQUIRE(echoed.has_value());
    CHECK(*echoed == "after");
    CHECK(s.running_now() == s.running);
}

TEST_CASE("hil: erase clears the secondary slot even when it is marked for test", "[hil][erase]")
{
    Session s;
    const auto& bytes = s.images.other_than(s.running);
    const ImageHash target = s.images.hash_other_than(s.running);

    REQUIRE(s.upload(bytes).result.has_value());
    SetStateRequest test;
    test.hash = target;
    const auto marked = s.rig.set_state(test);
    REQUIRE(marked.has_value());
    print_slots(s.rig, *marked, "after marking for test");

    // This build allows erasing a pending slot
    // (CONFIG_MCUMGR_GRP_IMG_ALLOW_ERASE_PENDING=y); the simulator refuses it.
    // Whichever the device does, it must be consistent with what follows.
    const auto erased = s.rig.erase(std::nullopt);
    s.rig.timeline().note(std::string{"erase of a pending slot: "} +
                          (erased.has_value() ? "accepted" : to_string(erased.error())));

    if (erased.has_value()) {
        const auto after = s.rig.read_state();
        REQUIRE(after.has_value());
        print_slots(s.rig, *after, "after the erase");
        for (const ImageSlot& slot : after->slots) {
            CHECK_FALSE(slot.pending);
        }
        s.reboot();
        CHECK(s.running_now() == s.running);
    } else {
        // Refused: the trial must then go ahead, and the rollback case covers it.
        SUCCEED("the device refused to erase a pending slot; recorded");
    }
}

// ---------------------------------------------------------------------------
// Giving up
// ---------------------------------------------------------------------------

TEST_CASE("hil: reconnection gives up when the device does not come back",
          "[hil][update][fault][.manual]")
{
    // MANUAL (tag [.manual], not in the unattended default): reliably keeping
    // the device gone during the reconnect window needs a person to power the
    // board off, or a probe that holds it in reset. An ST-LINK mass erase
    // races -- STM32CubeProgrammer toggles reset to attach, and the device
    // re-advertises before the erase halts it, so an automated run reconnects
    // and the update completes. The give-up *path* itself is covered
    // deterministically on every CI push by `cli_dfu --flaky-reconnect 99`;
    // this case is here for a bench operator who can pull the board and watch
    // reconnect_failed() fire with a pending revert. Run it with
    // `run_hil.py --cases give-up` and remove power on the HIL-MARK line.
    // The first reconnect attempt waits long enough that the supervisor's erase
    // -- which does not start instantly (STM32CubeProgrammer takes a second or
    // two to attach over SWD) -- has finished and the device is dead before the
    // first attempt, rather than racing a device that is still finishing its
    // post-reset reboot. This is not the shipped ReconnectPolicy; the case
    // tests the give-up *path*, and that a reconnect that never succeeds ends
    // in reconnect_failed() with a pending revert, not the schedule (which
    // cli_dfu --flaky-reconnect exercises on every push).
    Session s;
    UpdateHooks hooks;
    hooks.reconnect.first_delay = std::chrono::seconds{10};
    hooks.reconnect.max_delay = std::chrono::seconds{10};
    hooks.reconnect.max_attempts = 3;
    hooks.on_await_reconnect = [&] {
        std::cout << "HIL-MARK: device-gone-now" << std::endl; // NOLINT(performance-avoid-endl)
        s.rig.timeline().note("asked the supervisor to remove the device");
    };

    const auto report = s.update_to_other(UpdateMode::TestThenConfirm, hooks);
    REQUIRE(report.has_value());
    if (report->final_state == UpdateState::Completed) {
        FAIL("the device came back: run this case under run_hil.py, which removes it");
    }
    CHECK(report->final_state == UpdateState::Failed);
    REQUIRE(report->cause.has_value());
    CHECK(report->cause->code() == ErrorCode::Disconnected);
    CHECK(report->revert_pending);
}
