# SPDX-License-Identifier: Apache-2.0
#
# The component suite: the real client stack driven into ServerSimulator over
# FakeTransport, plus the simulator's own tests.
#
# A second executable rather than more files in tests/unit, because these tests
# are about the sequence of commands rather than about one component, and
# keeping them apart makes "the unit suite is green but the stack is not" a
# statement CTest can make.

add_executable(smply_component_tests
    test_firmware_update.cpp
    test_round_trip.cpp
    test_simulator.cpp
)

target_link_libraries(smply_component_tests
    PRIVATE
        smply::smply_internal
        smply::test_support
        smply_internal_options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(smply_component_tests)
