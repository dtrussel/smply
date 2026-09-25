// SPDX-License-Identifier: Apache-2.0

#include "serial_port/serial_port_config.hpp"

#include "serial/serial_framing.hpp"

#include "smply/error.hpp"
#include "smply/result.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace smply::transport {
namespace {

/// The rates `is_supported_baud()` accepts. Each platform maps them to its own
/// constant, and may still refuse one it cannot set.
constexpr std::array<std::uint32_t, 12> kSupportedBauds{
    1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000,
};

} // namespace

bool is_supported_baud(std::uint32_t baud) noexcept
{
    return std::find(kSupportedBauds.begin(), kSupportedBauds.end(), baud) != kSupportedBauds.end();
}

Result<void> validate(const SerialPortConfig& config)
{
    if (config.path.empty()) {
        return fail(ErrorCode::InvalidArgument, "serial_port: no port path");
    }
    if (!is_supported_baud(config.baud)) {
        return fail(ErrorCode::InvalidArgument, "serial_port: unsupported baud rate");
    }
    if (config.max_message_size < kMinSerialMessageSize) {
        return fail(ErrorCode::InvalidArgument, "serial_port: max_message_size below the floor");
    }
    if (config.max_message_size > kMaxSerialPacket) {
        return fail(ErrorCode::InvalidArgument,
                    "serial_port: max_message_size above what a frame can describe");
    }
    return {};
}

} // namespace smply::transport
