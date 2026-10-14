// SPDX-License-Identifier: Apache-2.0

/// \file
/// A console DFU driver, against a device on another thread.
///
/// **This is the file to read.** Everything else is scaffolding -- a stub device
/// to talk to (`examples/stub_device/`), a link to talk over, an image to
/// install and a file to read it from. What is demonstrated here is the arrangement every
/// application that uses smply has to build:
///
/// * **one client context**, the thread running `main()`. Every call into the
///   library and every callback out of it happens here (ADR-0004).
/// * **a `Dispatcher`** carrying inbound bytes from the driver thread to this
///   one. The library never learns there was another thread.
/// * **an application-driven pump**: nothing happens unless `poll()` is called,
///   so there is no hidden thread, no hidden queue and no callback from
///   somewhere surprising (ADR-0003). The pump is `smply::dfu_app::UpdateRun`
///   (`support/dfu_app/update_run.hpp`), run on this thread; what is left here
///   is what differs between applications: its three hooks.
/// * **the application owning the connection**. A reset drops the link by
///   design; re-establishing it is not the library's business, so the updater
///   asks, and the run calls this program's open-a-link hook.
///
/// Run it with no arguments and it invents a device and an image to install.
/// With `--demo-package` it invents a two-image device and a DFU package for it,
/// the coordinating-MCU update of docs/multi-image.md: image 0 confirmed by this
/// program, image 1 applied and committed by the device.

#include "loopback_transport.hpp"

#include "stub_device/demo_image.hpp"
#include "stub_device/demo_image_file.hpp"
#include "stub_device/demo_package.hpp"
#include "stub_device/stub_device.hpp"

#include "dfu_app/bootloader_mode.hpp"
#include "dfu_app/dispatcher_wait.hpp"
#include "dfu_app/reconnect_policy.hpp"
#include "dfu_app/update_command.hpp"
#include "dfu_app/update_inputs.hpp"
#include "dfu_app/update_run.hpp"

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
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace smply;          // NOLINT(google-build-using-namespace) -- an example
using namespace smply::example; // NOLINT(google-build-using-namespace)
using namespace smply::dfu_app; // NOLINT(google-build-using-namespace)

/// The demo's second build differs from the first in size alone.
constexpr std::size_t kDemoSecondaryBodySize = 4100;

struct Options
{
    /// The flags every tool shares.
    UpdateCommand command;

    /// `--demo-builds`: invent both builds, one per slot.
    bool demo_builds = false;

    /// Refuse this many reconnection attempts before letting one succeed.
    ///
    /// Not a toy. A device that has just rebooted is not immediately
    /// connectable, so every real application retries with a backoff and gives
    /// up eventually -- and against an in-process stub that reconnects
    /// instantly, none of that code would ever run. Setting this above the
    /// policy's attempt budget exercises the give-up path, which ends the
    /// update through `FirmwareUpdater::reconnect_failed()`.
    unsigned flaky_reconnect = 0;

    /// Invent the package, and a two-image device for it.
    bool demo_package = false;
    /// Invent a direct-XIP package: one image, one build per slot.
    bool demo_xip_package = false;
    /// The stub device fails to apply image 1 (package modes only).
    bool apply_fails = false;

    /// `--stub-mode`: the mode the stub device reports. Without it, the stub
    /// has no bootloader-information command, like most Zephyr builds.
    std::optional<McubootMode> stub_mode;
};

void usage()
{
    std::cerr << "usage: cli_dfu [--image PATH | --package PATH | --demo-package] [--mode MODE]\n"
                 "               [--quiet] [--flaky-reconnect N] [--commit N=client|device]\n"
                 "               [--apply-fails] [--fallback-mode M] [--stub-mode M]\n"
                 "               [--allow-no-revert] [--no-downgrade-check]\n"
                 "               [--image-secondary PATH | --demo-builds] [--board NAME]\n"
                 "Without --image or a package, a demo image is generated.\n";
    print_update_flags_help(std::cerr, UpdateFlags{});
    std::cerr << "  --demo-builds  generate both builds of the demo image, one per slot\n"
                 "  --demo-package  generate a two-image package, and a device to take it;\n"
                 "                the demo packages are for the invented board demo_board\n"
                 "  --demo-xip-package  generate a direct-XIP package: one image, a build\n"
                 "                per slot (needs a direct-XIP --stub-mode)\n"
                 "  --apply-fails  the demo device fails to apply image 1\n"
                 "  --stub-mode M  the mode the demo device reports (it still swaps)\n"
                 "  --flaky-reconnect N  refuse N reconnection attempts before succeeding,\n"
                 "                to exercise the backoff; above the attempt budget the\n"
                 "                update gives up, which is also worth demonstrating\n";
}

[[nodiscard]] bool parse_arguments(int argc, char** argv, Options& out)
{
    const std::vector<std::string> args{argv + 1, argv + argc};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--demo-builds") {
            out.demo_builds = true;
        } else if (arg == "--flaky-reconnect" && i + 1 < args.size()) {
            const std::string& count = args[++i];
            if (count.empty() || count.size() > 9 ||
                count.find_first_not_of("0123456789") != std::string::npos) {
                return false;
            }
            out.flaky_reconnect = static_cast<unsigned>(std::stoul(count));
        } else if (arg == "--demo-package") {
            out.demo_package = true;
            ++out.command.generated_packages;
        } else if (arg == "--demo-xip-package") {
            out.demo_xip_package = true;
            ++out.command.generated_packages;
        } else if (arg == "--apply-fails") {
            out.apply_fails = true;
        } else if (arg == "--stub-mode" && i + 1 < args.size()) {
            out.stub_mode = parse_mcuboot_mode(args[++i]);
            if (!out.stub_mode.has_value()) {
                return false;
            }
        } else if (parse_update_flag(args, i, UpdateFlags{}, out.command) != FlagResult::Taken) {
            return false;
        }
    }
    // The shared rules, and this tool's: a failed apply needs a device image
    // to fail, and the generated second build goes with the generated image.
    const UpdateCommand& command = out.command;
    return valid(command, false) && (command.package_mode() || !out.apply_fails) &&
           (!out.demo_builds || (command.image_path.empty() && !command.package_mode()));
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_arguments(argc, argv, options)) {
        usage();
        return 2;
    }

    // --- the "device", and the firmware to put on it ------------------------

    UpdateCommand command = options.command;
    const UpdateConsole console{"cli_dfu", command.quiet, std::cout, std::cerr};
    const std::vector<std::byte> running = build_demo_image(DemoVersion{.major = 1});
    DemoImageFile demo_file{"cli_dfu"};           // before `inputs`, which reads it
    DemoImageFile demo_secondary_file{"cli_dfu"}; // and this
    if (command.image_path.empty() && !command.package_mode()) {
        const std::optional<std::string> written =
            demo_file.write(build_demo_image(DemoVersion{.major = 2}));
        if (!written.has_value()) {
            std::cerr << "cli_dfu: cannot write the demo image\n";
            return 1;
        }
        command.image_path = *written;
    }
    if (options.demo_builds) {
        // The same version again, a different build: what a direct-XIP
        // project links for its second slot. Here only the size differs.
        const std::optional<std::string> written = demo_secondary_file.write(
            build_demo_image(DemoVersion{.major = 2}, kDemoSecondaryBodySize));
        if (!written.has_value()) {
            std::cerr << "cli_dfu: cannot write the demo image\n";
            return 1;
        }
        command.image_secondary_path = *written;
    }

    // What to send: a package's images, or image 0 from one file or two (one
    // build per slot). The demo files above must outlive it: it reads them.
    std::optional<std::vector<std::byte>> generated;
    if (options.demo_package) {
        generated = build_demo_two_image_package();
    } else if (options.demo_xip_package) {
        generated = build_demo_xip_package();
    }
    const std::unique_ptr<UpdateInputs> inputs = console.open_inputs(command, std::move(generated));
    if (!inputs) {
        return 1;
    }

    // The stub's image 1, with a package: another MCU's firmware, which it
    // applies itself after the reset. The demo device runs radio 5.0.0; a real
    // package's image 1 lands on a device with nothing applied yet. The
    // direct-XIP demo is one image, given twice, so its device has none. With
    // `--commit 1=client` image 1 is an ordinary image instead, which a device
    // built with Zephyr's defaults refuses to let the client confirm (A27).
    std::optional<SecondImage> second_image;
    if (command.package_mode() && !options.demo_xip_package) {
        CommitBy image_1 = CommitBy::Device;
        for (const auto& [image, commit] : command.commits) {
            if (image == 1) {
                image_1 = commit; // the last one given, as open_inputs() applies them
            }
        }
        second_image = SecondImage{
            .running = options.demo_package
                           ? build_demo_image(DemoVersion{.major = kDemoRadioRunning})
                           : std::vector<std::byte>{},
            .outcome = options.apply_fails ? ApplyOutcome::Failed : ApplyOutcome::Applied,
            .device_commits = image_1 == CommitBy::Device,
        };
    }

    // --- the pump's wake-up, and the marshalling queue -----------------------
    //
    // Declared before the client and the transports: everything below captures
    // them, and a callback outliving what it captured is the lifetime bug this
    // library's documentation warns about most.
    //
    // The wait is the update run's: a condition variable the dispatcher's wake
    // callback signals from the *device* thread, inside post(), and a drain of
    // this dispatcher on this thread after every wake.

    DispatcherWait wait;
    Dispatcher inbound{wait.waker()};
    wait.deliver_from(inbound);

    const std::optional<std::int64_t> reported_mode =
        options.stub_mode.has_value()
            ? std::optional<std::int64_t>{static_cast<std::int64_t>(*options.stub_mode)}
            : std::nullopt;
    StubDevice device{running, std::move(second_image), reported_mode};

    const UpdateRunSettings settings{
        // Deliberately brisk: these delays are waited for real, and this
        // example runs as a ctest with a timeout. A shipped tool would use the
        // defaults (500 ms doubling to 8 s), which is what
        // examples/winrt_ble_dfu/ does.
        .reconnect =
            ReconnectSettings{
                .first_delay = std::chrono::milliseconds{20},
                .max_delay = std::chrono::milliseconds{160},
                .max_attempts = 5,
            },
        .overall_timeout = std::chrono::seconds{30},
        // What the updater is told when every attempt was refused.
        .unreachable = Error{ErrorCode::Disconnected, "cli_dfu: could not reconnect"},
    };
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

    UpdatePlan plan = command.plan();
    // The stub applies image 1 within a few reads; a real second MCU takes a
    // whole UART transfer, and the default interval suits that instead.
    plan.apply_poll_interval = std::chrono::milliseconds{100};

    // The three hooks. The run calls each on this thread, after the poll that
    // raised the event -- never inside it -- so each may block, sleep and open
    // links.
    UpdateRun run{client, updater, wait, settings,
                  UpdateRunHooks{
                      // A real reconnect is a loop, not a statement. The device has just
                      // rebooted and is not connectable yet, so the run waits, asks here,
                      // waits longer, and eventually decides it has lost the device.
                      // `--flaky-reconnect` makes that visible; over BLE it is simply
                      // what happens.
                      .open_link = [&](const ReconnectAttempt& attempt) -> LinkAttempt {
                          if (refusals_left > 0) {
                              --refusals_left;
                              if (!command.quiet) {
                                  std::cout << "  reconnect attempt " << attempt.number
                                            << " failed; waiting " << attempt.waited.count()
                                            << " ms\n";
                                  if (attempt.number == attempt.max_attempts) {
                                      // The run now tells the updater, which ends the
                                      // update with `settings.unreachable`.
                                      std::cout << "  giving up after " << attempt.max_attempts
                                                << " reconnection attempts\n";
                                  }
                              }
                              return LinkAttempt::retry();
                          }
                          // A dropped link stays dropped, so this is a new one -- exactly
                          // what an application does with a BLE connection after a
                          // reboot. The run rebinds the client to it, then resumes.
                          links.push_back(std::make_unique<LoopbackTransport>(device, inbound));
                          device.attach(*links.back());
                          if (!command.quiet) {
                              std::cout << "  reconnected\n";
                          }
                          return LinkAttempt::opened(*links.back());
                      },
                      // The device is running the new image, unconfirmed. This is where a
                      // real application runs its self-test; declining here would let
                      // the device revert on its next reset, which is the point of the
                      // default mode (ADR-0014).
                      .approve =
                          [&] {
                              console.confirming();
                              return Approval::Confirm;
                          },
                      .observe = [&](const UpdateEvent& event) { console.event(event); },
                  }};

    // --- the pump -----------------------------------------------------------

    const UpdateRunOutcome ran = run.run(inputs->targets(), plan);
    switch (ran.end) {
    case RunEnd::Finished:
        break;
    case RunEnd::Refused:
        console.error(ran.result.error());
        return 1;
    case RunEnd::TimedOut:
        std::cerr << "cli_dfu: gave up waiting\n";
        return 1;
    case RunEnd::StoppedBeforeConfirm:
        // Not reachable: the approve hook above always confirms.
        console.error(ran.result.error());
        return 1;
    }

    // --- the report ---------------------------------------------------------

    // A failure still says what happened to each image, and what the next
    // reset will do.
    return console.finish(ran.result, updater.report()) ? 0 : 1;
}
