# SPDX-License-Identifier: Apache-2.0

# 3.24 is the floor for FetchContent's OVERRIDE_FIND_PACKAGE (ADR-0011);
# 3.25 adds FetchContent_Declare(SYSTEM), which keeps dependency headers out of
# clang-tidy's and the compiler's warning scope. See cmake/dependencies.cmake.
cmake_minimum_required(VERSION 3.25)

# C is required in addition to CXX: QCBOR is a C project and fails to
# configure as a subproject otherwise.
project(smply
    VERSION     0.1.0
    DESCRIPTION "Transport-agnostic C++ client library for Zephyr MCUmgr/SMP"
    HOMEPAGE_URL "https://github.com/dtrussel/smply"
    LANGUAGES   CXX C
)

# --- Options ----------------------------------------------------------------
# Default to OFF when consumed via add_subdirectory() so being a subproject is
# quiet and cheap.
option(SMPLY_BUILD_TESTS    "Build smply's tests"            ${PROJECT_IS_TOP_LEVEL})
option(SMPLY_BUILD_EXAMPLES "Build smply's examples"         ${PROJECT_IS_TOP_LEVEL})
option(SMPLY_BUILD_FUZZERS  "Build libFuzzer targets"        OFF)
option(SMPLY_BUILD_WINRT    "Build the WinRT BLE transport (P15)" OFF)
option(SMPLY_BUILD_HIL      "Build hardware-in-the-loop tests (P17)" OFF)
option(SMPLY_USE_SYSTEM_QCBOR "Use find_package(qcbor) instead of FetchContent" OFF)
# P1 introduces the expected<> fallback; the option exists now so CI can pin it.
option(SMPLY_FORCE_FALLBACK_EXPECTED
       "Use smply's expected<> even when std::expected is available" OFF)

# C++20 is the baseline (ADR-0001). 23 is buildable so the std::expected
# backing of Result<T> can be exercised in CI -- under C++20 the standard type
# does not exist, so without this the fallback would be the only thing ever
# tested (see docs/decisions/ADR-0002-result-and-error-type.md).
set(SMPLY_CXX_STANDARD 20 CACHE STRING "C++ standard: 20 (baseline) or 23")
set_property(CACHE SMPLY_CXX_STANDARD PROPERTY STRINGS 20 23)
set(CMAKE_CXX_STANDARD ${SMPLY_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(cmake/warnings.cmake)
include(cmake/sanitizers.cmake)
include(cmake/dependencies.cmake)

# --- Generated version header -----------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/smply/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/smply/version.hpp"
    @ONLY
)

# --- The library ------------------------------------------------------------
add_library(smply)
add_library(smply::smply ALIAS smply)

target_sources(smply
    PRIVATE
        src/cbor/mgmt_error.cpp
        src/cbor/reader.cpp
        src/cbor/writer.cpp
        src/core.cpp
        src/dfu/firmware_updater.cpp
        src/dfu/update_state_machine.cpp
        src/groups/image/image_management.cpp
        src/groups/image/upload_driver.cpp
        src/groups/image/upload_session.cpp
        src/groups/os/os_management.cpp
        src/image/image_source.cpp
        src/image/mcuboot_header.cpp
        src/image/sha256.cpp
        src/image/tlv.cpp
        src/smp/assembler.cpp
        src/smp/client.cpp
        src/smp/codec.cpp
        src/version.cpp
)

target_include_directories(smply
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # Internal headers (src/smp/assembler.hpp and the like) are included as
        # "smp/assembler.hpp" by smply's own sources. PRIVATE: consumers must
        # not see them, and the public-header gate keeps them out of include/.
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# PRIVATE: strict warnings and instrumentation must not reach consumers.
target_link_libraries(smply PRIVATE smply_internal_options)

# QCBOR is an implementation detail (ADR-0007): PRIVATE, and it must never
# appear in a public header (enforced by tools/check_public_headers.py).
if(TARGET qcbor::qcbor)
    target_link_libraries(smply PRIVATE qcbor::qcbor)
endif()

target_compile_features(smply PUBLIC cxx_std_20)

set_target_properties(smply PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
)

if(SMPLY_FORCE_FALLBACK_EXPECTED)
    target_compile_definitions(smply PUBLIC SMPLY_FORCE_FALLBACK_EXPECTED=1)
endif()

# --- smply::util: the adapter helper, NOT part of the core ------------------
#
# Dispatcher is the one concurrent component in the tree, and ADR-0004 says the
# core does not depend on it. A separate target is how that stops being a
# convention: nothing links smply_util into smply, so a change that made the
# core use it would fail to build rather than pass review.
#
# Adapters and examples opt in by linking smply::util.
add_library(smply_util STATIC src/util/dispatcher.cpp)
add_library(smply::util ALIAS smply_util)

target_link_libraries(smply_util
    PUBLIC
        smply::smply
    PRIVATE
        # smply's own code, so the strict warning set applies -- but PRIVATE,
        # so it does not reach whoever links smply::util.
        smply_internal_options
)

target_compile_features(smply_util PUBLIC cxx_std_20)
set_target_properties(smply_util PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(smply_util PUBLIC Threads::Threads)

# Internal access for tests only. Not installed, not for consumers.
add_library(smply_internal INTERFACE)
add_library(smply::smply_internal ALIAS smply_internal)
target_include_directories(smply_internal INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(smply_internal INTERFACE smply::smply)
if(TARGET qcbor::qcbor)
    # Internal headers under src/cbor/ name QCBOR, so anything testing them
    # needs its headers. smply::smply keeps QCBOR PRIVATE, so this reaches
    # tests only -- the public API stays free of it, which is what
    # tools/check_public_headers.py enforces.
    target_link_libraries(smply_internal INTERFACE qcbor::qcbor)
endif()

# --- Subdirectories ---------------------------------------------------------
if(SMPLY_BUILD_WINRT)
    if(NOT WIN32)
        message(FATAL_ERROR "SMPLY_BUILD_WINRT requires Windows")
    endif()
    # P15 adds transports/winrt_ble. The option exists now so the
    # core-without-winrt CI job is meaningful from P0 onwards.
    message(STATUS "smply: SMPLY_BUILD_WINRT is set but the transport arrives in P15")
endif()

if(SMPLY_BUILD_FUZZERS AND NOT SMPLY_BUILD_TESTS)
    # The targets link smply::test_support for FakeTransport and ManualClock.
    message(FATAL_ERROR "SMPLY_BUILD_FUZZERS requires SMPLY_BUILD_TESTS")
endif()

if(SMPLY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SMPLY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/support)
    add_subdirectory(tests/unit)
    add_subdirectory(tests/component)
    add_subdirectory(tests/consumer)
    if(SMPLY_BUILD_FUZZERS)
        add_subdirectory(tests/fuzz)
    endif()
endif()

# Install/export is deliberately deferred to P18, together with the
# out-of-tree consumption tests. See docs/roadmap.md.
