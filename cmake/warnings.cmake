# SPDX-License-Identifier: Apache-2.0
#
# Strict warnings for smply's OWN targets only.
#
# These are carried by the smply_internal_options INTERFACE target, which
# smply's targets link PRIVATE. Consumers and third-party dependencies never
# inherit them (ADR-0011). Never use add_compile_options() here.

add_library(smply_internal_options INTERFACE)
add_library(smply::internal_options ALIAS smply_internal_options)

if(MSVC)
    target_compile_options(smply_internal_options INTERFACE
        /W4 /WX
        /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8
        /w14242  # conversion, possible loss of data
        /w14254  # larger bit-field assigned to smaller
        /w14263  # member function does not override any base member
        /w14265  # class has virtual functions but non-virtual destructor
        /w14287  # unsigned/negative constant mismatch
        /we4289  # loop control variable used outside the loop
        /w14296  # expression is always true/false
        /w14311  # pointer truncation
        /w14545 /w14546 /w14547 /w14549 /w14555  # malformed expressions
        /w14619  # unknown #pragma warning
        /w14640  # non-thread-safe static member initialisation
        /w14826  # sign-extending conversion
        /w14905 /w14906  # string literal cast mismatches
        /w14928  # illegal copy-initialisation
    )
else()
    target_compile_options(smply_internal_options INTERFACE
        -Wall -Wextra -Wpedantic -Werror
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wextra-semi
    )

    # -Wnull-dereference is NOT in that list, and its absence is deliberate.
    #
    # It only fires under optimisation, and P15a's first Release build -- the
    # first this project has ever done -- showed why that matters: with GCC 13
    # at -O3 it reports "potential null pointer dereference" inside libstdc++'s
    # basic_string copy constructor, inlined through smply::Error's implicit
    # copy. The pointer is std::string's own `_M_dataplus._M_p`; no pointer of
    # smply's is involved, there is nothing to fix in our code, and declaring
    # the dependency SYSTEM does not help because the offending code has been
    # inlined into our translation unit by then. Clang at -O3 is clean.
    #
    # Keeping it would mean either -Wno-error for one check, which contradicts
    # warnings-as-errors, or leaving the library uncompilable in Release with
    # GCC -- which is what a consumer builds. Dropped, with the evidence
    # recorded, rather than suppressed at a call site that is not ours.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(smply_internal_options INTERFACE
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
        )
    endif()
endif()
