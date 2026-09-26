// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_STUB_DEVICE_HPP
#define SMPLY_EXAMPLE_STUB_DEVICE_HPP

/// \file
/// A pretend Zephyr device, on its own thread.
///
/// **This is not a protocol reference, and it must not become one.**
/// `tests/support/server_simulator.*` is the thing the test suites trust: it
/// models the awkward parts -- offset correction in both directions, session
/// resume by `sha`, the `off == 0` restart, `match` on the final chunk, the
/// whole swap and revert bookkeeping -- and it is anchored by 74 component
/// tests. This answers the five commands one clean update needs, and stops.
/// If the two ever disagree, the simulator is right. Do not reconcile them by
/// growing this one; a device worth trusting is a test double, and belongs
/// under `tests/`.
///
/// The one extension is a **second image that the device commits itself**
/// (`SecondImage`), because the multi-image examples need a device to take a
/// package (ADR-0021). It is the simulator's device-committed mode cut down to
/// the happy path and one failure: the same five commands, with an `image`
/// number on uploads and in the listing, and an apply that finishes after a
/// few state reads. It does not model the confirm-denial rules for image 1
/// (protocol-notes A27); the simulator does.
///
/// What it *is* for is the thread. A single-threaded example would demonstrate
/// nothing about the arrangement that actually catches people out -- a driver
/// delivering on a thread the library does not own -- and would leave
/// `smply::Dispatcher` with no caller anywhere in the tree.
///
/// It is built without `CONFIG_IMG_ENABLE_IMAGE_CHECK`, so it never sends
/// `match` (docs/protocol-notes.md section 9, A6: absence is not an error). That
/// is not laziness dressed up: it is what keeps this file off the whole-file
/// hash path, and therefore out of smply's private headers, which is what lets
/// an example use it at all.

#include "stub_device/device_link.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace smply::example {

/// What the bootloader will do on the next reset (protocol-notes section 7).
enum class SwapType : std::uint8_t
{
    None,  ///< Boot the primary slot again.
    Test,  ///< Swap in the secondary for a trial; revert unless confirmed.
    Perm,  ///< Swap in the secondary permanently.
    Revert ///< A trial was not confirmed: swap back.
};

/// How a device-committed image's apply ends (docs/multi-image.md).
enum class ApplyOutcome : std::uint8_t
{
    Applied, ///< The second MCU committed it: slot 0 new, confirmed.
    Failed,  ///< It could not: slot 0 back on the old image, nothing pending.
};

/// A second image, which the device commits itself: the staged firmware of
/// another MCU, as an STM32H5 holds a BLE module's (ADR-0021). After the reset
/// the device takes `apply_reads` image-state reads to apply it, then reports
/// `outcome`: the new image running on trial, or the old one still there. It
/// commits an applied image a read after image 0 is confirmed, or at once if
/// image 0 is not on trial (ADR-0022).
struct SecondImage
{
    /// What image 1 runs now. May be empty: nothing applied yet.
    std::vector<std::byte> running;
    ApplyOutcome outcome = ApplyOutcome::Applied;
    unsigned apply_reads = 3;
};

/// One flash slot.
struct Slot
{
    std::vector<std::byte> content;
    std::string version;
    std::optional<ImageHash> hash;

    [[nodiscard]] bool empty() const noexcept
    {
        return content.empty();
    }
};

/// A device with one image and two slots -- or two images, the second
/// committed by the device -- answering on a background thread.
class StubDevice
{
public:
    /// \param primary The image the device is already running.
    /// \param second  An image 1 the device commits itself, or nothing for the
    ///                ordinary one-image device.
    explicit StubDevice(std::vector<std::byte> primary,
                        std::optional<SecondImage> second = std::nullopt);

    StubDevice(const StubDevice&) = delete;
    StubDevice(StubDevice&&) = delete;
    StubDevice& operator=(const StubDevice&) = delete;
    StubDevice& operator=(StubDevice&&) = delete;

    /// The device's whole-SMP-message budget, reported as `buf_size` through
    /// mcumgr params. A link that models the device's receive side bounds
    /// inbound packets by it (`PtyStub` does, protocol-notes A25).
    static constexpr std::uint32_t kBufSize = 512;

    /// Calls `stop()`.
    ~StubDevice();

    /// Stops the device thread and joins it; anything still queued is
    /// dropped, and the link is called no more. Idempotent. An owner whose
    /// link has a thread of its own that calls `submit()` stops that first,
    /// then this, then destroys either.
    void stop() noexcept;

    /// Binds the device to a link. Called on the client context, before the
    /// client is given the same transport.
    void attach(DeviceLink& link);

    /// Queues a request, from whatever thread the link receives on; the device
    /// thread picks it up.
    void submit(std::vector<std::byte> request);

private:
    void run();
    /// Answers one request; returns the response message, or nothing when the
    /// request should be ignored.
    [[nodiscard]] std::optional<std::vector<std::byte>> answer(const std::vector<std::byte>& raw);

    [[nodiscard]] std::vector<std::byte> encode_state() const;
    /// One step of every device-committed apply in progress: each state read
    /// is "a while" passing.
    void advance_applies();
    /// Starts committing every device-committed image running on trial, as the
    /// coordinating MCU does when image 0 is confirmed (ADR-0022).
    void start_device_commits();
    /// Confirms \p image's running slot. Confirming image 0 is the coordinating
    /// MCU's cue to commit what it applied to the other MCU (protocol-notes S39).
    void confirm_running(std::size_t image);
    void reboot();
    /// Re-reads a slot's version and hash from the image it now holds.
    static void describe(Slot& slot);

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::deque<std::vector<std::byte>> inbox_;
    DeviceLink* link_ = nullptr;
    bool stop_ = false;
    /// Set while answering a reset, acted on once the answer is out. Touched
    /// only on the device thread.
    bool reboot_pending_ = false;

    // --- Device state, touched only on the device thread --------------------

    /// One image: its two slots, what the next boot does with them, and --
    /// for a device-committed image -- how its apply goes.
    struct ImagePair
    {
        std::vector<Slot> slots{2};
        SwapType swap = SwapType::None;
        std::optional<ApplyOutcome> device_commits;
        unsigned apply_reads = 0;
        /// State reads left before an apply in progress finishes.
        std::optional<unsigned> applying;
        /// State reads left before a commit in progress finishes.
        std::optional<unsigned> committing;
    };

    std::vector<ImagePair> images_;

    /// The upload in progress: the image it is for, the bytes accepted so far,
    /// and the total the client declared.
    std::uint32_t staging_image_ = 0;
    std::vector<std::byte> staging_;
    std::uint64_t declared_size_ = 0;

    std::thread thread_;
};

} // namespace smply::example

#endif // SMPLY_EXAMPLE_STUB_DEVICE_HPP
