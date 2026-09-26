// SPDX-License-Identifier: Apache-2.0

#include "server_simulator.hpp"

#include "message_builder.hpp"
#include "minicbor/minicbor.hpp"

#include "image/sha256.hpp"
#include "smply/error.hpp"
#include "smply/group.hpp"

#include <algorithm>
#include <array>
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

// --- MCUboot image layout ---------------------------------------------------
//
// Read here from the layout table in docs/protocol-notes.md section 7 rather
// than through src/image/, for the same reason the CBOR codec is independent:
// a component test that parsed slot content with the parser under test could
// not tell a correct image from one both halves agree to misread.

constexpr std::uint32_t kImageMagic = 0x96F3B83DU;
constexpr std::size_t kImageHeaderSize = 32;
constexpr std::uint16_t kTlvInfoMagic = 0x6907;
constexpr std::uint16_t kTlvProtInfoMagic = 0x6908;
constexpr std::uint16_t kTlvSha256 = 0x10;
constexpr std::uint32_t kImageNonBootable = 0x00000010U;
constexpr std::size_t kAreaHeaderSize = 4;

[[nodiscard]] std::uint16_t read16(ConstBytes bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
                                      (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

[[nodiscard]] std::uint32_t read32(ConstBytes bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + i])
                 << (8U * static_cast<unsigned>(i));
    }
    return value;
}

/// What the device can say about a slot: everything image-state reports.
struct SlotImage
{
    std::string version;
    std::vector<std::byte> hash;
    bool bootable = true;
};

/// Reads a slot's content the way `img_mgmt_read_info()` does.
///
/// Returns nullopt when the content is not a valid image, which is the
/// mechanism behind "a response only contains valid images, invalid ones are
/// simply skipped" (docs/protocol-notes.md section 6).
[[nodiscard]] std::optional<SlotImage> describe(ConstBytes content)
{
    if (content.size() < kImageHeaderSize || read32(content, 0) != kImageMagic) {
        return std::nullopt;
    }

    const std::uint16_t header_size = read16(content, 8);
    const std::uint32_t image_size = read32(content, 12);
    const std::uint32_t flags = read32(content, 16);

    SlotImage out;
    out.bootable = (flags & kImageNonBootable) == 0;

    // img_mgmt_ver_str(): "major.minor.revision", plus ".build" when non-zero.
    const auto major = std::to_integer<unsigned>(content[20]);
    const auto minor = std::to_integer<unsigned>(content[21]);
    const std::uint16_t revision = read16(content, 22);
    const std::uint32_t build = read32(content, 24);
    out.version =
        std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(revision);
    if (build != 0) {
        out.version += "." + std::to_string(build);
    }

    // The hash TLV lives in the unprotected area, which follows the protected
    // one when there is a protected one. Both areas are walked as one run.
    std::size_t offset = static_cast<std::size_t>(header_size) + image_size;
    for (int area = 0; area < 2; ++area) {
        if (offset + kAreaHeaderSize > content.size()) {
            break;
        }
        const std::uint16_t magic = read16(content, offset);
        const std::uint16_t total = read16(content, offset + 2);
        if (magic != kTlvInfoMagic && magic != kTlvProtInfoMagic) {
            break;
        }
        if (total < kAreaHeaderSize || offset + total > content.size()) {
            break;
        }

        const std::size_t area_end = offset + total;
        std::size_t entry = offset + kAreaHeaderSize;
        while (entry + kAreaHeaderSize <= area_end) {
            const std::uint16_t type = read16(content, entry);
            const std::uint16_t length = read16(content, entry + 2);
            const std::size_t value = entry + kAreaHeaderSize;
            if (value + length > area_end) {
                break;
            }
            if (type == kTlvSha256 && magic == kTlvInfoMagic) {
                const ConstBytes bytes = content.subspan(value, length);
                out.hash.assign(bytes.begin(), bytes.end());
            }
            entry = value + length;
        }

        offset = area_end;
        if (magic == kTlvInfoMagic) {
            break;
        }
    }

    return out;
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

ServerSimulator::ServerSimulator(FakeTransport& transport, ServerConfig config)
    : transport_{transport}, config_{config},
      images_(config.image_count == 0 ? 1U : config.image_count)
{}

void ServerSimulator::load_slot(std::size_t slot, std::vector<std::byte> content)
{
    assert(slot / 2 < images_.size());
    images_[slot / 2].slots[slot % 2] = std::move(content);
}

ConstBytes ServerSimulator::slot_content(std::size_t slot) const
{
    assert(slot / 2 < images_.size());
    return ConstBytes{images_[slot / 2].slots[slot % 2]};
}

void ServerSimulator::device_commits(std::uint32_t image, ApplyOutcome outcome, unsigned reads,
                                     CommitOutcome commit, unsigned commit_reads)
{
    assert(image < images_.size());
    images_[image].device_outcome = outcome;
    images_[image].apply_reads = reads;
    images_[image].commit = commit;
    images_[image].commit_reads = commit_reads;
}

void ServerSimulator::start_device_commits()
{
    for (ImagePair& pair : images_) {
        const bool on_trial = pair.device_outcome.has_value() && pair.swap == SwapType::Revert &&
                              !pair.applying.has_value();
        if (on_trial && pair.commit == CommitOutcome::Commits && !pair.committing.has_value()) {
            pair.committing = pair.commit_reads;
        }
    }
}

void ServerSimulator::reboot()
{
    // MCUboot runs every image's swap at the same boot.
    for (ImagePair& pair : images_) {
        switch (pair.swap) {
        case SwapType::None:
            break;
        case SwapType::Test:
            if (pair.device_outcome.has_value()) {
                // Not this MCU's image: the coordinating MCU starts applying it
                // to the other one, and slot 0 keeps reporting what the other
                // MCU runs -- still the old image, with slot 1 pending
                // (docs/multi-image.md, ADR-0022).
                pair.applying = pair.apply_reads;
                break;
            }
            std::swap(pair.slots[0], pair.slots[1]);
            pair.swap = SwapType::Revert;
            break;
        case SwapType::Perm:
        case SwapType::Revert:
            // Two different intentions with the same effect: `Perm` swaps the
            // new image in for good, `Revert` swaps the old one back after an
            // unconfirmed trial. Either way the contents exchange and nothing
            // is left scheduled.
            // For a device-committed image this is the other MCU reverting
            // its unconfirmed trial on a reset.
            std::swap(pair.slots[0], pair.slots[1]);
            pair.swap = SwapType::None;
            pair.applying.reset();
            pair.committing.reset();
            break;
        }
    }

    // The upload session does not survive: a device that has rebooted has
    // area_id == -1, so a continuation is answered off == 0 (rule 5).
    session_ = Session{};
    reset_requested_ = false;
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

        // `image` is read here, on the first packet only, and picks that
        // image's secondary slot (S36). An image number the device does not
        // have has no slot to upload to -- the answer a real server gives
        // when img_mgmt_get_unused_slot_area_id() finds none.
        const std::uint64_t image = request->get_uint("image").value_or(0);
        if (image >= images_.size()) {
            return image_failure(header.version, ImageError::NoFreeSlot);
        }
        const std::size_t target = (static_cast<std::size_t>(image) * 2) + 1;

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
            // works under.
            images_[target / 2].slots[1].assign(static_cast<std::size_t>(*size), std::byte{0xFF});
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
        auto& slot = images_[session_.slot / 2].slots[session_.slot % 2];
        const auto begin = static_cast<std::size_t>(session_.off);
        std::copy(data.begin(), data.end(), slot.begin() + static_cast<std::ptrdiff_t>(begin));
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

std::size_t ServerSimulator::next_boot_slot(std::uint32_t image) const noexcept
{
    return images_[image].swap == SwapType::None ? 0U : 1U;
}

void ServerSimulator::advance_applies()
{
    // The device-committed image's MCU finishes its update in the background;
    // each state read is one step of "a while" (docs/multi-image.md).
    for (ImagePair& pair : images_) {
        if (pair.committing.has_value()) {
            if (*pair.committing > 0) {
                --*pair.committing;
                continue;
            }
            // Committed: the other MCU's trial becomes permanent.
            pair.committing.reset();
            pair.swap = SwapType::None;
            continue;
        }
        if (!pair.applying.has_value()) {
            continue;
        }
        if (*pair.applying > 0) {
            --*pair.applying;
            continue;
        }
        pair.applying.reset();
        if (pair.device_outcome == ApplyOutcome::Applied) {
            // The other MCU runs it, on trial: slot 0 new and unconfirmed,
            // nothing pending.
            std::swap(pair.slots[0], pair.slots[1]);
            pair.swap = SwapType::Revert;
        } else {
            // Failed: the old image stays in slot 0, and the new one sits in
            // slot 1 with nothing pending -- the contract's failure.
            pair.swap = SwapType::None;
        }
    }
    // Nothing will confirm image 0 if it is not on trial, so the device
    // commits at once (ADR-0022).
    if (images_[0].swap != SwapType::Revert) {
        start_device_commits();
    }
}

std::vector<std::byte> ServerSimulator::handle_state_read()
{
    advance_applies();
    return encode_state();
}

std::vector<std::byte> ServerSimulator::encode_state() const
{
    struct Entry
    {
        std::uint32_t image;
        std::size_t slot;
        SlotImage content;
        bool pending;
        bool confirmed;
        bool active;
        bool permanent;
    };

    std::vector<Entry> entries;
    for (std::uint32_t image = 0; image < images_.size(); ++image) {
        // The flag table from img_mgmt_state_read() (S14, S35), which derives
        // every flag from the swap type rather than storing it per slot, and
        // flags each image's primary slot active.
        const ImagePair& pair = images_[image];
        const bool active_confirmed = pair.swap != SwapType::Revert;
        bool other_pending = false;
        bool other_permanent = false;
        bool other_confirmed = false;
        if (next_boot_slot(image) != 0) {
            switch (pair.swap) {
            case SwapType::Perm:
                other_pending = true;
                other_permanent = true;
                break;
            case SwapType::Revert:
                other_confirmed = true;
                break;
            case SwapType::Test:
                other_pending = true;
                break;
            case SwapType::None:
                break;
            }
        }

        for (std::size_t slot = 0; slot < pair.slots.size(); ++slot) {
            // A slot whose content is not a valid image is skipped silently,
            // which is why an empty images array is normal rather than an
            // error.
            std::optional<SlotImage> described = describe(ConstBytes{pair.slots[slot]});
            if (!described.has_value()) {
                continue;
            }
            const bool is_active = slot == 0;
            entries.push_back(Entry{image, slot, std::move(*described),
                                    is_active ? false : other_pending,
                                    is_active ? active_confirmed : other_confirmed, is_active,
                                    is_active ? false : other_permanent});
        }
    }

    // Zephyr omits "image" only when the device has one updatable image.
    const bool with_image = !config_.single_image || images_.size() > 1;

    tcbor::Writer out;
    out.map(2).text("images").array(entries.size());
    for (const Entry& entry : entries) {
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

ImageError ServerSimulator::set_next_boot_slot(std::size_t global_slot, bool confirm)
{
    const auto image = static_cast<std::uint32_t>(global_slot / 2);
    const std::size_t slot = global_slot % 2;
    const std::size_t active = 0;
    const std::size_t next = next_boot_slot(image);
    ImagePair& pair = images_[image];

    // img_mgmt_set_next_boot_slot() (S35), in its own order. First: confirming
    // an image that is not the running one is denied unless a Kconfig allows
    // it -- for any slot, or only for the secondary.
    const bool running_image = image == 0;
    if (confirm && !running_image && !config_.allow_confirm_non_active_image_any &&
        (!config_.allow_confirm_non_active_image_secondary || slot == active)) {
        return ImageError::ImageConfirmationDenied;
    }
    // Then, for every image: confirming a slot that is not the active one.
    if (confirm && slot != active && !config_.allow_confirm_non_active_slot) {
        return ImageError::ImageConfirmationDenied;
    }
    if (!confirm && slot == active) {
        return ImageError::ImageSettingTestToActiveDenied;
    }

    switch (pair.swap) {
    case SwapType::Test:
        if (!confirm && slot == next) {
            return ImageError::Ok; // Already set for test: nothing to do.
        }
        return ImageError::ImageAlreadyPending;

    case SwapType::None:
    case SwapType::Perm:
        if (confirm && slot == next) {
            return ImageError::Ok;
        }
        if ((slot == active && active != next) || (!confirm && slot != active && slot == next)) {
            return ImageError::ImageAlreadyPending;
        }
        break;

    case SwapType::Revert:
        if (!confirm) {
            return ImageError::ImageAlreadyPending;
        }
        break;
    }

    // boot_set_next(): confirming the running slot clears a pending revert;
    // marking the other slot schedules a test, or a permanent swap when it is
    // confirmed at the same time.
    if (slot == active) {
        if (pair.swap == SwapType::Revert) {
            pair.swap = SwapType::None;
            pair.applying.reset();
            pair.committing.reset();
            if (image == 0) {
                // The coordinating MCU's confirm hook: now commit the images
                // it applied to the other MCU (protocol-notes S39).
                start_device_commits();
            }
        }
    } else if (pair.swap == SwapType::None) {
        pair.swap = confirm ? SwapType::Perm : SwapType::Test;
    }
    return ImageError::Ok;
}

std::vector<std::byte> ServerSimulator::handle_state_write(Version version, ConstBytes payload)
{
    const std::optional<tcbor::Value> request = tcbor::parse(payload);
    if (!request.has_value() || !request->is(tcbor::Value::Kind::Map)) {
        return image_failure(version, ImageError::Unknown);
    }

    const bool confirm = request->get_bool("confirm").value_or(false);
    const tcbor::Value* hash_field = request->find("hash");

    // A hashless confirm names the RUNNING image's active slot, never any
    // other image's (S35) -- global slot 0 here.
    std::size_t slot = 0;
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
        // img_mgmt_find_by_hash() searches every image's slots.
        std::optional<std::size_t> found;
        for (std::size_t global = 0; global < images_.size() * 2 && !found.has_value(); ++global) {
            const std::optional<SlotImage> described = describe(slot_content(global));
            if (described.has_value() && described->hash == hash_field->bytes) {
                found = global;
            }
        }
        if (!found.has_value()) {
            return image_failure(version, ImageError::HashNotFound);
        }
        slot = *found;
    }

    const ImageError result = set_next_boot_slot(slot, confirm);
    if (result != ImageError::Ok) {
        return image_failure(version, result);
    }
    return encode_state();
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
    if (slot >= images_.size() * 2) {
        return image_failure(version, ImageError::InvalidSlot);
    }
    const auto image = static_cast<std::uint32_t>(slot / 2);
    if (slot % 2 == next_boot_slot(image) && next_boot_slot(image) != 0) {
        // A slot already marked for the next boot cannot be erased.
        return image_failure(version, ImageError::NoFreeSlot);
    }

    images_[image].slots[slot % 2 == 0 ? 0U : 1U].clear();
    session_ = Session{};

    tcbor::Writer out;
    out.map(0);
    return out.bytes();
}

std::vector<std::byte> ServerSimulator::handle_slot_info() const
{
    const std::uint32_t capacity = config_.slot_size != 0 ? config_.slot_size : kDefaultSlotSize;

    tcbor::Writer out;
    out.map(1).text("images").array(images_.size());
    for (std::uint32_t image = 0; image < images_.size(); ++image) {
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
