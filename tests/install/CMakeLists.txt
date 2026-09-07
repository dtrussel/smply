# SPDX-License-Identifier: Apache-2.0
#
# An out-of-tree consumer, configured against an INSTALLED smply.
#
# Deliberately NOT part of the main build: it is a separate CMake project, run
# by tools/check_install.sh against a prefix that `cmake --install` produced.
# Anything built inside our own tree would consume the build-tree targets and
# prove nothing about the exported package -- which is the whole point.
cmake_minimum_required(VERSION 3.25)
project(smply_install_check LANGUAGES CXX)

find_package(smply REQUIRED)

add_executable(smply_install_check main.cpp)

# Both targets an adapter needs: the library, and the marshalling helper
# without which it cannot honour the threading contract (ADR-0004).
target_link_libraries(smply_install_check PRIVATE smply::smply smply::util)

# Ordinary warnings only, like a real consumer. If smply's strict set leaked
# through the export, this would not build.
if(NOT MSVC)
    target_compile_options(smply_install_check PRIVATE -Wall)
endif()
