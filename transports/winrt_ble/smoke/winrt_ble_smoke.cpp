// SPDX-License-Identifier: Apache-2.0

/// \file
/// Proves the WinRT adapter links and can be called. Not a functional test.
///
/// **Why this exists.** P15b's adapter cannot be exercised by CI: a GitHub
/// runner has no Bluetooth radio, so `windows-winrt` compiles the transport and
/// then never runs a byte through it. A compile-only job would also miss a
/// whole class of failure that only appears at link time -- a missing
/// `WindowsApp.lib`, an out-of-line destructor that was never defined, a pimpl
/// whose `State` is incomplete where it needs to be complete. This executable
/// costs a second and closes that gap.
///
/// **What it does not prove.** Nothing about GATT. The one assertion that must
/// hold needs no radio at all; the connection attempt afterwards is required
/// only to return rather than to crash or hang. Every behavioural claim about
/// this adapter is unverified until P17.

#include "winrt_ble/winrt_ble_transport.hpp"

#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/transport.hpp"
#include "smply/util/dispatcher.hpp"

// Through the prelude, not <winrt/base.h> directly: this target is compiled at
// /W4 /WX too, and the projection headers do not survive that.
#include "winrt_ble/detail/winrt_prelude.hpp"

#include <cstddef>
#include <iostream>
#include <memory>

namespace {

using smply::Dispatcher;
using smply::ErrorCode;
using smply::Result;
using smply::transport::WinRtBleConfig;
using smply::transport::WinRtBleTransport;

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "winrt_ble_smoke: FAILED: " << what << '\n';
        ++failures;
    }
}

} // namespace

int main()
{
    // Multi-threaded, because connect() blocks on asynchronous operations and a
    // single-threaded apartment would deadlock. This is the obligation the
    // adapter's README puts on every caller.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    Dispatcher inbound;

    // Deterministic, and the reason this test can assert anything at all:
    // argument validation happens before the Bluetooth stack is touched, so it
    // gives the same answer on a machine with no radio.
    const Result<std::unique_ptr<WinRtBleTransport>> too_small =
        WinRtBleTransport::connect(0, inbound, WinRtBleConfig{.max_message_size = 8});
    check(!too_small.has_value(), "a tiny max_message_size must be rejected");
    if (!too_small.has_value()) {
        check(too_small.error().code() == ErrorCode::InvalidArgument,
              "and rejected as InvalidArgument");
    }

    // No device has address 0. What matters is that this returns a Result --
    // whatever it says -- rather than crashing, throwing out of a noexcept
    // boundary, or hanging. On a runner with no radio it fails immediately.
    const Result<std::unique_ptr<WinRtBleTransport>> nowhere =
        WinRtBleTransport::connect(0, inbound, WinRtBleConfig{});
    check(!nowhere.has_value(), "connecting to address 0 must not succeed");

    // And the dispatcher must have nothing to hand over: connect() failed, so
    // nothing was ever subscribed.
    check(inbound.drain() == 0, "a failed connect posts no work");

    if (failures == 0) {
        std::cout << "winrt_ble_smoke: the adapter links and refuses what it should\n";
    }
    return failures == 0 ? 0 : 1;
}
