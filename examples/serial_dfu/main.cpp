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
#include "stub_device/demo_image_file.hpp"
#include "stub_device/demo_package.hpp"
#include "stub_device/stub_device.hpp"

#ifndef _WIN32
#include "pty_stub.hpp"
#endif

#include "serial_port/serial_port_config.hpp"
#include "serial_port/serial_port_transport.hpp"

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
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
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
    /// The flags every tool shares.
    UpdateCommand command;

    std::string port; ///< Empty means "start the pty stub".
    std::uint32_t baud = 115200;
    FlowControl flow = FlowControl::None;
    bool stub_cdc = false; ///< The stub's reset shape: CDC (vanish) or UART (stay).
    /// The adapter's message cap (`SerialPortConfig::max_message_size`).
    std::size_t max_message = kDefaultSerialMessageSize;

    /// Without --port: invent the package, and give the stub a second image.
    bool demo_package = false;
    /// Without --port: the stub fails to apply image 1.
    bool apply_fails = false;
};

void usage()
{
    std::cerr << "usage: serial_dfu [--port PATH [--baud N] [--flow none|rtscts]]\n"
                 "                  [--image PATH | --package PATH | --demo-package]\n"
                 "                  [--commit N=client|device] [--apply-fails] [--board NAME]\n"
                 "                  [--stub uart|cdc] [--mode MODE] [--fallback-mode M]\n"
                 "                  [--allow-no-revert] [--no-downgrade-check]\n"
                 "                  [--image-secondary PATH] [--quiet]\n"
                 "  --port PATH   the device's serial port; prefer a stable name such as\n"
                 "                /dev/serial/by-id/... (a USB port may be renamed on reset)\n"
                 "  --baud N      line speed, default 115200 (ignored by USB CDC ACM)\n"
                 "  --flow F      none (default) or rtscts\n"
                 "  --max-message N  the adapter's message cap, default 256; above the\n"
                 "                device's buf_size, that buffer (less the frame's 4\n"
                 "                bytes) becomes the limit\n";
    print_update_flags_help(std::cerr, UpdateFlags{});
    std::cerr << "  --demo-package  without --port: a generated two-image package, for the\n"
                 "                invented board demo_board\n"
                 "  --apply-fails  without --port: the stub fails to apply image 1\n"
                 "  --stub SHAPE  without --port: the stub's reset, uart (default) or cdc\n"
                 "With --port, one of --image or --package is required.\n";
}

[[nodiscard]] bool parse_arguments(int argc, char** argv, Options& out)
{
    const std::vector<std::string> args{argv + 1, argv + argc};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--port" && has_value) {
            out.port = args[++i];
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
        } else if (arg == "--demo-package") {
            out.demo_package = true;
            ++out.command.generated_packages;
        } else if (arg == "--apply-fails") {
            out.apply_fails = true;
        } else if (parse_update_flag(args, i, UpdateFlags{}, out.command) != FlagResult::Taken) {
            return false;
        }
    }
    // The shared rules, and this tool's. A real device is updated with a real
    // image or package: inventing one for it would put a stub's firmware on
    // hardware, and only the stub can be told to fail.
    const bool real_device = !out.port.empty();
    return valid(out.command, real_device) && (out.command.package_mode() || !out.apply_fails) &&
           (!real_device || (!out.demo_package && !out.apply_fails));
}

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

    // What to send: a package's images, read before the device is set up, so a
    // bad package fails before anything is opened. The demo image file is
    // declared first because the inputs read it, so it must outlive them.
    UpdateCommand command = options.command;
    const UpdateConsole console{"serial_dfu", command.quiet, std::cout, std::cerr};
    DemoImageFile demo_file{"serial_dfu"};
    std::unique_ptr<UpdateInputs> inputs;
    if (command.package_mode()) {
        inputs = console.open_inputs(command, options.demo_package
                                                  ? std::optional{build_demo_two_image_package()}
                                                  : std::nullopt);
        if (!inputs) {
            return 1;
        }
    }

#ifndef _WIN32
    std::optional<StubRig> rig;
#endif
    std::string port = options.port;
    if (port.empty()) {
#ifndef _WIN32
        // With a package, the stub gets an image 1 it applies itself after the
        // reset, as a coordinating MCU does with its partner's firmware.
        std::optional<SecondImage> second;
        if (command.package_mode()) {
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
        if (command.image_path.empty() && !command.package_mode()) {
            const std::optional<std::string> written =
                demo_file.write(build_demo_image(DemoVersion{.major = 2}));
            if (!written.has_value()) {
                std::cerr << "serial_dfu: cannot write the demo image\n";
                return 1;
            }
            command.image_path = *written;
        }
#endif
    }

    // Or image 0 from one file or two (one build per slot).
    if (!inputs) {
        inputs = console.open_inputs(command);
        if (!inputs) {
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

    UpdatePlan plan = command.plan();
    // A UART does not drop on reset, so this is how long the updater waits
    // before assuming the reset happened anyway (design.md section 13).
    plan.disconnect_grace = std::chrono::seconds{2};
    // The stub applies image 1 within a few reads; a real second MCU needs a
    // whole transfer, and the library's default interval suits that.
    if (options.port.empty()) {
        plan.apply_poll_interval = std::chrono::milliseconds{100};
    }

    std::string reset_seen = "none";

    // Output, and one note: how the reset showed itself on this link, a port
    // that vanished (USB CDC), or nothing at all until the grace expired (a
    // UART). Observed right after the poll that raised the request, before
    // the run reopens anything, so the old link's state is still the one read.
    const auto observe = [&](const UpdateEvent& event) {
        if (std::holds_alternative<ReconnectRequired>(event)) {
            reset_seen = client.connected() ? "grace" : "dropped";
        }
        console.event(event);
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
                              if (!command.quiet) {
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
                              console.confirming();
                              return Approval::Confirm;
                          },
                      .observe = observe,
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
        std::cerr << "serial_dfu: gave up waiting\n";
        return 1;
    case RunEnd::StoppedBeforeConfirm:
        // Not reachable: the approve hook above always confirms.
        console.error(ran.result.error());
        return 1;
    }

    // --- the report ---------------------------------------------------------

    const bool completed = console.finish(ran.result, updater.report());
    if (!ran.result.has_value()) {
        return 1;
    }
    const UpdateReport& report = *ran.result;
    const SerialLinkCounters counters = total(links);
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
    return completed ? 0 : 1;
}
