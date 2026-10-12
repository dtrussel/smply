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
///   `smply::dfu_app::ReconnectPolicy`, shared with the portable tools so that
///   CI exercises it on Linux -- none of the code in *this* file is ever run by
///   CI, so anything that can live over there does;
/// * **giving up is a real outcome.** `reconnect_failed()` ends the update
///   rather than leaving the pump spinning, and this tool exits 4 for it.
///
/// For the same reason the pump itself is shared: it is
/// `smply::dfu_app::UpdateRun` (`support/dfu_app/update_run.hpp`), which the
/// component suite and `cli_dfu` run on every push. So are the update's flags,
/// their checks and its output (`support/dfu_app/update_command.hpp`). What is
/// left here is what a radio changes: finding the device, its own two modes,
/// and the three hooks the run calls -- open a link, approve the image,
/// observe events.
///
/// **CI never runs this program.** It is compiled at `/W4 /WX` on a runner
/// with no Bluetooth radio, and has run only on the hardware bench, by hand.
/// See the README.

#include "scanner.hpp"
#include "winrt_prelude.hpp"

#include "dfu_app/dispatcher_wait.hpp"
#include "dfu_app/reconnect_policy.hpp"
#include "dfu_app/update_command.hpp"
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
    /// The flags every tool shares, but the package ones: an image is
    /// required, see the note in main().
    UpdateCommand command;
    std::string name;    ///< --name: match a substring of the advertised name.
    std::string address; ///< --address: skip scanning entirely.
    std::chrono::milliseconds scan_timeout{std::chrono::seconds{10}};

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
    /// The other half of the same split. It installs nothing, so it takes no
    /// `--image`, and it is the only mode that does not build an updater.
    bool confirm_only = false;
};

/// The shared flags this tool offers: no package, so no `--package`,
/// `--commit` or `--board` (see the roadmap's backlog).
constexpr UpdateFlags kFlags{.packages = false};

/// Parses `--mode`. Two of the five are not `UpdateMode` values at all.
///
/// `test-only` and `confirm-only` are the two halves of `test-then-confirm`
/// split across two runs of this tool, which is why they set a flag here rather
/// than naming a library mode: the library's `UpdateMode` describes one update,
/// and these describe how much of one a single invocation performs.
[[nodiscard]] bool parse_mode(std::string_view text, Options& out)
{
    if (const std::optional<UpdateMode> mode = parse_update_mode(text); mode.has_value()) {
        out.command.mode = *mode;
        return true;
    }
    if (text == "test-only") {
        out.command.mode = UpdateMode::TestThenConfirm;
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
                 "  --name NAME    connect to the first device whose advertised name\n"
                 "                 contains NAME; without it, the first device that\n"
                 "                 advertises the SMP service is used\n"
                 "  --address ADDR AA:BB:CC:DD:EE:FF, or bare hex -- skips scanning\n"
                 "  --scan-timeout MS  how long to look for a device (default 10000)\n";
    print_update_flags_help(std::cerr, kFlags);
    std::cerr << "  --mode test-only  installs and reboots but does not confirm, so the\n"
                 "                 device is left in its trial boot\n"
                 "  --mode confirm-only  confirms the running image; takes no --image\n"
                 "--image is required, but for confirm-only.\n"
                 "\n"
                 "exit: 0 ok, 1 update failed, 2 usage, 3 no device, 4 reconnect failed\n";
}

[[nodiscard]] bool parse_arguments(int argc, char** argv, Options& out)
{
    const std::vector<std::string> args{argv + 1, argv + argc};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--name" && i + 1 < args.size()) {
            out.name = args[++i];
        } else if (arg == "--address" && i + 1 < args.size()) {
            out.address = args[++i];
        } else if (arg == "--scan-timeout" && i + 1 < args.size()) {
            const std::string& ms = args[++i];
            if (ms.empty() || ms.size() > 9 ||
                ms.find_first_not_of("0123456789") != std::string::npos) {
                return false;
            }
            out.scan_timeout = std::chrono::milliseconds{std::stoul(ms)};
        } else if (arg == "--mode" && i + 1 < args.size()) {
            if (!parse_mode(args[++i], out)) {
                return false;
            }
        } else if (parse_update_flag(args, i, kFlags, out.command) != FlagResult::Taken) {
            return false;
        }
    }
    // Unlike cli_dfu, there is no "invent an image" path. That example writes a
    // synthetic image to a stub in its own process; writing one to real
    // hardware would install firmware that does not run.
    //
    // `--mode confirm-only` uploads nothing, so it takes no image: a file named
    // but never opened is the sort of argument that goes stale without anyone
    // noticing.
    const UpdateCommand& command = out.command;
    const bool no_image = command.image_path.empty() && command.image_secondary_path.empty();
    return (out.confirm_only ? no_image : valid(command, true)) &&
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
    const UpdateCommand& command = options.command;
    UpdateConsole console{"winrt_ble_dfu", command.quiet, std::cout, std::cerr};
    std::unique_ptr<UpdateInputs> inputs;
    if (!options.confirm_only) {
        inputs = console.open_inputs(command);
        if (!inputs) {
            return kUpdateFailed;
        }
    }

    // --- find the device ----------------------------------------------------

    std::uint64_t address = 0;
    if (!options.address.empty()) {
        const Result<std::uint64_t> parsed = parse_address(options.address);
        if (!parsed.has_value()) {
            console.error(parsed.error());
            return kUsage;
        }
        address = *parsed;
    } else {
        if (!command.quiet) {
            std::cout << "scanning\n";
        }
        const Result<std::uint64_t> found =
            find_device(ScanFilter{.name = options.name, .timeout = options.scan_timeout});
        if (!found.has_value()) {
            console.error(found.error());
            return kNoDevice;
        }
        address = *found;
    }
    if (!command.quiet) {
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
            console.error(link.error());
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
            console.error("confirm failed", confirmed.error());
            return kUpdateFailed;
        }
        std::cout << "confirmed the running image\n";
        return kOk;
    }

    FirmwareUpdater updater{client, images, os};

    // --- the update ---------------------------------------------------------

    const UpdatePlan plan = command.plan();

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
    console.stamp_lines();

    // The three hooks. The run calls each on this thread, never inside a
    // library callback, so the open-a-link hook may block in connect().
    UpdateRun run{client, updater, wait, settings,
                  UpdateRunHooks{
                      // The device rebooted; it is not connectable yet. The run waits,
                      // asks here, waits longer -- and eventually decides it is gone.
                      // Every failure is worth another try: a device mid-boot answers
                      // nothing, and that is what all of them look like.
                      .open_link = [&](const ReconnectAttempt& attempt) -> LinkAttempt {
                          if (!command.quiet) {
                              std::cout << "  reconnect attempt " << attempt.number << '\n';
                          }
                          Result<std::unique_ptr<WinRtBleTransport>> fresh =
                              WinRtBleTransport::connect(address, inbound);
                          if (!fresh.has_value()) {
                              return LinkAttempt::retry();
                          }
                          links.push_back(std::move(*fresh));
                          if (!command.quiet) {
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
                              console.confirming();
                              return Approval::Confirm;
                          },
                      .observe = [&](const UpdateEvent& event) { console.event(event); },
                  }};

    // --- the pump -----------------------------------------------------------

    // One file, or one build per slot: an image list of one either way.
    const UpdateRunOutcome ran = run.run(inputs->targets(), plan);
    switch (ran.end) {
    case RunEnd::Finished:
        break;
    case RunEnd::Refused:
        console.error(ran.result.error());
        return kUpdateFailed;
    case RunEnd::StoppedBeforeConfirm:
        // Leaving the update waiting is the behaviour, not a leak: the process
        // exits, the link closes, and the device stays in a trial boot that
        // the next reset reverts unless something confirms it first.
        std::cout << "installed, running unconfirmed, not confirmed by request\n";
        return kOk;
    case RunEnd::TimedOut:
        // Not reachable: there is no overall deadline.
        console.error(ran.result.error());
        return kUpdateFailed;
    }

    // --- the report ---------------------------------------------------------

    const bool completed = console.finish(ran.result, updater.report());
    if (!ran.result.has_value()) {
        return ran.gave_up_reconnecting ? kReconnectFailed : kUpdateFailed;
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

    if (completed) {
        return kOk;
    }
    return ran.gave_up_reconnecting ? kReconnectFailed : kUpdateFailed;
}
