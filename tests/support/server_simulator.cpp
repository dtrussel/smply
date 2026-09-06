// SPDX-License-Identifier: Apache-2.0

#include "server_simulator.hpp"

#include "message_builder.hpp"
#include "test_cbor.hpp"

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
    : transport_{transport}, config_{config}
{}

void ServerSimulator::load_slot(std::size_t slot, std::vector<std::byte> content)
{
    assert(slot < slots_.size());
    slots_[slot] = std::move(content);
}

ConstBytes ServerSimulator::slot_content(std::size_t slot) const
{
    assert(slot < slots_.size());
    return ConstBytes{slots_[slot]};
}

void ServerSimulator::reboot()
{
    switch (swap_) {
    case SwapType::None:
        break;
    case SwapType::Test:
        std::swap(slots_[0], slots_[1]);
        swap_ = SwapType::Revert;
        break;
    case SwapType::Perm:
    case SwapType::Revert:
        // Two different intentions with the same effect: `Perm` swaps the new
        // image in for good, `Revert` swaps the old one back after an
        // unconfirmed trial. Either way the contents exchange and nothing is
        // left scheduled.
        std::swap(slots_[0], slots_[1]);
        swap_ = SwapType::None;
        break;
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

        // This device has one image pair, so any image number but zero has no
        // slot to upload to -- which is the same answer a real server gives
        // when img_mgmt_get_unused_slot_area_id() finds none. Refusing it
        // beats writing slot 1 regardless and pretending the field was
        // honoured.
        if (request->get_uint("image").value_or(0) != 0) {
            return image_failure(header.version, ImageError::NoFreeSlot);
        }

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
                sha256_of(ConstBytes{slots_[1]}) == sha) {
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
            session_.slot = 1;
            session_.size = *size;
            session_.sha = sha;
            // The implicit erase of the target slot (rule 12). The size is
            // bounded by the capacity check above, so a device-supplied `len`
            // never sizes an unbounded allocation -- the same rule the library
            // works under.
            slots_[1].assign(static_cast<std::size_t>(*size), std::byte{0xFF});
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
        auto& slot = slots_[session_.slot];
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
        match = sha256_of(ConstBytes{slots_[session_.slot]}) == padded;
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

std::size_t ServerSimulator::next_boot_slot() const noexcept
{
    return swap_ == SwapType::None ? 0U : 1U;
}

std::vector<std::byte> ServerSimulator::handle_state_read() const
{
    // The flag table from img_mgmt_state_read() (S14), which derives every flag
    // from the swap type rather than storing it per slot.
    const std::size_t next = next_boot_slot();
    const bool active_confirmed = swap_ != SwapType::Revert;
    bool other_pending = false;
    bool other_permanent = false;
    bool other_confirmed = false;
    if (next != 0) {
        switch (swap_) {
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

    struct Entry
    {
        std::size_t slot;
        SlotImage image;
        bool pending;
        bool confirmed;
        bool active;
        bool permanent;
    };

    std::vector<Entry> entries;
    for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
        // A slot whose content is not a valid image is skipped silently, which
        // is why an empty images array is normal rather than an error.
        std::optional<SlotImage> described = describe(ConstBytes{slots_[slot]});
        if (!described.has_value()) {
            continue;
        }
        const bool is_active = slot == 0;
        entries.push_back(Entry{slot, std::move(*described), is_active ? false : other_pending,
                                is_active ? active_confirmed : other_confirmed, is_active,
                                is_active ? false : other_permanent});
    }

    tcbor::Writer out;
    out.map(2).text("images").array(entries.size());
    for (const Entry& entry : entries) {
        // slot, version, hash, bootable, pending, confirmed, active,
        // permanent -- plus the image number when the device has more
        // than one.
        const std::uint64_t fields = config_.single_image ? 8U : 9U;
        out.map(fields);
        if (!config_.single_image) {
            out.text("image").uint(0);
        }
        out.text("slot").uint(entry.slot);
        out.text("version").text(entry.image.version);
        out.text("hash").blob(ConstBytes{entry.image.hash});
        out.text("bootable").boolean(entry.image.bootable);
        out.text("pending").boolean(entry.pending);
        out.text("confirmed").boolean(entry.confirmed);
        out.text("active").boolean(entry.active);
        out.text("permanent").boolean(entry.permanent);
    }
    out.text("splitStatus").uint(0);
    return out.bytes();
}

ImageError ServerSimulator::set_next_boot_slot(std::size_t slot, bool confirm)
{
    const std::size_t active = 0;
    const std::size_t next = next_boot_slot();

    // Confirming a slot that is not the running one is denied by default; the
    // Kconfig that allows it is off in an ordinary build (S14).
    if (confirm && slot != active) {
        return ImageError::ImageConfirmationDenied;
    }
    if (!confirm && slot == active) {
        return ImageError::ImageSettingTestToActiveDenied;
    }

    switch (swap_) {
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
        if (swap_ == SwapType::Revert) {
            swap_ = SwapType::None;
        }
    } else if (swap_ == SwapType::None) {
        swap_ = confirm ? SwapType::Perm : SwapType::Test;
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
        std::optional<std::size_t> found;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const std::optional<SlotImage> described = describe(ConstBytes{slots_[index]});
            if (described.has_value() && described->hash == hash_field->bytes) {
                found = index;
                break;
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
    return handle_state_read();
}

std::vector<std::byte> ServerSimulator::handle_erase(Version version, ConstBytes payload)
{
    const std::optional<tcbor::Value> request = tcbor::parse(payload);
    if (!request.has_value() || !request->is(tcbor::Value::Kind::Map)) {
        return image_failure(version, ImageError::Unknown);
    }

    // The default is the slot opposite the active one, not the constant 1 --
    // which happens to be 1 here, but the rule is what is modelled.
    const std::uint64_t slot = request->get_uint("slot").value_or(1);
    if (slot >= slots_.size()) {
        return image_failure(version, ImageError::InvalidSlot);
    }
    if (slot == next_boot_slot() && next_boot_slot() != 0) {
        // A slot already marked for the next boot cannot be erased.
        return image_failure(version, ImageError::NoFreeSlot);
    }

    slots_[static_cast<std::size_t>(slot)].clear();
    session_ = Session{};

    tcbor::Writer out;
    out.map(0);
    return out.bytes();
}

std::vector<std::byte> ServerSimulator::handle_slot_info() const
{
    const std::uint32_t capacity = config_.slot_size != 0 ? config_.slot_size : kDefaultSlotSize;

    tcbor::Writer out;
    out.map(1).text("images").array(1);
    out.map(3);
    out.text("image").uint(0);
    out.text("slots").array(slots_.size());
    for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
        out.map(2).text("slot").uint(slot).text("size").uint(capacity);
    }
    out.text("max_image_size").uint(capacity);
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
