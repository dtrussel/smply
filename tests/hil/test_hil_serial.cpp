// SPDX-License-Identifier: Apache-2.0
//
// The serial port adapter against the bench peer's console UART.
//
// **These cases have never run.** They were written without the bench, and the
// first run is what will say whether they are right (roadmap O7). They are here
// so that run is one command, and so that what it must show is written down
// before anybody sees the answer.
//
// The peer's console (USART1, the ST-LINK virtual COM port, 115200 8N1) carries
// the MCUmgr **shell** transport, an echoing shell, and a UART log backend in
// deferred mode -- the stream over which a third-party client could not
// complete an upload (protocol-notes section 9). That is the point: it is the
// hardest console smply's serial path will meet, and `SerialInbound` is built to
// ignore everything on it that is not a frame.
//
// Environment: `SMPLY_HIL_UART` names the port (`COM4`). Without it every case
// SKIPs, which run_hil.py reports as unavailable, never as a pass (ADR-0015).
// The supervisor does not start its UART logger for these groups: only one
// process may hold the port.

#include "support/bench.hpp"

#include "serial_port/serial_port_config.hpp"
#include "serial_port/serial_port_transport.hpp"

#include "smply/clock.hpp"
#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/image_source.hpp"
#include "smply/mcuboot_image.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"
#include "smply/util/dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace smply;            // NOLINT(google-build-using-namespace) -- a test
using namespace smply::transport; // NOLINT(google-build-using-namespace)

/// The console port, or a SKIP.
[[nodiscard]] std::string require_uart()
{
    const char* port = std::getenv("SMPLY_HIL_UART"); // NOLINT(concurrency-mt-unsafe)
    if (port == nullptr || *port == '\0') {
        SKIP("bench unavailable: SMPLY_HIL_UART is not set");
    }
    return port;
}

[[nodiscard]] SerialPortConfig console(const std::string& port)
{
    SerialPortConfig config;
    config.path = port;
    config.baud = 115200;
    return config;
}

/// A client context with a wake-up, as in examples/serial_dfu.
struct Pump
{
    Pump()
        : inbound{[this] {
              {
                  const std::lock_guard<std::mutex> lock{mutex};
                  woken = true;
              }
              wake.notify_one();
          }}
    {}

    /// Drains and polls until \p done holds or \p limit passes.
    bool until(SmpClient& client, const std::function<bool()>& done,
               std::chrono::milliseconds limit, FirmwareUpdater* updater = nullptr)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            inbound.drain();
            const TimePoint now = std::chrono::steady_clock::now();
            client.poll(now);
            if (updater != nullptr) {
                updater->poll(now);
            }
            if (done()) {
                return true;
            }
            std::unique_lock<std::mutex> lock{mutex};
            wake.wait_for(lock, std::chrono::milliseconds{20}, [this] { return woken; });
            woken = false;
        }
        return done();
    }

    std::mutex mutex;
    std::condition_variable wake;
    bool woken = false;
    Dispatcher inbound;
};

/// The link counters as `HIL-METRIC` lines, which run_hil.py collects. A green
/// case over this console with `ignored == 0` would mean the log backend was
/// silent, not that noise was handled -- so the numbers are the evidence.
void report(const SerialPortTransport& link, const char* prefix)
{
    const SerialLinkCounters c = link.counters();
    std::cout << "HIL-METRIC " << prefix << "_packets=" << c.deframe.packets << '\n'
              << "HIL-METRIC " << prefix << "_ignored=" << c.deframe.ignored << '\n'
              << "HIL-METRIC " << prefix << "_dropped_lines=" << c.dropped_lines << '\n'
              << "HIL-METRIC " << prefix << "_framing_errors=" << c.deframe.framing_errors << '\n'
              << "HIL-METRIC " << prefix << "_crc_failures=" << c.deframe.crc_failures << '\n'
              << "HIL-METRIC " << prefix << "_refused=" << c.send.refused << '\n';
}

} // namespace

TEST_CASE("hil: serial -- image state and echo over the console UART", "[hil][serial]")
{
    const std::string port = require_uart();
    Pump pump;
    Result<std::unique_ptr<SerialPortTransport>> opened =
        SerialPortTransport::open(console(port), pump.inbound);
    REQUIRE(opened.has_value());
    SerialPortTransport& link = **opened;

    SmpClient client{link};
    ImageManagement images{client};
    OsManagement os{client};

    std::optional<Result<std::string>> echoed;
    static_cast<void>(os.echo("smply-serial", [&](const Result<std::string>& r) { echoed = r; }));
    REQUIRE(pump.until(client, [&] { return echoed.has_value(); }, std::chrono::seconds{10}));
    REQUIRE(echoed->has_value());
    CHECK(**echoed == "smply-serial");

    std::optional<Result<ImageState>> state;
    static_cast<void>(images.get_state([&](const Result<ImageState>& r) { state = r; }));
    REQUIRE(pump.until(client, [&] { return state.has_value(); }, std::chrono::seconds{10}));
    REQUIRE(state->has_value());
    CHECK((*state)->active_slot() != nullptr);

    report(link, "serial");
    CHECK(link.counters().deframe.framing_errors == 0);
    CHECK(link.counters().deframe.crc_failures == 0);
}

TEST_CASE("hil: serial -- a whole update over the console UART (exploratory)",
          "[hil][serial][exploratory]")
{
    // Exploratory: not in run_hil.py's default run. What it is for is O7 -- the
    // first observation of what a real reset looks like on a serial port -- and
    // A26, whether the device keeps up with frames written back to back. A
    // failure here is a finding to record in protocol-notes.md, not a defect
    // to paper over.
    const std::string port = require_uart();
    const hil::Bench bench = hil::require_bench();
    const std::optional<std::vector<std::byte>> image_a = hil::read_file(bench.image_a);
    const std::optional<std::vector<std::byte>> image_b = hil::read_file(bench.image_b);
    REQUIRE(image_a.has_value());
    REQUIRE(image_b.has_value());

    Pump pump;
    std::vector<std::unique_ptr<SerialPortTransport>> links;
    {
        Result<std::unique_ptr<SerialPortTransport>> first =
            SerialPortTransport::open(console(port), pump.inbound);
        REQUIRE(first.has_value());
        links.push_back(std::move(*first));
    }
    SmpClient client{*links.back()};
    ImageManagement images{client};
    OsManagement os{client};
    FirmwareUpdater updater{client, images, os};

    // Install whichever image the device is not running.
    std::optional<Result<ImageState>> state;
    static_cast<void>(images.get_state([&](const Result<ImageState>& r) { state = r; }));
    REQUIRE(pump.until(client, [&] { return state.has_value(); }, std::chrono::seconds{10}));
    REQUIRE(state->has_value());
    const ImageSlot* running = (*state)->active_slot();
    REQUIRE(running != nullptr);
    MemoryImageSource source_a{ConstBytes{*image_a}};
    MemoryImageSource source_b{ConstBytes{*image_b}};
    const Result<McubootImageInfo> info_a =
        parse_mcuboot_header(ConstBytes{*image_a}.first(kMcubootHeaderSize));
    REQUIRE(info_a.has_value());
    const Result<std::optional<ImageHash>> hash_a = find_image_tlv_hash(source_a, *info_a);
    REQUIRE(hash_a.has_value());
    REQUIRE(hash_a->has_value());
    const bool running_a = running->hash.has_value() && *running->hash == **hash_a;
    ImageSource& target = running_a ? static_cast<ImageSource&>(source_b) : source_a;

    UpdatePlan plan;
    plan.disconnect_grace = std::chrono::seconds{3};
    std::optional<Result<UpdateReport>> finished;
    bool reconnect = false;
    bool confirm = false;
    std::string reset_seen = "none";
    const auto on_event = overloaded{
        [](const UpdateStateChanged& changed) {
            std::cout << "  " << to_string(changed.to) << '\n';
        },
        [](const UploadProgress&) {},
        [](const DisconnectExpected&) {},
        [&](const ReconnectRequired&) {
            reset_seen = client.connected() ? "grace" : "dropped";
            reconnect = true;
        },
        [&](const ConfirmationRequired&) { confirm = true; },
        [&](const UpdateFinished& f) { finished = f.result; },
    };
    REQUIRE(updater.start(target, plan, [&](const UpdateEvent& e) { std::visit(on_event, e); })
                .has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{8};
    while (!finished.has_value() && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(pump.until(
            client, [&] { return finished.has_value() || reconnect || confirm; },
            std::chrono::seconds{1}, &updater));
        if (reconnect) {
            reconnect = false;
            links.back()->close();
            bool attached = false;
            for (int attempt = 0; attempt < 20 && !attached; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
                Result<std::unique_ptr<SerialPortTransport>> again =
                    SerialPortTransport::open(console(port), pump.inbound);
                if (again.has_value()) {
                    links.push_back(std::move(*again));
                    client.rebind_transport(*links.back());
                    attached = true;
                }
            }
            if (!attached) {
                updater.reconnect_failed(
                    Error{ErrorCode::Disconnected, "hil: port did not return"});
                continue;
            }
            static_cast<void>(updater.resume_after_reconnect());
        }
        if (confirm) {
            confirm = false;
            static_cast<void>(updater.confirm());
        }
    }

    std::cout << "HIL-NOTE serial reset seen as: " << reset_seen << '\n';
    for (std::size_t i = 0; i < links.size(); ++i) {
        report(*links[i], i == 0 ? "serial_before_reset" : "serial_after_reset");
    }
    REQUIRE(finished.has_value());
    REQUIRE(finished->has_value());
    CHECK((*finished)->final_state == UpdateState::Completed);
}
