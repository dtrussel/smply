# SPDX-License-Identifier: Apache-2.0
#
# Code shared by the tests and the examples, and part of neither the library nor
# either of them.
#
# It exists because an example may not depend on tests/: examples build when
# SMPLY_BUILD_TESTS is OFF, and a shipped example has no business including a
# file called test_cbor.hpp. Nothing here is ever linked into libsmply.

add_library(smply_minicbor STATIC minicbor/minicbor.cpp)
add_library(smply::minicbor ALIAS smply_minicbor)

# The include root is support/, so a consumer writes "minicbor/minicbor.hpp"
# and it is obvious in the source which library it came from.
target_include_directories(smply_minicbor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(smply_minicbor
    PUBLIC
        # For smply/bytes.hpp only. This is the whole of its coupling to smply,
        # and deliberately so -- see the file comment on the independence rule.
        smply::smply
    PRIVATE
        smply_internal_options
)

target_compile_features(smply_minicbor PUBLIC cxx_std_20)

# --- smply::dfu_app ---------------------------------------------------------
#
# What a DFU *application* needs and the library deliberately does not ship:
# reading firmware off a disk, and deciding how hard to chase a device that has
# just rebooted. Both are the application's business (ADR-0005) and neither is
# platform-specific, so they live here rather than in one example -- two
# examples need them, and two copies drift.
#
# The reconnect policy in particular is why this target exists at all:
# examples/winrt_ble_dfu/ cannot be run by CI, so the one part of a real
# reconnect that is pure arithmetic is put somewhere cli_dfu can exercise it on
# every push (P16).
add_library(smply_dfu_app STATIC dfu_app/file_image_source.cpp)
add_library(smply::dfu_app ALIAS smply_dfu_app)

# The include root is support/, as for minicbor: a consumer writes
# "dfu_app/reconnect_policy.hpp" and it is obvious where it came from.
target_include_directories(smply_dfu_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(smply_dfu_app
    PUBLIC
        smply::smply     # ImageSource, Result, Duration
    PRIVATE
        smply_internal_options
)

target_compile_features(smply_dfu_app PUBLIC cxx_std_20)
