// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_STUB_DEVICE_HPP
#define SMPLY_EXAMPLE_STUB_DEVICE_HPP

/// \file
/// A pretend Zephyr device, on its own thread.
///
/// **This is not a reference for the wire, and it must not become one.**
/// `tests/support/server_simulator.*` is the thing the test suites trust for
/// SMP: it models the awkward parts of the upload -- offset correction in both
/// directions, session resume by `sha`, the `off == 0` restart, `match` on the
/// final chunk -- the faults and the v1 error translation, and it is anchored
/// by the component tests. This answers the five commands one clean update
/// needs, and stops. If the two ever disagree on the wire, the simulator is
/// right. Do not reconcile them by growing this one; a device worth trusting is
/// a test double, and belongs under `tests/`.
///
/// **The boot rules are shared and trusted.** The slot table, the flags, the
/// set-state answers, the reboot and the apply and commit of an image the
/// device commits itself all come from the boot model (`support/boot_model/`),
/// the same one the simulator decides with. So this device refuses what a
/// default Zephyr build refuses, a client confirm of image 1 included
/// (protocol-notes A27). It reads its slots with the model's image reader, not
/// smply's parser. An image-group refusal is answered as the group-scoped `err`
/// map whatever the request's SMP version, as a server without
/// `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` answers (protocol-notes A16),
/// so that the example tools show the device's own reason.
///
/// It also answers the bootloader-information command with a mode it is given,
/// so that the examples can show the mode being reported and an update being
/// refused for it (ADR-0025). It reports the mode and runs the model in
/// swap-with-revert mode whatever it reports.
///
/// The one extension is a **second image** (`SecondImage`), because the
/// multi-image examples need a device to take a package (ADR-0021): by default
/// one the device commits itself, which it applies after a few state reads, as
/// a coordinating MCU does with its partner's firmware.
///
/// What it *is* for is the thread. A single-threaded example would demonstrate
/// nothing about the arrangement that actually catches people out -- a driver
/// delivering on a thread the library does not own -- and would leave
/// `smply::Dispatcher` with no caller anywhere in the tree. The boot model is
/// single-threaded: it is touched only on the device thread.
///
/// It is built without `CONFIG_IMG_ENABLE_IMAGE_CHECK`, so it never sends
/// `match` (docs/protocol-notes.md section 9, A6: absence is not an error). That
/// is not laziness dressed up: it is what keeps this file off the whole-file
/// hash path, and therefore out of smply's private headers, which is what lets
/// an example use it at all.

#include "stub_device/device_link.hpp"

#include "boot_model/boot_model.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace smply::example {

/// How a device-committed image's apply ends: the boot model's, under the name
/// the examples have always used (docs/multi-image.md, ADR-0022).
using ApplyOutcome = boot_model::ApplyOutcome;

/// A second image: by default the staged firmware of another MCU, which the
/// device commits itself, as a coordinator holds a target's
/// (docs/multi-image.md). After the reset the device takes `apply_reads`
/// image-state reads to apply it, then reports `outcome`: the new image running
/// on trial, or the old one still there. It commits an applied image a read
/// after image 0 is confirmed, or at once if image 0 is not on trial
/// (ADR-0022).
struct SecondImage
{
    /// What image 1 runs now. May be empty: nothing applied yet.
    std::vector<std::byte> running;
    ApplyOutcome outcome = ApplyOutcome::Applied;
    unsigned apply_reads = 3;
    /// False makes image 1 an ordinary image, committed by the client: it
    /// swaps on trial at the reset as image 0 does, and a client confirm of it
    /// is denied, the device being built with Zephyr's defaults
    /// (`IMAGE_CONFIRMATION_DENIED`, protocol-notes A27). `outcome` and
    /// `apply_reads` are then unused.
    bool device_commits = true;
};

/// A device with one image and two slots -- or two images -- answering on a
/// background thread.
class StubDevice
{
public:
    /// \param primary The image the device is already running.
    /// \param second  An image 1, or nothing for the ordinary one-image device.
    /// \param bootloader_mode The number the device reports to the bootloader-
    ///                information `mode` query, or nothing for a device without
    ///                the command. **Reported only**: the device still swaps
    ///                with revert whatever it claims, so a demo of a refusal
    ///                is honest and a demo of any other mode is not.
    explicit StubDevice(std::vector<std::byte> primary,
                        std::optional<SecondImage> second = std::nullopt,
                        std::optional<std::int64_t> bootloader_mode = std::nullopt);

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
    void reboot();

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::deque<std::vector<std::byte>> inbox_;
    DeviceLink* link_ = nullptr;
    bool stop_ = false;
    /// Set while answering a reset, acted on once the answer is out. Touched
    /// only on the device thread.
    bool reboot_pending_ = false;

    // --- Device state, touched only on the device thread --------------------

    /// The slots and every boot decision over them. Built in the constructor,
    /// before the thread starts; touched only on the device thread after.
    boot_model::BootModel model_;

    /// What the bootloader-information `mode` query answers, if anything.
    std::optional<std::int64_t> bootloader_mode_;

    /// The upload in progress: the global slot it writes, the bytes accepted
    /// so far, and the total the client declared. No slot when none is open.
    std::optional<std::size_t> upload_slot_;
    std::size_t received_ = 0;
    std::size_t declared_size_ = 0;

    std::thread thread_;
};

} // namespace smply::example

#endif // SMPLY_EXAMPLE_STUB_DEVICE_HPP
