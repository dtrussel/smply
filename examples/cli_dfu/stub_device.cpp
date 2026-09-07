// SPDX-License-Identifier: Apache-2.0

#include "stub_device.hpp"

#include "loopback_transport.hpp"

#include "minicbor/minicbor.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/group.hpp"
#include "smply/image_source.hpp"
#include "smply/mcuboot_image.hpp"
#include "smply/result.hpp"
#include "smply/smp/header.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace smply::example {
namespace {

namespace cbor = smply::minicbor;

constexpr std::uint8_t kOsReset = 5;
constexpr std::uint8_t kOsMcumgrParams = 6;
constexpr std::uint8_t kImageState = 0;
constexpr std::uint8_t kImageUpload = 1;

/// The device's whole-SMP-message budget, reported through mcumgr params.
constexpr std::uint32_t kBufSize = 512;
constexpr std::uint32_t kBufCount = 2;

/// How long the device pretends a reboot takes. Long enough that the
/// application really does have to wait and reconnect, short enough that CI
/// does not notice.
constexpr auto kRebootDuration = std::chrono::milliseconds{150};

/// Builds a response message: header addressed to `request`, plus `payload`.
[[nodiscard]] std::vector<std::byte> respond(const Header& request, ConstBytes payload)
{
    const Header reply{.op = response_to(request.op),
                       .version = request.version,
                       .flags = 0,
                       .length = static_cast<std::uint16_t>(payload.size()),
                       .group = request.group,
                       .seq = request.seq,
                       .command = request.command};

    const std::array<std::byte, kHeaderSize> head = encode(reply);
    std::vector<std::byte> out;
    out.reserve(head.size() + payload.size());
    out.insert(out.end(), head.begin(), head.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// The empty CBOR map, which is what a bare success carries.
[[nodiscard]] std::vector<std::byte> empty_map()
{
    cbor::Writer out;
    out.map(0);
    return out.bytes();
}

/// An SMP v1 error: `{"rc": code}`.
[[nodiscard]] std::vector<std::byte> error_map(std::uint64_t code)
{
    cbor::Writer out;
    out.map(1).text("rc").uint(code);
    return out.bytes();
}

} // namespace

StubDevice::StubDevice(std::vector<std::byte> primary)
{
    slots_[0].content = std::move(primary);
    describe(slots_[0]);
    thread_ = std::thread{[this] { run(); }};
}

StubDevice::~StubDevice()
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        stop_ = true;
    }
    work_.notify_all();
    thread_.join();
}

void StubDevice::attach(LoopbackTransport& link)
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        link_ = &link;
    }
    work_.notify_all();
}

void StubDevice::submit(std::vector<std::byte> request)
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        inbox_.push_back(std::move(request));
    }
    work_.notify_all();
}

void StubDevice::describe(Slot& slot)
{
    slot.version.clear();
    slot.hash.reset();
    if (slot.content.empty()) {
        return;
    }

    // Read with smply's *public* image API. A device would read its own flash;
    // reading the bytes it was given is the same thing, and it keeps this file
    // clear of src/image/.
    const ConstBytes bytes{slot.content};
    const Result<McubootImageInfo> info = parse_mcuboot_header(bytes.first(kMcubootHeaderSize));
    if (!info.has_value()) {
        // A slot whose header will not parse is skipped in the state response,
        // which is what a real server does with an image it cannot validate
        // (protocol-notes section 6).
        slot.content.clear();
        return;
    }

    slot.version = std::to_string(info->version.major) + '.' + std::to_string(info->version.minor) +
                   '.' + std::to_string(info->version.revision);
    if (info->version.build != 0) {
        slot.version += '.' + std::to_string(info->version.build);
    }

    MemoryImageSource source{bytes};
    if (const Result<std::optional<ImageHash>> found = find_image_tlv_hash(source, *info);
        found.has_value()) {
        slot.hash = *found;
    }
}

std::vector<std::byte> StubDevice::encode_state() const
{
    // Flags are *derived from the swap type*, not stored (protocol-notes
    // section 7). After a test swap the running image reports `active` with no
    // `confirmed`, and the fallback slot reports `confirmed` -- which reads
    // backwards until you know it is the fallback being described.
    const bool trial = swap_ == SwapType::Revert;

    std::size_t present = 0;
    for (const Slot& slot : slots_) {
        if (!slot.empty()) {
            ++present;
        }
    }

    cbor::Writer out;
    out.map(1).text("images").array(present);
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (slot.empty()) {
            continue;
        }
        const bool primary = index == 0;
        const bool pending = !primary && swap_ == SwapType::Test;

        // Nine pairs with a hash, eight without. Count them against the block
        // below before changing either: a definite-length CBOR map that lies
        // about its size decodes as garbage from that point on, and the error
        // surfaces somewhere else entirely ("array element not a map"). P11 hit
        // this exact off-by-one in the simulator.
        out.map(slot.hash.has_value() ? 9 : 8);
        out.text("image").uint(0);
        out.text("slot").uint(index);
        out.text("version").text(slot.version);
        if (slot.hash.has_value()) {
            out.text("hash").blob(slot.hash->bytes());
        }
        out.text("bootable").boolean(true);
        out.text("pending").boolean(pending);
        out.text("confirmed").boolean(primary ? !trial : trial);
        out.text("active").boolean(primary);
        out.text("permanent").boolean(false);
    }
    return out.bytes();
}

void StubDevice::reboot()
{
    switch (swap_) {
    case SwapType::None:
        break;
    case SwapType::Test:
        std::swap(slots_[0], slots_[1]);
        swap_ = SwapType::Revert; // unconfirmed: the next reset undoes it
        break;
    case SwapType::Perm:
    case SwapType::Revert:
        // Identical *here* and not in general: Perm installs the secondary for
        // good, Revert puts the original back. Both are "swap the slots and
        // stop swapping", because this device holds two slots and models no
        // difference between them. A real bootloader distinguishes them by
        // which trailer it writes.
        std::swap(slots_[0], slots_[1]);
        swap_ = SwapType::None;
        break;
    }
    // A reboot forgets any upload session, which is the `area_id == -1` case in
    // protocol-notes section 6 rule 5.
    staging_.clear();
    declared_size_ = 0;
}

std::optional<std::vector<std::byte>> StubDevice::answer(const std::vector<std::byte>& raw)
{
    const Result<Header> header = decode_header(ConstBytes{raw});
    if (!header.has_value() || raw.size() < kHeaderSize + header->length) {
        return std::nullopt; // not addressed to anything this device understands
    }
    const ConstBytes payload = ConstBytes{raw}.subspan(kHeaderSize, header->length);
    const std::optional<cbor::Value> request = cbor::parse(payload);

    if (header->group == Group::Os) {
        if (header->command == kOsMcumgrParams) {
            cbor::Writer out;
            out.map(2).text("buf_size").uint(kBufSize).text("buf_count").uint(kBufCount);
            return respond(*header, ConstBytes{out.bytes()});
        }
        if (header->command == kOsReset) {
            // Acceptance, not completion: answer first, then go down
            // (protocol-notes section 5).
            reboot_pending_ = true;
            return respond(*header, ConstBytes{empty_map()});
        }
        return respond(*header, ConstBytes{error_map(8)}); // MGMT_ERR_ENOTSUP
    }

    if (header->group != Group::Image) {
        return respond(*header, ConstBytes{error_map(8)});
    }

    if (header->command == kImageState && header->op == Operation::Read) {
        return respond(*header, ConstBytes{encode_state()});
    }

    if (header->command == kImageState && header->op == Operation::Write) {
        if (!request.has_value()) {
            return respond(*header, ConstBytes{error_map(3)}); // MGMT_ERR_EINVAL
        }
        const bool confirm = request->get_bool("confirm").value_or(false);
        const cbor::Value* hash = request->find("hash");

        if (confirm && hash == nullptr) {
            // Confirm the running image: the trial, if any, is now permanent.
            swap_ = SwapType::None;
            return respond(*header, ConstBytes{encode_state()});
        }
        if (hash == nullptr || !hash->is(cbor::Value::Kind::Bytes)) {
            return respond(*header, ConstBytes{error_map(3)});
        }
        // Mark the slot holding that hash for the next boot.
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const Slot& slot = slots_[index];
            if (!slot.hash.has_value() ||
                !std::equal(hash->bytes.begin(), hash->bytes.end(), slot.hash->bytes().begin(),
                            slot.hash->bytes().end())) {
                continue;
            }
            if (index == 0) {
                // Already running it. A real device refuses to confirm a slot
                // that is not the running one, and this is the mirror case.
                swap_ = confirm ? SwapType::None : swap_;
                return respond(*header, ConstBytes{encode_state()});
            }
            swap_ = confirm ? SwapType::Perm : SwapType::Test;
            return respond(*header, ConstBytes{encode_state()});
        }
        return respond(*header, ConstBytes{error_map(3)});
    }

    if (header->command == kImageUpload && header->op == Operation::Write) {
        if (!request.has_value()) {
            return respond(*header, ConstBytes{error_map(3)});
        }
        const std::optional<std::uint64_t> off = request->get_uint("off");
        const cbor::Value* data = request->find("data");
        if (!off.has_value() || data == nullptr || !data->is(cbor::Value::Kind::Bytes)) {
            return respond(*header, ConstBytes{error_map(3)});
        }

        if (*off == 0) {
            const std::optional<std::uint64_t> len = request->get_uint("len");
            if (!len.has_value()) {
                return respond(*header, ConstBytes{error_map(3)}); // INVALID_LENGTH
            }
            if (data->bytes.size() < kMcubootHeaderSize) {
                return respond(*header, ConstBytes{error_map(3)});
            }
            if (!parse_mcuboot_header(ConstBytes{data->bytes}.first(kMcubootHeaderSize))
                     .has_value()) {
                return respond(*header, ConstBytes{error_map(3)}); // bad magic
            }
            declared_size_ = *len;
            staging_.clear();
        }

        // Offset mismatch is not an error: drop the data and answer with the
        // offset actually held (protocol-notes section 6, rule 5). smply's
        // upload machine is built on this being authoritative, so it is worth
        // getting right even in a stub.
        if (*off != staging_.size()) {
            cbor::Writer out;
            out.map(1).text("off").uint(staging_.size());
            return respond(*header, ConstBytes{out.bytes()});
        }

        staging_.insert(staging_.end(), data->bytes.begin(), data->bytes.end());
        if (staging_.size() >= declared_size_ && declared_size_ != 0) {
            slots_[1].content = staging_;
            describe(slots_[1]);
            staging_.clear();
        }

        cbor::Writer out;
        // No "match": this device is built without CONFIG_IMG_ENABLE_IMAGE_CHECK
        // (A6). Absence is a successful answer, not a missing one.
        out.map(1).text("off").uint(slots_[1].empty() ? staging_.size() : declared_size_);
        return respond(*header, ConstBytes{out.bytes()});
    }

    return respond(*header, ConstBytes{error_map(8)});
}

void StubDevice::run()
{
    while (true) {
        std::vector<std::byte> request;
        LoopbackTransport* link = nullptr;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            work_.wait(lock, [this] { return stop_ || !inbox_.empty(); });
            if (stop_) {
                return;
            }
            request = std::move(inbox_.front());
            inbox_.pop_front();
            link = link_;
        }

        if (link == nullptr) {
            continue; // arrived on a link that has already gone
        }

        const std::optional<std::vector<std::byte>> reply = answer(request);
        if (reply.has_value()) {
            link->deliver_from_device(*reply);
        }

        if (!reboot_pending_) {
            continue;
        }
        reboot_pending_ = false;

        // The reset was accepted and answered. Now the link drops, the device
        // reboots, and the application's job begins.
        link->drop_from_device(Error{ErrorCode::Disconnected, "device rebooting"});

        // Forget the dead link and everything queued for it **before** the
        // reboot delay, not after. The application reconnects as soon as it
        // sees the disconnect, which is while this thread is still sleeping --
        // so clearing `link_` afterwards would wipe the link it had just
        // attached, and clearing the inbox afterwards would swallow the first
        // request on it. Requests that arrive during the sleep are answered
        // after the swap, which is exactly what a booting device does.
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            link_ = nullptr;
            inbox_.clear();
        }

        std::this_thread::sleep_for(kRebootDuration);
        reboot();
    }
}

} // namespace smply::example
