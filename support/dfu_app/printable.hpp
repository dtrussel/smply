// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_PRINTABLE_HPP
#define SMPLY_DFU_APP_PRINTABLE_HPP

/// \file
/// Untrusted text made safe to print on a terminal.
///
/// `Error::reason()` is attacker-controlled text an application must escape
/// before it shows it (`smply/error.hpp`, docs/security.md T12): a device's
/// `rsn`, or a package refusal quoting the manifest. The example tools share
/// this one escape rather than each keeping a copy.

#include <string>
#include <string_view>

namespace smply::dfu_app {

/// \p text with its control characters shown as `\xNN`, so it cannot drive the
/// terminal: C0 (below 0x20), DEL (0x7F), and C1 (U+0080 to U+009F) as UTF-8
/// writes it, `0xC2` followed by `0x80` to `0x9F`, both bytes escaped. Every
/// other byte, printable ASCII and the rest of UTF-8 included, is kept as it
/// is.
[[nodiscard]] std::string printable(std::string_view text);

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_PRINTABLE_HPP
