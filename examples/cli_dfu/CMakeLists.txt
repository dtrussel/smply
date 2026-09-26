# SPDX-License-Identifier: Apache-2.0
#
# The portable DFU example: the pump loop, driven against a stub device running
# on its own thread.
#
# It links the PUBLIC targets only -- never smply::smply_internal, never
# smply::test_support. That is not tidiness: an example that reached into the
# library's private headers would demonstrate something no consumer can do.

add_executable(cli_dfu
    main.cpp
    loopback_transport.cpp
)

target_link_libraries(cli_dfu
    PRIVATE
        smply::smply
        smply::util       # Dispatcher: the whole point of the threading in here
        smply_example_stub # the pretend device, shared with serial_dfu
        smply::dfu_app    # FileImageSource, and the reconnect policy below
        smply_internal_options
)

target_include_directories(cli_dfu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(cli_dfu PRIVATE Threads::Threads)
endif()

# Run it as a test when there is a test harness to run it in. This is what makes
# "the example performs a complete simulated update end to end" a check rather
# than a claim: it exercises the pump, the Dispatcher, the reconnect protocol and
# the confirmation window on every push.
if(SMPLY_BUILD_TESTS)
    add_test(NAME cli_dfu_demo COMMAND cli_dfu --quiet)
    set_tests_properties(cli_dfu_demo PROPERTIES TIMEOUT 60)

    # The two below are why --flaky-reconnect exists. A real BLE reconnect after
    # a reboot needs retry, backoff and a point at which it gives up. This
    # example otherwise reconnects instantly to a stub in its own process, and
    # FirmwareUpdater::reconnect_failed() would be reached only by the component
    # suite. These make both paths run on every push.
    add_test(NAME cli_dfu_flaky_reconnect COMMAND cli_dfu --quiet --flaky-reconnect 3)
    set_tests_properties(cli_dfu_flaky_reconnect PROPERTIES TIMEOUT 60)

    # Refuses more attempts than the policy allows, so the policy is exhausted
    # and the update must fail cleanly -- not hang, which is the failure mode a
    # give-up path exists to prevent. It must fail *for that reason*: matching
    # the give-up message, rather than WILL_FAIL's "any non-zero exit", keeps a
    # failure anywhere else (a short read, a crash) from passing as this one.
    add_test(NAME cli_dfu_reconnect_gives_up COMMAND cli_dfu --quiet --flaky-reconnect 99)
    set_tests_properties(cli_dfu_reconnect_gives_up PROPERTIES
        PASS_REGULAR_EXPRESSION "update failed: disconnected \\(at cli_dfu: could not reconnect\\)"
        TIMEOUT 60
    )

    # The multi-image update of ADR-0021 end to end: a generated package read
    # by smply::dfu_package, both images staged before one reset, image 1
    # applied by the device, and image 0 confirmed only after that.
    add_test(NAME cli_dfu_package COMMAND cli_dfu --quiet --demo-package)
    set_tests_properties(cli_dfu_package PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*image 1 \\(device\\): applied"
        TIMEOUT 60
    )
    # The device fails to apply image 1, so image 0 must be left unconfirmed:
    # the update fails for that reason, and says the next reset will revert.
    add_test(NAME cli_dfu_package_apply_fails COMMAND cli_dfu --quiet --demo-package --apply-fails)
    set_tests_properties(cli_dfu_package_apply_fails PROPERTIES
        PASS_REGULAR_EXPRESSION
            "update failed: firmware update failed \\(at dfu: device did not apply an image\\).*image 1 \\(device\\): not applied.*will revert on the next reset"
        TIMEOUT 60
    )
endif()
