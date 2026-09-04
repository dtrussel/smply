# SPDX-License-Identifier: Apache-2.0
#
# Sanitizer and coverage instrumentation, applied via the same PRIVATE
# INTERFACE target as the warnings so consumers are unaffected.
#
# TSan is deliberately absent: the core is single-threaded by contract
# (ADR-0004). It is introduced in P14 for the Dispatcher only.

set(SMPLY_SANITIZER "" CACHE STRING
    "Sanitizer to enable for smply's own targets: '' or 'address,undefined'")
option(SMPLY_ENABLE_COVERAGE "Instrument smply's own targets for coverage" OFF)

if(SMPLY_SANITIZER)
    if(MSVC)
        message(FATAL_ERROR "SMPLY_SANITIZER is not supported with MSVC")
    endif()
    message(STATUS "smply: sanitizers enabled -> ${SMPLY_SANITIZER}")
    target_compile_options(smply_internal_options INTERFACE
        -fsanitize=${SMPLY_SANITIZER}
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        -g)
    target_link_options(smply_internal_options INTERFACE
        -fsanitize=${SMPLY_SANITIZER})
endif()

if(SMPLY_ENABLE_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "SMPLY_ENABLE_COVERAGE is not supported with MSVC")
    endif()
    message(STATUS "smply: coverage instrumentation enabled")
    target_compile_options(smply_internal_options INTERFACE
        --coverage -O0 -g)
    target_link_options(smply_internal_options INTERFACE --coverage)
endif()
