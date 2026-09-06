# SPDX-License-Identifier: Apache-2.0

add_executable(smply_unit_tests
    test_assembler.cpp
    test_cbor.cpp
    test_clock.cpp
    test_error.cpp
    test_fake_transport.cpp
    test_image_group.cpp
    test_image_source.cpp
    test_mcuboot_header.cpp
    test_mgmt_error.cpp
    test_os_group.cpp
    test_result.cpp
    test_sha256.cpp
    test_smp_client.cpp
    test_smp_codec.cpp
    test_tlv.cpp
    test_upload_driver.cpp
    test_upload_session.cpp
    test_version.cpp
)

target_link_libraries(smply_unit_tests
    PRIVATE
        smply::smply_internal
        smply::test_support
        smply_internal_options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(smply_unit_tests)
