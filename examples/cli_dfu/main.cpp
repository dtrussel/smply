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
#include "stub_device/demo_package.hpp"
#include "stub_device/stub_device.hpp"

#include "dfu_app/bootloader_mode.hpp"
#include "dfu_app/dispatcher_wait.hpp"
#include "dfu_app/reconnect_policy.hpp"
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
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace smply;          // NOLINT(google-build-using-namespace) -- an example
using namespace smply::example; // NOLINT(google-build-using-namespace)
using namespace smply::dfu_app; // NOLINT(google-build-using-namespace)

/// The demo's second build differs from the first in size alone.
constexpr std::size_t kDemoSecondaryBodySize = 4100;

struct Options
{
    std::string image_path; ///< Empty means "invent one".
    /// `--image-secondary`: the same image linked for the secondary slot, for
    /// a direct-XIP device; `image_path` is then the primary slot's build.
    std::string image_secondary_path;
    /// `--demo-builds`: invent both builds, one per slot.
    bool demo_builds = false;
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

    /// A multi-image DFU package to install instead of one image.
    std::string package_path;
    /// Invent the package, and a two-image device for it.
    bool demo_package = false;
    /// Invent a direct-XIP package: one image, one build per slot.
    bool demo_xip_package = false;
    /// The stub device fails to apply image 1 (package modes only).
    bool apply_fails = false;
    /// `--commit N=client|device`, in the order given.
    std::vector<std::pair<std::uint32_t, CommitBy>> commits;

    /// `--fallback-mode`: the mode to assume when the device does not report
    /// one (ADR-0025).
    std::optional<McubootMode> fallback_mode;
    /// `--stub-mode`: the mode the stub device reports. Without it, the stub
    /// has no bootloader-information command, like most Zephyr builds.
    std::optional<McubootMode> stub_mode;
    /// `--allow-no-revert`: accept an update the bootloader cannot revert.
    bool allow_no_revert = false;
    /// `--no-downgrade-check`: skip the downgrade check (ADR-0025).
    bool check_downgrade = true;

    [[nodiscard]] bool package_mode() const noexcept
    {
        return demo_package || demo_xip_package || !package_path.empty();
    }
};

void usage()
{
    std::cerr << "usage: cli_dfu [--image PATH | --package PATH | --demo-package] [--mode MODE]\n"
                 "               [--quiet] [--flaky-reconnect N] [--commit N=client|device]\n"
                 "               [--apply-fails] [--fallback-mode M] [--stub-mode M]\n"
                 "               [--allow-no-revert] [--no-downgrade-check]\n"
                 "               [--image-secondary PATH | --demo-builds]\n"
                 "  --image PATH  firmware to install; without it, a demo image is generated\n"
                 "  --image-secondary PATH  direct-XIP: the same image linked for the\n"
                 "                secondary slot (--image is then the primary slot's build);\n"
                 "                the one for the slot the device is not running is sent\n"
                 "  --demo-builds  generate both builds of the demo image, one per slot\n"
                 "  --package PATH  a multi-image DFU package (docs/multi-image.md): image 0\n"
                 "                is confirmed here, every other image is left to the device\n"
                 "  --demo-package  generate a two-image package, and a device to take it\n"
                 "  --demo-xip-package  generate a direct-XIP package: one image, a build\n"
                 "                per slot (needs a direct-XIP --stub-mode)\n"
                 "  --commit N=client|device  who commits image N (repeatable)\n"
                 "  --apply-fails  the demo device fails to apply image 1\n"
                 "  --mode MODE   test-then-confirm (default) | confirm-immediately | upload-only\n"
                 "  --fallback-mode M  the MCUboot mode to assume when the device does not\n"
                 "                report one: single-slot, swap-using-scratch, upgrade-only,\n"
                 "                swap-using-move, direct-xip, direct-xip-with-revert, ram-load,\n"
                 "                firmware-loader, single-slot-ram-load, swap-using-offset\n"
                 "  --stub-mode M  the mode the demo device reports (it still swaps)\n"
                 "  --allow-no-revert  update a device whose bootloader cannot revert\n"
                 "                (upgrade-only); without it, such an update is refused\n"
                 "  --no-downgrade-check  send an image older than the running one even\n"
                 "                when the device prevents downgrades; it will refuse it at boot\n"
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
        } else if (arg == "--image-secondary" && i + 1 < args.size()) {
            out.image_secondary_path = args[++i];
        } else if (arg == "--demo-builds") {
            out.demo_builds = true;
        } else if (arg == "--mode" && i + 1 < args.size()) {
            const std::optional<UpdateMode> mode = parse_update_mode(args[++i]);
            if (!mode.has_value()) {
                return false;
            }
            out.mode = *mode;
        } else if (arg == "--flaky-reconnect" && i + 1 < args.size()) {
            const std::string& count = args[++i];
            if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos) {
                return false;
            }
            out.flaky_reconnect = static_cast<unsigned>(std::stoul(count));
        } else if (arg == "--package" && i + 1 < args.size()) {
            out.package_path = args[++i];
        } else if (arg == "--demo-package") {
            out.demo_package = true;
        } else if (arg == "--demo-xip-package") {
            out.demo_xip_package = true;
        } else if (arg == "--apply-fails") {
            out.apply_fails = true;
        } else if (arg == "--allow-no-revert") {
            out.allow_no_revert = true;
        } else if (arg == "--no-downgrade-check") {
            out.check_downgrade = false;
        } else if ((arg == "--fallback-mode" || arg == "--stub-mode") && i + 1 < args.size()) {
            const std::optional<McubootMode> mode = parse_mcuboot_mode(args[++i]);
            if (!mode.has_value()) {
                return false;
            }
            (arg == "--fallback-mode" ? out.fallback_mode : out.stub_mode) = mode;
        } else if (arg == "--commit" && i + 1 < args.size()) {
            const auto commit = parse_commit(args[++i]);
            if (!commit.has_value()) {
                return false;
            }
            out.commits.push_back(*commit);
        } else {
            return false;
        }
    }
    // One thing to install, and the package-only options only with a package.
    const int sources = (out.image_path.empty() ? 0 : 1) + (out.package_path.empty() ? 0 : 1) +
                        (out.demo_package ? 1 : 0) + (out.demo_xip_package ? 1 : 0);
    // A second build goes with exactly one first build: a named file, or the
    // demo's; and never with a package, which names its own files.
    const bool one_image = !out.package_mode();
    const bool builds_ok = (out.image_secondary_path.empty() || !out.image_path.empty()) &&
                           (!out.demo_builds || out.image_path.empty()) &&
                           (one_image || (out.image_secondary_path.empty() && !out.demo_builds));
    return sources <= 1 && builds_ok &&
           (out.package_mode() || (!out.apply_fails && out.commits.empty()));
}

/// How far the device took an image it commits itself (ADR-0022).
[[nodiscard]] std::string_view device_image_state(const ImageReport& image)
{
    if (image.committed) {
        return "committed";
    }
    return image.applied ? "applied, not committed" : "not applied";
}

/// One line per image of a multi-image update.
void print_images(const UpdateReport& report, std::ostream& out)
{
    if (report.images.size() < 2) {
        return;
    }
    for (const ImageReport& image : report.images) {
        out << "  image " << image.image << " ("
            << (image.commit == CommitBy::Client ? "client" : "device") << "): ";
        if (image.commit == CommitBy::Device) {
            out << device_image_state(image);
        } else if (image.rolled_back) {
            out << "reverted";
        } else {
            out << image.bytes_transferred << " bytes transferred";
        }
        out << '\n';
    }
}

/// The generated image, as a file: `FileImageSource` reads through a real file
/// even in the no-arguments case. Removed when the run ends, however it ends.
///
/// Into the temporary directory rather than the working one: this runs as a
/// `ctest` test, and a test that drops files into the build tree is a test that
/// makes the next build's diff noisy.
///
/// **The name is unique per run.** Several `cli_dfu` tests run at once under
/// `ctest -j`, and with one shared name each truncated the file another was
/// reading. The failure was a short read, which also let the `WILL_FAIL` test
/// pass for the wrong reason.
class DemoImageFile
{
public:
    DemoImageFile() = default;
    DemoImageFile(const DemoImageFile&) = delete;
    DemoImageFile& operator=(const DemoImageFile&) = delete;
    DemoImageFile(DemoImageFile&&) = delete;
    DemoImageFile& operator=(DemoImageFile&&) = delete;

    ~DemoImageFile()
    {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    /// Writes `image` to a new file and returns its path, or `nullopt`.
    [[nodiscard]] std::optional<std::string> write(const std::vector<std::byte>& image)
    {
        std::error_code ec;
        const std::filesystem::path directory = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return std::nullopt;
        }
        std::random_device entropy;
        const std::string name = "cli_dfu_demo_image_" + std::to_string(entropy()) + "_" +
                                 std::to_string(entropy()) + ".bin";
        path_ = (directory / name).string();

        std::ofstream out{path_, std::ios::binary | std::ios::trunc};
        if (!out) {
            return std::nullopt;
        }
        // ostream speaks char, and these are bytes this process just built. The
        // marker has to be the last comment line before the code, or it
        // silences the comment instead.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        return out ? std::optional<std::string>{path_} : std::nullopt;
    }

private:
    std::string path_;
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
    std::string image_secondary_path = options.image_secondary_path;
    DemoImageFile demo_file;           // before `source`, which reads it
    DemoImageFile demo_secondary_file; // and `secondary`, which reads this
    if (image_path.empty() && !options.package_mode()) {
        const std::vector<std::byte> update = build_demo_image(DemoVersion{.major = 2});
        const std::optional<std::string> written = demo_file.write(update);
        if (!written.has_value()) {
            std::cerr << "cli_dfu: cannot write the demo image\n";
            return 1;
        }
        image_path = *written;
    }
    if (options.demo_builds) {
        // The same version again, a different build: what a direct-XIP
        // project links for its second slot. Here only the size differs.
        const std::vector<std::byte> other_build =
            build_demo_image(DemoVersion{.major = 2}, kDemoSecondaryBodySize);
        const std::optional<std::string> written = demo_secondary_file.write(other_build);
        if (!written.has_value()) {
            std::cerr << "cli_dfu: cannot write the demo image\n";
            return 1;
        }
        image_secondary_path = *written;
    }

    // What to send: a package's images, or image 0 from one file or two (one
    // build per slot). The demo files above must outlive it: it reads them.
    const auto read_inputs = [&]() -> Result<std::unique_ptr<UpdateInputs>> {
        if (options.demo_package) {
            return UpdateInputs::from_package_bytes(build_demo_two_image_package());
        }
        if (options.demo_xip_package) {
            return UpdateInputs::from_package_bytes(build_demo_xip_package());
        }
        if (!options.package_path.empty()) {
            return UpdateInputs::from_package(options.package_path);
        }
        return UpdateInputs::from_files(image_path, image_secondary_path);
    };
    Result<std::unique_ptr<UpdateInputs>> read = read_inputs();
    if (!read.has_value()) {
        std::cerr << "cli_dfu: " << to_string(read.error()) << '\n';
        return 1;
    }
    const std::unique_ptr<UpdateInputs> inputs = std::move(*read);
    for (const auto& [image, commit] : options.commits) {
        if (const Result<void> set = inputs->set_commit(image, commit); !set.has_value()) {
            std::cerr << "cli_dfu: --commit " << image << ": " << to_string(set.error()) << '\n';
            return 1;
        }
    }

    // The stub's image 1, with a package: another MCU's firmware, which it
    // applies itself after the reset. The demo device runs radio 5.0.0; a real
    // package's image 1 lands on a device with nothing applied yet. The
    // direct-XIP demo is one image, given twice, so its device has none.
    std::optional<SecondImage> second_image;
    if (options.package_mode() && !options.demo_xip_package) {
        second_image = SecondImage{
            .running = options.demo_package
                           ? build_demo_image(DemoVersion{.major = kDemoRadioRunning})
                           : std::vector<std::byte>{},
            .outcome = options.apply_fails ? ApplyOutcome::Failed : ApplyOutcome::Applied,
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

    UpdatePlan plan;
    plan.mode = options.mode;
    plan.fallback_mode = options.fallback_mode;
    plan.allow_no_revert = options.allow_no_revert;
    plan.check_downgrade = options.check_downgrade;
    // The stub applies image 1 within a few reads; a real second MCU takes a
    // whole UART transfer, and the default interval suits that instead.
    plan.apply_poll_interval = std::chrono::milliseconds{100};

    // One handler per kind of event. std::visit refuses to compile if a kind
    // is left out, which is the point of UpdateEvent being a variant. This is
    // output only: the run itself acts on the last three.
    const auto on_event = overloaded{
        [&](const UpdateStateChanged& changed) {
            if (!options.quiet) {
                std::cout << "  " << to_string(changed.to) << '\n';
            }
        },
        [&](const UploadProgress& progress) {
            if (!options.quiet && progress.total != 0) {
                std::cout << "\r  uploading " << progress.transferred << '/' << progress.total
                          << " bytes" << std::flush;
                if (progress.transferred == progress.total) {
                    std::cout << '\n';
                }
            }
        },
        [&](const DisconnectExpected&) {
            if (!options.quiet) {
                std::cout << "  the device is about to reboot; a dropped link is expected\n";
            }
        },
        [](const ReconnectRequired&) {},
        [](const ConfirmationRequired&) {},
        [](const UpdateFinished&) {},
    };

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
                              if (!options.quiet) {
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
                          if (!options.quiet) {
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
                              if (!options.quiet) {
                                  std::cout
                                      << "  the new image is running unconfirmed; confirming\n";
                              }
                              return Approval::Confirm;
                          },
                      .observe = [&](const UpdateEvent& event) { std::visit(on_event, event); },
                  }};

    const Result<void> begun = updater.start(inputs->targets(), plan, run.event_handler());

    if (!begun.has_value()) {
        std::cerr << "cli_dfu: " << to_string(begun.error()) << '\n';
        return 1;
    }

    // --- the pump -----------------------------------------------------------

    const UpdateRunOutcome ran = run.run();
    switch (ran.end) {
    case RunEnd::Finished:
        break;
    case RunEnd::TimedOut:
        std::cerr << "cli_dfu: gave up waiting\n";
        return 1;
    case RunEnd::StoppedBeforeConfirm:
        // Not reachable: the approve hook above always confirms.
        std::cerr << "cli_dfu: " << to_string(ran.result.error()) << '\n';
        return 1;
    }
    const Result<UpdateReport>& outcome = ran.result;

    // --- the report ---------------------------------------------------------

    if (!outcome.has_value()) {
        std::cerr << "cli_dfu: update failed: " << to_string(outcome.error()) << '\n';
        // The report still says what happened to each image, and what the next
        // reset will do.
        // On stderr with the failure itself, so the lines keep their order.
        std::cerr << "  " << describe_mode(updater.report()) << '\n';
        print_images(updater.report(), std::cerr);
        if (updater.report().revert_pending) {
            std::cerr
                << "  a swap is scheduled but unconfirmed: it will revert on the next reset\n";
        }
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
    std::cout << "  " << describe_mode(report) << '\n';
    print_images(report, std::cout);

    return report.final_state == UpdateState::Completed ? 0 : 1;
}
