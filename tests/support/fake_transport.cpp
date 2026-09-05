// SPDX-License-Identifier: Apache-2.0

#include "fake_transport.hpp"

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

namespace smply::test {

Result<void> FakeTransport::send(ConstBytes message)
{
    if (closed_ || disconnected_) {
        return fail(ErrorCode::Disconnected, "fake transport: send after close");
    }

    if (next_send_failure_.has_value()) {
        // One-shot: consume it whether or not the caller retries.
        Error error = std::move(next_send_failure_.value());
        next_send_failure_.reset();
        return fail(std::move(error));
    }

    if (busy_) {
        // Deliberately recorded as not sent: a busy medium did not accept it,
        // so a test asserting on `sent()` must not see it.
        return fail(ErrorCode::TransportBusy, "fake transport: busy");
    }

    // Copy: the contract says the buffer is borrowed for the call only, so a
    // transport that keeps it must take its own. Recording it here is exactly
    // that case, and copying is what a real deferred-send transport does.
    sent_.emplace_back(message.begin(), message.end());
    return {};
}

std::size_t FakeTransport::max_message_size() const noexcept
{
    return max_message_size_;
}

void FakeTransport::set_listener(TransportListener* listener) noexcept
{
    listener_ = listener;
}

void FakeTransport::close() noexcept
{
    // Idempotent, and does not report on_disconnected(): the caller asked.
    closed_ = true;
}

ConstBytes FakeTransport::last_sent() const
{
    assert(!sent_.empty());
    return ConstBytes{sent_.back()};
}

bool FakeTransport::deliverable() const noexcept
{
    return listener_ != nullptr && !closed_ && !disconnected_;
}

void FakeTransport::emit(ConstBytes bytes)
{
    if (!deliverable()) {
        ++suppressed_deliveries_;
        return;
    }
    ++on_bytes_calls_;
    listener_->on_bytes(bytes);
}

void FakeTransport::deliver(ConstBytes bytes)
{
    emit(bytes);
}

void FakeTransport::deliver_fragmented(ConstBytes bytes, std::size_t fragment)
{
    assert(fragment > 0);
    for (std::size_t offset = 0; offset < bytes.size(); offset += fragment) {
        const std::size_t take = std::min(fragment, bytes.size() - offset);
        emit(bytes.subspan(offset, take));
    }
}

void FakeTransport::deliver_byte_by_byte(ConstBytes bytes)
{
    deliver_fragmented(bytes, 1);
}

void FakeTransport::deliver_concatenated(std::span<const ConstBytes> buffers)
{
    std::vector<std::byte> joined;
    std::size_t total = 0;
    for (const auto& buffer : buffers) {
        total += buffer.size();
    }
    joined.reserve(total);
    for (const auto& buffer : buffers) {
        joined.insert(joined.end(), buffer.begin(), buffer.end());
    }
    emit(ConstBytes{joined});
}

void FakeTransport::deliver_split_at(ConstBytes bytes, std::span<const std::size_t> cuts)
{
    std::size_t offset = 0;
    for (const std::size_t cut : cuts) {
        // Ignore anything that would produce an empty or backwards slice, so a
        // test can pass a fixed cut list against inputs of varying length.
        if (cut <= offset || cut >= bytes.size()) {
            continue;
        }
        emit(bytes.subspan(offset, cut - offset));
        offset = cut;
    }
    if (offset < bytes.size()) {
        emit(bytes.subspan(offset));
    }
}

void FakeTransport::fail_next_send(Error error)
{
    next_send_failure_ = std::move(error);
}

void FakeTransport::raise_transport_error(Error error)
{
    if (!deliverable()) {
        ++suppressed_deliveries_;
        return;
    }
    listener_->on_transport_error(std::move(error));
}

void FakeTransport::disconnect(Error error)
{
    if (!deliverable()) {
        ++suppressed_deliveries_;
        return;
    }
    // Mark first: the contract forbids any further callback once
    // on_disconnected() has been issued, including one triggered from inside
    // the handler itself.
    disconnected_ = true;
    listener_->on_disconnected(std::move(error));
}

} // namespace smply::test
