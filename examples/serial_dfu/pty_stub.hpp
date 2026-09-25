// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_PTY_STUB_HPP
#define SMPLY_EXAMPLE_PTY_STUB_HPP

/// \file
/// The stub device behind a pseudo-terminal, speaking MCUmgr's console framing.
///
/// Scaffolding, like the stub device itself (`stub_device/stub_device.hpp`): it
/// exists so that `serial_dfu` can run a whole update, reset included, in CI.
/// The adapter under test opens a real tty -- the pty's slave end, reached
/// through a symlink this object publishes -- so every byte crosses the same
/// line discipline, `poll()` and `read()` a USB port would.
///
/// **Two reset shapes, because the two real ones differ** (roadmap O7):
///
/// * `ResetShape::Uart` -- a hardware UART. The port stays open across the
///   reset; the device goes quiet for the reboot, then prints boot banners and
///   a log line longer than any frame, which the adapter must ignore and drop.
/// * `ResetShape::Cdc` -- a USB CDC ACM port. The port **vanishes** (the master
///   is closed, so the adapter's reads fail with `EIO`) and comes back as a
///   **new** pseudo-terminal, under a different `/dev/pts/N`. The symlink is
///   re-pointed at it, which is what `/dev/serial/by-id/...` does for a real
///   device that re-enumerates as another `ttyACMn`.
///
/// POSIX only: nothing here can create a COM port on Windows.

#include "stub_device/device_link.hpp"
#include "stub_device/stub_device.hpp"

#include "serial_port/serial_link.hpp"

#include "smply/error.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace smply::example {

/// How the pretend device's port behaves across a reset.
enum class ResetShape : std::uint8_t
{
    Uart, ///< Stays open; boot output follows.
    Cdc,  ///< Vanishes, and returns as a different pseudo-terminal.
};

/// A `DeviceLink` from the stub device onto a pseudo-terminal.
class PtyStub final : public DeviceLink
{
public:
    /// Creates the first pseudo-terminal and publishes the symlink. Attach it
    /// to \p device with `StubDevice::attach()` before starting the update.
    PtyStub(StubDevice& device, ResetShape shape);

    PtyStub(const PtyStub&) = delete;
    PtyStub(PtyStub&&) = delete;
    PtyStub& operator=(const PtyStub&) = delete;
    PtyStub& operator=(PtyStub&&) = delete;

    /// Stops the reader, closes the master and removes the symlink.
    ~PtyStub() override;

    /// Stops the reader thread, so nothing more is submitted to the device.
    /// Idempotent. The destructor calls it; an owner that must stop both this
    /// and the device before destroying either calls it first.
    void stop() noexcept;

    /// The stable path an application opens. A symlink to the current slave.
    [[nodiscard]] const std::string& port_path() const noexcept
    {
        return link_path_;
    }

    /// False when construction could not create a pseudo-terminal.
    [[nodiscard]] bool ok() const noexcept
    {
        return ok_;
    }

    // --- DeviceLink, called on the device thread -----------------------------

    void deliver(std::vector<std::byte> message) override;
    void device_resetting(Error reason) override;
    void device_booted() override;

    /// True in both shapes. This object is the device's port, not the
    /// client's connection: it is still there after the reboot, even when
    /// the pseudo-terminal behind it has been replaced.
    [[nodiscard]] bool survives_reset() const noexcept override
    {
        return true;
    }

private:
    void wake() const noexcept;
    /// Writes to the current master. Caller holds `mutex_`.
    void write_locked(const std::vector<std::byte>& bytes);
    void read_loop();

    StubDevice* device_;
    ResetShape shape_;
    std::string directory_;
    std::string link_path_;
    bool ok_ = false;

    /// Guards every descriptor and name below. The device thread writes to
    /// `master_` and swaps it on a CDC-style reset; only the reader thread
    /// closes a retired master, because it may be inside `poll()` on it.
    std::mutex mutex_;
    int master_ = -1;
    /// The stub's own hold on the slave, so the master never reports a
    /// hang-up just because no client has the port open.
    int keeper_ = -1;
    int retiring_master_ = -1;
    int retiring_keeper_ = -1;
    /// The slave the symlink will point at once the device has "booted".
    std::string next_slave_;

    /// Wakes the reader out of `poll()`: to stop, or to retire a master.
    int wake_read_ = -1;
    int wake_write_ = -1;
    std::atomic<bool> stop_{false};
    std::thread reader_;
};

} // namespace smply::example

#endif // SMPLY_EXAMPLE_PTY_STUB_HPP
