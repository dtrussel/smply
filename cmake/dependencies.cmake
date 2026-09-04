# SPDX-License-Identifier: Apache-2.0
#
# Third-party dependency acquisition.
#
# Policy (ADR-0011): every dependency is pinned to an exact tag AND commit hash,
# declared with OVERRIDE_FIND_PACKAGE so a system / vcpkg copy is used when one
# is present, and listed in docs/dependencies.md (enforced by
# tools/check_deps.py).

include(FetchContent)

# SYSTEM on every FetchContent_Declare below: dependency headers become system
# includes, so neither our strict warning set nor clang-tidy reports findings
# inside them -- including findings attributed to our files through macro
# expansion (Catch2's TEST_CASE is the notable case). Requires CMake >= 3.25.

# --- QCBOR ------------------------------------------------------------------
# CBOR encode/decode. BSD-3-Clause. Hidden behind src/cbor/ (ADR-0007).
# NOTE: QCBOR is a C project. The top-level project() must enable the C
# language or configuring this dependency fails.
set(SMPLY_QCBOR_TAG    "v1.6.1")
set(SMPLY_QCBOR_COMMIT "930708bb86481e88879eb1d87fd4d664f1d69503")

if(SMPLY_USE_SYSTEM_QCBOR)
    find_package(qcbor REQUIRED)
else()
    FetchContent_Declare(qcbor
        GIT_REPOSITORY https://github.com/laurencelundblade/QCBOR.git
        GIT_TAG        ${SMPLY_QCBOR_COMMIT}   # == ${SMPLY_QCBOR_TAG}
        GIT_SHALLOW    FALSE
        SYSTEM
        OVERRIDE_FIND_PACKAGE
    )
    # Keep QCBOR's own test suite out of our build.
    set(BUILD_QCBOR_TEST "OFF" CACHE STRING "" FORCE)
    set(BUILD_QCBOR_WARN OFF   CACHE BOOL   "" FORCE)
    FetchContent_MakeAvailable(qcbor)
endif()

# --- Catch2 (tests only) ----------------------------------------------------
if(SMPLY_BUILD_TESTS)
    set(SMPLY_CATCH2_TAG    "v3.9.1")
    set(SMPLY_CATCH2_COMMIT "dfc2dff8d70d083c60c1c6986030e5389a867a93")

    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        ${SMPLY_CATCH2_COMMIT}   # == ${SMPLY_CATCH2_TAG}
        GIT_SHALLOW    FALSE
        SYSTEM
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(Catch2)

    # catch_discover_tests() lives in Catch2's extras directory. This file is
    # include()d rather than add_subdirectory()'d, so the append is already
    # visible to the caller -- no PARENT_SCOPE needed.
    if(DEFINED catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
endif()
