// SPDX-License-Identifier: Apache-2.0
//
// The real client stack driven into a simulated device.
//
// These tests are about the *sequence*: that the commands smply issues, in the
// order it issues them, are ones a server accepts, and that an image survives
// the round trip byte for byte. A case that is really about one decision
// belongs in tests/unit -- the response table lives in test_upload_session.cpp
// and the wire format in test_upload_driver.cpp.

#include "harness.hpp"

#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/image_source.hpp"
#include "smply/limits.hpp"
#include "smply/mcuboot_image.hpp"
#include "smply/smp_client.hpp"

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
using smply::Hash;
using smply::ImageState;
using smply::McumgrParameters;
using smply::MemoryImageSource;
using smply::Result;
using smply::SetStateRequest;
using smply::SmpClientConfig;
using smply::UploadHandle;
using smply::UploadOptions;
using smply::Version;
using smply::test::FakeTransport;
using smply::test::Fixture;
using smply::test::make_firmware;
using smply::test::ServerConfig;
using smply::test::SwapType;
using smply::test::UploadOutcome;

namespace Catch {
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

/// The SHA-256 of a firmware image, as the client computes it for the upload.
[[nodiscard]] Hash sha_of(const std::vector<std::byte>& firmware)
{
    MemoryImageSource source{ConstBytes{firmware}};
    const Result<Hash> hash = smply::sha256(source);
    REQUIRE(hash.has_value());
    return *hash;
}

[[nodiscard]] UploadOptions options_for(const std::vector<std::byte>& firmware)
{
    UploadOptions out;
    out.sha = sha_of(firmware);
    return out;
}

} // namespace

TEST_CASE("an upload reproduces the source image byte for byte", "[component][upload]")
{
    // The phase's acceptance criterion, in both SMP versions: the versions
    // differ only in how a failure is shaped, but a success path that quietly
    // depended on the v1 error shape would be a real defect.
    const Version version = GENERATE(Version::V1, Version::V2);

    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    SmpClientConfig client_config;
    client_config.smp_version = version;
    Fixture fixture{ServerConfig{}, client_config};
    MemoryImageSource source{ConstBytes{firmware}};

    const UploadHandle handle = fixture.management.upload(source, options_for(firmware),
                                                          outcome.on_progress(), outcome.on_done());
    REQUIRE(handle.valid());
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == std::nullopt);
    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->transferred == firmware.size());
    CHECK(outcome.value->match == true);

    const ConstBytes flashed = fixture.simulator.slot_content(1);
    REQUIRE(flashed.size() == firmware.size());
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));

    // More than one chunk, or the test would not be exercising continuation at
    // all: the default 256-byte budget cannot carry a 600-byte body in one.
    CHECK(fixture.simulator.requests().size() > 2);
}

TEST_CASE("progress advances monotonically to the total", "[component][upload]")
{
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture;
    MemoryImageSource source{ConstBytes{firmware}};

    static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                outcome.on_progress(), outcome.on_done()));
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    REQUIRE(!outcome.progress.empty());
    std::uint64_t previous = 0;
    for (const smply::UploadProgress& step : outcome.progress) {
        CHECK(step.total == firmware.size());
        CHECK(step.transferred >= previous);
        previous = step.transferred;
    }
    CHECK(outcome.progress.back().transferred == firmware.size());
}

TEST_CASE("an image the device already holds completes on the first packet", "[component][upload]")
{
    // Rule 9a. The shortest path through the whole update, and the one a client
    // that computes its own offsets can never reach.
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture;
    fixture.simulator.load_slot(1, firmware);
    MemoryImageSource source{ConstBytes{firmware}};

    static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                outcome.on_progress(), outcome.on_done()));
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->transferred == firmware.size());
    CHECK(outcome.value->match == true);

    // `transferred` reads as the whole image, because the device acknowledged
    // the whole image -- which is why a caller needs this flag to tell "already
    // present" from "uploaded". It is what makes UpdateReport::upload_skipped
    // true in this case; before P14 the report claimed a transfer.
    CHECK(outcome.value->already_present);

    // One request, and not one byte of image data written.
    CHECK(fixture.simulator.requests().size() == 1);
    CHECK(fixture.simulator.bytes_written() == 0);

    // Progress jumps straight to complete rather than climbing.
    REQUIRE(!outcome.progress.empty());
    CHECK(outcome.progress.back().transferred == firmware.size());
}

TEST_CASE("a device without the image check re-uploads and gives no verdict", "[component][upload]")
{
    // Without CONFIG_IMG_ENABLE_IMAGE_CHECK the already-present shortcut does
    // not exist, so the same content is uploaded again -- and `match` is absent
    // rather than false, which is a success and not a mismatch (A6).
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture{ServerConfig{.image_check_enabled = false}};
    fixture.simulator.load_slot(1, firmware);
    MemoryImageSource source{ConstBytes{firmware}};

    UploadOptions options;
    options.sha = sha_of(firmware);
    static_cast<void>(
        fixture.management.upload(source, options, outcome.on_progress(), outcome.on_done()));
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->match == std::nullopt); // No image check, so no verdict.
    CHECK(fixture.simulator.bytes_written() == firmware.size());
}

TEST_CASE("the server can force a restart mid-upload", "[component][upload]")
{
    // Rule 7: an `off` of zero means "re-send the first packet in full". The
    // upload must still complete, and the flash must still be exact.
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture;
    MemoryImageSource source{ConstBytes{firmware}};

    static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                outcome.on_progress(), outcome.on_done()));

    // Let a couple of chunks land, then answer the next one with zero.
    for (int i = 0; i < 3; ++i) {
        fixture.step();
    }
    fixture.simulator.answer_offset_once(0);

    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));
    REQUIRE(outcome.value.has_value());

    const ConstBytes flashed = fixture.simulator.slot_content(1);
    REQUIRE(flashed.size() == firmware.size());
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));
}

TEST_CASE("an upload survives the device rebooting mid-transfer", "[component][upload]")
{
    // The genuine restart: the device forgot the session entirely, so a
    // continuation is answered zero and the client must re-send a full first
    // packet -- len, sha, image and all -- or the upload can never continue.
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture;
    MemoryImageSource source{ConstBytes{firmware}};

    static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                outcome.on_progress(), outcome.on_done()));
    for (int i = 0; i < 3; ++i) {
        fixture.step();
    }
    REQUIRE(fixture.simulator.bytes_written() > 0);
    fixture.simulator.reboot();

    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));
    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->transferred == firmware.size());

    const ConstBytes flashed = fixture.simulator.slot_content(1);
    REQUIRE(flashed.size() == firmware.size());
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));
}

TEST_CASE("the client adopts an offset ahead of what it sent", "[component][upload]")
{
    // Rule 5 in the direction that catches a client computing its own next
    // offset: the device names an offset *larger* than the one acknowledged.
    // It is put right on the next round trip, so a client that follows the
    // server still flashes a correct image -- and one that does its own
    // arithmetic writes a corrupt one, which the byte comparison below catches.
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture;
    MemoryImageSource source{ConstBytes{firmware}};

    static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                outcome.on_progress(), outcome.on_done()));
    fixture.step();
    fixture.step();
    fixture.simulator.answer_offset_once(400);

    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));
    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->transferred == firmware.size());

    const ConstBytes flashed = fixture.simulator.slot_content(1);
    REQUIRE(flashed.size() == firmware.size());
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));
}

TEST_CASE("a lost response is retransmitted and the upload still completes", "[component][upload]")
{
    // The device answers and the answer never arrives. The client must retry
    // the same chunk after its deadline rather than stalling or skipping ahead,
    // and the flash must still come out exact.
    //
    // A 100 ms step, so the 5 s chunk deadline is reached in fifty turns rather
    // than five thousand: the budget is there to bound a stall, not to model
    // real time.
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture;
    MemoryImageSource source{ConstBytes{firmware}};

    static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                outcome.on_progress(), outcome.on_done()));
    fixture.step();
    fixture.simulator.drop_next_response();

    REQUIRE(fixture.run_until([&] { return outcome.finished(); }, smply::test::kDefaultBudget,
                              std::chrono::milliseconds{100}));
    REQUIRE(outcome.value.has_value());
    CHECK(fixture.simulator.dropped() == 1);

    const ConstBytes flashed = fixture.simulator.slot_content(1);
    REQUIRE(flashed.size() == firmware.size());
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));
}

TEST_CASE("a device that answers slowly still completes the upload", "[component][upload]")
{
    // The response delay puts each answer a turn or more into the future, so
    // the simulator's queue -- rather than its handlers -- is what is under
    // test here.
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture{ServerConfig{.response_delay = std::chrono::milliseconds{50}}};
    MemoryImageSource source{ConstBytes{firmware}};

    static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                outcome.on_progress(), outcome.on_done()));
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }, smply::test::kDefaultBudget,
                              std::chrono::milliseconds{10}));

    REQUIRE(outcome.value.has_value());
    const ConstBytes flashed = fixture.simulator.slot_content(1);
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));
}

TEST_CASE("an upload resumes across a disconnect", "[component][upload]")
{
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    UploadOutcome resumed;
    // The replacement link is declared before the fixture, because every
    // transport a client is ever bound to must outlive it.
    FakeTransport reconnected;
    Fixture fixture;
    MemoryImageSource source{ConstBytes{firmware}};

    const UploadHandle handle = fixture.management.upload(source, options_for(firmware),
                                                          outcome.on_progress(), outcome.on_done());

    // Part way in, the link drops. The callback fires once, with Disconnected.
    for (int i = 0; i < 3; ++i) {
        fixture.step();
    }
    const std::uint64_t before = fixture.management.transferred(handle);
    REQUIRE(before > 0);
    fixture.transport.disconnect();
    fixture.client.poll(fixture.clock.now());

    REQUIRE(outcome.calls == 1);
    CHECK(outcome.code == ErrorCode::Disconnected);

    // The application re-establishes the link. The device on the other end is
    // the same device, still holding the session, so the resumed first packet
    // is answered with the offset it reached rather than with zero.
    fixture.client.rebind_transport(reconnected);
    fixture.simulator.rebind_transport(reconnected);
    fixture.management.resume(handle, resumed.on_done());
    REQUIRE(fixture.run_until([&] { return resumed.finished(); }));

    REQUIRE(resumed.value.has_value());
    CHECK(resumed.value->transferred == firmware.size());

    const ConstBytes flashed = fixture.simulator.slot_content(1);
    REQUIRE(flashed.size() == firmware.size());
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));
}

TEST_CASE("mcumgr parameters drive the chunk size", "[component][upload]")
{
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture{ServerConfig{.buf_size = 512}};
    MemoryImageSource source{ConstBytes{firmware}};

    std::optional<McumgrParameters> parameters;
    static_cast<void>(fixture.os.mcumgr_parameters([&](const Result<McumgrParameters>& result) {
        if (result.has_value()) {
            parameters = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return parameters.has_value(); }));
    CHECK(parameters->buf_size == 512);

    UploadOptions options = options_for(firmware);
    options.server_buf_size = parameters->buf_size;
    static_cast<void>(
        fixture.management.upload(source, options, outcome.on_progress(), outcome.on_done()));
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    REQUIRE(outcome.value.has_value());
    const ConstBytes flashed = fixture.simulator.slot_content(1);
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));

    // A larger budget means fewer, larger chunks than the 256-byte default.
    const std::size_t upload_requests = fixture.simulator.requests().size() - 1;
    CHECK(upload_requests <= 3);
}

TEST_CASE("a device without mcumgr parameters still uploads", "[component][upload]")
{
    // ENOTSUP from an optional command is a normal answer to fall back from,
    // not an upload failure (protocol-notes section 9, A8).
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    Fixture fixture{ServerConfig{.supports_mcumgr_params = false}};
    MemoryImageSource source{ConstBytes{firmware}};

    std::optional<ErrorCode> failure;
    static_cast<void>(fixture.os.mcumgr_parameters([&](const Result<McumgrParameters>& result) {
        if (!result.has_value()) {
            failure = result.error().code();
        }
    }));
    REQUIRE(fixture.run_until([&] { return failure.has_value(); }));
    CHECK(failure == ErrorCode::ProtocolError);

    // No buf_size, so the conservative default applies and the upload proceeds.
    static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                outcome.on_progress(), outcome.on_done()));
    REQUIRE(fixture.run_until([&] { return outcome.finished(); }));

    REQUIRE(outcome.value.has_value());
    const ConstBytes flashed = fixture.simulator.slot_content(1);
    CHECK(std::equal(flashed.begin(), flashed.end(), firmware.begin()));
}

TEST_CASE("test then reset then confirm keeps the new image", "[component][state]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update);

    // Read the state, and mark the secondary image for test by its hash.
    std::optional<ImageState> state;
    static_cast<void>(fixture.management.get_state([&](const Result<ImageState>& result) {
        if (result.has_value()) {
            state = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return state.has_value(); }));
    REQUIRE(state->slots.size() == 2);
    CHECK(state->slots[0].active);
    CHECK(state->slots[0].confirmed);
    CHECK_FALSE(state->slots[1].pending);
    REQUIRE(state->slots[1].hash.has_value());

    SetStateRequest request;
    request.hash = state->slots[1].hash;
    request.confirm = false;
    std::optional<ImageState> after_test;
    static_cast<void>(fixture.management.set_state(request, [&](const Result<ImageState>& result) {
        if (result.has_value()) {
            after_test = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return after_test.has_value(); }));
    CHECK(after_test->slots[1].pending);
    CHECK_FALSE(after_test->slots[1].permanent);
    CHECK(fixture.simulator.swap_type() == SwapType::Test);

    // The device reboots into the new image, on trial.
    fixture.simulator.reboot();
    CHECK(fixture.simulator.swap_type() == SwapType::Revert);

    std::optional<ImageState> trial;
    static_cast<void>(fixture.management.get_state([&](const Result<ImageState>& result) {
        if (result.has_value()) {
            trial = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return trial.has_value(); }));

    // The signature of a trial boot: the running image is active but NOT
    // confirmed, and the slot holding the old one is.
    CHECK(trial->slots[0].active);
    CHECK_FALSE(trial->slots[0].confirmed);
    CHECK(trial->slots[1].confirmed);

    // Confirming ends the trial.
    SetStateRequest confirm;
    confirm.confirm = true;
    std::optional<ImageState> confirmed;
    static_cast<void>(fixture.management.set_state(confirm, [&](const Result<ImageState>& result) {
        if (result.has_value()) {
            confirmed = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return confirmed.has_value(); }));
    CHECK(fixture.simulator.swap_type() == SwapType::None);
    CHECK(confirmed->slots[0].confirmed);

    // A second reboot keeps the new image, because it was confirmed.
    fixture.simulator.reboot();
    const ConstBytes primary = fixture.simulator.slot_content(0);
    CHECK(std::equal(primary.begin(), primary.end(), update.begin()));
}

TEST_CASE("an unconfirmed trial boot reverts on the next reset", "[component][state]")
{
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update);

    std::optional<ImageState> state;
    static_cast<void>(fixture.management.get_state([&](const Result<ImageState>& result) {
        if (result.has_value()) {
            state = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return state.has_value(); }));

    SetStateRequest request;
    request.hash = state->slots[1].hash;
    std::optional<ImageState> marked;
    static_cast<void>(fixture.management.set_state(request, [&](const Result<ImageState>& result) {
        if (result.has_value()) {
            marked = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return marked.has_value(); }));

    fixture.simulator.reboot(); // Swaps, on trial.
    fixture.simulator.reboot(); // No confirm: MCUboot reverts.

    CHECK(fixture.simulator.swap_type() == SwapType::None);
    const ConstBytes primary = fixture.simulator.slot_content(0);
    CHECK(std::equal(primary.begin(), primary.end(), running.begin()));
}

TEST_CASE("confirming a slot that is not running is denied", "[component][state]")
{
    // The default build refuses to confirm anything but the running image
    // (S14). A "confirm immediately" update mode therefore cannot work by
    // confirming the uploaded image before the swap.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update);

    std::optional<ImageState> state;
    static_cast<void>(fixture.management.get_state([&](const Result<ImageState>& result) {
        if (result.has_value()) {
            state = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return state.has_value(); }));

    SetStateRequest request;
    request.hash = state->slots[1].hash;
    request.confirm = true;
    std::optional<ErrorCode> failure;
    static_cast<void>(fixture.management.set_state(request, [&](const Result<ImageState>& result) {
        if (!result.has_value()) {
            failure = result.error().code();
        }
    }));
    REQUIRE(fixture.run_until([&] { return failure.has_value(); }));
    CHECK(failure == ErrorCode::ProtocolError);
    CHECK(fixture.simulator.swap_type() == SwapType::None);
}

TEST_CASE("erasing a slot leaves an empty image list", "[component][state]")
{
    // "A response will only contain information for valid images" -- so an
    // erased slot simply vanishes from the list, and a short list is normal.
    const std::vector<std::byte> running = make_firmware(kBodySize, 1, 0, 0, 1);
    const std::vector<std::byte> update = make_firmware(kBodySize, 2, 0, 0, 2);

    Fixture fixture;
    fixture.simulator.load_slot(0, running);
    fixture.simulator.load_slot(1, update);

    bool erased = false;
    static_cast<void>(
        fixture.management.erase([&](const Result<void>& result) { erased = result.has_value(); }));
    REQUIRE(fixture.run_until([&] { return erased; }));

    std::optional<ImageState> state;
    static_cast<void>(fixture.management.get_state([&](const Result<ImageState>& result) {
        if (result.has_value()) {
            state = *result;
        }
    }));
    REQUIRE(fixture.run_until([&] { return state.has_value(); }));
    REQUIRE(state->slots.size() == 1);
    CHECK(state->slots[0].slot == 0);
}

TEST_CASE("a reset is accepted before the link goes down", "[component][os]")
{
    // The response is acceptance, not completion: the device answers and only
    // then resets, so losing the link afterwards is the normal case.
    Fixture fixture;

    bool accepted = false;
    static_cast<void>(
        fixture.os.reset([&](const Result<void>& result) { accepted = result.has_value(); }));
    REQUIRE(fixture.run_until([&] { return accepted; }));
    CHECK(fixture.simulator.reset_requested());

    fixture.transport.disconnect();
    fixture.client.poll(fixture.clock.now());
    CHECK_FALSE(fixture.client.connected());
}

TEST_CASE("a busy reset is retried with force", "[component][os]")
{
    Fixture fixture;
    fixture.simulator.reset_busy_once();

    std::optional<ErrorCode> failure;
    static_cast<void>(fixture.os.reset([&](const Result<void>& result) {
        if (!result.has_value()) {
            failure = result.error().code();
        }
    }));
    REQUIRE(fixture.run_until([&] { return failure.has_value(); }));
    CHECK(failure == ErrorCode::ProtocolError);
    CHECK_FALSE(fixture.simulator.reset_requested());

    smply::ResetOptions options;
    options.force = true;
    bool accepted = false;
    static_cast<void>(fixture.os.reset(
        options, [&](const Result<void>& result) { accepted = result.has_value(); }));
    REQUIRE(fixture.run_until([&] { return accepted; }));
    CHECK(fixture.simulator.reset_requested());
    CHECK(fixture.simulator.last_reset_force() == true);
}

TEST_CASE("destroying the group mid-upload completes the callback once", "[component][lifetime]")
{
    // The lifetime rule P10 introduced, exercised where it actually bites: the
    // outcome is declared before the fixture, so it outlives both the client
    // and the group whose destructors complete the callback. Clang's ASan is
    // the only thing that catches getting this wrong.
    const std::vector<std::byte> firmware = make_firmware(kBodySize);

    UploadOutcome outcome;
    {
        Fixture fixture;
        MemoryImageSource source{ConstBytes{firmware}};
        static_cast<void>(fixture.management.upload(source, options_for(firmware),
                                                    outcome.on_progress(), outcome.on_done()));
        for (int i = 0; i < 2; ++i) {
            fixture.step();
        }
        REQUIRE(outcome.calls == 0);
    }

    CHECK(outcome.calls == 1);
    CHECK(outcome.code == ErrorCode::Cancelled);
}
