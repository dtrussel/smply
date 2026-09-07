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
