# SPDX-License-Identifier: Apache-2.0
if(CONFIG_SMPLY_BENCH_SETTINGS)
  zephyr_library()
  zephyr_library_sources(smply_bench_settings.c)
endif()
