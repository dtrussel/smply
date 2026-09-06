// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_GROUPS_IMAGE_UPLOAD_DRIVER_HPP
#define SMPLY_SRC_GROUPS_IMAGE_UPLOAD_DRIVER_HPP

/// \file
/// The I/O half of an upload: encode, send, decode, report.
///
/// Every decision belongs to `upload_session.hpp`; this file only carries them
/// out. Keeping the split that way is what makes the response table testable
/// without a transport (ADR-0008), so resist putting a rule here -- if a
/// condition needs deciding, it belongs in `on_response`.

#include "groups/image/upload_session.hpp"
#include "smply/groups/image.hpp"
#include "smply/image_source.hpp"
#include "smply/limits.hpp"
#include "smply/smp_client.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace smply::image {

/// Enough for the largest chunk plus the largest first-packet envelope.
///
/// Fixed, because nothing in smply sizes an allocation from a number a device
/// or a file supplied. `UploadOptions::chunk_size` above
/// `limits::kUploadChunkMax` is rejected, which is what keeps this sufficient.
inline constexpr std::size_t kUploadBufferSize = limits::kUploadChunkMax + 128;

/// One upload session, and the machinery to run it.
///
/// Non-copyable and non-movable: `SmpClient` callbacks capture a pointer to it.
class UploadDriver
{
public:
    UploadDriver(SmpClient& client, ImageSource& source, const UploadConfig& config,
                 Duration first_chunk_timeout, Duration chunk_timeout,
                 std::function<void(UploadProgress)> on_progress,
                 Callback<UploadResult> on_done) noexcept;

    UploadDriver(const UploadDriver&) = delete;
    UploadDriver(UploadDriver&&) = delete;
    UploadDriver& operator=(const UploadDriver&) = delete;
    UploadDriver& operator=(UploadDriver&&) = delete;
    ~UploadDriver() = default;

    /// Sends the first request. Called once, by `ImageManagement::upload()`.
    void start();

    /// Sends a fresh first packet after a disconnect, with a new callback.
    void restart(Callback<UploadResult> on_done);

    /// Abandons the session. The callback receives `Cancelled` from the
    /// client's next `poll()`, like any other cancellation.
    void cancel();

    /// Abandons the session and completes the callback **inline**.
    ///
    /// For destruction only, where there is no later `poll()` to defer to --
    /// the same exception `~SmpClient` makes, and for the same reason.
    void abandon();

    [[nodiscard]] std::uint64_t transferred() const noexcept
    {
        return state_.confirmed_off;
    }

    /// True until the callback has fired.
    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }

    /// True once a disconnect left the session resumable.
    [[nodiscard]] bool resumable() const noexcept
    {
        return resumable_;
    }

private:
    /// Encodes and sends \p request, or completes with the reason it could not.
    void send(const UploadRequest& request);

    /// Turns a raw response into an `UploadResponse` and drives the next step.
    void handle(Result<RawResponse> response);

    /// Runs a step: send, complete, or fail.
    void advance(const Step& step);

    void finish(Result<UploadResult> outcome);

    SmpClient* client_;
    ImageSource* source_;
    UploadConfig config_;
    Duration first_chunk_timeout_;
    Duration chunk_timeout_;
    std::function<void(UploadProgress)> on_progress_;
    Callback<UploadResult> on_done_;

    UploadState state_;
    RequestHandle request_;
    std::array<std::byte, kUploadBufferSize> buffer_{};
    /// The last offset reported to `on_progress_`, so a repeat is not reported.
    std::uint64_t reported_off_ = 0;
    bool active_ = false;
    bool resumable_ = false;
    /// True while `start()` or `restart()` is on the stack.
    ///
    /// A failure there -- an unreadable source, a budget that cannot fit a
    /// chunk -- would otherwise complete the callback *inside* the call that
    /// began the upload, which is the one thing every layer here promises not
    /// to do (docs/design.md section 5, rule 4). While it is set, `finish()`
    /// defers instead.
    bool starting_ = false;

    /// Proves this driver still exists when a response arrives.
    ///
    /// The response callback held by `SmpClient` captures `this`, and a
    /// cancelled request's completion is *deferred* to the next `poll()` -- so
    /// without this a destroyed driver would be called back through a dangling
    /// pointer. The lambda holds a `weak_ptr` to it and does nothing when it
    /// has expired.
    std::shared_ptr<int> life_ = std::make_shared<int>(0);
};

} // namespace smply::image

#endif // SMPLY_SRC_GROUPS_IMAGE_UPLOAD_DRIVER_HPP
