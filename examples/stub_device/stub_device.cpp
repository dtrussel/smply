// SPDX-License-Identifier: Apache-2.0

#include "stub_device/stub_device.hpp"

#include "stub_device/device_link.hpp"

#include "boot_model/boot_model.hpp"
#include "minicbor/minicbor.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/group.hpp"
#include "smply/groups/image.hpp"
#include "smply/result.hpp"
#include "smply/smp/header.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace smply::example {
namespace {

namespace cbor = smply::minicbor;

constexpr std::uint8_t kOsReset = 5;
constexpr std::uint8_t kOsMcumgrParams = 6;
constexpr std::uint8_t kOsBootloaderInfo = 8;
constexpr std::uint8_t kImageState = 0;
constexpr std::uint8_t kImageUpload = 1;

constexpr std::uint32_t kBufCount = 2;

/// An MCUboot image header's size (protocol-notes section 7): a first chunk
/// shorter than this cannot start an image.
constexpr std::size_t kImageHeaderSize = 32;

/// What one slot holds. A declared upload larger than this is refused before
/// the slot is erased to its size, so a received `len` never sizes an
/// unbounded allocation. The simulator's default capacity.
constexpr std::size_t kSlotCapacity = std::size_t{512} * 1024U;

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

/// An image-group error, group-scoped in either SMP version: the shape of a
/// server without `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL`, which keeps
/// the device's own reason (protocol-notes A16).
[[nodiscard]] std::vector<std::byte> image_error_map(ImageError code)
{
    cbor::Writer out;
    out.map(1).text("err").map(2);
    out.text("group").uint(static_cast<std::uint64_t>(Group::Image));
    out.text("rc").uint(static_cast<std::uint64_t>(code));
    return out.bytes();
}

/// The image-state listing, as the boot model reports it.
[[nodiscard]] std::vector<std::byte> encode_state(const std::vector<boot_model::SlotEntry>& slots)
{
    cbor::Writer out;
    out.map(1).text("images").array(slots.size());
    for (const boot_model::SlotEntry& entry : slots) {
        const bool has_hash = !entry.content.hash.empty();
        // Nine pairs with a hash, eight without. Count them against the block
        // below before changing either: a definite-length CBOR map that lies
        // about its size decodes as garbage from that point on, and the error
        // surfaces somewhere else entirely ("array element not a map").
        out.map(has_hash ? 9 : 8);
        out.text("image").uint(entry.image);
        out.text("slot").uint(entry.slot);
        out.text("version").text(entry.content.version);
        if (has_hash) {
            out.text("hash").blob(ConstBytes{entry.content.hash});
        }
        out.text("bootable").boolean(entry.content.bootable);
        out.text("pending").boolean(entry.pending);
        out.text("confirmed").boolean(entry.confirmed);
        out.text("active").boolean(entry.active);
        out.text("permanent").boolean(entry.permanent);
    }
    return out.bytes();
}

/// The device's build: one image, or two with image 1 committed by the device
/// or by the client. The three A27 Kconfigs stay off, as in Zephyr.
[[nodiscard]] boot_model::BootConfig boot_config(const std::optional<SecondImage>& second)
{
    boot_model::BootConfig config;
    if (!second.has_value()) {
        return config;
    }
    config.image_count = 2;
    if (second->device_commits) {
        config.device_commits = {std::nullopt, boot_model::DeviceCommit{
                                                   .outcome = second->outcome,
                                                   .apply_reads = second->apply_reads,
                                               }};
    }
    return config;
}

} // namespace

StubDevice::StubDevice(std::vector<std::byte> primary, std::optional<SecondImage> second,
                       std::optional<std::int64_t> bootloader_mode)
    : model_{boot_config(second)}, bootloader_mode_{bootloader_mode}
{
    // Global slots, as Zephyr numbers them: image n's primary is slot 2n.
    model_.load_slot(0, std::move(primary));
    if (second.has_value()) {
        model_.load_slot(2, std::move(second->running));
    }
    thread_ = std::thread{[this] { run(); }};
}

StubDevice::~StubDevice()
{
    stop();
}

void StubDevice::stop() noexcept
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        stop_ = true;
    }
    work_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void StubDevice::attach(DeviceLink& link)
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

void StubDevice::reboot()
{
    // MCUboot runs every image's swap, and the device starts applying an image
    // it commits itself: the boot model's reboot.
    model_.reboot();

    // A reboot forgets any upload session, which is the `area_id == -1` case in
    // protocol-notes section 6 rule 5.
    upload_slot_.reset();
    received_ = 0;
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
        if (header->command == kOsBootloaderInfo && bootloader_mode_.has_value()) {
            cbor::Writer out;
            const bool asks_mode = request.has_value() && request->find("query") != nullptr;
            if (!asks_mode) {
                out.map(1).text("bootloader").text("MCUboot");
            } else if (*bootloader_mode_ < 0) {
                out.map(1).text("mode").nint(*bootloader_mode_);
            } else {
                out.map(1).text("mode").uint(static_cast<std::uint64_t>(*bootloader_mode_));
            }
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
        // Each state read is "a while" passing for an image the device
        // commits itself (ADR-0022).
        return respond(*header, ConstBytes{encode_state(model_.read_state())});
    }

    if (header->command == kImageState && header->op == Operation::Write) {
        if (!request.has_value()) {
            return respond(*header, ConstBytes{error_map(3)}); // MGMT_ERR_EINVAL
        }
        const bool confirm = request->get_bool("confirm").value_or(false);
        const cbor::Value* hash = request->find("hash");

        // The hash's encoding is the wire's to check; everything after it --
        // which slot it names, in any image, and whether the device lets it be
        // marked or confirmed (S14, S35, A27) -- is the boot model's.
        std::optional<ConstBytes> named;
        if (hash != nullptr) {
            if (!hash->is(cbor::Value::Kind::Bytes)) {
                return respond(*header, ConstBytes{error_map(3)});
            }
            named = ConstBytes{hash->bytes};
        } else if (!confirm) {
            return respond(*header, ConstBytes{error_map(3)}); // a test names no image
        }
        if (const ImageError result = model_.set_state(named, confirm); result != ImageError::Ok) {
            return respond(*header, ConstBytes{image_error_map(result)});
        }
        return respond(*header, ConstBytes{encode_state(model_.listing())});
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
            if (data->bytes.size() < kImageHeaderSize) {
                return respond(*header, ConstBytes{error_map(3)});
            }
            // Read on first packets only, where it picks the image whose
            // secondary slot receives the upload (protocol-notes section 6,
            // S36). Whether the bytes are an image is the model's reader's
            // call once they are in flash: one that is not is never listed.
            const std::optional<std::size_t> slot =
                model_.upload_target(request->get_uint("image").value_or(0));
            if (!slot.has_value()) {
                return respond(*header, ConstBytes{image_error_map(ImageError::NoFreeSlot)});
            }
            if (*len > kSlotCapacity) {
                return respond(*header,
                               ConstBytes{image_error_map(ImageError::InvalidImageTooLarge)});
            }
            // The first chunk erases the slot it is about to fill.
            model_.begin_upload(*slot, static_cast<std::size_t>(*len));
            upload_slot_ = slot;
            declared_size_ = static_cast<std::size_t>(*len);
            received_ = 0;
        }

        // Offset mismatch is not an error: drop the data and answer with the
        // offset actually held (protocol-notes section 6, rule 5). smply's
        // upload machine is built on this being authoritative, so it is worth
        // getting right even in a stub.
        if (!upload_slot_.has_value() || *off != received_) {
            cbor::Writer out;
            out.map(1).text("off").uint(received_);
            return respond(*header, ConstBytes{out.bytes()});
        }

        if (!model_.write(*upload_slot_, received_, ConstBytes{data->bytes})) {
            return respond(*header,
                           ConstBytes{image_error_map(ImageError::InvalidImageDataOverrun)});
        }
        received_ += data->bytes.size();
        const bool complete = received_ == declared_size_;
        if (complete) {
            // The session ends with the image: a repeat of the last chunk is
            // answered from zero, as rule 9b says.
            upload_slot_.reset();
            received_ = 0;
        }

        cbor::Writer out;
        // No "match": this device is built without CONFIG_IMG_ENABLE_IMAGE_CHECK
        // (A6). Absence is a successful answer, not a missing one.
        out.map(1).text("off").uint(complete ? declared_size_ : received_);
        return respond(*header, ConstBytes{out.bytes()});
    }

    return respond(*header, ConstBytes{error_map(8)});
}

void StubDevice::run()
{
    while (true) {
        std::vector<std::byte> request;
        DeviceLink* link = nullptr;
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
            link->deliver(*reply);
        }

        if (!reboot_pending_) {
            continue;
        }
        reboot_pending_ = false;

        // The reset was accepted and answered. Now the device reboots: a link
        // that a reset drops goes down here, and the application's job begins.
        link->device_resetting(Error{ErrorCode::Disconnected, "device rebooting"});

        // Forget the dead link and everything queued for it **before** the
        // reboot delay, not after. The application reconnects as soon as it
        // sees the disconnect, which is while this thread is still sleeping --
        // so clearing `link_` afterwards would wipe the link it had just
        // attached, and clearing the inbox afterwards would swallow the first
        // request on it. Requests that arrive during the sleep are answered
        // after the swap, which is exactly what a booting device does. A link
        // that survives the reset -- the device's own serial port -- is kept.
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (!link->survives_reset()) {
                link_ = nullptr;
            }
            inbox_.clear();
        }

        std::this_thread::sleep_for(kRebootDuration);
        reboot();

        // Whatever link is attached now hears the device come up. For a
        // serial port that is where the boot banners appear.
        DeviceLink* booted = nullptr;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            booted = link_;
        }
        if (booted != nullptr) {
            booted->device_booted();
        }
    }
}

} // namespace smply::example
