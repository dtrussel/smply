# SPDX-License-Identifier: Apache-2.0
#
# Platform adapters, and the portable pieces they share.
#
# transports/common/ is built everywhere and tested on Linux; the platform
# adapters below it are guarded by their own options. Nothing here is part of
# libsmply -- an adapter consumes smply::smply like any other consumer
# (architecture.md section 2).

# --- smply::transport_common ------------------------------------------------
#
# Header-only: it is arithmetic and a two-bit enum. INTERFACE rather than STATIC
# so there is nothing to build, nothing to install and no ABI to keep.
add_library(smply_transport_common INTERFACE)
add_library(smply::transport_common ALIAS smply_transport_common)

# The include root is transports/, so an adapter writes
# "common/ble_framing.hpp" and it is obvious where the header came from.
target_include_directories(smply_transport_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(smply_transport_common INTERFACE smply::smply)
target_compile_features(smply_transport_common INTERFACE cxx_std_20)

# --- platform adapters -------------------------------------------------------
#
# The option is validated in the top-level CMakeLists (it refuses a non-Windows
# host there), so the core-without-winrt gate is meaningful whether or not the
# directory below is ever entered.
if(SMPLY_BUILD_WINRT)
    add_subdirectory(winrt_ble)
endif()
