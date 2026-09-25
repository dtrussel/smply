# SPDX-License-Identifier: Apache-2.0
#
# The serial port adapter against real operating-system objects.
#
# Separate from tests/unit and tests/component on purpose: those suites never
# read the real clock and never start a thread (testing.md section 2). These do
# both, because what they test is exactly the part a simulated suite cannot
# reach: a real tty, a real I/O thread, a real hang-up. Every wait is bounded,
# and a test that times out fails rather than hangs.
#
# On POSIX the peer is a pseudo-terminal the test opens itself; on Windows,
# where nothing here can create a COM port, only the cases that need no port
# run -- which makes this the link-and-call smoke test for the Win32 half.

add_executable(smply_serial_port_tests test_serial_port.cpp)

target_link_libraries(smply_serial_port_tests
    PRIVATE
        smply::serial_port
        smply_internal_options
        Catch2::Catch2WithMain
)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(smply_serial_port_tests PRIVATE Threads::Threads)
endif()

include(Catch)
catch_discover_tests(smply_serial_port_tests)
