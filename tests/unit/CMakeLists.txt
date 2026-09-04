# SPDX-License-Identifier: Apache-2.0

add_executable(smply_unit_tests
    test_clock.cpp
    test_error.cpp
    test_result.cpp
    test_version.cpp
)

target_link_libraries(smply_unit_tests
    PRIVATE
        smply::smply_internal
        smply_internal_options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(smply_unit_tests)
