// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_WINRT_BLE_DFU_SCANNER_HPP
#define SMPLY_EXAMPLE_WINRT_BLE_DFU_SCANNER_HPP

/// \file
/// Finding a device to update.
///
/// **Scanning is the application's job, not the transport's** (ADR-0005, and
/// `transports/winrt_ble/README.md` says so explicitly). `WinRtBleTransport`
/// is handed a Bluetooth address; turning "the device on my desk" into one of
/// those is what this file does.
///
/// ### What Zephyr actually advertises
///
/// Checked against Zephyr's own source rather than assumed
/// (docs/protocol-notes.md section 8, source S18):
///
/// * the **SMP service UUID is in the primary advertisement**
///   (`BT_DATA_UUID128_ALL` in the `smp_svr` sample's `ad[]`), so filtering on
///   it is reliable and works under a passive scan;
/// * the **device name is in the scan response** (`BT_DATA_NAME_COMPLETE` in
///   `sd[]`), not the advertisement -- so matching a name requires an **active**
///   scan. With WinRT's default passive mode a `--name` filter would match
///   nothing, silently, forever.
///
/// That is why the watcher below always scans actively, and why the service
/// UUID is the default filter.

#include "smply/result.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace smply::example {

/// How to choose a device.
struct ScanFilter
{
    /// A substring of the advertised local name. Empty means "any device
    /// advertising the SMP service", which is the usual case.
    std::string name;

    /// How long to look before giving up.
    std::chrono::milliseconds timeout{std::chrono::seconds{10}};
};

/// Scans until a matching device is seen, and returns its Bluetooth address.
///
/// \return The address, or `ErrorCode::Timeout` if nothing matched in time.
///         Blocks for up to `filter.timeout`; call it before the pump exists.
[[nodiscard]] Result<std::uint64_t> find_device(const ScanFilter& filter);

/// Renders an address the way Windows and most tools print one: `AA:BB:...`.
[[nodiscard]] std::string format_address(std::uint64_t address);

/// Parses `--address`, accepting `AA:BB:CC:DD:EE:FF` or a bare hex number.
[[nodiscard]] Result<std::uint64_t> parse_address(const std::string& text);

} // namespace smply::example

#endif // SMPLY_EXAMPLE_WINRT_BLE_DFU_SCANNER_HPP
