# SPDX-License-Identifier: Apache-2.0
#
# The pretend device both portable examples drive: cli_dfu over an in-process
# loopback, serial_dfu over a pseudo-terminal. Scaffolding, not a protocol
# reference -- tests/support/server_simulator.* is that (stub_device.hpp says
# why). A library only so the two examples share one copy, and like them it
# links the PUBLIC targets only.

add_library(smply_example_stub STATIC
    stub_device.cpp
    demo_image.cpp
)

# The include root is examples/, so a user writes "stub_device/stub_device.hpp"
# and it is obvious where the header came from.
target_include_directories(smply_example_stub PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(smply_example_stub
    PUBLIC
        smply::smply
    PRIVATE
        smply::minicbor   # the device has to speak CBOR; smply's own is internal
        smply_internal_options
)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(smply_example_stub PUBLIC Threads::Threads)
endif()
