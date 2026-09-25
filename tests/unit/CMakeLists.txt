# SPDX-License-Identifier: Apache-2.0

add_executable(smply_unit_tests
    test_assembler.cpp
    test_ble_framing.cpp
    test_cbor.cpp
    test_clock.cpp
    test_dispatcher.cpp
    test_error.cpp
    test_file_image_source.cpp
    test_fake_transport.cpp
    test_image_group.cpp
    test_image_source.cpp
    test_limits.cpp
    test_mcuboot_header.cpp
    test_narrow.cpp
    test_mgmt_error.cpp
    test_os_group.cpp
    test_reconnect_policy.cpp
    test_result.cpp
    test_send_queue.cpp
    test_serial_framing.cpp
    test_serial_port_config.cpp
    test_sha256.cpp
    test_smp_client.cpp
    test_smp_codec.cpp
    test_source_reader.cpp
    test_tlv.cpp
    test_update_state_machine.cpp
    test_upload_driver.cpp
    test_upload_session.cpp
    test_version.cpp
)

target_link_libraries(smply_unit_tests
    PRIVATE
        smply::smply_internal
        # Dispatcher is not part of the core, so testing it means linking the
        # separate target a real adapter would link.
        smply::util
        smply::transport_common
        # The serial port adapter's pure parts: configuration, the byte-level
        # link helpers, and the POSIX baud and errno tables. Nothing here opens
        # a port; tests/serial_port/ does.
        smply::serial_port
        # The application-side helpers the examples share: the reconnect policy
        # is tested here because examples/winrt_ble_dfu/ cannot be run at all.
        smply::dfu_app
        smply::test_support
        smply_internal_options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(smply_unit_tests)
