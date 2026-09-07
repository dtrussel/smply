// SPDX-License-Identifier: Apache-2.0

#include "loopback_transport.hpp"

#include "stub_device.hpp"

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace smply::example {
namespace {

/// A believable BLE payload: 247-byte ATT MTU less the three-byte notification
/// header. Small enough that an upload takes many chunks, which is the point --
/// a demo whose image fits in one packet would exercise none of the upload
/// state machine.
constexpr std::size_t kMaxMessageSize = 244;

} // namespace

LoopbackTransport::LoopbackTransport(StubDevice& device, Dispatcher& inbound) noexcept
    : device_{&device}, inbound_{&inbound}
{}

Result<void> LoopbackTransport::send(ConstBytes message)
{
    if (!open_) {
        return fail(ErrorCode::Disconnected, "loopback: the link is down");
    }

    // Hand the whole message over and return. Answering here -- calling the
    // listener before this returns -- is what design.md section 9 forbids, and
    // it is an easy mistake to make in a loopback precisely because the answer
    // is available.
    device_->submit(std::vector<std::byte>{message.begin(), message.end()});
    return {};
}

std::size_t LoopbackTransport::max_message_size() const noexcept
{
    return kMaxMessageSize;
}

void LoopbackTransport::set_listener(TransportListener* listener) noexcept
{
    listener_ = listener;
}

void LoopbackTransport::close() noexcept
{
    open_ = false;
    listener_ = nullptr;
}

void LoopbackTransport::deliver_from_device(std::vector<std::byte> message)
{
    // The closure owns the bytes, and everything it reads it reads on the
    // client context. That is the whole marshalling pattern.
    inbound_->post([this, bytes = std::move(message)] {
        if (!open_ || listener_ == nullptr) {
            return;
        }
        listener_->on_bytes(ConstBytes{bytes});
    });
}

void LoopbackTransport::drop_from_device(Error reason)
{
    inbound_->post([this, reason = std::move(reason)] {
        if (!open_ || listener_ == nullptr) {
            return;
        }
        open_ = false;
        TransportListener* listener = listener_;
        listener_ = nullptr;
        // After on_disconnected the contract allows no further callbacks, which
        // clearing the listener first makes true by construction.
        listener->on_disconnected(reason);
    });
}

} // namespace smply::example
