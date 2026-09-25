// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_DEVICE_LINK_HPP
#define SMPLY_EXAMPLE_DEVICE_LINK_HPP

/// \file
/// The stub device's side of whatever connects it to the client.
///
/// `StubDevice` answers requests on its own thread and hands each answer to a
/// `DeviceLink`. What the link does with it is the example's business:
/// `cli_dfu`'s `LoopbackTransport` posts it straight to the client context, and
/// `serial_dfu`'s `PtyStub` frames it onto a pseudo-terminal. The device does
/// not know which, which is what lets both examples share one stub.
///
/// Every method is called **on the device thread**.

#include "smply/error.hpp"

#include <cstddef>
#include <vector>

namespace smply::example {

/// Where the stub device's answers and reset go.
class DeviceLink
{
public:
    DeviceLink(const DeviceLink&) = delete;
    DeviceLink(DeviceLink&&) = delete;
    DeviceLink& operator=(const DeviceLink&) = delete;
    DeviceLink& operator=(DeviceLink&&) = delete;
    virtual ~DeviceLink() = default;

    /// One complete SMP response, owned by the callee from here on.
    virtual void deliver(std::vector<std::byte> message) = 0;

    /// The device has answered a reset and is about to reboot. A link that a
    /// reset drops -- BLE, a USB CDC port -- goes down here.
    virtual void device_resetting(Error reason) = 0;

    /// The device has finished rebooting. A link that survived the reset -- a
    /// hardware UART -- is where the boot output appears.
    virtual void device_booted() = 0;

    /// Does this link stay attached across a reset? If not, the device forgets
    /// it at the reset and answers nothing until the application attaches a
    /// new one, as it must after a BLE reconnect.
    [[nodiscard]] virtual bool survives_reset() const noexcept = 0;

protected:
    DeviceLink() = default;
};

} // namespace smply::example

#endif // SMPLY_EXAMPLE_DEVICE_LINK_HPP
