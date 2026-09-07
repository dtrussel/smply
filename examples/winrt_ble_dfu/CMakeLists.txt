# SPDX-License-Identifier: Apache-2.0
#
# The Bluetooth LE DFU tool: the pump loop against a real radio (P16).
#
# Windows-only, and reached only when SMPLY_BUILD_WINRT is on -- examples/
# guards the add_subdirectory. Like every example it links the PUBLIC targets
# only, never smply::smply_internal.
#
# NOT RUN BY CI. The windows-winrt job compiles and links this; a GitHub runner
# has no Bluetooth radio, so nothing here has ever been executed. See README.md.

add_executable(winrt_ble_dfu
    main.cpp
    scanner.cpp
)

target_link_libraries(winrt_ble_dfu
    PRIVATE
        smply::smply
        smply::util        # Dispatcher: WinRT calls back on pool threads
        smply::winrt_ble   # the transport itself
        smply::dfu_app     # FileImageSource and ReconnectPolicy, shared with cli_dfu
        smply_internal_options
)

target_include_directories(winrt_ble_dfu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# No add_test(). There is nothing to assert without a device, and a test that
# only proved the binary starts would be a green tick standing in for coverage
# this example does not have. The adapter's own smoke test already proves the
# WinRT side links (transports/winrt_ble/smoke/).
