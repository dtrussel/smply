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

class LoopbackTransport;

/// What the bootloader will do on the next reset (protocol-notes section 7).
enum class SwapType : std::uint8_t
{
    None,  ///< Boot the primary slot again.
    Test,  ///< Swap in the secondary for a trial; revert unless confirmed.
    Perm,  ///< Swap in the secondary permanently.
    Revert ///< A trial was not confirmed: swap back.
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

/// A device with one image and two slots, answering on a background thread.
class StubDevice
{
public:
    /// \param primary The image the device is already running.
    explicit StubDevice(std::vector<std::byte> primary);

    StubDevice(const StubDevice&) = delete;
    StubDevice(StubDevice&&) = delete;
    StubDevice& operator=(const StubDevice&) = delete;
    StubDevice& operator=(StubDevice&&) = delete;

    /// Stops the thread and joins it. Anything still queued is dropped.
    ~StubDevice();

    /// Binds the device to a link. Called on the client context, before the
    /// client is given the same transport.
    void attach(LoopbackTransport& link);

    /// Queues a request. Called from `LoopbackTransport::send()`, on the client
    /// context; the device thread picks it up.
    void submit(std::vector<std::byte> request);

private:
    void run();
    /// Answers one request; returns the response message, or nothing when the
    /// request should be ignored.
    [[nodiscard]] std::optional<std::vector<std::byte>> answer(const std::vector<std::byte>& raw);

    [[nodiscard]] std::vector<std::byte> encode_state() const;
    void reboot();
    /// Re-reads a slot's version and hash from the image it now holds.
    static void describe(Slot& slot);

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::deque<std::vector<std::byte>> inbox_;
    LoopbackTransport* link_ = nullptr;
    bool stop_ = false;
    /// Set while answering a reset, acted on once the answer is out. Touched
    /// only on the device thread.
    bool reboot_pending_ = false;

    // --- Device state, touched only on the device thread --------------------
    std::vector<Slot> slots_{2};
    SwapType swap_ = SwapType::None;
    /// The upload in progress: the bytes accepted so far, and the total the
    /// client declared.
    std::vector<std::byte> staging_;
    std::uint64_t declared_size_ = 0;

    std::thread thread_;
};

} // namespace smply::example

#endif // SMPLY_EXAMPLE_STUB_DEVICE_HPP
