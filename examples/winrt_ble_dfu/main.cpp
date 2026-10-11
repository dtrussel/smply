// SPDX-License-Identifier: Apache-2.0

/// \file
/// A console DFU tool over Bluetooth LE, on Windows.
///
/// **Read `examples/cli_dfu/main.cpp` first.** It is the same arrangement
/// against a stub device in this process, it runs in CI on every push, and it
/// is the one that has actually been executed. This file is that arrangement pointed
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
///   rather than leaving the pump spinning, and this tool exits 4 for it.
///
/// For the same reason the pump itself is shared: it is
/// `smply::dfu_app::UpdateRun` (`support/dfu_app/update_run.hpp`), which the
/// component suite and `cli_dfu` run on every push. What is left here is what a
/// radio changes: the three hooks the run calls -- open a link, approve the
/// image, observe events.
///
/// **This program has never been run.** It is compiled by CI at `/W4 /WX` on a
/// runner with no Bluetooth radio. See the README.

#include "scanner.hpp"
#include "winrt_prelude.hpp"

#include "dfu_app/bootloader_mode.hpp"
#include "dfu_app/dispatcher_wait.hpp"
#include "dfu_app/reconnect_policy.hpp"
#include "dfu_app/update_inputs.hpp"
#include "dfu_app/update_run.hpp"

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
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
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
    /// `--image-secondary`: the same image linked for the secondary slot, for
    /// a direct-XIP device; `image_path` is then the primary slot's build.
    std::string image_secondary_path;
    std::string name;    ///< --name: match a substring of the advertised name.
    std::string address; ///< --address: skip scanning entirely.
    UpdateMode mode = UpdateMode::TestThenConfirm;
    std::chrono::milliseconds scan_timeout{std::chrono::seconds{10}};
    bool quiet = false;

    /// `--mode test-only`: install and reboot, then stop without confirming.
    ///
    /// The device is left in its trial boot, which MCUboot reverts on the next
    /// reset unless something confirms first. A real deployment tool does
    /// exactly this when the decision to keep an image belongs to a self-test
    /// that runs later, or to an operator. The hardware cross-check needs it
    /// too: it can compare clients at the trial boot only if every client
    /// stops there.
    bool stop_before_confirm = false;

    /// `--mode confirm-only`: confirm whatever the device is running, and stop.
    ///
    /// The other half of the same split. It installs nothing, so it needs no
    /// `--image`, and it is the only mode that does not build an updater.
    bool confirm_only = false;

    /// `--fallback-mode`: the MCUboot mode to assume when the device does not
    /// report one (ADR-0025).
    std::optional<McubootMode> fallback_mode;
    /// `--allow-no-revert`: accept an update the bootloader cannot revert.
    bool allow_no_revert = false;
    /// `--no-downgrade-check`: skip the downgrade check (ADR-0025).
    bool check_downgrade = true;
};

/// Parses `--mode`. Two of the five are not `UpdateMode` values at all.
///
/// `test-only` and `confirm-only` are the two halves of `test-then-confirm`
/// split across two runs of this tool, which is why they set a flag here rather
/// than naming a library mode: the library's `UpdateMode` describes one update,
/// and these describe how much of one a single invocation performs.
[[nodiscard]] bool parse_mode(std::string_view text, Options& out)
{
    if (const std::optional<UpdateMode> mode = parse_update_mode(text); mode.has_value()) {
        out.mode = *mode;
        return true;
    }
    if (text == "test-only") {
        out.mode = UpdateMode::TestThenConfirm;
        out.stop_before_confirm = true;
        return true;
    }
    if (text == "confirm-only") {
        out.confirm_only = true;
        return true;
    }
    return false;
}

void usage()
{
    std::cerr << "usage: winrt_ble_dfu --image PATH [--name NAME | --address ADDR]\n"
                 "                     [--mode MODE] [--fallback-mode M] [--scan-timeout MS]\n"
                 "                     [--allow-no-revert] [--no-downgrade-check]\n"
                 "                     [--image-secondary PATH] [--quiet]\n"
                 "  --image PATH   the firmware to install (required)\n"
                 "  --image-secondary PATH  direct-XIP: the same image linked for the\n"
                 "                 secondary slot (--image is then the primary slot's build)\n"
                 "  --name NAME    connect to the first device whose advertised name\n"
                 "                 contains NAME; without it, the first device that\n"
                 "                 advertises the SMP service is used\n"
                 "  --address ADDR AA:BB:CC:DD:EE:FF, or bare hex -- skips scanning\n"
                 "  --mode MODE    test-then-confirm (default) | confirm-immediately |\n"
                 "                 upload-only | test-only | confirm-only\n"
                 "                 test-only installs and reboots but does not confirm,\n"
                 "                 so the device is left in its trial boot;\n"
                 "                 confirm-only confirms the running image and needs\n"
                 "                 no --image\n"
                 "  --fallback-mode M  the MCUboot mode to assume when the device does\n"
                 "                 not report one, named as the report prints it\n"
                 "                 (swap-using-move, upgrade-only, direct-xip, ...)\n"
                 "  --allow-no-revert  update a device whose bootloader cannot revert\n"
                 "                 (upgrade-only); without it, such an update is refused\n"
                 "  --no-downgrade-check  send an image older than the running one even\n"
                 "                 when the device prevents downgrades; it will refuse it\n"
                 "                 at boot\n"
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
        } else if (arg == "--image-secondary" && i + 1 < args.size()) {
            out.image_secondary_path = args[++i];
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
            if (!parse_mode(args[++i], out)) {
                return false;
            }
        } else if (arg == "--allow-no-revert") {
            out.allow_no_revert = true;
        } else if (arg == "--no-downgrade-check") {
            out.check_downgrade = false;
        } else if (arg == "--fallback-mode" && i + 1 < args.size()) {
            out.fallback_mode = parse_mcuboot_mode(args[++i]);
            if (!out.fallback_mode.has_value()) {
                return false;
            }
        } else {
            return false;
        }
    }
    // Unlike cli_dfu, there is no "invent an image" path. That example writes a
    // synthetic image to a stub in its own process; writing one to real
    // hardware would install firmware that does not run.
    //
    // `--mode confirm-only` uploads nothing, so requiring an image there would
    // make the caller name a file that is never opened -- and a file named but
    // unused is the sort of argument that goes stale without anyone noticing.
    return (out.confirm_only || !out.image_path.empty()) &&
           (out.image_secondary_path.empty() || (!out.confirm_only && !out.image_path.empty())) &&
           !(!out.name.empty() && !out.address.empty());
}

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

    // Opened here, and held for the whole of main(): an upload keeps its source
    // by reference and a resume reads from it again long after start() returned
    // (handoff.md, "Lifetime"). `confirm-only` installs nothing and so opens
    // nothing.
    std::unique_ptr<UpdateInputs> inputs;
    if (!options.confirm_only) {
        Result<std::unique_ptr<UpdateInputs>> opened =
            UpdateInputs::from_files(options.image_path, options.image_secondary_path);
        if (!opened.has_value()) {
            std::cerr << "winrt_ble_dfu: " << to_string(opened.error()) << '\n';
            return kUpdateFailed;
        }
        inputs = std::move(*opened);
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
    //
    // The dispatcher's wake callback runs on a WinRT thread-pool thread, inside
    // post(); the wait's only signals and returns.

    DispatcherWait wait;
    Dispatcher inbound{wait.waker()};
    wait.deliver_from(inbound);

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

    SmpClient client{*links.back()};
    ImageManagement images{client};
    OsManagement os{client};

    // --- confirm-only -------------------------------------------------------
    //
    // No updater, no plan, no reconnect: one request, and the device stops
    // being able to revert. A `SetStateRequest` with `confirm` and no `hash`
    // confirms the image that is *running* (smply/groups/image.hpp), which is
    // the only safe form -- confirming a slot the device has not booted is how
    // a device is bricked.
    if (options.confirm_only) {
        bool answered = false;
        Result<ImageState> confirmed = fail(ErrorCode::InvalidState, "no result");
        if (!images.set_state(SetStateRequest{.confirm = true}, [&](Result<ImageState> result) {
                confirmed = std::move(result);
                answered = true;
            })) {
            std::cerr << "winrt_ble_dfu: the confirm request could not be sent\n";
            return kUpdateFailed;
        }
        // A pump of one request, through the same wait the update run uses:
        // it drains the dispatcher after every wake, and wakes at least every
        // 50 ms when the client has no deadline.
        while (!answered) {
            client.poll(wait.now());
            if (answered) {
                break;
            }
            wait.wait_until(client.next_deadline());
        }
        if (!confirmed.has_value()) {
            std::cerr << "winrt_ble_dfu: confirm failed: " << to_string(confirmed.error()) << '\n';
            return kUpdateFailed;
        }
        std::cout << "confirmed the running image\n";
        return kOk;
    }

    FirmwareUpdater updater{client, images, os};

    // --- the update ---------------------------------------------------------

    UpdatePlan plan;
    plan.mode = options.mode;
    plan.fallback_mode = options.fallback_mode;
    plan.allow_no_revert = options.allow_no_revert;
    plan.check_downgrade = options.check_downgrade;

    // The reconnect settings are the defaults -- 500 ms doubling to 8 s, six
    // attempts -- and there is no overall deadline: over a real radio, how
    // long an update may take is the updater's own timers' business.
    const UpdateRunSettings settings{
        .reconnect = ReconnectSettings{},
        .overall_timeout = std::nullopt,
        // What the updater is told when every attempt failed.
        .unreachable = Error{ErrorCode::Disconnected, "winrt_ble_dfu: could not reconnect"},
    };

    // Every line is stamped with the milliseconds since the update began. On a
    // real link that is the only way to see *where* the time goes -- the
    // device's own work on the final chunk, the reboot, the reconnect -- and it
    // is what the bench measured the reconnect policy from.
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };

    // One handler per kind of event; std::visit refuses to compile if a kind
    // is left out. Output only: the run itself acts on the last three.
    const auto on_event = overloaded{
        [&](const UpdateStateChanged& changed) {
            if (!options.quiet) {
                std::cout << "  [" << elapsed_ms() << " ms] " << to_string(changed.to) << '\n';
            }
        },
        [&](const UploadProgress& progress) {
            if (!options.quiet && progress.total != 0) {
                std::cout << "\r  [" << elapsed_ms() << " ms] uploading " << progress.transferred
                          << '/' << progress.total << " bytes" << std::flush;
                if (progress.transferred == progress.total) {
                    std::cout << '\n';
                }
            }
        },
        [&](const DisconnectExpected&) {
            if (!options.quiet) {
                std::cout << "  the device is about to reboot\n";
            }
        },
        [](const ReconnectRequired&) {},
        [](const ConfirmationRequired&) {},
        [](const UpdateFinished&) {},
    };

    // The three hooks. The run calls each on this thread, never inside a
    // library callback, so the open-a-link hook may block in connect().
    UpdateRun run{client, updater, wait, settings,
                  UpdateRunHooks{
                      // The device rebooted; it is not connectable yet. The run waits,
                      // asks here, waits longer -- and eventually decides it is gone.
                      // Every failure is worth another try: a device mid-boot answers
                      // nothing, and that is what all of them look like.
                      .open_link = [&](const ReconnectAttempt& attempt) -> LinkAttempt {
                          if (!options.quiet) {
                              std::cout << "  reconnect attempt " << attempt.number << '\n';
                          }
                          Result<std::unique_ptr<WinRtBleTransport>> fresh =
                              WinRtBleTransport::connect(address, inbound);
                          if (!fresh.has_value()) {
                              return LinkAttempt::retry();
                          }
                          links.push_back(std::move(*fresh));
                          if (!options.quiet) {
                              std::cout << "  reconnected\n";
                          }
                          return LinkAttempt::opened(*links.back());
                      },
                      // The device is running the new image, unconfirmed. A real tool
                      // runs its self-test here; declining to confirm lets MCUboot
                      // revert on the next reset, which is the point of the default
                      // mode (ADR-0014).
                      .approve =
                          [&] {
                              if (options.stop_before_confirm) {
                                  // `--mode test-only` stops exactly here, with the update
                                  // still waiting on this application.
                                  return Approval::Stop;
                              }
                              if (!options.quiet) {
                                  std::cout
                                      << "  the new image is running unconfirmed; confirming\n";
                              }
                              return Approval::Confirm;
                          },
                      .observe = [&](const UpdateEvent& event) { std::visit(on_event, event); },
                  }};

    // One file, or one build per slot: an image list of one either way.
    const Result<void> begun = updater.start(inputs->targets(), plan, run.event_handler());

    if (!begun.has_value()) {
        std::cerr << "winrt_ble_dfu: " << to_string(begun.error()) << '\n';
        return kUpdateFailed;
    }

    // --- the pump -----------------------------------------------------------

    const UpdateRunOutcome ran = run.run();
    switch (ran.end) {
    case RunEnd::Finished:
        break;
    case RunEnd::StoppedBeforeConfirm:
        // Leaving the update waiting is the behaviour, not a leak: the process
        // exits, the link closes, and the device stays in a trial boot that
        // the next reset reverts unless something confirms it first.
        std::cout << "installed, running unconfirmed, not confirmed by request\n";
        return kOk;
    case RunEnd::TimedOut:
        // Not reachable: there is no overall deadline.
        std::cerr << "winrt_ble_dfu: " << to_string(ran.result.error()) << '\n';
        return kUpdateFailed;
    }
    const Result<UpdateReport>& outcome = ran.result;

    // --- the report ---------------------------------------------------------

    if (!outcome.has_value()) {
        std::cerr << "winrt_ble_dfu: update failed: " << to_string(outcome.error()) << '\n';
        std::cerr << "  " << describe_mode(updater.report()) << '\n';
        return ran.gave_up_reconnecting ? kReconnectFailed : kUpdateFailed;
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
    std::cout << "  " << describe_mode(report) << '\n';
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
    return ran.gave_up_reconnecting ? kReconnectFailed : kUpdateFailed;
}
