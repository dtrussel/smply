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
        smply::dfu_app    # UpdateInputs, and UpdateRun: the pump and its reconnects
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

    # The MCUboot mode (ADR-0025): reported by the device, and the plan's
    # fallback used when the device reports none.
    add_test(NAME cli_dfu_mode_reported COMMAND cli_dfu --quiet --stub-mode swap-using-move)
    set_tests_properties(cli_dfu_mode_reported PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*bootloader: swap-using-move \\(reported by the device\\)"
        TIMEOUT 60
    )
    add_test(NAME cli_dfu_mode_fallback COMMAND cli_dfu --quiet --fallback-mode swap-using-scratch)
    set_tests_properties(cli_dfu_mode_fallback PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*bootloader: swap-using-scratch \\(not reported; the fallback was used\\)"
        TIMEOUT 60
    )

    # A mode that cannot revert is refused before anything is sent, and the
    # plan's opt-in lets the same update through (ADR-0025). The stub runs the
    # mode it reports, so the second run overwrites the image.
    add_test(NAME cli_dfu_mode_refused COMMAND cli_dfu --quiet --stub-mode upgrade-only)
    set_tests_properties(cli_dfu_mode_refused PROPERTIES
        PASS_REGULAR_EXPRESSION "update failed: firmware update refused \\(at dfu: the bootloader cannot revert this update\\).*bootloader: upgrade-only \\(reported by the device\\)"
        TIMEOUT 60
    )
    add_test(NAME cli_dfu_mode_allow_no_revert
        COMMAND cli_dfu --quiet --stub-mode upgrade-only --allow-no-revert)
    set_tests_properties(cli_dfu_mode_allow_no_revert PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*bootloader: upgrade-only \\(reported by the device\\)"
        TIMEOUT 60
    )

    # One build per slot (ADR-0025): the stub reports direct-XIP with revert,
    # and the build for the free slot is the one sent. The stub runs it: the
    # upload lands in slot 1, and the device boots from it on trial.
    add_test(NAME cli_dfu_mode_two_builds
        COMMAND cli_dfu --quiet --stub-mode direct-xip-with-revert --demo-builds)
    set_tests_properties(cli_dfu_mode_two_builds PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*bootloader: direct-xip-with-revert \\(reported by the device\\)"
        TIMEOUT 60
    )

    # Plain direct-XIP (ADR-0025): no revert, so the update needs the opt-in,
    # and no set-state (protocol-notes S10). The expected lines are what the
    # updater does against ServerSimulator in mode 4: the build for the free
    # slot sent (the 4172-byte second build, the stub running slot 0), a reset
    # with no mark and no confirm, and the device found running the new image.
    # Run without --quiet so that a mark or a confirm would show.
    add_test(NAME cli_dfu_mode_direct_xip
        COMMAND cli_dfu --stub-mode direct-xip --demo-builds --allow-no-revert)
    set_tests_properties(cli_dfu_mode_direct_xip PROPERTIES
        PASS_REGULAR_EXPRESSION
            "Uploading.*Resetting.*VerifyingBooted.*update Completed.*bytes transferred: 4172.*bootloader: direct-xip \\(reported by the device\\)"
        FAIL_REGULAR_EXPRESSION "MarkingForTest|Confirming|confirming"
        TIMEOUT 60
    )

    # An nRF Connect SDK direct-XIP package (S58): one image, a build per slot,
    # read by smply::dfu_package as one image with two builds, and the free
    # slot's build sent, and the stub booting from that slot.
    add_test(NAME cli_dfu_xip_package
        COMMAND cli_dfu --quiet --stub-mode direct-xip-with-revert --demo-xip-package)
    set_tests_properties(cli_dfu_xip_package PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*bootloader: direct-xip-with-revert \\(reported by the device\\)"
        TIMEOUT 60
    )

    # The multi-image update of ADR-0021 end to end: a generated package read
    # by smply::dfu_package, both images staged before one reset, image 1
    # applied by the device, image 0 confirmed only after that, and image 1
    # committed by the device only after image 0 (ADR-0022).
    add_test(NAME cli_dfu_package COMMAND cli_dfu --quiet --demo-package)
    set_tests_properties(cli_dfu_package PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*image 1 \\(device\\): committed"
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
    # Image 1 committed by the client on a device built with Zephyr's
    # defaults (protocol-notes A27): both images swap on trial, image 0 is
    # confirmed, and the confirm of image 1 by its hash is denied with
    # IMAGE_CONFIRMATION_DENIED (32). cli_dfu speaks SMP v1, so the denial
    # arrives translated onto a flat rc=1 (MGMT_ERR_EUNKNOWN, protocol-notes
    # A16) and the updater reports it at the SMP layer. The expected lines are
    # what the updater does against ServerSimulator with its default config
    # (two images, the A27 Kconfigs off, translate_v1_errors on): it fails in
    # Confirming with [smp rc=1] after its one confirm landed, leaving image 0
    # confirmed and image 1's trial to revert, and warns of the revert. The
    # report lists a client image by bytes only, so image 0's confirm shows as
    # the failure coming after it, not as a line of its own.
    add_test(NAME cli_dfu_package_client_confirm_denied
        COMMAND cli_dfu --quiet --demo-package --commit 1=client)
    set_tests_properties(cli_dfu_package_client_confirm_denied PROPERTIES
        PASS_REGULAR_EXPRESSION
            "update failed: MCUmgr protocol error \\[smp rc=1\\] \\(at smp client\\).*image 0 \\(client\\).*image 1 \\(client\\).*will revert on the next reset"
        TIMEOUT 60
    )
    # The package board check (protocol-notes section 5). The demo packages'
    # manifests name the invented board "demo_board": a bare board name for
    # image 0, a board target for image 1. A matching --board completes, also
    # given as a board target; the direct-XIP package's two builds are checked
    # too.
    add_test(NAME cli_dfu_package_board COMMAND cli_dfu --quiet --demo-package --board demo_board)
    set_tests_properties(cli_dfu_package_board PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*image 1 \\(device\\): committed"
        TIMEOUT 60
    )
    add_test(NAME cli_dfu_package_board_target
        COMMAND cli_dfu --quiet --demo-package --board demo_board@1.0.0/demo_soc/cpuapp)
    set_tests_properties(cli_dfu_package_board_target PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed.*image 1 \\(device\\): committed"
        TIMEOUT 60
    )
    add_test(NAME cli_dfu_xip_package_board
        COMMAND cli_dfu --quiet --stub-mode direct-xip-with-revert --demo-xip-package
                --board demo_board)
    set_tests_properties(cli_dfu_xip_package_board PROPERTIES
        PASS_REGULAR_EXPRESSION "update Completed"
        TIMEOUT 60
    )
    # A package for another board is refused before anything is sent. Run
    # without --quiet, so an update that had begun would print something:
    # opening the link prints nothing, but every line the tool writes once
    # updater.start() has been called is indented by two spaces (each state
    # change, QueryingParameters first, the upload progress, the reboot and
    # reconnect lines, a failure's details) or starts "update " (the report,
    # "update failed"), and the refusal line is neither.
    # FAIL_REGULAR_EXPRESSION turns any of that into a failure even though the
    # refusal is also printed.
    add_test(NAME cli_dfu_package_board_mismatch
        COMMAND cli_dfu --demo-package --board other_board)
    set_tests_properties(cli_dfu_package_board_mismatch PROPERTIES
        PASS_REGULAR_EXPRESSION
            "cli_dfu: invalid argument: image 0: expected board name \"other_board\", the manifest says \"demo_board\""
        FAIL_REGULAR_EXPRESSION "  [A-Za-z]|update |gave up waiting"
        TIMEOUT 60
    )
    # --board with anything but a package is a usage error, as is a board with
    # no board name: the built-in image, a named image, a build pair, an empty
    # expectation, and one that is empty once cut at its first '/' or '@'.
    foreach(case IN ITEMS builtin image pair qualifier)
        if(case STREQUAL "builtin")
            set(args --board demo_board)
        elseif(case STREQUAL "image")
            set(args --image app.bin --board demo_board)
        elseif(case STREQUAL "pair")
            set(args --image app.bin --image-secondary app_slot1.bin --board demo_board)
        else()
            set(args --demo-package --board /cpuapp)
        endif()
        add_test(NAME cli_dfu_board_usage_${case} COMMAND cli_dfu ${args})
        set_tests_properties(cli_dfu_board_usage_${case} PROPERTIES
            PASS_REGULAR_EXPRESSION "usage: cli_dfu"
            FAIL_REGULAR_EXPRESSION "QueryingParameters|update Completed"
            TIMEOUT 60
        )
    endforeach()
    # Spelled out: an empty list element would vanish from ${args}.
    add_test(NAME cli_dfu_board_usage_empty COMMAND cli_dfu --demo-package --board "")
    set_tests_properties(cli_dfu_board_usage_empty PROPERTIES
        PASS_REGULAR_EXPRESSION "usage: cli_dfu"
        FAIL_REGULAR_EXPRESSION "QueryingParameters|update Completed"
        TIMEOUT 60
    )
    # The rules every tool shares (support/dfu_app/update_command.hpp), and
    # this tool's own, each rejected as a usage error before anything starts:
    # two things to install, --commit without a package, a second build without
    # a named first, two generated packages, a failed apply with no device
    # image to fail, and the generated second build beside a named first.
    foreach(case IN ITEMS two_sources commit secondary two_packages apply_fails builds)
        if(case STREQUAL "two_sources")
            set(args --image app.bin --package app.zip)
        elseif(case STREQUAL "commit")
            set(args --commit 1=device)
        elseif(case STREQUAL "secondary")
            set(args --image-secondary app_slot1.bin)
        elseif(case STREQUAL "two_packages")
            set(args --demo-package --demo-xip-package)
        elseif(case STREQUAL "apply_fails")
            set(args --apply-fails)
        else()
            set(args --image app.bin --demo-builds)
        endif()
        add_test(NAME cli_dfu_usage_${case} COMMAND cli_dfu ${args})
        set_tests_properties(cli_dfu_usage_${case} PROPERTIES
            PASS_REGULAR_EXPRESSION "usage: cli_dfu"
            FAIL_REGULAR_EXPRESSION "QueryingParameters|update Completed"
            TIMEOUT 60
        )
    endforeach()
endif()
