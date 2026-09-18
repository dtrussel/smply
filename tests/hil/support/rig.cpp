// SPDX-License-Identifier: Apache-2.0

#include "support/rig.hpp"

#include "smply/error.hpp"
#include "smply/mcuboot_image.hpp"

#include <array>
#include <sstream>
#include <thread>
#include <utility>

namespace smply::hil {

// --- Timeline ---------------------------------------------------------------

void Timeline::note(std::string what)
{
    entries_.push_back(Entry{elapsed_ms(), std::move(what)});
}

std::int64_t Timeline::elapsed_ms() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 started_)
        .count();
}

void Timeline::metric(std::string_view name, std::int64_t value)
{
    metrics_.emplace_back(std::string{name}, value);
}

std::string Timeline::dump() const
{
    std::ostringstream out;
    for (const Entry& entry : entries_) {
        out << "  [" << entry.ms << " ms] " << entry.what << '\n';
    }
    for (const auto& [name, value] : metrics_) {
        out << "HIL-METRIC " << name << '=' << value << '\n';
    }
    return out.str();
}

// --- Rig --------------------------------------------------------------------

Rig::Rig(const Bench& bench)
    : bench_{bench}, inbound_{[this] {
          // Runs on a WinRT thread-pool thread, inside post(). Signal and return.
          wake_up();
      }}
{}

Rig::~Rig() = default;

void Rig::wake_up()
{
    {
        const std::lock_guard<std::mutex> lock{wake_mutex_};
        woken_ = true;
    }
    wake_.notify_one();
}

Result<void> Rig::connect()
{
    // Close the link we are replacing before opening another. The contract
    // requires a transport *object* to outlive every client bound to it
    // (smply/transport.hpp) -- it does not require the link to stay open, and
    // leaving one open means a GattSession with MaintainConnection(true)
    // outliving the reconnect. Whether that contributes to Windows handing back
    // a service with no characteristic (protocol-notes section 9, A22) could not
    // be answered on this bench, because that behaviour did not reproduce; the
    // hygiene is right either way, and close() is idempotent so a link a case
    // already dropped is untouched.
    if (!links_.empty()) {
        links_.back()->close();
    }

    const auto t0 = std::chrono::steady_clock::now();
    Result<std::unique_ptr<transport::WinRtBleTransport>> link =
        transport::WinRtBleTransport::connect(bench_.address, inbound_);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    if (!link.has_value()) {
        timeline_.note("connect failed after " + std::to_string(took.count()) +
                       " ms: " + to_string(link.error()));
        return fail(link.error());
    }
    links_.push_back(std::move(*link));
    if (!client_.has_value()) {
        // The SMP version comes from the bench, not from the default, so a
        // whole group of cases can be re-run in v2 by setting one environment
        // variable (bench.hpp; O2).
        client_.emplace(*links_.back(), system_clock(),
                        SmpClientConfig{.smp_version = bench_.smp_version});
        images_.emplace(*client_);
        os_.emplace(*client_);
    } else {
        client_->rebind_transport(*links_.back());
    }
    timeline_.note("connected in " + std::to_string(took.count()) + " ms (link " +
                   std::to_string(links_.size()) + ")");
    return {};
}

Result<void> Rig::reconnect(const dfu_app::ReconnectSettings& settings)
{
    dfu_app::ReconnectPolicy policy{settings};
    policy.begin();
    while (!policy.exhausted()) {
        std::this_thread::sleep_for(policy.next_delay());
        timeline_.note("reconnect attempt " + std::to_string(policy.attempts()));
        if (connect().has_value()) {
            policy.succeeded();
            return {};
        }
    }
    timeline_.note("reconnect gave up");
    return fail(ErrorCode::Disconnected, "hil: could not reconnect");
}

void Rig::drop_link()
{
    if (links_.empty()) {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    links_.back()->close();
    last_close_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    timeline_.note("link closed by the application; close() took " +
                   std::to_string(last_close_.count()) + " ms");
}

Duration Rig::last_close_duration() const noexcept
{
    return last_close_;
}

void Rig::record_send_counters()
{
    transport::SendCounters total;
    for (const auto& link : links_) {
        const transport::SendCounters one = link->send_counters();
        total.deferred += one.deferred;
        total.refused += one.refused;
    }
    timeline_.metric("deferred_sends", static_cast<std::int64_t>(total.deferred));
    timeline_.metric("refused_sends", static_cast<std::int64_t>(total.refused));
}

Timeline& Rig::timeline() noexcept
{
    return timeline_;
}

const SmpClientStats& Rig::stats() const noexcept
{
    return client_->stats();
}

ImageManagement& Rig::images() noexcept
{
    return *images_;
}

bool Rig::connected() const noexcept
{
    return client_.has_value() && client_->connected();
}

const std::vector<UpdateState>& Rig::states() const noexcept
{
    return states_;
}

// --- the pump ----------------------------------------------------------------

void Rig::pump_step()
{
    inbound_.drain();
    const TimePoint now = std::chrono::steady_clock::now();
    client_->poll(now);
    if (updater_.has_value()) {
        updater_->poll(now);
    }
}

void Rig::pump_wait(std::optional<TimePoint> until)
{
    std::optional<TimePoint> deadline = client_->next_deadline();
    if (updater_.has_value()) {
        if (const auto theirs = updater_->next_deadline();
            theirs.has_value() && (!deadline.has_value() || *theirs < *deadline)) {
            deadline = theirs;
        }
    }
    if (until.has_value() && (!deadline.has_value() || *until < *deadline)) {
        deadline = until;
    }

    std::unique_lock<std::mutex> lock{wake_mutex_};
    if (deadline.has_value()) {
        wake_.wait_until(lock, *deadline, [this] { return woken_; });
    } else {
        wake_.wait_for(lock, std::chrono::milliseconds{50}, [this] { return woken_; });
    }
    woken_ = false;
}

template<class Pred>
bool Rig::pump_until(Pred&& done, Duration limit)
{
    // Drain and poll, *then* look, and only then sleep: a completion that a
    // poll() just delivered must be seen before the pump waits for the next
    // deadline, or every request costs a whole timeout. The first version of
    // this loop waited first and made a 26 s update take five minutes.
    const TimePoint until = std::chrono::steady_clock::now() + limit;
    while (true) {
        pump_step();
        if (done()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        pump_wait(until);
    }
}

template<class T>
Result<T> Rig::await(const std::function<void(Callback<T>)>& issue, Duration limit,
                     const char* what)
{
    std::optional<Result<T>> out;
    issue([&out](Result<T> result) { out = std::move(result); });
    if (!pump_until([&out] { return out.has_value(); }, limit)) {
        timeline_.note(std::string{what} + ": no completion within the case's deadline");
        return fail(ErrorCode::Timeout, "hil: the operation did not complete in time");
    }
    if (out->has_value()) {
        timeline_.note(std::string{what} + " ok");
    } else {
        timeline_.note(std::string{what} + " failed: " + to_string(out->error()));
    }
    return std::move(*out);
}

// --- requests ----------------------------------------------------------------

Result<ImageState> Rig::read_state(Duration limit)
{
    return await<ImageState>(
        [this](Callback<ImageState> cb) { static_cast<void>(images_->get_state(std::move(cb))); },
        limit, "image state read");
}

Result<ImageState> Rig::set_state(const SetStateRequest& request, Duration limit)
{
    return await<ImageState>(
        [this, &request](Callback<ImageState> cb) {
            static_cast<void>(images_->set_state(request, std::move(cb)));
        },
        limit, request.confirm ? "image confirm" : "image test");
}

Result<SlotInfo> Rig::slot_info(Duration limit)
{
    return await<SlotInfo>(
        [this](Callback<SlotInfo> cb) { static_cast<void>(images_->get_slot_info(std::move(cb))); },
        limit, "slot info");
}

Result<McumgrParameters> Rig::params(Duration limit)
{
    return await<McumgrParameters>(
        [this](Callback<McumgrParameters> cb) {
            static_cast<void>(os_->mcumgr_parameters(std::move(cb)));
        },
        limit, "mcumgr parameters");
}

Result<std::string> Rig::echo(std::string_view text, Duration limit)
{
    return await<std::string>(
        [this, text](Callback<std::string> cb) {
            static_cast<void>(os_->echo(text, std::move(cb)));
        },
        limit, "echo");
}

Result<void> Rig::reset(Duration limit)
{
    return await<void>([this](Callback<void> cb) { static_cast<void>(os_->reset(std::move(cb))); },
                       limit, "reset");
}

Result<void> Rig::erase(std::optional<std::uint32_t> slot, Duration limit)
{
    EraseOptions options;
    options.slot = slot;
    return await<void>(
        [this, options](Callback<void> cb) {
            static_cast<void>(images_->erase(options, std::move(cb)));
        },
        limit, "erase");
}

bool Rig::wait_disconnected(Duration limit)
{
    const auto t0 = timeline_.elapsed_ms();
    const bool dropped = pump_until([this] { return !client_->connected(); }, limit);
    if (dropped) {
        timeline_.note("the client saw the link drop after " +
                       std::to_string(timeline_.elapsed_ms() - t0) + " ms");
        timeline_.metric("disconnect_seen_ms", timeline_.elapsed_ms() - t0);
    } else {
        timeline_.note("the link did not drop within the deadline");
    }
    return dropped;
}

// --- uploads and updates -------------------------------------------------------

UploadOutcome Rig::upload(ImageSource& source, const UploadOptions& options,
                          UploadInterrupt interrupt, Duration limit)
{
    UploadOutcome outcome;
    std::optional<Result<UploadResult>> done;
    const auto t0 = timeline_.elapsed_ms();
    progress_.clear();
    outcome.handle = images_->upload(
        source, options, [this](UploadProgress progress) { progress_.push_back(progress); },
        [&done](Result<UploadResult> result) { done = std::move(result); });
    timeline_.note("upload started");

    // The pump, with the interruption applied from here rather than from the
    // progress callback (see UploadInterrupt).
    const TimePoint until = std::chrono::steady_clock::now() + limit;
    bool interrupted = false;
    while (true) {
        pump_step();
        if (done.has_value()) {
            break;
        }
        if (!interrupted && interrupt.action != UploadInterrupt::Action::None &&
            !progress_.empty()) {
            const UploadProgress& last = progress_.back();
            if (last.total != 0 && static_cast<double>(last.transferred) >=
                                       interrupt.at_fraction * static_cast<double>(last.total)) {
                interrupted = true;
                if (interrupt.action == UploadInterrupt::Action::DropLink) {
                    timeline_.note("interrupting: the application drops the link at " +
                                   std::to_string(last.transferred) + " bytes");
                    drop_link();
                } else {
                    timeline_.note("interrupting: the application cancels at " +
                                   std::to_string(last.transferred) + " bytes");
                    images_->cancel(outcome.handle);
                }
            }
        }
        if (std::chrono::steady_clock::now() >= until) {
            timeline_.note("upload: no completion within the case's deadline");
            outcome.result = fail(ErrorCode::Timeout, "hil: the upload did not complete in time");
            return outcome;
        }
        pump_wait(until);
    }
    outcome.result = std::move(*done);
    outcome.progress = progress_;
    timeline_.metric("upload_ms", timeline_.elapsed_ms() - t0);
    if (outcome.result.has_value()) {
        timeline_.note("upload complete: " + std::to_string(outcome.result->transferred) +
                       " bytes" + (outcome.result->already_present ? ", already present" : ""));
    } else {
        timeline_.note("upload ended: " + to_string(outcome.result.error()));
    }
    return outcome;
}

Result<UploadResult> Rig::resume(UploadOutcome& outcome, Duration limit)
{
    outcome.progress.clear();
    std::optional<Result<UploadResult>> done;
    // The progress callback registered by upload() still points at
    // outcome.progress, which is why the caller passes the same outcome.
    images_->resume(outcome.handle,
                    [&done](Result<UploadResult> result) { done = std::move(result); });
    timeline_.note("upload resumed");
    if (!pump_until([&done] { return done.has_value(); }, limit)) {
        return fail(ErrorCode::Timeout, "hil: the resumed upload did not complete in time");
    }
    outcome.progress = progress_;
    if (done->has_value()) {
        timeline_.note("resumed upload complete: " + std::to_string((*done)->transferred) +
                       " bytes");
    } else {
        timeline_.note("resumed upload ended: " + to_string(done->error()));
    }
    return std::move(*done);
}

Result<UpdateReport> Rig::update(ImageSource& source, const UpdatePlan& plan,
                                 const UpdateHooks& hooks)
{
    states_.clear();
    updater_.emplace(*client_, *images_, *os_);

    struct Pending
    {
        bool reconnect = false;
        bool confirm = false;
        bool finished = false;
    } pending;

    Result<UpdateReport> outcome = fail(ErrorCode::InvalidState, "hil: no result");
    const auto t0 = timeline_.elapsed_ms();

    const Result<void> begun = updater_->start(source, plan, [&](const UpdateEvent& event) {
        switch (event.kind) {
        case UpdateEvent::Kind::StateChanged:
            states_.push_back(event.to);
            timeline_.note(std::string{"update: "} + std::string{to_string(event.to)});
            break;
        case UpdateEvent::Kind::Progress:
            if (hooks.on_progress) {
                hooks.on_progress(event.progress);
            }
            break;
        case UpdateEvent::Kind::DisconnectExpected:
            timeline_.note("update: the device is about to reboot");
            break;
        case UpdateEvent::Kind::ReconnectRequired:
            pending.reconnect = true;
            break;
        case UpdateEvent::Kind::ConfirmationRequired:
            pending.confirm = true;
            break;
        case UpdateEvent::Kind::Finished:
            outcome = *event.result;
            pending.finished = true;
            break;
        }
    });
    if (!begun.has_value()) {
        return fail(begun.error());
    }

    const TimePoint until = std::chrono::steady_clock::now() + hooks.deadline;
    while (true) {
        pump_step();
        if (pending.finished) {
            break;
        }
        if (std::chrono::steady_clock::now() >= until) {
            timeline_.note("update: the case's deadline passed");
            updater_->cancel();
            return fail(ErrorCode::Timeout, "hil: the update did not finish in time");
        }

        if (pending.reconnect) {
            pending.reconnect = false;
            if (hooks.on_await_reconnect) {
                hooks.on_await_reconnect();
            }
            const auto t_reconnect = timeline_.elapsed_ms();
            if (reconnect(hooks.reconnect).has_value()) {
                timeline_.metric("reconnect_ms", timeline_.elapsed_ms() - t_reconnect);
                static_cast<void>(updater_->resume_after_reconnect());
            } else {
                timeline_.metric("give_up_ms", timeline_.elapsed_ms() - t_reconnect);
                updater_->reconnect_failed(
                    Error{ErrorCode::Disconnected, "hil: could not reconnect"});
            }
            continue;
        }
        if (pending.confirm) {
            pending.confirm = false;
            if (hooks.auto_confirm) {
                timeline_.note("update: confirming");
                static_cast<void>(updater_->confirm());
            }
            continue;
        }
        pump_wait(until);
    }
    timeline_.metric("update_ms", timeline_.elapsed_ms() - t0);
    return outcome;
}

Result<ImageHash> image_hash_of(ImageSource& source)
{
    std::array<std::byte, 32> head{};
    const Result<std::size_t> read = source.read(0, MutBytes{head});
    if (!read.has_value()) {
        return fail(read.error());
    }
    if (*read != head.size()) {
        return fail(ErrorCode::InvalidArgument, "hil: the image is shorter than a header");
    }
    const Result<McubootImageInfo> info = parse_mcuboot_header(ConstBytes{head});
    if (!info.has_value()) {
        return fail(info.error());
    }
    const Result<std::optional<ImageHash>> found = find_image_tlv_hash(source, *info);
    if (!found.has_value()) {
        return fail(found.error());
    }
    if (!found->has_value()) {
        return fail(ErrorCode::InvalidArgument, "hil: the image carries no hash TLV");
    }
    return **found;
}

} // namespace smply::hil
