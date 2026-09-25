# SPDX-License-Identifier: Apache-2.0
#
# smply::serial_port -- the reference Transport over a serial port: a hardware
# UART, a USB CDC ACM port, or anything else that looks like a tty.
#
# Built on every platform: posix/ (termios and poll(); Linux and macOS) or
# win32/ (CreateFile and overlapped I/O), behind one header that names no OS
# type. Nothing here is part of libsmply: an adapter consumes smply::smply like
# any other consumer (architecture.md section 2), and the I/O thread it starts
# is the adapter's, never the core's (ADR-0004).
#
# NOT INSTALLED, as ADR-0020 records: a *reference* adapter, like
# smply::winrt_ble, shipped as source to read and copy. Its Win32 half is seen
# only by MSVC /W4 (tools/lint.sh excludes transports/serial_port/win32/), so a
# SemVer promise about its surface would rest on that alone. Everything a
# third-party serial adapter needs from the package -- the core, Dispatcher,
# the framing, SendQueue and LinkState -- is installed already.

add_library(smply_serial_port STATIC serial_port_config.cpp)
add_library(smply::serial_port ALIAS smply_serial_port)

if(WIN32)
    # Compiled by the windows-msvc job; never run against a port (ADR-0020).
    target_sources(smply_serial_port PRIVATE win32/serial_port_win32.cpp)
else()
    target_sources(smply_serial_port PRIVATE
        posix/posix_detail.cpp
        posix/serial_port_posix.cpp
    )
    # The adapter's own I/O thread (ADR-0020); the core still starts none.
    find_package(Threads REQUIRED)
    target_link_libraries(smply_serial_port PRIVATE Threads::Threads)
endif()

# transport_common is INTERFACE and carries both smply::smply and the
# transports/ include root, which is what lets this target write
# "serial/serial_framing.hpp" and "serial_port/serial_port_transport.hpp".
target_link_libraries(smply_serial_port
    PUBLIC
        smply::smply
        smply::util            # Dispatcher: the marshalling helper (ADR-0004)
        smply::transport_common
    PRIVATE
        $<BUILD_INTERFACE:smply_internal_options>
)

target_compile_features(smply_serial_port PUBLIC cxx_std_20)
set_target_properties(smply_serial_port PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME serial_port
)
