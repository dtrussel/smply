// SPDX-License-Identifier: Apache-2.0

#include "serial_port/posix/posix_detail.hpp"

#include "smply/error.hpp"

#include <termios.h>

#include <cerrno>
#include <cstdint>
#include <optional>

namespace smply::transport::posix {

std::optional<speed_t> speed_for_baud(std::uint32_t baud) noexcept
{
    switch (baud) {
    case 1200:
        return B1200;
    case 2400:
        return B2400;
    case 4800:
        return B4800;
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    case 230400:
        return B230400;
#ifdef B460800
    case 460800:
        return B460800;
#endif
#ifdef B921600
    case 921600:
        return B921600;
#endif
#ifdef B1000000
    case 1000000:
        return B1000000;
#endif
    default:
        return std::nullopt;
    }
}

Error error_from_errno(int error_number, const char* where) noexcept
{
    switch (error_number) {
    case ENOENT:
    case ENODEV:
    case ENXIO:
    case EIO:
        return Error{ErrorCode::Disconnected, where};
    case ENOTTY:
        return Error{ErrorCode::InvalidArgument, where};
    default:
        return Error{ErrorCode::TransportError, where};
    }
}

} // namespace smply::transport::posix
