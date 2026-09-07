// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_WINRT_BLE_DFU_WINRT_PRELUDE_HPP
#define SMPLY_EXAMPLE_WINRT_BLE_DFU_WINRT_PRELUDE_HPP

/// \file
/// The C++/WinRT includes this example needs, with smply's warning set held off
/// them.
///
/// The adapter has a prelude of its own at
/// `transports/winrt_ble/detail/winrt_prelude.hpp`, and this example
/// deliberately does **not** use it. `examples/cli_dfu/CMakeLists.txt` states
/// the rule these examples live by: an example that reaches into another
/// target's private headers demonstrates something no consumer can do. Ten
/// duplicated lines is the honest price of that.
///
/// Advertisement.h is the addition over the adapter's list -- scanning is the
/// application's job, not the transport's.

#ifndef _WIN32
#error "the WinRT BLE example is Windows-only; build it behind SMPLY_BUILD_WINRT"
#endif

#pragma warning(push, 0)

#include <winrt/base.h>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#pragma warning(pop)

#endif // SMPLY_EXAMPLE_WINRT_BLE_DFU_WINRT_PRELUDE_HPP
