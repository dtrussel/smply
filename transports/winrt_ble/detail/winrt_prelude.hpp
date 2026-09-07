// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_WINRT_BLE_DETAIL_WINRT_PRELUDE_HPP
#define SMPLY_TRANSPORTS_WINRT_BLE_DETAIL_WINRT_PRELUDE_HPP

/// \file
/// Every C++/WinRT include the adapter needs, in one place, with smply's
/// warning set held off them.
///
/// smply compiles its own targets at `/W4 /WX` (cmake/warnings.cmake), and the
/// projection headers that ship with the Windows SDK are machine-generated and
/// do not survive that. `#pragma warning(push, 0)` is used rather than
/// `/external:` because it is independent of the MSVC version and because it
/// says, at the place it applies, exactly which headers are being excused --
/// the adapter's own code stays fully warned.
///
/// This header is internal to the adapter. Nothing under `include/smply/` may
/// include it, and `tools/check_public_headers.py` enforces that.

#ifndef _WIN32
#error "the WinRT BLE adapter is Windows-only; build it behind SMPLY_BUILD_WINRT"
#endif

#pragma warning(push, 0)

#include <winrt/base.h>

#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma warning(pop)

#endif // SMPLY_TRANSPORTS_WINRT_BLE_DETAIL_WINRT_PRELUDE_HPP
