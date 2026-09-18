# SPDX-License-Identifier: Apache-2.0
#
# Consumption mode 3 of 3: FetchContent.
#
# FetchContent_MakeAvailable ends in add_subdirectory(), so what this adds over
# mode 2 is the declaration itself -- that smply can be a FetchContent
# dependency at all, and that being populated into a build directory whose name
# it does not choose breaks nothing.
#
# SOURCE_DIR, deliberately, and not GIT_REPOSITORY: a git fetch would populate
# the last *commit*, so the gate would pass or fail on a tree nobody has. That
# is the stale-binary trap wearing different clothes -- a green check that is
# not a check of the tree you have. The download path is CMake's to test.
cmake_minimum_required(VERSION 3.25)
project(smply_consumption_fetchcontent LANGUAGES CXX)

if(NOT DEFINED SMPLY_SOURCE_ROOT)
    message(FATAL_ERROR "pass -DSMPLY_SOURCE_ROOT=<smply checkout>")
endif()

include(FetchContent)

FetchContent_Declare(smply SOURCE_DIR "${SMPLY_SOURCE_ROOT}")
FetchContent_MakeAvailable(smply)

add_executable(smply_consumption_check ../smoke.cpp)

target_link_libraries(smply_consumption_check
    PRIVATE smply::smply smply::util smply::transport_common)

if(NOT MSVC)
    target_compile_options(smply_consumption_check PRIVATE -Wall)
endif()
