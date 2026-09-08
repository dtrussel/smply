# SPDX-License-Identifier: Apache-2.0
#
# smply_hil -- the hardware interoperability cases (P17b).
#
# Opt-in (SMPLY_BUILD_HIL=ON), Windows-only, and never part of a PR preset:
# every case needs the bench in tests/hil/README.md -- a NUCLEO-WB55RG running
# the pinned smp_svr, reachable over the host's Bluetooth radio. A case that
# finds no bench SKIPs, which the supervisor (run_hil.py) reports as
# "unavailable", never as a pass.
#
# Links the PUBLIC targets only, like an application would: smply::smply, the
# adapter, the marshalling helper and the shared application pieces. Not
# smply::smply_internal -- these cases prove the library from the outside.

add_executable(smply_hil
    support/bench.cpp
    support/rig.cpp
    test_hil_cases.cpp
)

target_link_libraries(smply_hil
    PRIVATE
        smply::smply
        smply::util
        smply::winrt_ble
        smply::dfu_app
        smply_internal_options
        Catch2::Catch2WithMain
)

target_include_directories(smply_hil PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Registered with CTest for convenience, under a label no PR preset selects.
# The supervisor runs the cases itself so that each one is wrapped in a
# baseline flash, a UART capture and a verdict; `ctest -L hil` is the manual
# shortcut once the environment variables in support/bench.hpp are set.
include(Catch)
catch_discover_tests(smply_hil PROPERTIES LABELS hil)
