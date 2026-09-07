// SPDX-License-Identifier: Apache-2.0

/// \file
/// A console DFU driver: the canonical pump loop, against a device on another
/// thread.
///
/// **This is the file to read.** The other four are scaffolding -- a stub device
/// to talk to, a link to talk over, an image to install and a file to read it
/// from. What is demonstrated here is the arrangement every application that
/// uses smply has to build:
///
/// * **one client context**, the thread running this loop. Every call into the
///   library and every callback out of it happens here (ADR-0004).
/// * **a `Dispatcher`** carrying inbound bytes from the driver thread to this
///   one. The library never learns there was another thread.
/// * **an application-driven pump**: nothing happens unless `poll()` is called,
///   so there is no hidden thread, no hidden queue and no callback from
///   somewhere surprising (ADR-0003).
/// * **the application owning the connection**. A reset drops the link by
///   design; re-establishing it is not the library's business, so the updater
///   asks and waits.
///
/// Run it with no arguments and it invents a device and an image to install.

#include "demo_image.hpp"
#include "loopback_transport.hpp"
#include "stub_device.hpp"

#include "dfu_app/file_image_source.hpp"
#include "dfu_app/reconnect_policy.hpp"

#include "smply/clock.hpp"
#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/image_source.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"
#include "smply/util/dispatcher.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace smply;          // NOLINT(google-build-using-namespace) -- an example
using namespace smply::example; // NOLINT(google-build-using-namespace)
using namespace smply::dfu_app; // NOLINT(google-build-using-namespace)

struct Options
{
    std::string image_path; ///< Empty means "invent one".
    UpdateMode mode = UpdateMode::TestThenConfirm;
    bool quiet = false;

    /// Refuse this many reconnection attempts before letting one succeed.
    ///
    /// Not a toy. A device that has just rebooted is not immediately
    /// connectable, so every real application retries with a backoff and gives
    /// up eventually -- and against an in-process stub that reconnects
    /// instantly, none of that code would ever run. Setting this above the
    /// policy's attempt budget exercises the give-up path, which ends the
    /// update through `FirmwareUpdater::reconnect_failed()`.
    unsigned flaky_reconnect = 0;
};

[[nodiscard]] bool parse_mode(std::string_view text, UpdateMode& out)
{
    if (text == "test-then-confirm") {
        out = UpdateMode::TestThenConfirm;
        return true;
    }
    if (text == "confirm-immediately") {
        out = UpdateMode::ConfirmImmediately;
        return true;
    }
    if (text == "upload-only") {
        out = UpdateMode::UploadOnly;
        return true;
    }
    return false;
}

void usage()
{
    std::cerr << "usage: cli_dfu [--image PATH] [--mode MODE] [--quiet]\n"
                 "               [--flaky-reconnect N]\n"
                 "  --image PATH  firmware to install; without it, a demo image is generated\n"
                 "  --mode MODE   test-then-confirm (default) | confirm-immediately | upload-only\n"
                 "  --quiet       print only the outcome\n"
                 "  --flaky-reconnect N  refuse N reconnection attempts before succeeding,\n"
                 "                to exercise the backoff; above the attempt budget the\n"
                 "                update gives up, which is also worth demonstrating\n";
}

[[nodiscard]] bool parse_arguments(int argc, char** argv, Options& out)
{
    const std::vector<std::string> args{argv + 1, argv + argc};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--quiet") {
            out.quiet = true;
        } else if (arg == "--image" && i + 1 < args.size()) {
            out.image_path = args[++i];
        } else if (arg == "--mode" && i + 1 < args.size()) {
            if (!parse_mode(args[++i], out.mode)) {
                return false;
            }
        } else if (arg == "--flaky-reconnect" && i + 1 < args.size()) {
            const std::string& count = args[++i];
            if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos) {
                return false;
            }
            out.flaky_reconnect = static_cast<unsigned>(std::stoul(count));
        } else {
            return false;
        }
    }
    return true;
}

/// Writes the generated image somewhere `FileImageSource` can open it, so the
/// update reads through a real file even in the no-arguments case.
///
/// Into the temporary directory rather than the working one: this runs as a
/// `ctest` test, and a test that drops files into the build tree is a test that
/// makes the next build's diff noisy.
[[nodiscard]] std::optional<std::string> write_demo_image(const std::vector<std::byte>& image)
{
    std::error_code ec;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::nullopt;
    }
    const std::string path = (directory / "cli_dfu_demo_image.bin").string();

    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return std::nullopt;
    }
    // ostream speaks char, and these are bytes this process just built. The
    // marker has to be the last comment line before the code, or it silences
    // the comment instead.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
    return out ? std::optional<std::string>{path} : std::nullopt;
}

/// The application's side of the loop: what it has been asked to do next.
struct Pending
{
    bool reconnect = false;
    bool confirm = false;
    bool finished = false;
};

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_arguments(argc, argv, options)) {
        usage();
        return 2;
    }

    // --- the "device", and the firmware to put on it ------------------------

    const std::vector<std::byte> running = build_demo_image(DemoVersion{.major = 1});
    std::string image_path = options.image_path;
    if (image_path.empty()) {
        const std::vector<std::byte> update = build_demo_image(DemoVersion{.major = 2});
        const std::optional<std::string> written = write_demo_image(update);
        if (!written.has_value()) {
            std::cerr << "cli_dfu: cannot write the demo image\n";
            return 1;
        }
        image_path = *written;
    }

    Result<FileImageSource> source = FileImageSource::open(image_path);
    if (!source.has_value()) {
        std::cerr << "cli_dfu: " << to_string(source.error()) << '\n';
        return 1;
    }

    // --- the pump's wake-up, and the marshalling queue -----------------------
    //
    // Declared before the client and the transports: everything below captures
    // them, and a callback outliving what it captured is the lifetime bug this
    // library's documentation warns about most.

    std::mutex wake_mutex;
    std::condition_variable wake;
    bool woken = false;

    Dispatcher inbound{[&] {
        // Runs on the *device* thread, inside post(). Signal and return: doing
        // work here, or taking a lock the client context holds, is how an
        // adapter deadlocks.
        {
            const std::lock_guard<std::mutex> lock{wake_mutex};
            woken = true;
        }
        wake.notify_one();
    }};

    Pending pending;
    StubDevice device{running};

    // Deliberately brisk: these delays are waited for real, and this example
    // runs as a ctest with a timeout. A shipped tool would use the defaults
    // (500 ms doubling to 8 s), which is what examples/winrt_ble_dfu/ does.
    ReconnectPolicy policy{ReconnectSettings{
        .first_delay = std::chrono::milliseconds{20},
        .max_delay = std::chrono::milliseconds{160},
        .max_attempts = 5,
    }};
    unsigned refusals_left = options.flaky_reconnect;

    // The application owns every link it ever opens. A dropped transport is
    // never reused -- like a real one -- but it must outlive the client, which
    // detaches from it on destruction and on rebind.
    std::vector<std::unique_ptr<LoopbackTransport>> links;
    links.push_back(std::make_unique<LoopbackTransport>(device, inbound));
    device.attach(*links.back());

    SmpClient client{*links.back()};
    ImageManagement images{client};
    OsManagement os{client};
    FirmwareUpdater updater{client, images, os};

    // --- the update ---------------------------------------------------------

    UpdatePlan plan;
    plan.mode = options.mode;

    Result<UpdateReport> outcome = fail(ErrorCode::InvalidState, "no result");
    const auto started = std::chrono::steady_clock::now();

    const Result<void> begun = updater.start(*source, plan, [&](const UpdateEvent& event) {
        switch (event.kind) {
        case UpdateEvent::Kind::StateChanged:
            if (!options.quiet) {
                std::cout << "  " << to_string(event.to) << '\n';
            }
            break;
        case UpdateEvent::Kind::Progress:
            if (!options.quiet && event.progress.total != 0) {
                std::cout << "\r  uploading " << event.progress.transferred << '/'
                          << event.progress.total << " bytes" << std::flush;
                if (event.progress.transferred == event.progress.total) {
                    std::cout << '\n';
                }
            }
            break;
        case UpdateEvent::Kind::DisconnectExpected:
            if (!options.quiet) {
                std::cout << "  the device is about to reboot; a dropped link is expected\n";
            }
            break;
        case UpdateEvent::Kind::ReconnectRequired:
            // Not done here. The handler runs inside poll(), and reconnecting
            // is the application's own work -- so it is noted and done on the
            // loop's next turn, where it reads as what it is.
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
        std::cerr << "cli_dfu: " << to_string(begun.error()) << '\n';
        return 1;
    }

    // --- the pump -----------------------------------------------------------

    constexpr auto kOverallTimeout = std::chrono::seconds{30};

    while (!pending.finished) {
        inbound.drain();

        const TimePoint now = std::chrono::steady_clock::now();
        client.poll(now);
        updater.poll(now);

        if (pending.reconnect) {
            pending.reconnect = false;

            // A real reconnect is a loop, not a statement. The device has just
            // rebooted and is not connectable yet, so an application waits,
            // tries, waits longer, and eventually decides it has lost the
            // device. `--flaky-reconnect` makes that visible here; over BLE it
            // is simply what happens.
            policy.begin();
            bool attached = false;
            while (!policy.exhausted()) {
                const Duration delay = policy.next_delay();
                std::this_thread::sleep_for(delay);

                if (refusals_left > 0) {
                    --refusals_left;
                    if (!options.quiet) {
                        std::cout << "  reconnect attempt " << policy.attempts()
                                  << " failed; waiting " << delay.count() << " ms\n";
                    }
                    continue;
                }

                // A dropped link stays dropped, so this is a new one -- exactly
                // what an application does with a BLE connection after a reboot.
                links.push_back(std::make_unique<LoopbackTransport>(device, inbound));
                device.attach(*links.back());
                client.rebind_transport(*links.back());
                policy.succeeded();
                attached = true;
                break;
            }

            if (!attached) {
                // Terminal, and the updater has to be told: it is waiting on
                // the application and has no deadline of its own here, so
                // without this the pump would spin until the overall timeout.
                if (!options.quiet) {
                    std::cout << "  giving up after " << policy.settings().max_attempts
                              << " reconnection attempts\n";
                }
                updater.reconnect_failed(
                    Error{ErrorCode::Disconnected, "cli_dfu: could not reconnect"});
                continue;
            }

            if (!options.quiet) {
                std::cout << "  reconnected\n";
            }
            static_cast<void>(updater.resume_after_reconnect());
            continue;
        }

        if (pending.confirm) {
            pending.confirm = false;
            // The device is running the new image, unconfirmed. This is where a
            // real application runs its self-test; doing nothing here would let
            // the device revert on its next reset, which is the point of the
            // default mode (ADR-0014).
            if (!options.quiet) {
                std::cout << "  the new image is running unconfirmed; confirming\n";
            }
            static_cast<void>(updater.confirm());
            continue;
        }

        if (std::chrono::steady_clock::now() - started > kOverallTimeout) {
            std::cerr << "cli_dfu: gave up waiting\n";
            return 1;
        }

        // Sleep until there is something to do: a deadline, or a wake from the
        // device thread. This is api.md's `app.wait_until(client.next_deadline())`.
        std::optional<TimePoint> deadline = client.next_deadline();
        if (const std::optional<TimePoint> theirs = updater.next_deadline();
            theirs.has_value() && (!deadline.has_value() || *theirs < *deadline)) {
            deadline = theirs;
        }

        std::unique_lock<std::mutex> lock{wake_mutex};
        if (deadline.has_value()) {
            wake.wait_until(lock, *deadline, [&] { return woken; });
        } else {
            // No deadline at all means the library is waiting on the
            // application -- during a reboot, say. Cap the wait so the overall
            // timeout above stays reachable.
            wake.wait_for(lock, std::chrono::milliseconds{50}, [&] { return woken; });
        }
        woken = false;
    }

    // --- the report ---------------------------------------------------------

    if (!outcome.has_value()) {
        std::cerr << "cli_dfu: update failed: " << to_string(outcome.error()) << '\n';
        return 1;
    }

    const UpdateReport& report = *outcome;
    std::cout << "update " << to_string(report.final_state) << '\n';
    std::cout << "  bytes transferred: " << report.bytes_transferred
              << (report.upload_skipped ? " (the device already held this image)" : "") << '\n';
    if (report.rolled_back) {
        std::cout << "  the device reverted to its previous image\n";
    }
    if (report.revert_pending) {
        std::cout << "  a swap is scheduled but unconfirmed: it will revert on the next reset\n";
    }

    return report.final_state == UpdateState::Completed ? 0 : 1;
}
