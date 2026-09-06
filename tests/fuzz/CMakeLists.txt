# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer targets over the untrusted-input surface (docs/testing.md section 5).
#
# The preset compiles the whole tree with -fsanitize=fuzzer-no-link so every
# translation unit is instrumented for coverage; each target below adds
# -fsanitize=fuzzer at link time, which is what supplies libFuzzer's main().
#
# Not part of ctest: a fuzz target runs until it is told to stop, so CI drives
# them explicitly -- a short smoke run per pull request, and a scheduled soak.

set(SMPLY_FUZZ_TARGETS
    fuzz_assembler
    fuzz_cbor_image_state
    fuzz_cbor_upload_response
    fuzz_header
    fuzz_mcuboot_header
    fuzz_smp_client_rx
    fuzz_tlv_scan
)

foreach(target IN LISTS SMPLY_FUZZ_TARGETS)
    add_executable(${target} ${target}.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target}
        PRIVATE
            smply::smply_internal
            smply::test_support
            smply_internal_options
    )
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()

# A corpus directory per target, so a fresh clone can run one straight away.
foreach(target IN LISTS SMPLY_FUZZ_TARGETS)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${target})
endforeach()
