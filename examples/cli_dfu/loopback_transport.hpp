// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_LOOPBACK_TRANSPORT_HPP
#define SMPLY_EXAMPLE_LOOPBACK_TRANSPORT_HPP

/// \file
/// A `Transport` onto a device running in this process, on another thread.
///
/// This is the part of the example worth copying. It is the shape every real
/// adapter has:
///
/// * **outbound**, `send()` hands one complete SMP message to the driver and
///   returns. It does **not** deliver anything inbound before returning --
///   docs/design.md section 9 forbids it, because the core would re-enter
///   reassembly (ADR-0006);
/// * **inbound**, the driver thread does not touch the client. It posts a
///   closure to a `smply::Dispatcher`, and the pump thread drains it, so
///   `on_bytes()` arrives on the client context as ADR-0004 requires. Getting
///   that wrong trips `SMPLY_ASSERT_CLIENT_THREAD()` in a debug build rather
///   than corrupting the assembler quietly.
///
/// **A dropped link stays dropped**, like a real one: after `close()` or a
/// device-side disconnect this object delivers nothing further, and reconnecting
/// means constructing a new one and calling `SmpClient::rebind_transport()`.
/// `FakeTransport` behaves the same way in the tests, for the same reason.

#include "stub_device/device_link.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"
#include "smply/util/dispatcher.hpp"

#include <cstddef>
#include <vector>

namespace smply::example {

class StubDevice;

/// One link between the client and the stub device.
class LoopbackTransport final : public Transport, public DeviceLink
{
public:
    /// \param device   Answers what is sent. Must outlive this transport.
    /// \param inbound  Marshals device-thread callbacks onto the client
    ///                 context. Must outlive this transport.
    LoopbackTransport(StubDevice& device, Dispatcher& inbound) noexcept;

    LoopbackTransport(const LoopbackTransport&) = delete;
    LoopbackTransport(LoopbackTransport&&) = delete;
    LoopbackTransport& operator=(const LoopbackTransport&) = delete;
    LoopbackTransport& operator=(LoopbackTransport&&) = delete;
    ~LoopbackTransport() override = default;

    // --- Transport, all called on the client context ------------------------

    [[nodiscard]] Result<void> send(ConstBytes message) override;
    [[nodiscard]] std::size_t max_message_size() const noexcept override;
    void set_listener(TransportListener* listener) noexcept override;
    void close() noexcept override;

    // --- DeviceLink, called from the device thread ---------------------------

    /// Queues \p message for delivery on the client context.
    ///
    /// Takes the bytes by value: an inbound buffer is borrowed for the duration
    /// of the callback (design.md section 9), so anything crossing a thread
    /// boundary has to own its own copy. This is the single most common bug in
    /// a first adapter.
    void deliver(std::vector<std::byte> message) override;

    /// Queues a disconnect, as a BLE link does when the device reboots.
    void device_resetting(Error reason) override;

    /// Nothing: a BLE device's boot is invisible until the application
    /// reconnects, which it does on a new link.
    void device_booted() override {}

    /// False: a reboot drops a BLE connection, so the device forgets this
    /// link and waits for the application to attach a new one.
    [[nodiscard]] bool survives_reset() const noexcept override
    {
        return false;
    }

private:
    StubDevice* device_;
    Dispatcher* inbound_;

    /// Both are read and written **only on the client context**, and that is
    /// what makes this class free of synchronisation of its own. The device
    /// thread never inspects them: it posts a closure, and the closure looks at
    /// them later, on the pump thread. Reading `open_` from `deliver()`
    /// would be a data race for no gain -- TSan says so, and it would be right.
    TransportListener* listener_ = nullptr;
    bool open_ = true;
};

} // namespace smply::example

#endif // SMPLY_EXAMPLE_LOOPBACK_TRANSPORT_HPP
