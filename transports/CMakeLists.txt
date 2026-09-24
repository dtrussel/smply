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
# Header-only: arithmetic, small state machines and the send-admission queue.
# INTERFACE rather than STATIC so there is nothing to build and no ABI to keep.
# No file list either, so a new header here needs no CMake change -- it is
# picked up by the include root below, by tools/sources.sh for clang-format,
# and by tools/coverage.sh --filter transports/common/.
add_library(smply_transport_common INTERFACE)
add_library(smply::transport_common ALIAS smply_transport_common)

# The include root is transports/, so an adapter writes
# "common/ble_framing.hpp" and it is obvious where the header came from.
#
# The installed root is <prefix>/include/smply/transports, with common/ kept
# underneath it, so that spelling is the same in the build tree and out of an
# install -- an adapter's #include lines do not change when it stops being
# in-tree. The root is nested under smply/ rather than sitting at
# <prefix>/include, so linking this target cannot put a bare "common/" on a
# consumer's search path.
target_include_directories(smply_transport_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/smply/transports>
)

target_link_libraries(smply_transport_common INTERFACE smply::smply)
target_compile_features(smply_transport_common INTERFACE cxx_std_20)

# Installed, because a third-party adapter cannot be written correctly without
# it (ADR-0016). fragment_size() is arithmetic that took a unit suite to get
# right, and SendQueue is the fix for A22 -- a defect that killed an upload on
# the bench and that the whole simulated suite could not see. Leaving this out
# means every out-of-tree adapter re-derives both, bug included.
#
# EXPORT_NAME, not just the ALIAS: install(EXPORT) names an exported target
# <namespace><target-name>, which would give smply::smply_transport_common. The
# in-tree ALIAS does not travel, and only tests/consumption/find_package
# notices.
set_target_properties(smply_transport_common PROPERTIES EXPORT_NAME transport_common)

install(TARGETS smply_transport_common EXPORT smplyTargets)
install(DIRECTORY common DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/smply/transports
    FILES_MATCHING PATTERN "*.hpp")

# serial/ ships under the SAME target rather than as a fourth installed one.
# ADR-0016 clause 1 names the package's targets and says "and nothing else";
# clause 4 promises "the installed transport headers". Adding a directory
# keeps both true, adding a target would not (ADR-0017). One more
# install(DIRECTORY) is the whole cost, and the include root above already
# makes "serial/serial_framing.hpp" the spelling in-tree and out of a prefix.
install(DIRECTORY serial DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/smply/transports
    FILES_MATCHING PATTERN "*.hpp")

# --- platform adapters -------------------------------------------------------
#
# The option is validated in the top-level CMakeLists (it refuses a non-Windows
# host there), so the core-without-winrt gate is meaningful whether or not the
# directory below is ever entered.
if(SMPLY_BUILD_WINRT)
    add_subdirectory(winrt_ble)
endif()
