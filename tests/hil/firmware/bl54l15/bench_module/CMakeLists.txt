# SPDX-License-Identifier: Apache-2.0
#
# The BL54L15 peer's bench-only additions to the unmodified smp_svr sample
# (tests/hil/README.md). Each part has its own Kconfig switch.
if(CONFIG_SMPLY_BENCH_SETTINGS OR CONFIG_SMPLY_BENCH_STATS OR CONFIG_SMPLY_BENCH_BLE_LOG)
  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_SMPLY_BENCH_SETTINGS smply_bench_settings.c)
  zephyr_library_sources_ifdef(CONFIG_SMPLY_BENCH_STATS smply_bench_stats.c)
  zephyr_library_sources_ifdef(CONFIG_SMPLY_BENCH_BLE_LOG smply_bench_ble_log.c)
endif()
