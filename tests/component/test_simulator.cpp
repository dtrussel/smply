// SPDX-License-Identifier: Apache-2.0
//
// The double, checked against the specification it claims to implement.
//
// Driven with hand-built requests and no smply client at all, so that a
// simulator bug is diagnosed here rather than through three layers of library
// in test_round_trip.cpp. Every case names the rule in
// docs/protocol-notes.md section 5 or 6 that it comes from.

#include "fake_transport.hpp"
#include "manual_clock.hpp"
#include "message_builder.hpp"
#include "server_simulator.hpp"
#include "test_cbor.hpp"

#include "harness.hpp"

#include "image/sha256.hpp"

#include "smply/error.hpp"
#include "smply/group.hpp"
#include "smply/smp/header.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using smply::ConstBytes;
using smply::Group;
using smply::Header;
using smply::ImageError;
using smply::Operation;
using smply::SmpError;
using smply::Version;
using smply::test::FakeTransport;
using smply::test::make_firmware;
using smply::test::make_message;
using smply::test::ManualClock;
using smply::test::ServerConfig;
using smply::test::ServerSimulator;
using smply::test::SwapType;
namespace tcbor = smply::test::tcbor;

namespace {

/// Drives the simulator directly: builds requests, delivers them through the
/// transport's own record of what was "sent", and decodes what comes back.
///
/// The listener is the harness itself rather than a client, which is what makes
/// these tests independent of everything above the wire.
class Device : public smply::TransportListener
{
public:
    Device() : Device(ServerConfig{}) {}

    explicit Device(ServerConfig config) : simulator_{transport_, config}
    {
        transport_.set_listener(this);
    }

    [[nodiscard]] ServerSimulator& simulator() noexcept
    {
        return simulator_;
    }

    [[nodiscard]] FakeTransport& transport() noexcept
    {
        return transport_;
    }

    /// Sends one request and returns the decoded response payload.
    ///
    /// Fails the test rather than returning nullopt when nothing comes back:
    /// every request in this suite is one the device must answer.
    [[nodiscard]] tcbor::Value exchange(Operation op, Group group, std::uint8_t command,
                                        ConstBytes payload, Version version = Version::V1)
    {
        const Header header{.op = op,
                            .version = version,
                            .flags = 0,
                            .length = 0,
                            .group = group,
                            .seq = seq_++,
                            .command = command};
        const std::vector<std::byte> message = make_message(header, payload);
        REQUIRE(transport_.send(ConstBytes{message}).has_value());

        received_.clear();
        simulator_.pump(clock_.now());
        clock_.advance(std::chrono::milliseconds{1});

        REQUIRE(received_.size() >= smply::kHeaderSize);
        const ConstBytes body = ConstBytes{received_}.subspan(smply::kHeaderSize);
        const std::optional<tcbor::Value> decoded = tcbor::parse(body);
        REQUIRE(decoded.has_value());
        return *decoded;
    }

    /// An upload request, with only the fields a caller states.
    [[nodiscard]] tcbor::Value upload(std::optional<std::uint64_t> off, ConstBytes data,
                                      std::optional<std::uint64_t> length = std::nullopt,
                                      std::optional<ConstBytes> sha = std::nullopt,
                                      Version version = Version::V1)
    {
        std::uint64_t pairs = data.empty() ? 0 : 1;
        if (off.has_value()) {
            ++pairs;
        }
        if (length.has_value()) {
            ++pairs;
        }
        if (sha.has_value()) {
            ++pairs;
        }

        tcbor::Writer out;
        out.map(pairs);
        if (off.has_value()) {
            out.text("off").uint(*off);
        }
        if (length.has_value()) {
            out.text("len").uint(*length);
        }
        if (sha.has_value()) {
            out.text("sha").blob(*sha);
        }
        if (!data.empty()) {
            out.text("data").blob(data);
        }
        return exchange(Operation::Write, Group::Image, 1, out.view(), version);
    }

private:
    void on_bytes(ConstBytes bytes) override
    {
        received_.insert(received_.end(), bytes.begin(), bytes.end());
    }

    void on_transport_error(smply::Error /*error*/) override {}

    void on_disconnected(smply::Error /*error*/) override {}

    FakeTransport transport_;
    ManualClock clock_;
    ServerSimulator simulator_;
    std::vector<std::byte> received_;
    std::uint8_t seq_ = 0;
};

/// The `off` a successful upload response carries.
[[nodiscard]] std::uint64_t offset_of(const tcbor::Value& response)
{
    const std::optional<std::uint64_t> off = response.get_uint("off");
    REQUIRE(off.has_value());
    return *off;
}

/// The flat `rc` a v1 failure carries.
[[nodiscard]] std::uint64_t flat_rc(const tcbor::Value& response)
{
    const std::optional<std::uint64_t> rc = response.get_uint("rc");
    REQUIRE(rc.has_value());
    return *rc;
}

/// The group-scoped code a v2 failure carries.
[[nodiscard]] std::uint64_t scoped_rc(const tcbor::Value& response, std::uint64_t expected_group)
{
    const tcbor::Value* err = response.find("err");
    REQUIRE(err != nullptr);
    REQUIRE(err->is(tcbor::Value::Kind::Map));
    CHECK(err->get_uint("group") == expected_group);
    const std::optional<std::uint64_t> rc = err->get_uint("rc");
    REQUIRE(rc.has_value());
    return *rc;
}

constexpr std::uint32_t kBody = 300;

std::vector<std::byte> chunk_of(const std::vector<std::byte>& image, std::size_t off,
                                std::size_t length)
{
    const std::size_t end = std::min(off + length, image.size());
    return {image.begin() + static_cast<std::ptrdiff_t>(off),
            image.begin() + static_cast<std::ptrdiff_t>(end)};
}

std::vector<std::byte> sha_bytes(const std::vector<std::byte>& image)
{
    smply::image::Sha256 hasher;
    hasher.update(ConstBytes{image});
    const auto digest = hasher.finish();
    return {digest.begin(), digest.end()};
}

} // namespace

// --- Group 0 ----------------------------------------------------------------

TEST_CASE("echo is registered under both the read and the write op", "[simulator][os]")
{
    Device device;
    tcbor::Writer request;
    request.map(1).text("d").text("hello");

    for (const Operation op : {Operation::Read, Operation::Write}) {
        const tcbor::Value response = device.exchange(op, Group::Os, 0, request.view());
        const tcbor::Value* echoed = response.find("r");
        REQUIRE(echoed != nullptr);
        CHECK(echoed->text == "hello");
    }
}

TEST_CASE("reset is write-only and mcumgr parameters read-only", "[simulator][os]")
{
    // The handler table registers each under one slot only, so the wrong op is
    // ENOTSUP rather than an answer (section 5, S13).
    Device device;
    tcbor::Writer empty;
    empty.map(0);

    const tcbor::Value read_reset = device.exchange(Operation::Read, Group::Os, 5, empty.view());
    CHECK(flat_rc(read_reset) == static_cast<std::uint64_t>(SmpError::NotSupported));
    CHECK_FALSE(device.simulator().reset_requested());

    const tcbor::Value write_params = device.exchange(Operation::Write, Group::Os, 6, empty.view());
    CHECK(flat_rc(write_params) == static_cast<std::uint64_t>(SmpError::NotSupported));
}

TEST_CASE("mcumgr parameters can be absent", "[simulator][os]")
{
    tcbor::Writer empty;
    empty.map(0);

    Device present;
    const tcbor::Value answered = present.exchange(Operation::Read, Group::Os, 6, empty.view());
    CHECK(answered.get_uint("buf_size") == 256);
    CHECK(answered.get_uint("buf_count").has_value());

    Device absent{ServerConfig{.supports_mcumgr_params = false}};
    const tcbor::Value refused = absent.exchange(Operation::Read, Group::Os, 6, empty.view());
    CHECK(flat_rc(refused) == static_cast<std::uint64_t>(SmpError::NotSupported));
}

TEST_CASE("a reset is accepted, and can be refused as busy first", "[simulator][os]")
{
    Device device;
    device.simulator().reset_busy_once();
    tcbor::Writer empty;
    empty.map(0);

    const tcbor::Value busy = device.exchange(Operation::Write, Group::Os, 5, empty.view());
    CHECK(flat_rc(busy) == static_cast<std::uint64_t>(SmpError::Busy));
    CHECK_FALSE(device.simulator().reset_requested());

    tcbor::Writer forced;
    forced.map(1).text("force").boolean(true);
    const tcbor::Value accepted = device.exchange(Operation::Write, Group::Os, 5, forced.view());
    CHECK(accepted.size() == 0);
    CHECK(device.simulator().reset_requested());
    CHECK(device.simulator().last_reset_force() == true);
}

// --- Group 1: upload --------------------------------------------------------

TEST_CASE("a first chunk shorter than an image header is refused", "[simulator][upload]")
{
    // Rule 2: the header is the first thing in the image, so a first chunk that
    // cannot contain one is rejected before anything else is looked at.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> data = chunk_of(image, 0, 16);

    const tcbor::Value response = device.upload(0, ConstBytes{data}, image.size());
    CHECK(flat_rc(response) == static_cast<std::uint64_t>(SmpError::Unknown));

    const tcbor::Value v2 =
        device.upload(0, ConstBytes{data}, image.size(), std::nullopt, Version::V2);
    CHECK(scoped_rc(v2, 1) == static_cast<std::uint64_t>(ImageError::InvalidImageHeader));
}

TEST_CASE("a first chunk without a length is refused", "[simulator][upload]")
{
    // Rule 4, and the order matters: the length is checked after the size of
    // the chunk but before the magic.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> data = chunk_of(image, 0, 64);

    const tcbor::Value response =
        device.upload(0, ConstBytes{data}, std::nullopt, std::nullopt, Version::V2);
    CHECK(scoped_rc(response, 1) == static_cast<std::uint64_t>(ImageError::InvalidLength));
}

TEST_CASE("a first chunk without the MCUboot magic is refused", "[simulator][upload]")
{
    // Rule 3: uploading a raw binary always fails.
    Device device;
    std::vector<std::byte> raw(64, std::byte{0x00});

    const tcbor::Value response =
        device.upload(0, ConstBytes{raw}, raw.size(), std::nullopt, Version::V2);
    CHECK(scoped_rc(response, 1) ==
          static_cast<std::uint64_t>(ImageError::InvalidImageHeaderMagic));
}

TEST_CASE("a request without an offset is refused", "[simulator][upload]")
{
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> data = chunk_of(image, 0, 64);

    const tcbor::Value response =
        device.upload(std::nullopt, ConstBytes{data}, image.size(), std::nullopt, Version::V2);
    CHECK(scoped_rc(response, 1) == static_cast<std::uint64_t>(ImageError::InvalidOffset));
}

TEST_CASE("a wrong offset is answered with success and the wanted offset", "[simulator][upload]")
{
    // Rule 5, in both directions. This is the rule the whole upload state
    // machine is built on, and it is *not* an error response.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> first = chunk_of(image, 0, 64);

    CHECK(offset_of(device.upload(0, ConstBytes{first}, image.size())) == 64);

    // Too far ahead.
    const std::vector<std::byte> ahead = chunk_of(image, 128, 32);
    CHECK(offset_of(device.upload(128, ConstBytes{ahead})) == 64);

    // Behind.
    const std::vector<std::byte> behind = chunk_of(image, 32, 32);
    CHECK(offset_of(device.upload(32, ConstBytes{behind})) == 64);

    // And the data was dropped, not written, in both cases.
    CHECK(device.simulator().bytes_written() == 64);
}

TEST_CASE("a continuation against a forgotten session gets zero", "[simulator][upload]")
{
    // A device that rebooted has area_id == -1 and an offset of zero, so the
    // "wrong offset" answer carries zero and rule 7 takes over. No special case
    // is needed for it, which is the point.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> first = chunk_of(image, 0, 64);
    CHECK(offset_of(device.upload(0, ConstBytes{first}, image.size())) == 64);

    device.simulator().reboot();

    const std::vector<std::byte> next = chunk_of(image, 64, 64);
    CHECK(offset_of(device.upload(64, ConstBytes{next})) == 0);
}

TEST_CASE("data past the declared length is an overrun", "[simulator][upload]")
{
    // Rule 8, and it is checked only after the offset matches.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> first = chunk_of(image, 0, 64);
    static_cast<void>(device.upload(0, ConstBytes{first}, 96));

    std::vector<std::byte> overlong(64, std::byte{0x11});
    const tcbor::Value response =
        device.upload(64, ConstBytes{overlong}, std::nullopt, std::nullopt, Version::V2);
    CHECK(scoped_rc(response, 1) ==
          static_cast<std::uint64_t>(ImageError::InvalidImageDataOverrun));
}

TEST_CASE("an upload to a second image has no slot", "[simulator][upload]")
{
    // The device models one image pair. Rather than write slot 1 anyway and
    // pretend the field was honoured, it answers as a real server does when no
    // slot is available -- so `UploadOptions::image` fails visibly here instead
    // of appearing to work.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);

    tcbor::Writer out;
    out.map(4)
        .text("off")
        .uint(0)
        .text("len")
        .uint(image.size())
        .text("image")
        .uint(1)
        .text("data")
        .blob(ConstBytes{chunk_of(image, 0, 64)});
    const tcbor::Value response =
        device.exchange(Operation::Write, Group::Image, 1, out.view(), Version::V2);
    CHECK(scoped_rc(response, 1) == static_cast<std::uint64_t>(ImageError::NoFreeSlot));
}

TEST_CASE("a first chunk longer than the declared image is an overrun", "[simulator][upload]")
{
    // Zephyr checks the overrun only on the continuation path, where a first
    // chunk longer than the whole image would instead run off the end of the
    // flash area. The double raises the same error rather than corrupting its
    // own memory; the deviation is recorded in the roadmap.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);

    const tcbor::Value response =
        device.upload(0, ConstBytes{image}, 64, std::nullopt, Version::V2);
    CHECK(scoped_rc(response, 1) ==
          static_cast<std::uint64_t>(ImageError::InvalidImageDataOverrun));
}

TEST_CASE("a session is resumed by its sha rather than restarted", "[simulator][upload]")
{
    // Rule 6: a first packet whose sha matches the session in progress is
    // answered with the offset already reached, and does not restart it.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> sha = sha_bytes(image);
    const std::vector<std::byte> first = chunk_of(image, 0, 64);

    CHECK(offset_of(device.upload(0, ConstBytes{first}, image.size(), ConstBytes{sha})) == 64);
    CHECK(offset_of(device.upload(64, ConstBytes{chunk_of(image, 64, 64)})) == 128);

    // The same first packet again: the device keeps its place.
    CHECK(offset_of(device.upload(0, ConstBytes{first}, image.size(), ConstBytes{sha})) == 128);
    CHECK(device.simulator().bytes_written() == 128);
}

TEST_CASE("a different sha restarts the session", "[simulator][upload]")
{
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> other = make_firmware(kBody, 9, 9, 9, 9);
    const std::vector<std::byte> sha = sha_bytes(image);
    const std::vector<std::byte> other_sha = sha_bytes(other);

    CHECK(offset_of(device.upload(0, ConstBytes{chunk_of(image, 0, 64)}, image.size(),
                                  ConstBytes{sha})) == 64);
    CHECK(offset_of(device.upload(0, ConstBytes{chunk_of(other, 0, 64)}, other.size(),
                                  ConstBytes{other_sha})) == 64);
}

TEST_CASE("the final chunk carries match, and completing resets the session", "[simulator][upload]")
{
    // Rules 9 and 9b together: `match` appears only on the last chunk, and the
    // very next request -- a retransmission of that same chunk -- is answered
    // zero, because the session is already gone.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    const std::vector<std::byte> sha = sha_bytes(image);

    std::size_t off = 0;
    constexpr std::size_t kStep = 128;
    tcbor::Value response;
    while (off < image.size()) {
        const std::vector<std::byte> data = chunk_of(image, off, kStep);
        response = off == 0 ? device.upload(0, ConstBytes{data}, image.size(), ConstBytes{sha})
                            : device.upload(off, ConstBytes{data});
        off += data.size();
        if (off < image.size()) {
            CHECK(response.find("match") == nullptr);
        }
    }

    CHECK(offset_of(response) == image.size());
    CHECK(response.get_bool("match") == true);

    const ConstBytes flashed = device.simulator().slot_content(1);
    REQUIRE(flashed.size() == image.size());
    CHECK(std::equal(flashed.begin(), flashed.end(), image.begin()));

    // The lost-final-chunk case: the same chunk again, answered with zero.
    const std::vector<std::byte> last = chunk_of(image, image.size() - 44, 44);
    CHECK(offset_of(device.upload(image.size() - 44, ConstBytes{last})) == 0);
}

TEST_CASE("an image the slot already holds completes on the first packet", "[simulator][upload]")
{
    // Rule 9a: `off == len` and `match` on the very first response, with no
    // data transferred at all.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    device.simulator().load_slot(1, image);

    const std::vector<std::byte> sha = sha_bytes(image);
    const tcbor::Value response =
        device.upload(0, ConstBytes{chunk_of(image, 0, 64)}, image.size(), ConstBytes{sha});

    CHECK(offset_of(response) == image.size());
    CHECK(response.get_bool("match") == true);
    CHECK(device.simulator().bytes_written() == 0);
}

TEST_CASE("a trimmed sha disables the already-present check", "[simulator][upload]")
{
    // The server only runs it for a full 32 bytes, which is the concrete reason
    // smply always sends the whole value.
    Device device;
    const std::vector<std::byte> image = make_firmware(kBody);
    device.simulator().load_slot(1, image);

    std::vector<std::byte> trimmed = sha_bytes(image);
    trimmed.resize(8);
    const tcbor::Value response =
        device.upload(0, ConstBytes{chunk_of(image, 0, 64)}, image.size(), ConstBytes{trimmed});

    CHECK(offset_of(response) == 64);
    CHECK(device.simulator().bytes_written() == 64);
}

TEST_CASE("without the image check there is no match and no shortcut", "[simulator][upload]")
{
    Device device{ServerConfig{.image_check_enabled = false}};
    const std::vector<std::byte> image = make_firmware(kBody);
    device.simulator().load_slot(1, image);
    const std::vector<std::byte> sha = sha_bytes(image);

    tcbor::Value response =
        device.upload(0, ConstBytes{chunk_of(image, 0, 256)}, image.size(), ConstBytes{sha});
    CHECK(offset_of(response) == 256);

    response = device.upload(256, ConstBytes{chunk_of(image, 256, image.size() - 256)});
    CHECK(offset_of(response) == image.size());
    CHECK(response.find("match") == nullptr);
}

TEST_CASE("an upload larger than the slot is refused", "[simulator][upload]")
{
    Device device{ServerConfig{.slot_size = 128}};
    const std::vector<std::byte> image = make_firmware(kBody);

    const tcbor::Value response = device.upload(0, ConstBytes{chunk_of(image, 0, 64)}, image.size(),
                                                std::nullopt, Version::V2);
    CHECK(scoped_rc(response, 1) == static_cast<std::uint64_t>(ImageError::InvalidImageTooLarge));
}

// --- Group 1: state, erase, slot info ---------------------------------------

TEST_CASE("image state reports the slot flags the swap type implies", "[simulator][state]")
{
    // The table from img_mgmt_state_read(): every flag is derived from the swap
    // type, and the REVERT row is the one that matters -- the running image is
    // active but not confirmed.
    Device device;
    device.simulator().load_slot(0, make_firmware(kBody, 1, 0, 0, 1));
    device.simulator().load_slot(1, make_firmware(kBody, 2, 0, 0, 2));

    tcbor::Writer empty;
    empty.map(0);

    const auto slots_of = [&] {
        const tcbor::Value state = device.exchange(Operation::Read, Group::Image, 0, empty.view());
        const tcbor::Value* images = state.find("images");
        REQUIRE(images != nullptr);
        REQUIRE(images->is(tcbor::Value::Kind::Array));
        return images->items;
    };

    {
        const std::vector<tcbor::Value> slots = slots_of();
        REQUIRE(slots.size() == 2);
        CHECK(slots[0].get_bool("active") == true);
        CHECK(slots[0].get_bool("confirmed") == true);
        CHECK(slots[1].get_bool("pending") == false);
        CHECK(slots[1].get_bool("confirmed") == false);
        // A single-image device omits the image number entirely.
        CHECK(slots[0].find("image") == nullptr);
    }

    // Mark the secondary for test.
    const std::vector<tcbor::Value> before = slots_of();
    const tcbor::Value* hash = before[1].find("hash");
    REQUIRE(hash != nullptr);
    tcbor::Writer request;
    request.map(2).text("hash").blob(ConstBytes{hash->bytes}).text("confirm").boolean(false);
    static_cast<void>(device.exchange(Operation::Write, Group::Image, 0, request.view()));
    CHECK(device.simulator().swap_type() == SwapType::Test);

    {
        const std::vector<tcbor::Value> slots = slots_of();
        CHECK(slots[1].get_bool("pending") == true);
        CHECK(slots[1].get_bool("permanent") == false);
    }

    device.simulator().reboot();
    CHECK(device.simulator().swap_type() == SwapType::Revert);

    {
        const std::vector<tcbor::Value> slots = slots_of();
        CHECK(slots[0].get_bool("active") == true);
        CHECK(slots[0].get_bool("confirmed") == false);
        CHECK(slots[1].get_bool("confirmed") == true);
    }
}

TEST_CASE("a test with no hash, and a hash of the wrong length, are refused", "[simulator][state]")
{
    Device device;
    device.simulator().load_slot(0, make_firmware(kBody));

    tcbor::Writer no_hash;
    no_hash.map(1).text("confirm").boolean(false);
    const tcbor::Value refused =
        device.exchange(Operation::Write, Group::Image, 0, no_hash.view(), Version::V2);
    CHECK(scoped_rc(refused, 1) == static_cast<std::uint64_t>(ImageError::InvalidHash));

    std::vector<std::byte> short_hash(8, std::byte{0x01});
    tcbor::Writer wrong_length;
    wrong_length.map(1).text("hash").blob(ConstBytes{short_hash});
    const tcbor::Value bad_length =
        device.exchange(Operation::Write, Group::Image, 0, wrong_length.view(), Version::V2);
    CHECK(scoped_rc(bad_length, 1) == static_cast<std::uint64_t>(ImageError::InvalidHash));

    std::vector<std::byte> unknown(32, std::byte{0x02});
    tcbor::Writer missing;
    missing.map(1).text("hash").blob(ConstBytes{unknown});
    const tcbor::Value not_found =
        device.exchange(Operation::Write, Group::Image, 0, missing.view(), Version::V2);
    CHECK(scoped_rc(not_found, 1) == static_cast<std::uint64_t>(ImageError::HashNotFound));
}

TEST_CASE("setting test on the running slot is denied", "[simulator][state]")
{
    Device device;
    const std::vector<std::byte> running = make_firmware(kBody, 1, 0, 0, 1);
    device.simulator().load_slot(0, running);

    tcbor::Writer empty;
    empty.map(0);
    const tcbor::Value state = device.exchange(Operation::Read, Group::Image, 0, empty.view());
    const tcbor::Value* images = state.find("images");
    REQUIRE(images != nullptr);
    const tcbor::Value* hash = images->items.at(0).find("hash");
    REQUIRE(hash != nullptr);

    tcbor::Writer request;
    request.map(1).text("hash").blob(ConstBytes{hash->bytes});
    const tcbor::Value refused =
        device.exchange(Operation::Write, Group::Image, 0, request.view(), Version::V2);
    CHECK(scoped_rc(refused, 1) ==
          static_cast<std::uint64_t>(ImageError::ImageSettingTestToActiveDenied));
}

TEST_CASE("erase refuses a slot marked for the next boot", "[simulator][state]")
{
    Device device;
    device.simulator().load_slot(0, make_firmware(kBody, 1, 0, 0, 1));
    device.simulator().load_slot(1, make_firmware(kBody, 2, 0, 0, 2));

    tcbor::Writer empty;
    empty.map(0);
    const tcbor::Value state = device.exchange(Operation::Read, Group::Image, 0, empty.view());
    const tcbor::Value* images = state.find("images");
    REQUIRE(images != nullptr);
    const tcbor::Value* hash = images->items.at(1).find("hash");
    REQUIRE(hash != nullptr);

    tcbor::Writer mark;
    mark.map(1).text("hash").blob(ConstBytes{hash->bytes});
    static_cast<void>(device.exchange(Operation::Write, Group::Image, 0, mark.view()));

    const tcbor::Value refused =
        device.exchange(Operation::Write, Group::Image, 5, empty.view(), Version::V2);
    CHECK(scoped_rc(refused, 1) == static_cast<std::uint64_t>(ImageError::NoFreeSlot));
}

TEST_CASE("erase leaves a slot the state read skips", "[simulator][state]")
{
    Device device;
    device.simulator().load_slot(0, make_firmware(kBody, 1, 0, 0, 1));
    device.simulator().load_slot(1, make_firmware(kBody, 2, 0, 0, 2));

    tcbor::Writer empty;
    empty.map(0);
    const tcbor::Value erased = device.exchange(Operation::Write, Group::Image, 5, empty.view());
    CHECK(erased.size() == 0);

    const tcbor::Value state = device.exchange(Operation::Read, Group::Image, 0, empty.view());
    const tcbor::Value* images = state.find("images");
    REQUIRE(images != nullptr);
    CHECK(images->items.size() == 1);
}

TEST_CASE("slot info is optional and describes both slots", "[simulator][state]")
{
    tcbor::Writer empty;
    empty.map(0);

    Device without;
    const tcbor::Value refused = without.exchange(Operation::Read, Group::Image, 6, empty.view());
    CHECK(flat_rc(refused) == static_cast<std::uint64_t>(SmpError::NotSupported));

    Device with{ServerConfig{.supports_slot_info = true, .slot_size = 4096}};
    const tcbor::Value response = with.exchange(Operation::Read, Group::Image, 6, empty.view());
    const tcbor::Value* images = response.find("images");
    REQUIRE(images != nullptr);
    REQUIRE(images->items.size() == 1);
    const tcbor::Value* slots = images->items.at(0).find("slots");
    REQUIRE(slots != nullptr);
    REQUIRE(slots->items.size() == 2);
    CHECK(slots->items.at(1).get_uint("size") == 4096);
    CHECK(images->items.at(0).get_uint("max_image_size") == 4096);
}

TEST_CASE("a v1 client sees the translated code, a v2 client the real one", "[simulator][errors]")
{
    // A16: the translation is lossy and many-to-one, which is why an absent
    // group code is normal rather than a malformed reply.
    Device device;
    device.simulator().load_slot(0, make_firmware(kBody));

    tcbor::Writer empty;
    empty.map(0);

    device.simulator().fail_next(ImageError::HashNotFound);
    const tcbor::Value v1 = device.exchange(Operation::Read, Group::Image, 0, empty.view());
    CHECK(flat_rc(v1) == static_cast<std::uint64_t>(SmpError::Unknown));

    device.simulator().fail_next(ImageError::HashNotFound);
    const tcbor::Value v2 =
        device.exchange(Operation::Read, Group::Image, 0, empty.view(), Version::V2);
    CHECK(scoped_rc(v2, 1) == static_cast<std::uint64_t>(ImageError::HashNotFound));

    device.simulator().fail_next(ImageError::ImageAlreadyPending);
    const tcbor::Value pending = device.exchange(Operation::Read, Group::Image, 0, empty.view());
    CHECK(flat_rc(pending) == static_cast<std::uint64_t>(SmpError::BadState));
}

TEST_CASE("a device can be built without the v1 translation", "[simulator][errors]")
{
    Device device{ServerConfig{.translate_v1_errors = false}};
    device.simulator().load_slot(0, make_firmware(kBody));
    device.simulator().fail_next(ImageError::HashNotFound);

    tcbor::Writer empty;
    empty.map(0);
    const tcbor::Value response = device.exchange(Operation::Read, Group::Image, 0, empty.view());
    CHECK(scoped_rc(response, 1) == static_cast<std::uint64_t>(ImageError::HashNotFound));
}

TEST_CASE("an unknown group is not supported", "[simulator][errors]")
{
    Device device;
    tcbor::Writer empty;
    empty.map(0);
    const tcbor::Value response = device.exchange(Operation::Read, Group::Stat, 0, empty.view());
    CHECK(flat_rc(response) == static_cast<std::uint64_t>(SmpError::NotSupported));
}
