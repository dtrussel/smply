// SPDX-License-Identifier: Apache-2.0

#include "groups/image/upload_driver.hpp"

#include "cbor/cbor.hpp"
#include "smply/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace smply::image {
namespace {

/// Image-group command ID for upload (docs/protocol-notes.md section 6).
constexpr std::uint8_t kUploadCommand = 1;

} // namespace

UploadDriver::UploadDriver(SmpClient& client, ImageSource& source, const UploadConfig& config,
                           Duration first_chunk_timeout, Duration chunk_timeout,
                           std::function<void(UploadProgress)> on_progress,
                           Callback<UploadResult> on_done) noexcept
    : client_{&client}, source_{&source}, config_{config},
      first_chunk_timeout_{first_chunk_timeout}, chunk_timeout_{chunk_timeout},
      on_progress_{std::move(on_progress)}, on_done_{std::move(on_done)}
{}

void UploadDriver::start()
{
    active_ = true;
    resumable_ = false;
    starting_ = true;
    advance(plan_next(state_, config_));
    starting_ = false;
}

void UploadDriver::restart(Callback<UploadResult> on_done)
{
    on_done_ = std::move(on_done);
    active_ = true;
    resumable_ = false;
    // Whatever the device holds, the way to find out is to ask: a first packet
    // carrying the same sha, whose answer is adopted as authoritative
    // (docs/protocol-notes.md section 6, rule 6).
    state_.first_packet_pending = true;
    state_.phase = Phase::Idle;
    state_.retries = 0;
    state_.consecutive_no_progress = 0;
    starting_ = true;
    advance(plan_next(state_, config_));
    starting_ = false;
}

void UploadDriver::cancel()
{
    if (!active_) {
        return;
    }
    client_->cancel(request_);
    request_ = {};
    // The client completes the cancelled request with Cancelled on the next
    // poll(), and handle() turns that into the callback -- so there is nothing
    // to finish here, and finishing would fire the callback twice.
}

void UploadDriver::abandon()
{
    if (!active_) {
        return;
    }
    client_->cancel(request_);
    request_ = {};
    // Inline, because this runs from a destructor and the deferred completion
    // the line above queued will find this driver gone.
    finish(fail(Error{ErrorCode::Cancelled, "upload: image management destroyed"}));
}

void UploadDriver::send(const UploadRequest& request)
{
    cbor::Writer writer{MutBytes{buffer_}};
    writer.open_map();
    if (request.first_packet) {
        // Rule 7: a first packet re-sends every field the original carried, or
        // the server cannot rebuild the session.
        writer.put_uint("image", config_.image);
        writer.put_uint("len", config_.image_size);
    }
    writer.put_uint("off", request.off);
    if (request.first_packet) {
        if (config_.sha.has_value()) {
            writer.put_bytes("sha", ConstBytes{*config_.sha});
        }
        if (config_.upgrade_only) {
            writer.put_bool("upgrade", true);
        }
    }

    // Read into its own buffer first: cbor::Writer needs the bytes up front,
    // and this is the one place the chunk is copied.
    std::array<std::byte, limits::kUploadChunkMax> chunk{};
    const MutBytes into{chunk.data(), request.length};
    const auto read = source_->read(request.off, into);
    if (!read.has_value()) {
        finish(fail(read.error()));
        return;
    }
    if (*read != request.length) {
        // A short read anywhere but the end of the image is a broken source --
        // the same rule src/image/ applies, and for the same reason.
        finish(fail(Error{ErrorCode::InvalidArgument, "upload: source returned a short read"}));
        return;
    }
    writer.put_bytes("data", ConstBytes{into});

    const auto payload = writer.close_map().finish();
    if (!payload.has_value()) {
        // Unreachable: kUploadBufferSize covers the largest legal chunk plus
        // the largest envelope, and chunk_size is bounded before a session
        // exists. Kept so a future change to the sizing fails loudly.
        finish(fail(Error{ErrorCode::Internal, "upload: request buffer too small"}));
        return;
    }

    const RequestSpec spec{.op = Operation::Write,
                           .group = Group::Image,
                           .command = kUploadCommand,
                           .payload = *payload,
                           // The first chunk may trigger an implicit erase of
                           // unbounded duration (A7).
                           .timeout = request.first_packet ? first_chunk_timeout_ : chunk_timeout_};

    record_sent(state_, request);
    request_ = client_->request(
        spec, [this, life = std::weak_ptr<int>{life_}](Result<RawResponse> response) {
            if (life.expired()) {
                // The driver went away while a cancelled request's completion
                // was still queued. Nothing to report to.
                return;
            }
            handle(std::move(response));
        });
}

void UploadDriver::handle(Result<RawResponse> response)
{
    if (!active_) {
        return;
    }
    request_ = {};

    UploadResponse decoded;
    if (!response.has_value()) {
        decoded.failure = response.error();
    } else {
        cbor::Reader reader{response->payload};
        if (const auto entered = reader.enter_map(); !entered.has_value()) {
            // Unreachable today: SmpClient::interpret() has already required a
            // map. Checked anyway -- a decoder that assumes its input was
            // validated elsewhere is one refactor away from trusting a device.
            decoded.failure = entered.error();
        } else {
            const std::optional<std::uint64_t> off = reader.uint("off");
            const std::optional<bool> match = reader.boolean("match");
            static_cast<void>(reader.leave_map());

            if (const auto status = reader.status(); !status.has_value()) {
                // A wrong-typed field poisons the reader and makes every field
                // look absent, so a malformed response would otherwise read as
                // a success with no offset.
                decoded.failure = status.error();
            } else {
                decoded.off = off;
                decoded.match = match;
            }
        }
    }

    const bool disconnected =
        decoded.failure.has_value() && decoded.failure->code() == ErrorCode::Disconnected;

    const Step step = on_response(state_, decoded, config_);

    if (step.action == Action::SendChunk && state_.confirmed_off != reported_off_ && on_progress_) {
        // Only on a confirmed advance, and only when it moved -- progress is
        // read from confirmed_off, never from what was put on the wire.
        reported_off_ = state_.confirmed_off;
        on_progress_(
            UploadProgress{.transferred = state_.confirmed_off, .total = config_.image_size});
    }

    if (disconnected) {
        // The session survives the link: confirmed_off and sha are still good,
        // so resume() can pick it up. The callback still fires exactly once.
        resumable_ = true;
    }

    advance(step);
}

void UploadDriver::advance(const Step& step)
{
    switch (step.action) {
    case Action::SendChunk:
        send(step.request);
        return;
    case Action::Complete:
        if (on_progress_ && state_.confirmed_off != reported_off_) {
            reported_off_ = state_.confirmed_off;
            on_progress_(
                UploadProgress{.transferred = state_.confirmed_off, .total = config_.image_size});
        }
        finish(UploadResult{.transferred = state_.confirmed_off, .match = step.match});
        return;
    case Action::Fail:
        finish(fail(step.error));
        return;
    }
}

void UploadDriver::finish(Result<UploadResult> outcome)
{
    if (!active_) {
        return;
    }
    active_ = false;
    // Moved out before the call: the callback may start another upload, and it
    // must not see a callback that has already run.
    Callback<UploadResult> callback = std::move(on_done_);
    on_done_ = {};
    if (!callback) {
        return;
    }

    if (starting_) {
        // Still inside upload() or resume(). Queued rather than called, so a
        // callback never runs inside the call that started the operation.
        client_->defer([life = std::weak_ptr<int>{life_}, callback = std::move(callback),
                        result = std::move(outcome)]() mutable {
            if (life.expired()) {
                return;
            }
            callback(std::move(result));
        });
        return;
    }
    callback(std::move(outcome));
}

} // namespace smply::image
