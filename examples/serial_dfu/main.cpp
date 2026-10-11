// SPDX-License-Identifier: Apache-2.0

/// \file
/// A serial DFU driver: `cli_dfu`'s arrangement, over a real serial port.
///
/// With `--port`, it updates a device on that port -- a Zephyr board's USB CDC
/// ACM port, or a UART. Without one (POSIX only) it starts the stub device
/// behind a pseudo-terminal and updates that, which is how CI runs it.
///
/// What is new relative to `examples/cli_dfu/main.cpp`, which is still the file
/// to read first (the loop itself is `smply::dfu_app::UpdateRun`), is how a
/// serial link survives a device reset (roadmap O7, design.md section 13):
///
/// * `UpdatePlan::disconnect_grace` is **two seconds**, not ten. A USB CDC port
///   vanishes on reset and the adapter reports it at once; a hardware UART
///   does not drop at all, so the updater moves on only when the grace
///   expires. Two seconds is past MCUboot's reset without being a wait anyone
///   notices.
/// * Its open-a-link hook always **closes the old port and opens a new one, by
///   path**, retrying while the port is absent and giving up on a port that
///   exists and refuses. That one piece of code
///   covers a UART that never went away, a CDC port that came back under the
///   same name, and one that came back under another -- provided the path is
///   stable, which on Linux is what `/dev/serial/by-id/` is for.

#include "stub_device/demo_image.hpp"
#include "stub_device/demo_package.hpp"
#include "stub_device/stub_device.hpp"

#ifndef _WIN32
#include "pty_stub.hpp"
#endif

#include "serial_port/serial_port_config.hpp"
#include "serial_port/serial_port_transport.hpp"

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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace smply;            // NOLINT(google-build-using-namespace) -- an example
using namespace smply::example;   // NOLINT(google-build-using-namespace)
using namespace smply::dfu_app;   // NOLINT(google-build-using-namespace)
using namespace smply::transport; // NOLINT(google-build-using-namespace)

struct Options
{
    std::string port;       ///< Empty means "start the pty stub".
    std::string image_path; ///< Required with --port; otherwise a demo image.
    /// `--image-secondary`: the same image linked for the secondary slot, for
    /// a direct-XIP device; `image_path` is then the primary slot's build.
    std::string image_secondary_path;
    std::uint32_t baud = 115200;
    FlowControl flow = FlowControl::None;
    UpdateMode mode = UpdateMode::TestThenConfirm;
    bool stub_cdc = false; ///< The stub's reset shape: CDC (vanish) or UART (stay).
    bool quiet = false;
    /// The adapter's message cap (`SerialPortConfig::max_message_size`).
    std::size_t max_message = kDefaultSerialMessageSize;

    /// A multi-image DFU package instead of one image (docs/multi-image.md).
    std::string package_path;
    /// Without --port: invent the package, and give the stub a second image.
    bool demo_package = false;
    /// Without --port: the stub fails to apply image 1.
    bool apply_fails = false;
    std::vector<std::pair<std::uint32_t, CommitBy>> commits;
    /// The MCUboot mode to assume when the device does not report one
    /// (ADR-0025).
    std::optional<McubootMode> fallback_mode;
    /// Accept an update the bootloader cannot revert.
    bool allow_no_revert = false;
    /// Skip the downgrade check (ADR-0025).
    bool check_downgrade = true;

    [[nodiscard]] bool package_mode() const noexcept
    {
        return demo_package || !package_path.empty();
    }
};

void usage()
{
    std::cerr << "usage: serial_dfu [--port PATH [--baud N] [--flow none|rtscts]]\n"
                 "                  [--image PATH | --package PATH | --demo-package]\n"
                 "                  [--commit N=client|device] [--apply-fails]\n"
                 "                  [--stub uart|cdc] [--mode MODE] [--fallback-mode M]\n"
                 "                  [--allow-no-revert] [--no-downgrade-check]\n"
                 "                  [--image-secondary PATH] [--quiet]\n"
                 "  --port PATH   the device's serial port; prefer a stable name such as\n"
                 "                /dev/serial/by-id/... (a USB port may be renamed on reset)\n"
                 "  --baud N      line speed, default 115200 (ignored by USB CDC ACM)\n"
                 "  --flow F      none (default) or rtscts\n"
                 "  --image PATH  the signed MCUboot image to install\n"
                 "  --image-secondary PATH  direct-XIP: the same image linked for the\n"
                 "                secondary slot (--image is then the primary slot's build)\n"
                 "  --package PATH  a multi-image DFU package; image 0 is confirmed here and\n"
                 "                every other image left to the device (docs/multi-image.md)\n"
                 "  --commit N=client|device  who commits image N (repeatable)\n"
                 "  --demo-package  without --port: a generated two-image package\n"
                 "  --apply-fails  without --port: the stub fails to apply image 1\n"

                 "  --stub SHAPE  without --port: the stub's reset, uart (default) or cdc\n"
                 "  --max-message N  the adapter's message cap, default 256; above the\n"
                 "                device's buf_size, that buffer (less the frame's 4\n"
                 "                bytes) becomes the limit\n"
                 "  --mode MODE   test-then-confirm (default) | confirm-immediately | upload-only\n"
                 "  --fallback-mode M  the MCUboot mode to assume when the device does not\n"
                 "                report one, named as the report prints it (swap-using-move,\n"
                 "                upgrade-only, direct-xip, ...)\n"
                 "  --allow-no-revert  update a device whose bootloader cannot revert\n"
                 "                (upgrade-only); without it, such an update is refused\n"
                 "  --no-downgrade-check  send an image older than the running one even\n"
                 "                when the device prevents downgrades; it will refuse it at boot\n"
                 "  --quiet       print only the outcome\n"
                 "With --port, one of --image or --package is required.\n";
}

[[nodiscard]] bool parse_arguments(int argc, char** argv, Options& out)
{
    const std::vector<std::string> args{argv + 1, argv + argc};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--quiet") {
            out.quiet = true;
        } else if (arg == "--port" && has_value) {
            out.port = args[++i];
        } else if (arg == "--image" && has_value) {
            out.image_path = args[++i];
        } else if (arg == "--image-secondary" && has_value) {
            out.image_secondary_path = args[++i];
        } else if (arg == "--baud" && has_value) {
            const std::string& value = args[++i];
            if (value.empty() || value.size() > 9 ||
                value.find_first_not_of("0123456789") != std::string::npos) {
                return false;
            }
            out.baud = static_cast<std::uint32_t>(std::stoul(value));
        } else if (arg == "--max-message" && has_value) {
            const std::string& value = args[++i];
            if (value.empty() || value.size() > 5 ||
                value.find_first_not_of("0123456789") != std::string::npos) {
                return false;
            }
            out.max_message = std::stoul(value); // at most five digits: fits any size_t
        } else if (arg == "--flow" && has_value) {
            const std::string& value = args[++i];
            if (value != "none" && value != "rtscts") {
                return false;
            }
            out.flow = value == "rtscts" ? FlowControl::RtsCts : FlowControl::None;
        } else if (arg == "--stub" && has_value) {
            const std::string& value = args[++i];
            if (value != "uart" && value != "cdc") {
                return false;
            }
            out.stub_cdc = value == "cdc";
        } else if (arg == "--mode" && has_value) {
            const std::optional<UpdateMode> mode = parse_update_mode(args[++i]);
            if (!mode.has_value()) {
                return false;
            }
            out.mode = *mode;
        } else if (arg == "--package" && has_value) {
            out.package_path = args[++i];
        } else if (arg == "--demo-package") {
            out.demo_package = true;
        } else if (arg == "--apply-fails") {
            out.apply_fails = true;
        } else if (arg == "--allow-no-revert") {
            out.allow_no_revert = true;
        } else if (arg == "--no-downgrade-check") {
            out.check_downgrade = false;
        } else if (arg == "--fallback-mode" && has_value) {
            out.fallback_mode = parse_mcuboot_mode(args[++i]);
            if (!out.fallback_mode.has_value()) {
                return false;
            }
        } else if (arg == "--commit" && has_value) {
            const auto commit = parse_commit(args[++i]);
            if (!commit.has_value()) {
                return false;
            }
            out.commits.push_back(*commit);
        } else {
            return false;
        }
    }
    const int sources = (out.image_path.empty() ? 0 : 1) + (out.package_path.empty() ? 0 : 1) +
                        (out.demo_package ? 1 : 0);
    if (sources > 1 || (!out.package_mode() && (out.apply_fails || !out.commits.empty()))) {
        return false;
    }
    // A second build goes with a first one named on the command line, and
    // never with a package, which names its own files.
    if (!out.image_secondary_path.empty() && (out.image_path.empty() || out.package_mode())) {
        return false;
    }
    // A real device is updated with a real image or package; inventing one for
    // it would put a stub's firmware on hardware.
    if (!out.port.empty()) {
        return !out.demo_package && !out.apply_fails &&
               (!out.image_path.empty() || !out.package_path.empty());
    }
    return true;
}

/// The generated image, as a temporary file, as in cli_dfu. Unique per run so
/// parallel ctest runs do not truncate each other's.
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

    [[nodiscard]] std::optional<std::string> write(const std::vector<std::byte>& image)
    {
        std::error_code ec;
        const std::filesystem::path directory = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return std::nullopt;
        }
        std::random_device entropy;
        path_ = (directory / ("serial_dfu_demo_image_" + std::to_string(entropy()) + "_" +
                              std::to_string(entropy()) + ".bin"))
                    .string();
        std::ofstream out{path_, std::ios::binary | std::ios::trunc};
        if (!out) {
            return std::nullopt;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- bytes this process built
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        return out ? std::optional<std::string>{path_} : std::nullopt;
    }

private:
    std::string path_;
};

#ifndef _WIN32
/// The stub device and the pseudo-terminal in front of it, torn down in the
/// one order that is safe. Each has a thread that calls into the other -- the
/// pty's reader submits requests to the device, the device's thread writes
/// answers to the pty -- so both threads are stopped before either object is
/// destroyed.
struct StubRig
{
    StubRig(ResetShape shape, std::optional<SecondImage> second)
        : device{build_demo_image(DemoVersion{.major = 1}), std::move(second)}, pty{device, shape}
    {
        if (pty.ok()) {
            device.attach(pty);
        }
    }

    StubRig(const StubRig&) = delete;
    StubRig& operator=(const StubRig&) = delete;
    StubRig(StubRig&&) = delete;
    StubRig& operator=(StubRig&&) = delete;

    ~StubRig()
    {
        pty.stop();    // no more requests reach the device
        device.stop(); // no more answers reach the pty
    }

    StubDevice device;
    PtyStub pty;
};
#endif

/// Every link's counters, added up: a reconnect opens a new transport, and the
/// boot output of a UART reset arrives on the old one.
[[nodiscard]] SerialLinkCounters
total(const std::vector<std::unique_ptr<SerialPortTransport>>& links)
{
    SerialLinkCounters sum;
    for (const auto& link : links) {
        const SerialLinkCounters c = link->counters();
        sum.deframe.ignored += c.deframe.ignored;
        sum.deframe.crc_failures += c.deframe.crc_failures;
        sum.deframe.framing_errors += c.deframe.framing_errors;
        sum.deframe.packets += c.deframe.packets;
        sum.dropped_lines += c.dropped_lines;
        sum.send.deferred += c.send.deferred;
        sum.send.refused += c.send.refused;
        sum.bytes_read += c.bytes_read;
        sum.bytes_written += c.bytes_written;
    }
    return sum;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_arguments(argc, argv, options)) {
        usage();
        return 2;
    }
#ifdef _WIN32
    if (options.port.empty()) {
        std::cerr << "serial_dfu: --port is required on Windows (the stub needs a pty)\n";
        return 2;
    }
#endif

    // --- the pump's wake-up, and the marshalling queue -----------------------
    //
    // Declared first: the stub device, every transport and the client all post
    // into it or capture it, so it must outlive every one of them.

    DispatcherWait wait;
    Dispatcher inbound{wait.waker()};
    wait.deliver_from(inbound);

    // --- the device: a real port, or the stub behind a pseudo-terminal -------

    std::string image_path = options.image_path;
    DemoImageFile demo_file;
#ifndef _WIN32
    std::optional<StubRig> rig;
#endif
    std::string port = options.port;
    if (port.empty()) {
#ifndef _WIN32
        // With a package, the stub gets an image 1 it applies itself after the
        // reset, as a coordinating MCU does with its partner's firmware.
        std::optional<SecondImage> second;
        if (options.package_mode()) {
            second = SecondImage{
                .running = options.demo_package
                               ? build_demo_image(DemoVersion{.major = kDemoRadioRunning})
                               : std::vector<std::byte>{},
                .outcome = options.apply_fails ? ApplyOutcome::Failed : ApplyOutcome::Applied,
            };
        }
        const StubRig& stub =
            rig.emplace(options.stub_cdc ? ResetShape::Cdc : ResetShape::Uart, std::move(second));
        if (!stub.pty.ok()) {
            std::cerr << "serial_dfu: cannot create a pseudo-terminal\n";
            return 1;
        }
        port = stub.pty.port_path();
        if (image_path.empty() && !options.package_mode()) {
            const std::optional<std::string> written =
                demo_file.write(build_demo_image(DemoVersion{.major = 2}));
            if (!written.has_value()) {
                std::cerr << "serial_dfu: cannot write the demo image\n";
                return 1;
            }
            image_path = *written;
        }
#endif
    }

    // What to send: a package's images, or image 0 from one file or two (one
    // build per slot). The demo file above must outlive it: it reads it.
    const auto read_inputs = [&]() -> Result<std::unique_ptr<UpdateInputs>> {
        if (options.demo_package) {
            return UpdateInputs::from_package_bytes(build_demo_two_image_package());
        }
        if (!options.package_path.empty()) {
            return UpdateInputs::from_package(options.package_path);
        }
        return UpdateInputs::from_files(image_path, options.image_secondary_path);
    };
    Result<std::unique_ptr<UpdateInputs>> read = read_inputs();
    if (!read.has_value()) {
        std::cerr << "serial_dfu: " << to_string(read.error()) << '\n';
        return 1;
    }
    const std::unique_ptr<UpdateInputs> inputs = std::move(*read);
    for (const auto& [image, commit] : options.commits) {
        if (const Result<void> set = inputs->set_commit(image, commit); !set.has_value()) {
            std::cerr << "serial_dfu: --commit " << image << ": " << to_string(set.error()) << '\n';
            return 1;
        }
    }

    SerialPortConfig config;
    config.path = port;
    config.baud = options.baud;
    config.flow = options.flow;
    config.max_message_size = options.max_message;

    // Every link ever opened stays alive until the client is gone: ~SmpClient
    // and rebind_transport() both detach from the transport they hold.
    std::vector<std::unique_ptr<SerialPortTransport>> links;
    // What the path resolved to at each successful open. A USB port that came
    // back renamed behind a stable link shows up here as a second name.
    std::set<std::string> devices;
    const auto note_device = [&] {
        std::error_code ec;
        const std::filesystem::path real = std::filesystem::canonical(port, ec);
        devices.insert(ec ? port : real.string());
    };
    {
        Result<std::unique_ptr<SerialPortTransport>> first =
            SerialPortTransport::open(config, inbound);
        if (!first.has_value()) {
            std::cerr << "serial_dfu: " << port << ": " << to_string(first.error()) << '\n';
            return 1;
        }
        links.push_back(std::move(*first));
        note_device();
    }

    SmpClient client{*links.back()};
    ImageManagement images{client};
    OsManagement os{client};
    FirmwareUpdater updater{client, images, os};

    const UpdateRunSettings settings{
        // A real device gets the library's defaults (500 ms doubling to 8 s);
        // the stub reboots in 150 ms and runs under a ctest timeout.
        .reconnect = options.port.empty() ? ReconnectSettings{
                                                .first_delay = std::chrono::milliseconds{50},
                                                .max_delay = std::chrono::milliseconds{400},
                                                .max_attempts = 10,
                                            }
                                          : ReconnectSettings{},
        .overall_timeout = std::chrono::minutes{10},
        .unreachable = Error{ErrorCode::Disconnected, "serial_dfu: could not reopen the port"},
    };

    UpdatePlan plan;
    plan.mode = options.mode;
    plan.fallback_mode = options.fallback_mode;
    plan.allow_no_revert = options.allow_no_revert;
    plan.check_downgrade = options.check_downgrade;
    // A UART does not drop on reset, so this is how long the updater waits
    // before assuming the reset happened anyway (design.md section 13).
    plan.disconnect_grace = std::chrono::seconds{2};
    // The stub applies image 1 within a few reads; a real second MCU needs a
    // whole transfer, and the library's default interval suits that.
    if (options.port.empty()) {
        plan.apply_poll_interval = std::chrono::milliseconds{100};
    }

    std::string reset_seen = "none";

    // Output only, but for one note: the run itself acts on the last three.
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
                std::cout << "  the device is resetting\n";
            }
        },
        [&](const ReconnectRequired&) {
            // How the reset showed itself on this link: a port that vanished
            // (USB CDC), or nothing at all until the grace expired (a UART).
            // Observed right after the poll that raised it, before the run
            // reopens anything, so the old link's state is still the one read.
            reset_seen = client.connected() ? "grace" : "dropped";
        },
        [](const ConfirmationRequired&) {},
        [](const UpdateFinished&) {},
    };

    UpdateRun run{client, updater, wait, settings,
                  UpdateRunHooks{
                      .open_link = [&](const ReconnectAttempt& attempt) -> LinkAttempt {
                          // The old port is closed first, whether or not it dropped: a
                          // UART that stayed open is held exclusively, and reopening
                          // flushes the boot output and any half-frame the reset left
                          // behind. Once per episode: later attempts find it closed.
                          if (attempt.number == 1) {
                              links.back()->close();
                          }
                          Result<std::unique_ptr<SerialPortTransport>> again =
                              SerialPortTransport::open(config, inbound);
                          if (again.has_value()) {
                              links.push_back(std::move(*again));
                              note_device();
                              if (!options.quiet) {
                                  std::cout << "  reopened " << port << '\n';
                              }
                              return LinkAttempt::opened(*links.back());
                          }
                          // Absent is worth retrying -- a USB port mid-reset -- but a
                          // port that exists and refuses is not going to change its mind.
                          if (again.error().code() == ErrorCode::Disconnected) {
                              return LinkAttempt::retry(again.error());
                          }
                          return LinkAttempt::give_up(again.error());
                      },
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

    if (const Result<void> begun = updater.start(inputs->targets(), plan, run.event_handler());
        !begun.has_value()) {
        std::cerr << "serial_dfu: " << to_string(begun.error()) << '\n';
        return 1;
    }

    // --- the pump -----------------------------------------------------------

    const UpdateRunOutcome ran = run.run();
    switch (ran.end) {
    case RunEnd::Finished:
        break;
    case RunEnd::TimedOut:
        std::cerr << "serial_dfu: gave up waiting\n";
        return 1;
    case RunEnd::StoppedBeforeConfirm:
        // Not reachable: the approve hook above always confirms.
        std::cerr << "serial_dfu: " << to_string(ran.result.error()) << '\n';
        return 1;
    }
    const Result<UpdateReport>& outcome = ran.result;

    // --- the report ---------------------------------------------------------

    const SerialLinkCounters counters = total(links);
    if (!outcome.has_value()) {
        std::cerr << "serial_dfu: update failed: " << to_string(outcome.error()) << '\n';
        std::cerr << "  " << describe_mode(updater.report()) << '\n';
        return 1;
    }
    const UpdateReport& report = *outcome;
    std::size_t applied = 0;
    std::size_t committed = 0;
    for (const ImageReport& image : report.images) {
        applied += image.applied ? 1U : 0U;
        committed += image.committed ? 1U : 0U;
    }
    // One line, so a ctest can match all of it with a single expression.
    std::cout << "serial_dfu: " << to_string(report.final_state) << " reset=" << reset_seen
              << " links=" << links.size() << " devices=" << devices.size()
              << " packets=" << counters.deframe.packets << " ignored=" << counters.deframe.ignored
              << " dropped_lines=" << counters.dropped_lines
              << " framing_errors=" << counters.deframe.framing_errors
              << " crc_failures=" << counters.deframe.crc_failures
              << " refused=" << counters.send.refused << " images=" << report.images.size()
              << " applied=" << applied << " committed=" << committed << '\n';
    std::cout << "  " << describe_mode(report) << '\n';
    return report.final_state == UpdateState::Completed ? 0 : 1;
}
