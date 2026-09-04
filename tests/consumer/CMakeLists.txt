# SPDX-License-Identifier: Apache-2.0
#
# The flag-leak gate.
#
# This target links smply::smply the way a real consumer does and compiles with
# ONLY -Wall. It deliberately contains code that smply's own strict warning set
# rejects (-Wconversion, -Wold-style-cast, -Werror). If any of those flags were
# to leak out through smply's PUBLIC/INTERFACE usage requirements, this target
# would fail to build.
#
# The fuller out-of-tree consumption matrix (find_package / FetchContent /
# add_subdirectory against an install tree) is P18.

add_executable(smply_consumer_check consumer_check.cpp)

# Note the absence of smply_internal_options: a consumer never links it.
target_link_libraries(smply_consumer_check PRIVATE smply::smply)

if(MSVC)
    target_compile_options(smply_consumer_check PRIVATE /W1)
else()
    target_compile_options(smply_consumer_check PRIVATE -Wall)
endif()

add_test(NAME consumer_check COMMAND smply_consumer_check)

# Belt and braces: fail at configure time, with a clear message, rather than as
# a confusing compile error in an unrelated target.
#
# Two distinct leaks are possible and BOTH must be checked:
#   * options written straight onto smply's interface, which land in
#     INTERFACE_COMPILE_OPTIONS; and
#   * smply_internal_options linked PUBLIC/INTERFACE instead of PRIVATE, which
#     leaves INTERFACE_COMPILE_OPTIONS empty and instead propagates the flags
#     transitively through INTERFACE_LINK_LIBRARIES.
# Checking only the first misses the far more likely mistake.
get_target_property(_smply_iface_opts smply INTERFACE_COMPILE_OPTIONS)
if(_smply_iface_opts)
    foreach(_forbidden "-Werror" "/WX" "-Wconversion" "-Wold-style-cast")
        if("${_smply_iface_opts}" MATCHES "${_forbidden}")
            message(FATAL_ERROR
                "smply's INTERFACE_COMPILE_OPTIONS leaks '${_forbidden}' to consumers. "
                "Strict flags belong on smply_internal_options, linked PRIVATE (ADR-0011). "
                "Interface options were: ${_smply_iface_opts}")
        endif()
    endforeach()
endif()

get_target_property(_smply_iface_libs smply INTERFACE_LINK_LIBRARIES)
if(_smply_iface_libs)
    foreach(_internal "smply_internal_options" "smply::internal_options")
        # A PRIVATE link appears wrapped in $<LINK_ONLY:...>, which propagates
        # linking but not compile options -- that form is correct and expected.
        string(REGEX MATCHALL "\\$<LINK_ONLY:[^>]*>" _link_only "${_smply_iface_libs}")
        string(REPLACE "${_link_only}" "" _propagating "${_smply_iface_libs}")
        if("${_propagating}" MATCHES "${_internal}")
            message(FATAL_ERROR
                "smply propagates '${_internal}' to consumers through "
                "INTERFACE_LINK_LIBRARIES, so they inherit smply's strict warning set. "
                "Link it PRIVATE instead (ADR-0011). Interface libraries were: "
                "${_smply_iface_libs}")
        endif()
    endforeach()
endif()
