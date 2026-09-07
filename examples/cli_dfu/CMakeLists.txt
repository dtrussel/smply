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
    demo_image.cpp
    file_image_source.cpp
    loopback_transport.cpp
    stub_device.cpp
)

target_link_libraries(cli_dfu
    PRIVATE
        smply::smply
        smply::util       # Dispatcher: the whole point of the threading in here
        smply::minicbor   # the stub device has to speak CBOR; smply's own is internal
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
endif()
