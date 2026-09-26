# SPDX-License-Identifier: Apache-2.0
#
# The serial DFU example: cli_dfu's pump loop over smply::serial_port.
#
# With --port it drives a real device. Without one it starts the stub device
# behind a pseudo-terminal -- POSIX only, since nothing here can create a COM
# port -- which is how it runs in CI. On Windows it builds (so the Win32 adapter
# has a real caller that MSVC compiles) and needs --port to run.
#
# Like cli_dfu it links public targets only, plus the example-side libraries.

add_executable(serial_dfu main.cpp)
if(NOT WIN32)
    target_sources(serial_dfu PRIVATE pty_stub.cpp)
endif()

target_link_libraries(serial_dfu
    PRIVATE
        smply::smply
        smply::util
        smply::serial_port
        smply::dfu_app
        smply_example_stub
        smply_internal_options
)
target_include_directories(serial_dfu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The end-to-end tests: a whole update, reset included, over a real tty, in
# both of the reset shapes roadmap O7 distinguishes. Each prints one summary
# line and passes on it -- PASS_REGULAR_EXPRESSION, not the exit code, so a
# run that "succeeds" without the reset having looked the way it should still
# fails (handoff.md, on WILL_FAIL tests passing for the wrong reason).
if(SMPLY_BUILD_TESTS AND NOT WIN32)
    # A UART stays open: the updater moves on when disconnect_grace expires,
    # and the boot output arrives as ignored lines and a dropped long line.
    add_test(NAME serial_dfu_pty_uart COMMAND serial_dfu --quiet --stub uart)
    set_tests_properties(serial_dfu_pty_uart PROPERTIES
        PASS_REGULAR_EXPRESSION
            "serial_dfu: Completed reset=grace links=2 devices=1 packets=[1-9][0-9]* ignored=[1-9][0-9]* dropped_lines=[1-9][0-9]* framing_errors=0 crc_failures=0 refused=0"
        TIMEOUT 60
    )

    # A USB CDC port vanishes and returns as a different pseudo-terminal
    # behind the same symlink: the adapter sees the hang-up, and the
    # application reopens the stable path. devices=2 is the proof that the
    # second open really reached a different tty.
    add_test(NAME serial_dfu_pty_cdc COMMAND serial_dfu --quiet --stub cdc)
    set_tests_properties(serial_dfu_pty_cdc PROPERTIES
        PASS_REGULAR_EXPRESSION
            "serial_dfu: Completed reset=dropped links=2 devices=2 packets=[1-9][0-9]* ignored=0 dropped_lines=0 framing_errors=0 crc_failures=0 refused=0"
        TIMEOUT 60
    )
    # A two-image package over the UART shape (ADR-0021): both images staged
    # before the one reset, image 1 applied by the stub, image 0 confirmed.
    add_test(NAME serial_dfu_pty_package COMMAND serial_dfu --quiet --stub uart --demo-package)
    set_tests_properties(serial_dfu_pty_package PROPERTIES
        PASS_REGULAR_EXPRESSION
            "serial_dfu: Completed reset=grace links=2 devices=1 packets=[1-9][0-9]* .*framing_errors=0 crc_failures=0 refused=0 images=2 applied=1 committed=1"
        TIMEOUT 60
    )
endif()
