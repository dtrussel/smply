// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_SERIAL_PORT_POSIX_DETAIL_HPP
#define SMPLY_TRANSPORTS_SERIAL_PORT_POSIX_DETAIL_HPP

/// \file
/// The pure parts of the POSIX implementation: which `termios` speed a baud
/// rate is, and what an `errno` means to the transport contract.
///
/// Separate from `serial_port_posix.cpp` so a unit test can check both
/// without opening anything, which is how the error mapping gets checked for
/// failures (a port another process holds, a path that is not a tty) that a
/// test cannot conveniently cause for real.

#include "smply/error.hpp"

#include <termios.h>

#include <cstdint>
#include <optional>

namespace smply::transport::posix {

/// The `termios` speed constant for \p baud, or `std::nullopt` when this
/// platform has none. Only rates `is_supported_baud()` accepts are mapped.
[[nodiscard]] std::optional<speed_t> speed_for_baud(std::uint32_t baud) noexcept;

/// What a failed system call means to a `Transport` caller.
///
/// | `errno` | `ErrorCode` | Why |
/// | ------- | ----------- | --- |
/// | `ENOENT`, `ENODEV`, `ENXIO`, `EIO` | `Disconnected` | The port is not there, or stopped being
/// there. A USB port mid-reset looks like this, so a reconnect loop retries it. | | `ENOTTY` |
/// `InvalidArgument` | The path exists but is not a serial port. Retrying will not help. | |
/// anything else | `TransportError` | Includes `EACCES` and `EBUSY`: the port exists but cannot be
/// used. |
///
/// \param where A static, literal call-site tag, as `Error` requires.
[[nodiscard]] Error error_from_errno(int error_number, const char* where) noexcept;

} // namespace smply::transport::posix

#endif // SMPLY_TRANSPORTS_SERIAL_PORT_POSIX_DETAIL_HPP
