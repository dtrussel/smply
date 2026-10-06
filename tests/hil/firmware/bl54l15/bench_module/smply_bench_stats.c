// SPDX-License-Identifier: Apache-2.0
//
// A bench-only 64-bit statistics group for the BL54L15 peer
// (tests/hil/README.md).
//
// A Zephyr server widens every statistic to 64 bits and then encodes it with
// zcbor_uint32_put(), so a 64-bit counter above 2^32 - 1 reaches the client
// truncated to its low 32 bits (docs/protocol-notes.md section 9, A28). The
// sample's own group is 32-bit and cannot show it. This group holds one
// counter, `big`, set to 2^32 + 5 at boot: a client reading 5 has observed the
// truncation.
//
// The entry size is per group, so a 64-bit counter needs a group of its own.

#include <zephyr/init.h>
#include <zephyr/stats/stats.h>

#include <stdint.h>

#define SMPLY_BENCH_BIG ((UINT64_C(1) << 32) + 5)

STATS_SECT_START(smply_bench)
STATS_SECT_ENTRY64(big)
STATS_SECT_END;

STATS_NAME_START(smply_bench)
STATS_NAME(smply_bench, big)
STATS_NAME_END(smply_bench);

/* Named as the section: STATS_INIT_AND_REG() finds the name map by the
 * variable's name, as the sample's smp_svr_stats does.
 */
static STATS_SECT_DECL(smply_bench) smply_bench;

static int smply_bench_stats_init(void)
{
	int rc = STATS_INIT_AND_REG(smply_bench, STATS_SIZE_64, "smply_bench");

	/* After registering: initialisation may clear the counters. */
	STATS_SET(smply_bench, big, SMPLY_BENCH_BIG);
	return rc;
}

SYS_INIT(smply_bench_stats_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
