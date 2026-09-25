// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_SERIAL_PORT_CONFIG_HPP
#define SMPLY_TRANSPORTS_SERIAL_PORT_CONFIG_HPP

/// \file
/// What a caller may choose about a serial port, and what the adapter reports
/// back. Portable: no OS type appears here, so it is tested on every preset.
///
/// The adapter itself is `serial_port/serial_port_transport.hpp`; the decision
/// to ship it, and why it is not installed, is
/// [ADR-0020](../../docs/decisions/ADR-0020-serial-port-reference-adapter.md).

#include "common/send_queue.hpp"
#include "serial/serial_framing.hpp"

#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace smply::transport {

/// Hardware flow control. There is no software (XON/XOFF) option: the frames
/// are binary markers plus base64, and nothing in MCUmgr asks for it.
enum class FlowControl : std::uint8_t
{
    /// No flow control. What a Zephyr console UART and a USB CDC ACM port use.
    None,
    /// RTS/CTS. Only for a port whose peer actually drives CTS; on one that
    /// does not, every write stalls.
    RtsCts,
};

/// The default `SerialPortConfig::max_message_size`.
///
/// 256 is the Zephyr default of both `CONFIG_MCUMGR_TRANSPORT_UART_MTU` and
/// `CONFIG_MCUMGR_TRANSPORT_SHELL_MTU`, and it is below `384 - 4`, the largest
/// message a device with the default netbuf accepts **over serial**: the
/// device decodes the serial length prefix and CRC into the same buffer as the
/// message (docs/protocol-notes.md section 9, A25). Raise it only for a device
/// whose `buf_size` is known to be at least four bytes larger than the value
/// chosen.
inline constexpr std::size_t kDefaultSerialMessageSize = 256;

/// The smallest `SerialPortConfig::max_message_size` accepted.
///
/// Below this an 8-byte SMP header plus the first upload packet's CBOR
/// overhead cannot leave the 32-byte minimum chunk (protocol-notes section 6,
/// rule 2), so a smaller value is a mistake rather than a conservative choice.
/// The same floor, for the same reason, as the WinRT adapter's.
inline constexpr std::size_t kMinSerialMessageSize = 128;

/// Everything about a serial port a caller may choose.
///
/// **Always 8 data bits, no parity, one stop bit (8N1).** That is what every
/// Zephyr console and MCUmgr UART uses, so it is not a setting.
struct SerialPortConfig
{
    /// The port. A device path on POSIX (`/dev/ttyACM0`, or better a stable
    /// `/dev/serial/by-id/...` link), a port name on Windows (`COM4`).
    ///
    /// Opened afresh on every `SerialPortTransport::open()`, which is what lets
    /// an application reconnect to a USB CDC port that vanished across a reset
    /// and came back -- by the same path, if the path is a stable one.
    std::string path;

    /// Line speed in bits per second. Must be one `is_supported_baud()`
    /// accepts. A USB CDC ACM port and a pseudo-terminal ignore it, and open
    /// cleanly whatever it is.
    std::uint32_t baud = 115200;

    /// See `FlowControl`.
    FlowControl flow = FlowControl::None;

    /// What `Transport::max_message_size()` reports: the largest whole SMP
    /// message this transport will carry.
    ///
    /// **Not the frame size.** A message spans as many 127-byte console frames
    /// as it needs, so this is unrelated to `kMaxRawPerFrame`, and reporting
    /// that would cap every upload chunk at 93 bytes. At most
    /// `kMaxSerialPacket`, which is what the frame's length field can carry.
    std::size_t max_message_size = kDefaultSerialMessageSize;
};

/// Is \p baud a rate this adapter will configure?
///
/// The standard rates from 1200 to 921600, plus 1000000. A platform may still
/// refuse one it cannot set (macOS has no `B460800`, for instance), in which
/// case `SerialPortTransport::open()` answers `InvalidArgument`.
[[nodiscard]] bool is_supported_baud(std::uint32_t baud) noexcept;

/// Checks \p config without touching any port.
///
/// \return `InvalidArgument` for an empty path, an unsupported baud rate, or a
///         `max_message_size` outside `[kMinSerialMessageSize,
///         kMaxSerialPacket]`; success otherwise.
[[nodiscard]] Result<void> validate(const SerialPortConfig& config);

/// What the link has seen, as one snapshot.
///
/// Diagnostics, not protocol: nothing here changes what the transport does.
/// They exist because a serial link fails **quietly**. A peer whose frames are
/// consistently too long, or a console so full of log output that frames are
/// lost, looks exactly like a device that is switched off -- unless something
/// counts what was thrown away.
struct SerialLinkCounters
{
    /// What the deframer made of every line it was given: ignored console
    /// traffic, CRC failures, framing errors and packets delivered.
    SerialCounters deframe;
    /// Lines longer than a frame, dropped before the deframer saw them
    /// (`LineSplitter::dropped_lines()`). Non-zero is normal on a console that
    /// also carries a log backend; growing while nothing else moves is not.
    std::uint64_t dropped_lines = 0;
    /// Send admission: how often a message waited for the writer, and how
    /// often one was refused with `TransportBusy`.
    SendCounters send;
    /// Bytes read from and written to the port, framing included.
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
};

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_SERIAL_PORT_CONFIG_HPP
