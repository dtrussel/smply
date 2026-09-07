// SPDX-License-Identifier: Apache-2.0

#include "scanner.hpp"

#include "winrt_prelude.hpp"

#include "common/smp_ble_uuid.hpp"

#include "smply/error.hpp"
#include "smply/result.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace smply::example {
namespace {

namespace advertisement = winrt::Windows::Devices::Bluetooth::Advertisement;

/// The SMP service UUID as a projection GUID.
///
/// Built from `transports/common/smp_ble_uuid.hpp` rather than typed again --
/// those bytes and the endian split are unit-tested on every platform, and a
/// second transcription here would be a second thing to get wrong.
[[nodiscard]] winrt::guid smp_service_guid() noexcept
{
    const transport::Uuid128Fields fields = transport::uuid_fields(transport::kSmpServiceUuid);
    return winrt::guid{fields.data1, fields.data2, fields.data3, fields.data4};
}

[[nodiscard]] std::string to_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

Result<std::uint64_t> find_device(const ScanFilter& filter)
{
    std::mutex mutex;
    std::condition_variable found;
    std::optional<std::uint64_t> address;

    advertisement::BluetoothLEAdvertisementWatcher watcher;

    // Active, always. The name a --name filter matches lives in the SCAN
    // RESPONSE (protocol-notes section 8), which a passive scan never asks for
    // -- so a passive watcher would match nothing and look like a device that
    // is not there. Active scanning costs a scan request per advertisement and
    // removes a whole class of "it just never finds it".
    watcher.ScanningMode(advertisement::BluetoothLEScanningMode::Active);

    const std::string wanted = to_lower(filter.name);
    if (wanted.empty()) {
        // Let the OS do the filtering: Zephyr's sample puts the SMP service
        // UUID in the primary advertisement, so this is both reliable and
        // cheaper than inspecting every packet ourselves.
        watcher.AdvertisementFilter().Advertisement().ServiceUuids().Append(smp_service_guid());
    }

    const auto revoker =
        watcher.Received(winrt::auto_revoke,
                         [&](const advertisement::BluetoothLEAdvertisementWatcher&,
                             const advertisement::BluetoothLEAdvertisementReceivedEventArgs& args) {
                             if (!wanted.empty()) {
                                 const std::string name =
                                     to_lower(winrt::to_string(args.Advertisement().LocalName()));
                                 if (name.find(wanted) == std::string::npos) {
                                     return;
                                 }
                             }
                             {
                                 const std::lock_guard<std::mutex> lock{mutex};
                                 if (address.has_value()) {
                                     return; // already answered; later advertisements are noise
                                 }
                                 address = args.BluetoothAddress();
                             }
                             found.notify_one();
                         });

    try {
        watcher.Start();
    } catch (const winrt::hresult_error&) {
        return fail(ErrorCode::TransportError, "winrt_ble_dfu: could not start scanning");
    }

    {
        std::unique_lock<std::mutex> lock{mutex};
        found.wait_for(lock, filter.timeout, [&] { return address.has_value(); });
    }

    // Stopped before returning, and the handler revoked as `revoker` goes out
    // of scope, so nothing touches the locals above once this function ends.
    try {
        watcher.Stop();
    } catch (const winrt::hresult_error&) { // NOLINT(bugprone-empty-catch)
        // Stopping a watcher that already stopped is not a failure worth
        // reporting to a caller that has its answer.
    }

    const std::lock_guard<std::mutex> lock{mutex};
    if (!address.has_value()) {
        return fail(ErrorCode::Timeout, "winrt_ble_dfu: no matching device was seen");
    }
    return *address;
}

std::string format_address(std::uint64_t address)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (int shift = 40; shift >= 0; shift -= 8) {
        const auto byte = static_cast<unsigned>((address >> shift) & 0xFFU);
        if (!out.empty()) {
            out.push_back(':');
        }
        out.push_back(kDigits[byte >> 4U]);
        out.push_back(kDigits[byte & 0x0FU]);
    }
    return out;
}

Result<std::uint64_t> parse_address(const std::string& text)
{
    std::uint64_t value = 0;
    unsigned digits = 0;
    for (const char c : text) {
        if (c == ':' || c == '-') {
            continue;
        }
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A') + 10U;
        } else {
            return fail(ErrorCode::InvalidArgument, "winrt_ble_dfu: not a Bluetooth address");
        }
        value = (value << 4U) | digit;
        ++digits;
        if (digits > 12) {
            return fail(ErrorCode::InvalidArgument, "winrt_ble_dfu: address is too long");
        }
    }
    if (digits == 0) {
        return fail(ErrorCode::InvalidArgument, "winrt_ble_dfu: address is empty");
    }
    return value;
}

} // namespace smply::example
