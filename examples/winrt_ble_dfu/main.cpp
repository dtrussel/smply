// SPDX-License-Identifier: Apache-2.0

/// \file
/// A console DFU tool over Bluetooth LE, on Windows.
///
/// **Read `examples/cli_dfu/main.cpp` first.** It is the same pump loop against
/// a stub device in this process, it runs in CI on every push, and it is the
/// one that has actually been executed. This file is that arrangement pointed
/// at a real radio, and it differs in exactly three ways -- each of which is
/// what a real application has to deal with and a stub cannot show you:
///
/// * **the device has to be found** before anything else happens (`scanner.*`);
/// * **reconnection is a loop, not a statement.** After the reset the device is
///   gone for a second or two and the first attempts fail. The backoff lives in
///   `smply::dfu_app::ReconnectPolicy`, shared with `cli_dfu` so that CI
///   exercises it on Linux -- none of the code in *this* file is ever run by
///   CI, so anything that can live over there does;
/// * **giving up is a real outcome.** `reconnect_failed()` ends the update
///   rather than leaving the pump spinning.
///
/// The pump loop itself is written out here rather than shared, deliberately.
/// An example exists to be read, and a reader of this file should see the loop.
///
/// **This program has never been run.** It is compiled by CI at `/W4 /WX` on a
/// runner with no Bluetooth radio. See the README.

#include "scanner.hpp"
#include "winrt_prelude.hpp"

#include "dfu_app/file_image_source.hpp"
#include "dfu_app/reconnect_policy.hpp"

#include "winrt_ble/winrt_ble_transport.hpp"

#include "smply/clock.hpp"
#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"
#include "smply/util/dispatcher.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace smply;          // NOLINT(google-build-using-namespace) -- an example
using namespace smply::example; // NOLINT(google-build-using-namespace)
using namespace smply::dfu_app; // NOLINT(google-build-using-namespace)

using smply::transport::WinRtBleTransport;

/// Exit codes, so a script can tell the cases apart. Documented in the README.
enum ExitCode : int
{
    kOk = 0,
    kUpdateFailed = 1,
    kUsage = 2,
    kNoDevice = 3,
    kReconnectFailed = 4,
};

struct Options
{
    std::string image_path; ///< Required: see the note in main().
    std::string name;       ///< --name: match a substring of the advertised name.
    std::string address;    ///< --address: skip scanning entirely.
    UpdateMode mode = UpdateMode::TestThenConfirm;
    std::chrono::milliseconds scan_timeout{std::chrono::seconds{10}};
    bool quiet = false;
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
    std::cerr << "usage: winrt_ble_dfu --image PATH [--name NAME | --address ADDR]\n"
                 "                     [--mode MODE] [--scan-timeout MS] [--quiet]\n"
                 "  --image PATH   the firmware to install (required)\n"
                 "  --name NAME    connect to the first device whose advertised name\n"
                 "                 contains NAME; without it, the first device that\n"
                 "                 advertises the SMP service is used\n"
                 "  --address ADDR AA:BB:CC:DD:EE:FF, or bare hex -- skips scanning\n"
                 "  --mode MODE    test-then-confirm (default) | confirm-immediately |\n"
                 "                 upload-only\n"
                 "  --scan-timeout MS  how long to look for a device (default 10000)\n"
                 "  --quiet        print only the outcome\n"
                 "\n"
                 "exit: 0 ok, 1 update failed, 2 usage, 3 no device, 4 reconnect failed\n";
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
        } else if (arg == "--name" && i + 1 < args.size()) {
            out.name = args[++i];
        } else if (arg == "--address" && i + 1 < args.size()) {
            out.address = args[++i];
        } else if (arg == "--scan-timeout" && i + 1 < args.size()) {
            const std::string& ms = args[++i];
            if (ms.empty() || ms.find_first_not_of("0123456789") != std::string::npos) {
                return false;
            }
            out.scan_timeout = std::chrono::milliseconds{std::stoul(ms)};
        } else if (arg == "--mode" && i + 1 < args.size()) {
            if (!parse_mode(args[++i], out.mode)) {
                return false;
            }
        } else {
            return false;
        }
    }
    // Unlike cli_dfu, there is no "invent an image" path. That example writes a
    // synthetic image to a stub in its own process; writing one to real
    // hardware would install firmware that does not run.
    return !out.image_path.empty() && !(!out.name.empty() && !out.address.empty());
}

/// What the update has asked the application to do next.
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
        return kUsage;
    }

    // Multi-threaded, and not negotiable: WinRtBleTransport::connect() and the
    // scanner both block on WinRT asynchronous operations, which deadlocks on a
    // single-threaded apartment -- silently, with no diagnostic at all.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    Result<FileImageSource> source = FileImageSource::open(options.image_path);
    if (!source.has_value()) {
        std::cerr << "winrt_ble_dfu: " << to_string(source.error()) << '\n';
        return kUpdateFailed;
    }

    // --- find the device ----------------------------------------------------

    std::uint64_t address = 0;
    if (!options.address.empty()) {
        const Result<std::uint64_t> parsed = parse_address(options.address);
        if (!parsed.has_value()) {
            std::cerr << "winrt_ble_dfu: " << to_string(parsed.error()) << '\n';
            return kUsage;
        }
        address = *parsed;
    } else {
        if (!options.quiet) {
            std::cout << "scanning\n";
        }
        const Result<std::uint64_t> found =
            find_device(ScanFilter{.name = options.name, .timeout = options.scan_timeout});
        if (!found.has_value()) {
            std::cerr << "winrt_ble_dfu: " << to_string(found.error()) << '\n';
            return kNoDevice;
        }
        address = *found;
    }
    if (!options.quiet) {
        std::cout << "connecting to " << format_address(address) << '\n';
    }

    // --- the pump's wake-up, and the marshalling queue -----------------------
    //
    // Declared before the client and the transports, because everything below
    // captures them and a callback outliving what it captured is the lifetime
    // bug this library warns about most.

    std::mutex wake_mutex;
    std::condition_variable wake;
    bool woken = false;

    Dispatcher inbound{[&] {
        // Runs on a WinRT thread-pool thread, inside post(). Signal and return.
        {
            const std::lock_guard<std::mutex> lock{wake_mutex};
            woken = true;
        }
        wake.notify_one();
    }};

    // Every link ever opened is kept: a transport must outlive every client
    // bound to it, and the client detaches from the old one on rebind.
    std::vector<std::unique_ptr<WinRtBleTransport>> links;
    {
        Result<std::unique_ptr<WinRtBleTransport>> link =
            WinRtBleTransport::connect(address, inbound);
        if (!link.has_value()) {
            std::cerr << "winrt_ble_dfu: " << to_string(link.error()) << '\n';
            return kNoDevice;
        }
        links.push_back(std::move(*link));
    }

    Pending pending;
    ReconnectPolicy policy; // the defaults: 500 ms doubling to 8 s, six attempts

    SmpClient client{*links.back()};
    ImageManagement images{client};
    OsManagement os{client};
    FirmwareUpdater updater{client, images, os};

    // --- the update ---------------------------------------------------------

    UpdatePlan plan;
    plan.mode = options.mode;

    Result<UpdateReport> outcome = fail(ErrorCode::InvalidState, "no result");

    // Every line is stamped with the milliseconds since the update began. On a
    // real link that is the only way to see *where* the time goes -- the
    // device's own work on the final chunk, the reboot, the reconnect -- and it
    // is what P17 measured the reconnect policy from.
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };

    const Result<void> begun = updater.start(*source, plan, [&](const UpdateEvent& event) {
        switch (event.kind) {
        case UpdateEvent::Kind::StateChanged:
            if (!options.quiet) {
                std::cout << "  [" << elapsed_ms() << " ms] " << to_string(event.to) << '\n';
            }
            break;
        case UpdateEvent::Kind::Progress:
            if (!options.quiet && event.progress.total != 0) {
                std::cout << "\r  [" << elapsed_ms() << " ms] uploading "
                          << event.progress.transferred << '/' << event.progress.total << " bytes"
                          << std::flush;
                if (event.progress.transferred == event.progress.total) {
                    std::cout << '\n';
                }
            }
            break;
        case UpdateEvent::Kind::DisconnectExpected:
            if (!options.quiet) {
                std::cout << "  the device is about to reboot\n";
            }
            break;
        case UpdateEvent::Kind::ReconnectRequired:
            // Noted, not done here: this handler runs inside poll(), and
            // reconnecting is the application's own work.
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
        std::cerr << "winrt_ble_dfu: " << to_string(begun.error()) << '\n';
        return kUpdateFailed;
    }

    // --- the pump -----------------------------------------------------------

    bool gave_up_reconnecting = false;

    while (!pending.finished) {
        inbound.drain();

        const TimePoint now = std::chrono::steady_clock::now();
        client.poll(now);
        updater.poll(now);

        if (pending.reconnect) {
            pending.reconnect = false;

            // The device rebooted; it is not connectable yet. Wait, try, wait
            // longer -- and eventually decide it is gone. This is the loop the
            // stub in cli_dfu cannot show, and the reason ReconnectPolicy is
            // shared code with tests rather than written out here.
            policy.begin();
            bool attached = false;
            while (!policy.exhausted()) {
                std::this_thread::sleep_for(policy.next_delay());
                if (!options.quiet) {
                    std::cout << "  reconnect attempt " << policy.attempts() << '\n';
                }

                Result<std::unique_ptr<WinRtBleTransport>> link =
                    WinRtBleTransport::connect(address, inbound);
                if (!link.has_value()) {
                    continue;
                }

                links.push_back(std::move(*link));
                client.rebind_transport(*links.back());
                policy.succeeded();
                attached = true;
                break;
            }

            if (!attached) {
                // Terminal. The updater is waiting on the application and has
                // no deadline of its own here, so without this the pump would
                // spin until something else gave out.
                gave_up_reconnecting = true;
                updater.reconnect_failed(
                    Error{ErrorCode::Disconnected, "winrt_ble_dfu: could not reconnect"});
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
            // The device is running the new image, unconfirmed. A real tool
            // runs its self-test here; declining to confirm lets MCUboot revert
            // on the next reset, which is the point of the default mode
            // (ADR-0014).
            if (!options.quiet) {
                std::cout << "  the new image is running unconfirmed; confirming\n";
            }
            static_cast<void>(updater.confirm());
            continue;
        }

        // Sleep until there is something to do: a deadline, or a wake from a
        // WinRT thread. This is api.md's `app.wait_until(client.next_deadline())`.
        std::optional<TimePoint> deadline = client.next_deadline();
        if (const std::optional<TimePoint> theirs = updater.next_deadline();
            theirs.has_value() && (!deadline.has_value() || *theirs < *deadline)) {
            deadline = theirs;
        }

        std::unique_lock<std::mutex> lock{wake_mutex};
        if (deadline.has_value()) {
            wake.wait_until(lock, *deadline, [&] { return woken; });
        } else {
            // No deadline means the library is waiting on the application.
            wake.wait_for(lock, std::chrono::milliseconds{50}, [&] { return woken; });
        }
        woken = false;
    }

    // --- the report ---------------------------------------------------------

    if (!outcome.has_value()) {
        std::cerr << "winrt_ble_dfu: update failed: " << to_string(outcome.error()) << '\n';
        return gave_up_reconnecting ? kReconnectFailed : kUpdateFailed;
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
    // The client's counters, because a real link is where they earn their keep:
    // a retransmitted final chunk is answered as a fresh session (protocol-notes
    // section 6, rules 9b then 9a) and reads as "already held" above, and the
    // only way to tell that from a device that really did hold the image is to
    // see that a request timed out.
    const SmpClientStats& stats = client.stats();
    std::cout << "  smp: " << stats.sent << " sent, " << stats.received << " received, "
              << stats.timeouts << " timed out, " << stats.late << " late, " << stats.unmatched
              << " unmatched, " << stats.mismatched << " mismatched, " << stats.malformed
              << " malformed\n";

    if (report.final_state == UpdateState::Completed) {
        return kOk;
    }
    return gave_up_reconnecting ? kReconnectFailed : kUpdateFailed;
}
