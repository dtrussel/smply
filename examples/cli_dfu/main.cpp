// SPDX-License-Identifier: Apache-2.0

/// \file
/// A console DFU driver: the canonical pump loop, against a device on another
/// thread.
///
/// **This is the file to read.** Everything else is scaffolding -- a stub device
/// to talk to (`examples/stub_device/`), a link to talk over, an image to
/// install and a file to read it from. What is demonstrated here is the arrangement every
/// application that uses smply has to build:
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
/// With `--demo-package` it invents a two-image device and a DFU package for it,
/// the coordinating-MCU update of docs/multi-image.md: image 0 confirmed by this
/// program, image 1 applied and committed by the device.

#include "loopback_transport.hpp"

#include "stub_device/demo_image.hpp"
#include "stub_device/demo_package.hpp"
#include "stub_device/stub_device.hpp"

#include "dfu_app/file_image_source.hpp"
#include "dfu_app/package_update.hpp"
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
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
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

    /// A multi-image DFU package to install instead of one image.
    std::string package_path;
    /// Invent the package, and a two-image device for it.
    bool demo_package = false;
    /// The stub device fails to apply image 1 (package modes only).
    bool apply_fails = false;
    /// `--commit N=client|device`, in the order given.
    std::vector<std::pair<std::uint32_t, CommitBy>> commits;

    [[nodiscard]] bool package_mode() const noexcept
    {
        return demo_package || !package_path.empty();
    }
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
    std::cerr << "usage: cli_dfu [--image PATH | --package PATH | --demo-package] [--mode MODE]\n"
                 "               [--quiet] [--flaky-reconnect N] [--commit N=client|device]\n"
                 "               [--apply-fails]\n"
                 "  --image PATH  firmware to install; without it, a demo image is generated\n"
                 "  --package PATH  a multi-image DFU package (docs/multi-image.md): image 0\n"
                 "                is confirmed here, every other image is left to the device\n"
                 "  --demo-package  generate a two-image package, and a device to take it\n"
                 "  --commit N=client|device  who commits image N (repeatable)\n"
                 "  --apply-fails  the demo device fails to apply image 1\n"
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
        } else if (arg == "--package" && i + 1 < args.size()) {
            out.package_path = args[++i];
        } else if (arg == "--demo-package") {
            out.demo_package = true;
        } else if (arg == "--apply-fails") {
            out.apply_fails = true;
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
                        (out.demo_package ? 1 : 0);
    return sources <= 1 && (out.package_mode() || (!out.apply_fails && out.commits.empty()));
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
    DemoImageFile demo_file; // before `source`, which reads it
    if (image_path.empty() && !options.package_mode()) {
        const std::vector<std::byte> update = build_demo_image(DemoVersion{.major = 2});
        const std::optional<std::string> written = demo_file.write(update);
        if (!written.has_value()) {
            std::cerr << "cli_dfu: cannot write the demo image\n";
            return 1;
        }
        image_path = *written;
    }

    // A package, when there is one: the images, and who commits each.
    std::unique_ptr<PackageUpdate> package;
    std::optional<SecondImage> second_image;
    if (options.package_mode()) {
        Result<std::unique_ptr<PackageUpdate>> read =
            options.demo_package ? PackageUpdate::from_bytes(build_demo_two_image_package())
                                 : PackageUpdate::load(options.package_path);
        if (!read.has_value()) {
            std::cerr << "cli_dfu: " << to_string(read.error()) << '\n';
            return 1;
        }
        package = std::move(*read);
        for (const auto& [image, commit] : options.commits) {
            if (const Result<void> set = package->set_commit(image, commit); !set.has_value()) {
                std::cerr << "cli_dfu: --commit " << image << ": " << to_string(set.error())
                          << '\n';
                return 1;
            }
        }
        // The stub's image 1: another MCU's firmware, which it applies itself
        // after the reset. The demo device runs radio 5.0.0; a real package's
        // image 1 lands on a device with nothing applied yet.
        second_image = SecondImage{
            .running = options.demo_package
                           ? build_demo_image(DemoVersion{.major = kDemoRadioRunning})
                           : std::vector<std::byte>{},
            .outcome = options.apply_fails ? ApplyOutcome::Failed : ApplyOutcome::Applied,
        };
    }

    // A package brings its own sources; only a single image is read from a file.
    Result<FileImageSource> source =
        options.package_mode()
            ? Result<FileImageSource>{fail(ErrorCode::InvalidArgument, "cli_dfu: a package")}
            : FileImageSource::open(image_path);
    if (!options.package_mode() && !source.has_value()) {
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
    StubDevice device{running, std::move(second_image)};

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
    // The stub applies image 1 within a few reads; a real second MCU takes a
    // whole UART transfer, and the default interval suits that instead.
    plan.apply_poll_interval = std::chrono::milliseconds{100};

    Result<UpdateReport> outcome = fail(ErrorCode::InvalidState, "no result");
    const auto started = std::chrono::steady_clock::now();

    // One handler per kind of event. std::visit refuses to compile if a kind
    // is left out, which is the point of UpdateEvent being a variant.
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
        // Not done here. The handler runs inside poll(), and reconnecting is the
        // application's own work -- so it is noted and done on the loop's next
        // turn, where it reads as what it is.
        [&](const ReconnectRequired&) { pending.reconnect = true; },
        [&](const ConfirmationRequired&) { pending.confirm = true; },
        [&](const UpdateFinished& finished) {
            outcome = finished.result;
            pending.finished = true;
        },
    };
    // A package's image list, or the single image, with the same event handler.
    const auto handler = [&] {
        return UpdateEventCallback{[&](const UpdateEvent& event) { std::visit(on_event, event); }};
    };
    const Result<void> begun = package ? updater.start(package->targets(), plan, handler())
                                       : updater.start(*source, plan, handler());

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
        // device thread, whichever deadline is earlier. This is api.md's
        // `app.wait_until(earliest(...))`.
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
        // The report still says what happened to each image, and what the next
        // reset will do.
        // On stderr with the failure itself, so the lines keep their order.
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
    print_images(report, std::cout);

    return report.final_state == UpdateState::Completed ? 0 : 1;
}
