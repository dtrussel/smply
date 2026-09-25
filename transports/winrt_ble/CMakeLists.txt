# SPDX-License-Identifier: Apache-2.0
#
# smply::winrt_ble -- the reference Transport, over C++/WinRT GATT.
#
# Windows-only, and guarded by SMPLY_BUILD_WINRT so that the windows-msvc job,
# which sets it OFF, proves the core builds without it. Nothing here is part of
# libsmply: an adapter consumes smply::smply like any other consumer
# (architecture.md section 2).
#
# NOT INSTALLED, as ADR-0016 records: this is a *reference*
# adapter, shipped as source to read and copy rather than as a package member.
# It is the one target neither clang-tidy nor cppcheck sees (tools/lint.sh), so
# a SemVer promise about its surface would rest on MSVC /W4 alone. What a
# third-party adapter actually needs from the package is smply::smply,
# smply::util and smply::transport_common -- all three of which are installed,
# and all three of which this file links. An in-tree consumer -- the
# winrt_ble_dfu example -- links this target directly.

add_library(smply_winrt_ble STATIC winrt_ble_transport.cpp)
add_library(smply::winrt_ble ALIAS smply_winrt_ble)

# transport_common is INTERFACE and already carries both smply::smply and the
# transports/ include root, which is what lets this target write
# "common/ble_framing.hpp" and "winrt_ble/winrt_ble_transport.hpp".
target_link_libraries(smply_winrt_ble
    PUBLIC
        smply::smply
        smply::util            # Dispatcher: the marshalling helper (ADR-0004)
        smply::transport_common
    PRIVATE
        # $<BUILD_INTERFACE:> for the same reason the core does it: these flags
        # must never reach a consumer or an export set.
        $<BUILD_INTERFACE:smply_internal_options>
        WindowsApp.lib         # the C++/WinRT runtime
)

target_compile_features(smply_winrt_ble PUBLIC cxx_std_20)
set_target_properties(smply_winrt_ble PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME winrt_ble
)

# A link-and-call test. It needs no radio and proves what a compile-only CI job
# cannot: that the archive actually links. See the file's own header.
if(SMPLY_BUILD_TESTS)
    add_executable(smply_winrt_ble_smoke smoke/winrt_ble_smoke.cpp)
    target_link_libraries(smply_winrt_ble_smoke
        PRIVATE smply::winrt_ble $<BUILD_INTERFACE:smply_internal_options>)
    add_test(NAME winrt_ble_smoke COMMAND smply_winrt_ble_smoke)
endif()
