# SPDX-License-Identifier: Apache-2.0
#
# Examples are consumers, not part of the library: they link smply::smply (and
# smply::util where they need marshalling) and never the internal target.
#
# They are smply's own code, so the strict warning set and the format and lint
# gates apply to them -- but PRIVATE, so nothing about that reaches a real
# consumer copying the code out.

add_subdirectory(cli_dfu)

# The Bluetooth LE tool needs the WinRT transport, so it appears only when that
# is being built. cli_dfu above is the portable one and is always present --
# which matters, because it is the one CI can actually run.
if(SMPLY_BUILD_WINRT)
    add_subdirectory(winrt_ble_dfu)
endif()
