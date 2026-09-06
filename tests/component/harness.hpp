// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_COMPONENT_HARNESS_HPP
#define SMPLY_TESTS_COMPONENT_HARNESS_HPP

/// \file
/// The loop that lets a real client and a simulated device take turns, and the
/// fixture that wires them together in an order that survives destruction.

#include "fake_transport.hpp"
#include "image_builder.hpp"
#include "manual_clock.hpp"
#include "server_simulator.hpp"

#include "smply/clock.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/image_source.hpp"
#include "smply/smp_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smply::test {

/// How many turns `run_until` will take before giving up.
///
/// A budget rather than a wait: a state machine that stops making progress must
/// fail the test in bounded time, not hang a CI job until the runner kills it.
inline constexpr int kDefaultBudget = 4000;

/// Runs device-then-client turns until \p done, or the budget is exhausted.
///
/// The device goes first, so a request sent in one turn is answered in the same
/// one when the configured response delay is zero.
///
/// \return true if \p done became true within the budget.
template<class Predicate>
bool run_until(ServerSimulator& simulator, SmpClient& client, ManualClock& clock, Predicate done,
               int budget = kDefaultBudget, Duration step = std::chrono::milliseconds{1})
{
    for (int i = 0; i < budget; ++i) {
        if (done()) {
            return true;
        }
        simulator.pump(clock.now());
        client.poll(clock.now());
        clock.advance(step);
    }
    return done();
}

/// Builds a signed-looking MCUboot image with a hash TLV, as imgtool would.
///
/// The content is what actually gets uploaded, so every test that checks "the
/// flashed bytes equal the source" is comparing real image bytes rather than
/// filler.
[[nodiscard]] inline std::vector<std::byte>
make_firmware(std::uint32_t body_size, std::uint8_t major = 1, std::uint8_t minor = 2,
              std::uint16_t revision = 3, std::uint8_t fill = 0)
{
    // A distinct hash TLV per image, so two firmwares are distinguishable by
    // the value the device reports for a slot.
    std::vector<std::byte> hash(32);
    for (std::size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<std::byte>((i * 7 + major * 31 + minor * 17 + fill) & 0xFFU);
    }

    ImageBuilder builder;
    builder.version(major, minor, revision, 0).body(body_size).tlv(0x10, std::move(hash));
    return builder.build();
}

/// Transport, clock, device, client and groups, declared in the one order that
/// is safe.
///
/// Everything a callback touches must outlive **both** the client and the
/// group: `~SmpClient` completes outstanding requests, and `~ImageManagement`
/// completes an upload still holding its callback. Members are destroyed in
/// reverse declaration order, so the two of them must come last -- a mistake
/// this fixture exists to make impossible, and which Clang's ASan is the only
/// thing that catches.
struct Fixture
{
    explicit Fixture(ServerConfig config = {}, SmpClientConfig client_config = {})
        : simulator{transport, config}, client{transport, clock, client_config}, management{client},
          os{client}
    {}

    FakeTransport transport;
    ManualClock clock;
    ServerSimulator simulator;
    SmpClient client;
    ImageManagement management;
    OsManagement os;

    /// One device-then-client turn.
    void step(Duration by = std::chrono::milliseconds{1})
    {
        simulator.pump(clock.now());
        client.poll(clock.now());
        clock.advance(by);
    }

    template<class Predicate>
    bool run_until(Predicate done, int budget = kDefaultBudget,
                   Duration step = std::chrono::milliseconds{1})
    {
        return smply::test::run_until(simulator, client, clock, done, budget, step);
    }
};

/// What an upload reported, and how many times.
///
/// Declared by a test **before** its fixture, so it outlives the client and the
/// group whose destructors complete the callback.
struct UploadOutcome
{
    int calls = 0;
    std::optional<ErrorCode> code;
    std::optional<UploadResult> value;
    std::vector<UploadProgress> progress;

    [[nodiscard]] auto on_done()
    {
        return [this](Result<UploadResult> result) {
            ++calls;
            if (result.has_value()) {
                value = *result;
            } else {
                code = result.error().code();
            }
        };
    }

    [[nodiscard]] auto on_progress()
    {
        return [this](UploadProgress step) { progress.push_back(step); };
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return calls > 0;
    }
};

} // namespace smply::test

#endif // SMPLY_TESTS_COMPONENT_HARNESS_HPP
