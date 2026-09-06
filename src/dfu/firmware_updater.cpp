// SPDX-License-Identifier: Apache-2.0

#include "smply/dfu/firmware_updater.hpp"

#include "dfu/update_state_machine.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/image_source.hpp"
#include "smply/mcuboot_image.hpp"
#include "smply/result.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace smply {
namespace {

using dfu::Effect;
using dfu::Event;

/// Any failure of the optional parameters command is an ordinary answer, not a
/// reason to abandon an update (docs/protocol-notes.md section 9, A8).
[[nodiscard]] Event parameters_unavailable()
{
    Event event;
    event.kind = Event::Kind::ParametersUnavailable;
    return event;
}

[[nodiscard]] Event failure(Error error)
{
    Event event;
    event.kind = Event::Kind::Failed;
    event.error = std::move(error);
    return event;
}

[[nodiscard]] Event plain(Event::Kind kind)
{
    Event event;
    event.kind = kind;
    return event;
}

} // namespace

std::string_view to_string(UpdateState state) noexcept
{
    switch (state) {
    case UpdateState::Idle:
        return "Idle";
    case UpdateState::QueryingParameters:
        return "QueryingParameters";
    case UpdateState::InspectingImages:
        return "InspectingImages";
    case UpdateState::Planning:
        return "Planning";
    case UpdateState::Uploading:
        return "Uploading";
    case UpdateState::VerifyingUpload:
        return "VerifyingUpload";
    case UpdateState::MarkingForTest:
        return "MarkingForTest";
    case UpdateState::Resetting:
        return "Resetting";
    case UpdateState::AwaitingDisconnect:
        return "AwaitingDisconnect";
    case UpdateState::AwaitingReconnect:
        return "AwaitingReconnect";
    case UpdateState::VerifyingBooted:
        return "VerifyingBooted";
    case UpdateState::AwaitingConfirmation:
        return "AwaitingConfirmation";
    case UpdateState::Confirming:
        return "Confirming";
    case UpdateState::VerifyingConfirmed:
        return "VerifyingConfirmed";
    case UpdateState::Completed:
        return "Completed";
    case UpdateState::Failed:
        return "Failed";
    case UpdateState::Cancelled:
        return "Cancelled";
    }
    // Unreachable: the switch is exhaustive over the enum and has no default
    // (docs/design.md section 11), so this exists only because a function
    // returning a value must. The marker has to sit on the excluded line
    // itself; on the comment above it, gcovr ignores it.
    return "Unknown"; // LCOV_EXCL_LINE
}

/// The I/O half: it carries out effects and owns nothing that decides anything.
///
/// If a condition needs deciding it belongs in `dfu::advance()`; keeping that
/// line is what makes the whole failure table testable without a device.
class FirmwareUpdater::Impl
{
public:
    Impl(SmpClient& client, ImageManagement& image, OsManagement& os) noexcept
        : client_{&client}, image_{&image}, os_{&os}
    {}

    [[nodiscard]] Result<void> start(ImageSource& source, const UpdatePlan& plan,
                                     UpdateEventCallback on_event)
    {
        if (running_) {
            return fail(ErrorCode::InvalidState, "updater: an update is already running");
        }
        if (!on_event) {
            return fail(ErrorCode::InvalidArgument, "updater: no event callback");
        }

        // The image-state hash of the file: what the device will report for the
        // slot holding it, and therefore how every later step recognises it.
        // Read before anything goes on the wire, so a file that is not an
        // MCUboot image fails here rather than half way through an update.
        const Result<ImageHash> target = read_target_hash(source);
        if (!target.has_value()) {
            return unexpected<Error>{target.error()};
        }

        plan_ = plan;
        source_ = &source;
        on_event_ = std::move(on_event);
        context_ = dfu::Context{};
        context_.target = *target;
        report_ = UpdateReport{};
        state_ = UpdateState::Idle;
        running_ = true;

        post(plain(Event::Kind::Start));
        return {};
    }

    [[nodiscard]] Result<void> confirm()
    {
        if (state_ != UpdateState::AwaitingConfirmation) {
            return fail(ErrorCode::InvalidState, "updater: not awaiting confirmation");
        }
        post(plain(Event::Kind::ConfirmApproved));
        return {};
    }

    [[nodiscard]] Result<void> resume_after_reconnect()
    {
        if (state_ != UpdateState::AwaitingReconnect) {
            return fail(ErrorCode::InvalidState, "updater: not awaiting a reconnect");
        }
        post(plain(Event::Kind::Reconnected));
        return {};
    }

    void reconnect_failed(Error error)
    {
        if (!running_) {
            return;
        }
        Event event;
        event.kind = Event::Kind::ReconnectFailed;
        event.error = std::move(error);
        post(std::move(event));
    }

    void cancel() noexcept
    {
        if (!running_) {
            return;
        }
        if (upload_.valid()) {
            image_->cancel(upload_);
        }
        post(plain(Event::Kind::Cancel));
    }

    void poll(TimePoint now)
    {
        // Recorded first: an effect applied from a group callback needs a
        // reference point for the grace timer, and this is the only time the
        // updater is given one.
        last_poll_ = now;
        if (!running_) {
            return;
        }
        if (state_ != UpdateState::AwaitingDisconnect) {
            return;
        }
        // The updater never touches the transport, so it learns about the drop
        // the only way it can: by asking the client whether the link is still
        // there (ADR-0004).
        if (!client_->connected()) {
            dispatch(plain(Event::Kind::Disconnected));
            return;
        }
        if (grace_deadline_.has_value() && now >= *grace_deadline_) {
            grace_deadline_.reset();
            dispatch(plain(Event::Kind::GraceExpired));
        }
    }

    [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept
    {
        return grace_deadline_;
    }

    [[nodiscard]] UpdateState state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] const UpdateReport& report() const noexcept
    {
        return report_;
    }

    /// Completes a running update inline, for the destructor.
    void abandon()
    {
        if (!running_) {
            return;
        }
        if (upload_.valid()) {
            image_->cancel(upload_);
        }
        dispatch(plain(Event::Kind::Cancel));
    }

private:
    /// Queues an event for the next `poll()`.
    ///
    /// Every externally triggered event goes through here, so that no callback
    /// runs inside the call that caused it (ADR-0003). Events raised by a group
    /// callback are already inside `poll()` or `on_bytes()` and dispatch
    /// directly.
    void post(Event event)
    {
        client_->defer([life = std::weak_ptr<int>{life_}, this, event = std::move(event)]() {
            if (life.expired()) {
                return;
            }
            dispatch(event);
        });
    }

    void dispatch(const Event& event)
    {
        if (!running_) {
            return;
        }
        dfu::Step step = dfu::advance(state_, event, plan_, context_);
        for (;;) {
            enter(step.next);
            if (step.effect != Effect::Continue) {
                apply(step.effect);
                return;
            }
            // A decision that needed no I/O: re-enter immediately, so the state
            // it was decided in is still a real, observable state.
            step = dfu::advance(state_, plain(Event::Kind::Continue), plan_, context_);
        }
    }

    void enter(UpdateState next)
    {
        if (next == state_) {
            return;
        }
        const UpdateState previous = state_;
        state_ = next;

        UpdateEvent event;
        event.kind = UpdateEvent::Kind::StateChanged;
        event.from = previous;
        event.to = next;
        emit(event);
    }

    void apply(Effect effect)
    {
        switch (effect) {
        case Effect::None:
        case Effect::Continue:
            return;

        case Effect::QueryParameters:
            static_cast<void>(os_->mcumgr_parameters(
                guarded<McumgrParameters>([](Impl& self, const Result<McumgrParameters>& result) {
                    if (!result.has_value()) {
                        self.dispatch(parameters_unavailable());
                        return;
                    }
                    Event event;
                    event.kind = Event::Kind::ParametersRead;
                    event.buf_size = result->buf_size;
                    self.dispatch(event);
                })));
            return;

        case Effect::ReadState:
            static_cast<void>(image_->get_state(
                guarded<ImageState>([](Impl& self, const Result<ImageState>& result) {
                    if (!result.has_value()) {
                        self.dispatch(failure(result.error()));
                        return;
                    }
                    Event event;
                    event.kind = Event::Kind::StateRead;
                    event.state = &*result;
                    self.dispatch(event);
                })));
            return;

        case Effect::StartUpload:
            start_upload();
            return;

        case Effect::ResumeUpload:
            image_->resume(upload_, upload_done());
            return;

        case Effect::MarkForTest: {
            SetStateRequest request;
            request.hash = context_.target;
            request.confirm = false;
            static_cast<void>(image_->set_state(
                request, guarded<ImageState>([](Impl& self, const Result<ImageState>& result) {
                    if (!result.has_value()) {
                        self.dispatch(failure(result.error()));
                        return;
                    }
                    self.context_.device = *result;
                    self.dispatch(plain(Event::Kind::MarkedForTest));
                })));
            return;
        }

        case Effect::Reset:
        case Effect::ForceReset: {
            ResetOptions options;
            options.force = effect == Effect::ForceReset;
            static_cast<void>(
                os_->reset(options, guarded<void>([](Impl& self, const Result<void>& result) {
                               if (!result.has_value()) {
                                   self.dispatch(failure(result.error()));
                                   return;
                               }
                               self.dispatch(plain(Event::Kind::ResetAccepted));
                           })));
            return;
        }

        case Effect::AwaitDisconnect: {
            grace_deadline_ = last_poll_ + plan_.disconnect_grace;
            UpdateEvent event;
            event.kind = UpdateEvent::Kind::DisconnectExpected;
            emit(event);
            return;
        }

        case Effect::RequestReconnect: {
            grace_deadline_.reset();
            UpdateEvent event;
            event.kind = UpdateEvent::Kind::ReconnectRequired;
            event.reconnect_hint = plan_.reconnect_hint;
            emit(event);
            return;
        }

        case Effect::RequestConfirmation: {
            UpdateEvent event;
            event.kind = UpdateEvent::Kind::ConfirmationRequired;
            emit(event);
            return;
        }

        case Effect::Confirm: {
            SetStateRequest request;
            request.confirm = true; // No hash: the running image is the target.
            static_cast<void>(image_->set_state(
                request, guarded<ImageState>([](Impl& self, const Result<ImageState>& result) {
                    if (!result.has_value()) {
                        self.dispatch(failure(result.error()));
                        return;
                    }
                    self.context_.device = *result;
                    self.dispatch(plain(Event::Kind::Confirmed));
                })));
            return;
        }

        case Effect::Finish:
            finish();
            return;
        }
    }

    void start_upload()
    {
        UploadOptions options = plan_.upload;
        options.image = plan_.image;
        if (!options.server_buf_size.has_value() && context_.buf_size != 0) {
            options.server_buf_size = context_.buf_size;
        }

        upload_ = image_->upload(
            *source_, options,
            [this](UploadProgress progress) {
                UpdateEvent event;
                event.kind = UpdateEvent::Kind::Progress;
                event.progress = progress;
                emit(event);
            },
            upload_done());

        // An invalid handle means `upload()` refused the request outright. Its
        // callback still reports why, on the next poll, so there is nothing to
        // dispatch from here.
    }

    [[nodiscard]] Callback<UploadResult> upload_done()
    {
        return guarded<UploadResult>([](Impl& self, const Result<UploadResult>& result) {
            if (!result.has_value()) {
                self.dispatch(failure(result.error()));
                return;
            }
            Event event;
            event.kind = Event::Kind::UploadFinished;
            event.transferred = result->transferred;
            self.dispatch(event);
        });
    }

    void finish()
    {
        running_ = false;
        grace_deadline_.reset();

        report_.final_state = state_;
        report_.bytes_transferred = context_.bytes_transferred;
        report_.upload_skipped = context_.upload_skipped;
        report_.target_hash = context_.target;
        report_.final_device_state = context_.device;
        report_.cause = context_.cause;
        report_.rolled_back = context_.rolled_back;
        report_.revert_pending = context_.revert_pending;

        Result<UpdateReport> outcome = report_;
        if (state_ != UpdateState::Completed) {
            outcome = unexpected<Error>{
                context_.cause.value_or(Error{ErrorCode::Cancelled, "updater: cancelled"})};
        }

        UpdateEvent event;
        event.kind = UpdateEvent::Kind::Finished;
        event.result = &outcome;
        emit(event);

        // Nothing may reach the application after `Finished`.
        on_event_ = {};
        source_ = nullptr;
        upload_ = UploadHandle{};
    }

    void emit(const UpdateEvent& event)
    {
        if (on_event_) {
            on_event_(event);
        }
    }

    /// Wraps a member handler so a callback outliving this object is inert
    /// rather than a dangling `this` -- the same guard the upload driver uses.
    template<class T, class Handler>
    [[nodiscard]] Callback<T> guarded(Handler handler)
    {
        return [life = std::weak_ptr<int>{life_}, this, handler](Result<T> result) {
            if (life.expired()) {
                return;
            }
            handler(*this, result);
        };
    }

    /// The file's MCUboot hash TLV.
    [[nodiscard]] static Result<ImageHash> read_target_hash(ImageSource& source)
    {
        std::array<std::byte, kMcubootHeaderSize> head{};
        const Result<std::size_t> read = source.read(0, MutBytes{head});
        if (!read.has_value()) {
            return unexpected<Error>{read.error()};
        }
        if (*read != head.size()) {
            return fail(ErrorCode::InvalidArgument, "updater: source shorter than an image header");
        }

        const Result<McubootImageInfo> info = parse_mcuboot_header(ConstBytes{head});
        if (!info.has_value()) {
            return unexpected<Error>{info.error()};
        }

        const Result<std::optional<ImageHash>> found = find_image_tlv_hash(source, *info);
        if (!found.has_value()) {
            return unexpected<Error>{found.error()};
        }
        // Bound once: the engaged state is then visible where the value is
        // read, to a reader and to static analysis alike.
        const std::optional<ImageHash>& hash = *found;
        if (!hash.has_value()) {
            // Without it there is no way to recognise the image in the device's
            // slot table, so every verification step would be guesswork.
            return fail(ErrorCode::InvalidArgument, "updater: image carries no hash TLV");
        }
        return *hash;
    }

    SmpClient* client_;
    ImageManagement* image_;
    OsManagement* os_;

    UpdatePlan plan_;
    ImageSource* source_ = nullptr;
    UpdateEventCallback on_event_;
    UploadHandle upload_;

    dfu::Context context_;
    UpdateReport report_;
    UpdateState state_ = UpdateState::Idle;
    bool running_ = false;

    std::optional<TimePoint> grace_deadline_;
    TimePoint last_poll_;

    /// Kept alive only while this object is; every callback holds a weak
    /// reference and does nothing once it expires.
    std::shared_ptr<int> life_ = std::make_shared<int>(0);
};

FirmwareUpdater::FirmwareUpdater(SmpClient& client, ImageManagement& image,
                                 OsManagement& os) noexcept
    : impl_{std::make_unique<Impl>(client, image, os)}
{}

FirmwareUpdater::~FirmwareUpdater()
{
    impl_->abandon();
}

Result<void> FirmwareUpdater::start(ImageSource& source, const UpdatePlan& plan,
                                    UpdateEventCallback on_event)
{
    return impl_->start(source, plan, std::move(on_event));
}

Result<void> FirmwareUpdater::confirm()
{
    return impl_->confirm();
}

void FirmwareUpdater::cancel() noexcept
{
    impl_->cancel();
}

Result<void> FirmwareUpdater::resume_after_reconnect()
{
    return impl_->resume_after_reconnect();
}

void FirmwareUpdater::reconnect_failed(Error error)
{
    impl_->reconnect_failed(std::move(error));
}

void FirmwareUpdater::poll(TimePoint now)
{
    impl_->poll(now);
}

std::optional<TimePoint> FirmwareUpdater::next_deadline() const noexcept
{
    return impl_->next_deadline();
}

UpdateState FirmwareUpdater::state() const noexcept
{
    return impl_->state();
}

const UpdateReport& FirmwareUpdater::report() const noexcept
{
    return impl_->report();
}

} // namespace smply
