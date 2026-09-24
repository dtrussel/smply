# SPDX-License-Identifier: Apache-2.0
#
# Consumption mode 2 of 3: add_subdirectory() over the source tree.
#
# The mode a consumer reaches for first -- a vendored checkout, or a submodule.
# It consumes the build-tree targets, so it proves something different from
# find_package/: that smply behaves as a *subproject*. Specifically that
# SMPLY_BUILD_TESTS and SMPLY_BUILD_EXAMPLES default to OFF when smply is not
# the top-level project, that its strict warning set does not reach the parent,
# and that the ALIAS names are the ones the documentation gives.
cmake_minimum_required(VERSION 3.25)
project(smply_consumption_add_subdirectory LANGUAGES CXX)

if(NOT DEFINED SMPLY_SOURCE_ROOT)
    message(FATAL_ERROR "pass -DSMPLY_SOURCE_ROOT=<smply checkout>")
endif()

# EXCLUDE_FROM_ALL so a consumer builds only what it links, which is what a
# vendoring consumer wants and what makes an accidental dependency on one of
# smply's own test targets fail here rather than in their CI.
add_subdirectory("${SMPLY_SOURCE_ROOT}" smply-build EXCLUDE_FROM_ALL)

add_executable(smply_consumption_check ../smoke.cpp)

target_link_libraries(smply_consumption_check
    PRIVATE smply::smply smply::util smply::transport_common smply::asyncutil)

if(NOT MSVC)
    target_compile_options(smply_consumption_check PRIVATE -Wall)
endif()
