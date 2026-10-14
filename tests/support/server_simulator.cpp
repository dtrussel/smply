// SPDX-License-Identifier: Apache-2.0

#include "server_simulator.hpp"

#include "message_builder.hpp"
#include "minicbor/minicbor.hpp"

#include "image/sha256.hpp"
#include "smply/error.hpp"
#include "smply/group.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace smply::test {

// The codec lives in support/, outside this namespace. Aliased for brevity:
// this file uses it a hundred times.
namespace tcbor = smply::minicbor;

namespace {

// --- The upload's header check ------------------------------------------------
//
// img_mgmt_upload_inspect() looks at the first packet's magic before it writes
// anything (section 6). Read by hand from docs/protocol-notes.md section 7, as
// the boot model's own reader is, and never through src/image/.

constexpr std::uint32_t kImageMagic = 0x96F3B83DU;
constexpr std::size_t kImageHeaderSize = 32;

[[nodiscard]] std::uint32_t read32(ConstBytes bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + i])
                 << (8U * static_cast<unsigned>(i));
    }
    return value;
}

[[nodiscard]] std::vector<std::byte> sha256_of(ConstBytes content)
{
    image::Sha256 hasher;
    hasher.update(content);
    const auto digest = hasher.finish();
    return {digest.begin(), digest.end()};
}

/// `img_mgmt_translate_error_code()`: the lossy, many-to-one mapping an
/// image-group code goes through on its way to a v1 client (section 9, A16).
[[nodiscard]] SmpError translate(ImageError code)
{
    switch (code) {
    case ImageError::Ok:
        return SmpError::Ok;
    case ImageError::NoFreeSlot:
    case ImageError::CurrentVersionIsNewer:
    case ImageError::ImageAlreadyPending:
        return SmpError::BadState;
    case ImageError::NoFreeMemory:
        return SmpError::NoMemory;
    case ImageError::InvalidOffset:
    case ImageError::InvalidLength:
        return SmpError::InvalidArgument;
    default:
        return SmpError::Unknown;
    }
}

constexpr std::uint32_t kBufCount = 4;
constexpr std::uint32_t kDefaultSlotSize = 512U * 1024U;

} // namespace

namespace {

/// The device's build, as the boot model takes it.
[[nodiscard]] boot_model::BootConfig boot_config(const ServerConfig& config)
{
    boot_model::BootConfig out;
    // The reported mode picks what the device does; an absent, negative or
    // unmodelled number runs as swap with revert (docs/testing.md section 2).
    out.behaviour = boot_model::behaviour_for_mode(config.bootloader_mode);
    out.image_count = config.image_count;
    out.no_downgrade = config.no_downgrade;
    out.allow_confirm_non_active_image_secondary = config.allow_confirm_non_active_image_secondary;
    out.allow_confirm_non_active_image_any = config.allow_confirm_non_active_image_any;
    out.allow_confirm_non_active_slot = config.allow_confirm_non_active_slot;
    return out;
}

} // namespace

ServerSimulator::ServerSimulator(FakeTransport& transport, const ServerConfig& config)
    : transport_{transport}, config_{config}, model_{boot_config(config)}
{}

void ServerSimulator::load_slot(std::size_t slot, std::vector<std::byte> content)
{
    model_.load_slot(slot, std::move(content));
}

ConstBytes ServerSimulator::slot_content(std::size_t slot) const
{
    return model_.slot_content(slot);
}

void ServerSimulator::device_commits(std::uint32_t image, ApplyOutcome outcome, unsigned reads,
                                     CommitOutcome commit, unsigned commit_reads)
{
    model_.device_commits(image, boot_model::DeviceCommit{.outcome = outcome,
                                                          .apply_reads = reads,
                                                          .commit = commit,
                                                          .commit_reads = commit_reads});
}

void ServerSimulator::reboot()
{
    // Every boot decision -- swap, overwrite, downgrade prevention and
    // direct-XIP alike -- is the boot model's.
    model_.reboot();

    // The upload session does not survive: a device that has rebooted has
    // area_id == -1, so a continuation is answered off == 0 (rule 5).
    session_ = Session{};
    reset_requested_ = false;
}

void ServerSimulator::boot_from_slot(std::size_t slot)
{
    model_.boot_from_slot(slot);
}

void ServerSimulator::rebind_transport(FakeTransport& transport)
{
    transport_ = transport;
    consumed_ = transport.sent().size();
    // Anything still queued was destined for a link that no longer exists.
    pending_.clear();
}

void ServerSimulator::answer_offset_once(std::uint64_t off)
{
    forced_offset_ = off;
}

void ServerSimulator::fail_next(ImageError code, std::optional<Operation> op)
{
    forced_failure_ = code;
    forced_failure_op_ = op;
}

void ServerSimulator::drop_next_response()
{
    drop_next_ = true;
}

void ServerSimulator::reset_busy_once()
{
    reset_busy_ = true;
}

void ServerSimulator::pump(TimePoint now)
{
    const auto& sent = transport_.get().sent();
    assert(sent.size() >= consumed_ && "the simulator's transport must not be cleared");

    // Snapshot the count: delivering below runs client callbacks, which send
    // the next request into this same vector.
    const std::size_t end = sent.size();
    while (consumed_ < end) {
        // Copy before dispatching: the vector reallocates as it grows.
        const std::vector<std::byte> message = sent[consumed_];
        ++consumed_;

        const Result<Header> header = decode_header(ConstBytes{message});
        if (!header.has_value() || message.size() < header->total_size()) {
            continue; // Not something a device could answer.
        }
        handle(*header, ConstBytes{message}.subspan(kHeaderSize, header->length), now);
    }

    std::vector<Pending> still_waiting;
    std::vector<std::vector<std::byte>> due;
    for (Pending& entry : pending_) {
        if (entry.due <= now) {
            due.push_back(std::move(entry.message));
        } else {
            still_waiting.push_back(std::move(entry));
        }
    }
    pending_ = std::move(still_waiting);

    for (const std::vector<std::byte>& message : due) {
        transport_.get().deliver(ConstBytes{message});
    }
}

void ServerSimulator::handle(const Header& header, ConstBytes payload, TimePoint now)
{
    requests_.push_back(header);

    std::vector<std::byte> response;
    switch (header.group) {
    case Group::Os:
        response = handle_os(header, payload);
        break;
    case Group::Image:
        response = handle_image(header, payload);
        break;
    default:
        response = smp_failure(header.version, SmpError::NotSupported);
        break;
    }

    enqueue(header, ConstBytes{response}, now);
}

void ServerSimulator::enqueue(const Header& request, ConstBytes payload, TimePoint now)
{
    if (drop_next_) {
        drop_next_ = false;
        ++dropped_;
        return;
    }

    const Header reply{.op = response_to(request.op),
                       .version = request.version,
                       .flags = 0,
                       .length = 0,
                       .group = request.group,
                       .seq = request.seq,
                       .command = request.command};
    pending_.push_back(Pending{now + config_.response_delay, make_message(reply, payload)});
}

// --- Group 0: OS ------------------------------------------------------------

std::vector<std::byte> ServerSimulator::handle_os(const Header& header, ConstBytes payload)
{
    const std::optional<tcbor::Value> request = tcbor::parse(payload);
    if (!request.has_value() || !request->is(tcbor::Value::Kind::Map)) {
        return smp_failure(header.version, SmpError::InvalidArgument);
    }

    switch (header.command) {
    case 0: { // Echo, registered under both the read and the write slot.
        const tcbor::Value* text = request->find("d");
        if (text == nullptr || !text->is(tcbor::Value::Kind::Text)) {
            return smp_failure(header.version, SmpError::InvalidArgument);
        }
        tcbor::Writer out;
        out.map(1).text("r").text(text->text);
        return out.bytes();
    }

    case 5: { // Reset: write-only.
        if (header.op != Operation::Write) {
            return smp_failure(header.version, SmpError::NotSupported);
        }
        last_reset_force_ = request->get_bool("force");
        if (reset_busy_) {
            reset_busy_ = false;
            return smp_failure(header.version, SmpError::Busy);
        }
        // Acceptance, not completion: the link drops afterwards, and it is the
        // test that drops it.
        reset_requested_ = true;
        tcbor::Writer out;
        out.map(0);
        return out.bytes();
    }

    case 6: { // MCUmgr parameters: read-only, and optional.
        if (header.op != Operation::Read || !config_.supports_mcumgr_params) {
            return smp_failure(header.version, SmpError::NotSupported);
        }
        tcbor::Writer out;
        out.map(2).text("buf_size").uint(config_.buf_size).text("buf_count").uint(kBufCount);
        return out.bytes();
    }

    case 8: { // Bootloader information: read-only, and optional.
        if (header.op != Operation::Read || !config_.bootloader_mode.has_value()) {
            return smp_failure(header.version, SmpError::NotSupported);
        }
        constexpr std::uint64_t kQueryYieldsNoAnswer = 3;
        const tcbor::Value* query = request->find("query");
        if (query == nullptr) {
            tcbor::Writer out;
            out.map(1).text("bootloader").text(config_.bootloader_name);
            return out.bytes();
        }
        if (!query->is(tcbor::Value::Kind::Text) || query->text != "mode" ||
            config_.bootloader_name != "MCUboot") {
            return os_failure(kQueryYieldsNoAnswer);
        }
        tcbor::Writer out;
        out.map(config_.no_downgrade ? 2 : 1).text("mode");
        if (*config_.bootloader_mode < 0) {
            out.nint(*config_.bootloader_mode);
        } else {
            out.uint(static_cast<std::uint64_t>(*config_.bootloader_mode));
        }
        if (config_.no_downgrade) {
            out.text("no-downgrade").boolean(true);
        }
        return out.bytes();
    }

    default:
        return smp_failure(header.version, SmpError::NotSupported);
    }
}

// --- Group 1: image ---------------------------------------------------------

std::vector<std::byte> ServerSimulator::handle_image(const Header& header, ConstBytes payload)
{
    if (forced_failure_.has_value() &&
        (!forced_failure_op_.has_value() || *forced_failure_op_ == header.op)) {
        const ImageError code = *forced_failure_;
        forced_failure_.reset();
        forced_failure_op_.reset();
        return image_failure(header.version, code);
    }

    switch (header.command) {
    case 0:
        if (header.op == Operation::Read) {
            return handle_state_read();
        }
        if (!model_.supports_set_state()) {
            // The write handler is NULL under direct-XIP (S10).
            return smp_failure(header.version, SmpError::NotSupported);
        }
        return handle_state_write(header.version, payload);

    case 1:
        if (header.op != Operation::Write) {
            return smp_failure(header.version, SmpError::NotSupported);
        }
        return handle_upload(header, payload);

    case 5:
        if (header.op != Operation::Write) {
            return smp_failure(header.version, SmpError::NotSupported);
        }
        return handle_erase(header.version, payload);

    case 6:
        if (header.op != Operation::Read || !config_.supports_slot_info) {
            return smp_failure(header.version, SmpError::NotSupported);
        }
        return handle_slot_info();

    default:
        return smp_failure(header.version, SmpError::NotSupported);
    }
}

std::vector<std::byte> ServerSimulator::handle_upload(const Header& header, ConstBytes payload)
{
    const std::optional<tcbor::Value> request = tcbor::parse(payload);
    if (!request.has_value() || !request->is(tcbor::Value::Kind::Map)) {
        return image_failure(header.version, ImageError::InvalidOffset);
    }

    const std::optional<std::uint64_t> off = request->get_uint("off");
    if (!off.has_value()) {
        return image_failure(header.version, ImageError::InvalidOffset);
    }

    const tcbor::Value* data_field = request->find("data");
    ConstBytes data;
    if (data_field != nullptr && data_field->is(tcbor::Value::Kind::Bytes)) {
        data = ConstBytes{data_field->bytes};
    }

    // img_mgmt_upload_inspect(), in its own order (section 6, rules 2-8).
    bool proceed = true;
    if (*off == 0) {
        if (data.size() < kImageHeaderSize) {
            return image_failure(header.version, ImageError::InvalidImageHeader);
        }
        const std::optional<std::uint64_t> size = request->get_uint("len");
        if (!size.has_value()) {
            return image_failure(header.version, ImageError::InvalidLength);
        }
        if (read32(data, 0) != kImageMagic) {
            return image_failure(header.version, ImageError::InvalidImageHeaderMagic);
        }

        // `image` is read here, on the first packet only, and the boot model
        // picks its slot: the image's secondary (S36), or under direct-XIP
        // the slot not running (S10). An image number the device does not
        // have has no slot to upload to -- the answer a real server gives
        // when img_mgmt_get_unused_slot_area_id() finds none.
        const std::uint64_t image = request->get_uint("image").value_or(0);
        const std::optional<std::size_t> slot = model_.upload_target(image);
        if (!slot.has_value()) {
            return image_failure(header.version, ImageError::NoFreeSlot);
        }
        const std::size_t target = *slot;

        const tcbor::Value* sha_field = request->find("sha");
        std::vector<std::byte> sha;
        if (sha_field != nullptr && sha_field->is(tcbor::Value::Kind::Bytes)) {
            sha = sha_field->bytes;
        }
        if (sha.size() > image::kSha256DigestSize) {
            return image_failure(header.version, ImageError::InvalidHash);
        }

        // Resume: a live session whose stored sha matches is continued where it
        // stands, without restarting (rule 6).
        if (!sha.empty() && session_.active && session_.sha == sha) {
            proceed = false;
        } else {
            const std::uint32_t capacity =
                config_.slot_size != 0 ? config_.slot_size : kDefaultSlotSize;
            if (*size > capacity) {
                return image_failure(header.version, ImageError::InvalidImageTooLarge);
            }

            // Rule 9a: with a full 32-byte sha the server checks whether the
            // slot already holds this exact image *before* erasing anything,
            // and finishes there if it does. Checking after the erase -- which
            // an earlier draft of this file did -- can never match, and would
            // have quietly removed the shortest path through the whole update.
            if (config_.image_check_enabled && sha.size() == image::kSha256DigestSize &&
                sha256_of(slot_content(target)) == sha) {
                session_ = Session{};
                tcbor::Writer out;
                out.map(2).text("off").uint(*size).text("match").boolean(true);
                return out.bytes();
            }

            // Zephyr checks the overrun only on the continuation path, where
            // a first chunk longer than the whole image would run off the end
            // of the flash area instead. A double must not corrupt its own
            // memory to be faithful, so the same error is raised here.
            if (data.size() > *size) {
                return image_failure(header.version, ImageError::InvalidImageDataOverrun);
            }

            session_ = Session{};
            session_.active = true;
            session_.slot = target;
            session_.size = *size;
            session_.sha = sha;
            // The implicit erase of the target slot (rule 12). The size is
            // bounded by the capacity check above, so a device-supplied `len`
            // never sizes an unbounded allocation -- the same rule the library
            // works under. Under direct-XIP the model's erase takes the slot's
            // boot state too.
            model_.begin_upload(target, static_cast<std::size_t>(*size));
        }
    } else {
        if (!session_.active || *off != session_.off) {
            // Rule 5: not an error. Drop the data and say what is wanted --
            // which after a completed upload (rule 9b) or a reboot is zero.
            proceed = false;
        } else if (*off + data.size() > session_.size) {
            return image_failure(header.version, ImageError::InvalidImageDataOverrun);
        }
    }

    if (forced_offset_.has_value()) {
        const std::uint64_t forced = *forced_offset_;
        forced_offset_.reset();
        tcbor::Writer out;
        out.map(1).text("off").uint(forced);
        return out.bytes();
    }

    if (!proceed) {
        tcbor::Writer out;
        out.map(1).text("off").uint(session_.active ? session_.off : 0);
        return out.bytes();
    }

    bool last = false;
    if (!data.empty()) {
        last = session_.off + data.size() == session_.size;
        // The session's bounds check above keeps the write inside the erase.
        const bool written =
            model_.write(session_.slot, static_cast<std::size_t>(session_.off), data);
        assert(written);
        static_cast<void>(written);
        session_.off += data.size();
        bytes_written_ += data.size();
    }

    const std::uint64_t reported = session_.off;
    bool match = false;
    if (last) {
        // The final-chunk check runs whatever length of sha was supplied, and
        // compares against it zero-padded -- so a trimmed or absent sha yields
        // match == false rather than no answer (rule 9c).
        std::vector<std::byte> padded = session_.sha;
        padded.resize(image::kSha256DigestSize, std::byte{0});
        match = sha256_of(slot_content(session_.slot)) == padded;
        session_ = Session{};
    }

    tcbor::Writer out;
    if (last && config_.image_check_enabled) {
        out.map(2).text("off").uint(reported).text("match").boolean(match);
    } else {
        out.map(1).text("off").uint(reported);
    }
    return out.bytes();
}

std::vector<std::byte> ServerSimulator::handle_state_read()
{
    // A state read is one step of a device-committed apply or commit
    // (ADR-0022); the boot model advances it.
    return encode_state(model_.read_state());
}

std::vector<std::byte>
ServerSimulator::encode_state(const std::vector<boot_model::SlotEntry>& listing) const
{
    // Zephyr omits "image" only when the device has one updatable image.
    const bool with_image = !config_.single_image || model_.image_count() > 1;

    tcbor::Writer out;
    out.map(2).text("images").array(listing.size());
    for (const boot_model::SlotEntry& entry : listing) {
        // slot, version, hash, bootable, pending, confirmed, active,
        // permanent -- plus the image number when the device reports it.
        out.map(with_image ? 9U : 8U);
        if (with_image) {
            out.text("image").uint(entry.image);
        }
        out.text("slot").uint(entry.slot);
        out.text("version").text(entry.content.version);
        out.text("hash").blob(ConstBytes{entry.content.hash});
        out.text("bootable").boolean(entry.content.bootable);
        out.text("pending").boolean(entry.pending);
        out.text("confirmed").boolean(entry.confirmed);
        out.text("active").boolean(entry.active);
        out.text("permanent").boolean(entry.permanent);
    }
    out.text("splitStatus").uint(0);
    return out.bytes();
}

std::vector<std::byte> ServerSimulator::handle_state_write(Version version, ConstBytes payload)
{
    const std::optional<tcbor::Value> request = tcbor::parse(payload);
    if (!request.has_value() || !request->is(tcbor::Value::Kind::Map)) {
        return image_failure(version, ImageError::Unknown);
    }

    const bool confirm = request->get_bool("confirm").value_or(false);
    const tcbor::Value* hash_field = request->find("hash");

    // The hash's encoding is the wire's to check; which slot it names and
    // what may be done to it is the boot model's (S14, S35).
    std::optional<ConstBytes> hash;
    if (hash_field == nullptr) {
        if (!confirm) {
            // A test with no hash names no image, and is refused.
            return image_failure(version, ImageError::InvalidHash);
        }
    } else {
        if (!hash_field->is(tcbor::Value::Kind::Bytes) ||
            hash_field->bytes.size() != image::kSha256DigestSize) {
            return image_failure(version, ImageError::InvalidHash);
        }
        hash = ConstBytes{hash_field->bytes};
    }

    const ImageError result = model_.set_state(hash, confirm);
    if (result != ImageError::Ok) {
        return image_failure(version, result);
    }
    return encode_state(model_.listing());
}

std::vector<std::byte> ServerSimulator::handle_erase(Version version, ConstBytes payload)
{
    const std::optional<tcbor::Value> request = tcbor::parse(payload);
    if (!request.has_value() || !request->is(tcbor::Value::Kind::Map)) {
        return image_failure(version, ImageError::Unknown);
    }

    // The default is the slot opposite the active one of the running image,
    // not the constant 1 -- which happens to be 1 here, but the rule is what is
    // modelled. Slots are global, as in the listing.
    const std::uint64_t slot = request->get_uint("slot").value_or(1);
    const ImageError result = model_.erase(slot);
    if (result != ImageError::Ok) {
        return image_failure(version, result);
    }
    session_ = Session{};

    tcbor::Writer out;
    out.map(0);
    return out.bytes();
}

std::vector<std::byte> ServerSimulator::handle_slot_info() const
{
    const std::uint32_t capacity = config_.slot_size != 0 ? config_.slot_size : kDefaultSlotSize;

    tcbor::Writer out;
    out.map(1).text("images").array(model_.image_count());
    for (std::uint32_t image = 0; image < model_.image_count(); ++image) {
        out.map(3);
        out.text("image").uint(image);
        out.text("slots").array(2);
        for (std::size_t slot = 0; slot < 2; ++slot) {
            out.map(2).text("slot").uint(slot).text("size").uint(capacity);
        }
        out.text("max_image_size").uint(capacity);
    }
    return out.bytes();
}

// --- Error shapes -----------------------------------------------------------

std::vector<std::byte> ServerSimulator::image_failure(Version version, ImageError code) const
{
    tcbor::Writer out;
    if (version == Version::V1 && config_.translate_v1_errors) {
        out.map(1).text("rc").uint(static_cast<std::uint64_t>(translate(code)));
        return out.bytes();
    }
    out.map(1).text("err").map(2);
    out.text("group").uint(static_cast<std::uint64_t>(Group::Image));
    out.text("rc").uint(static_cast<std::uint64_t>(code));
    return out.bytes();
}

std::vector<std::byte> ServerSimulator::os_failure(std::uint64_t code)
{
    tcbor::Writer out;
    out.map(1).text("err").map(2);
    out.text("group").uint(static_cast<std::uint64_t>(Group::Os));
    out.text("rc").uint(code);
    return out.bytes();
}

std::vector<std::byte> ServerSimulator::smp_failure(Version version, SmpError code)
{
    // An SMP-level failure is raised below the group handler, so it is flat in
    // both versions.
    static_cast<void>(version);
    tcbor::Writer out;
    out.map(1).text("rc").uint(static_cast<std::uint64_t>(code));
    return out.bytes();
}

} // namespace smply::test
