# SPDX-License-Identifier: Apache-2.0
#
# Consumption mode 1 of 3: find_package() against an INSTALLED prefix.
#
# This is the mode that proves the *export*. Deliberately not part of the main
# build -- it is a separate CMake project, run by tools/check_install.sh against
# a prefix that `cmake --install` produced. Anything built inside our own tree
# would consume the build-tree targets, where the in-tree ALIAS names resolve,
# and would prove nothing about what a consumer receives.
cmake_minimum_required(VERSION 3.25)
project(smply_consumption_find_package LANGUAGES CXX)

find_package(smply REQUIRED)

add_executable(smply_consumption_check ../smoke.cpp)

# Every installed target: the library, the marshalling helper without which an
# adapter cannot honour the threading contract (ADR-0004), and the portable BLE
# helpers it would otherwise re-derive (ADR-0016).
target_link_libraries(smply_consumption_check
    PRIVATE smply::smply smply::util smply::transport_common smply::asyncutil)

# Ordinary warnings only, like a real consumer. If smply's strict set leaked
# through the export, this would not build.
if(NOT MSVC)
    target_compile_options(smply_consumption_check PRIVATE -Wall)
endif()
