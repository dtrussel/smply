# SPDX-License-Identifier: Apache-2.0
#
# Shared test doubles. A library rather than a source list, because from P11
# the component tests build a second executable against the same doubles.
#
# Never linked into smply itself.

add_library(smply_test_support STATIC
    fake_transport.cpp
    server_simulator.cpp
)
add_library(smply::test_support ALIAS smply_test_support)

target_include_directories(smply_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(smply_test_support
    PUBLIC
        smply::smply_internal
        # The CBOR codec moved to support/ in P14b so the cli_dfu example could
        # use it too. PUBLIC: the suites build request bodies with it directly.
        smply::minicbor
    PRIVATE
        smply_internal_options
)
