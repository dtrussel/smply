// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_HIL_SUPPORT_BENCH_HPP
#define SMPLY_TESTS_HIL_SUPPORT_BENCH_HPP

/// \file
/// Where the bench is, read from the environment.
///
/// Every hardware case begins with `require_bench()`. A case that cannot find
/// its device does not fail -- it **skips**, and `run_hil.py` reports a skip as
/// "unavailable". That distinction is ADR-0015's first rule: a missing bench is
/// never a test result, in either direction.
///
/// | Variable             | Meaning                                              |
/// | -------------------- | ---------------------------------------------------- |
/// | `SMPLY_HIL_ADDRESS`  | the peer's Bluetooth address, `AA:BB:CC:DD:EE:FF`    |
/// | `SMPLY_HIL_IMAGE_A`  | path to `a.signed.bin` from `build_peer.py`           |
/// | `SMPLY_HIL_IMAGE_B`  | path to `b.signed.bin`                                |
/// | `SMPLY_HIL_SMP_VERSION` | `1` (default) or `2`: the version requests carry   |
///
/// The last one exists for one measurement. smply sends SMP v1 by default and
/// offers v2 as an application opt-in, with no probing and no fallback
/// (ADR-0010); this peer is built with
/// `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL`, which makes a v1 request lose
/// the image-group error code (protocol-notes.md section 9, A16). Flipping the
/// version and re-running a case is therefore how open question O2 gets an
/// answer from hardware instead of from reading.

#include "smply/smp/header.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smply::hil {

struct Bench
{
    std::uint64_t address = 0;
    std::string image_a;
    std::string image_b;
    /// What every request in this run carries. See the table above.
    Version smp_version = Version::V1;
};

/// Reads the three variables. On failure `why` says which one is missing.
[[nodiscard]] std::optional<Bench> bench_from_environment(std::string& why);

/// `AA:BB:CC:DD:EE:FF`, `AA-BB-...` or bare hex, into the 48-bit value WinRT uses.
[[nodiscard]] std::optional<std::uint64_t> parse_address(const std::string& text);

/// The whole file, or nothing. Small firmware images only; this is a test.
[[nodiscard]] std::optional<std::vector<std::byte>> read_file(const std::string& path);

/// The bench, or a Catch2 SKIP. Call at the top of every case.
[[nodiscard]] Bench require_bench();

} // namespace smply::hil

#endif // SMPLY_TESTS_HIL_SUPPORT_BENCH_HPP
