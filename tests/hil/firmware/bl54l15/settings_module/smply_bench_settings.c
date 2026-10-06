// SPDX-License-Identifier: Apache-2.0
//
// A bench-only settings handler for the BL54L15 peer (tests/hil/README.md).
//
// The smp_svr sample registers no settings handler, and none of Zephyr's own
// has an `h_get`, so without this every MCUmgr settings read fails before it
// reaches a value. The settings HIL cases need a key they can write, read
// back, save, reload and delete, and one value longer than
// CONFIG_MCUMGR_GRP_SETTINGS_VALUE_LEN so that the read limit is observable
// (docs/protocol-notes.md section 9, A31).
//
// Keys under the root "smply":
//   smply/v        read/write, up to SMPLY_VALUE_MAX bytes, exported by save
//   smply/ro       read-only constant; a write is -ENOTSUP (WRITE_NOT_SUPPORTED)
//   smply/commits  read-only little-endian u32: how many times h_commit ran,
//                  which is what makes commit and load observable from outside
//   anything else  -ENOENT (KEY_NOT_FOUND)
//
// A read into a buffer shorter than the value is truncated to the buffer: the
// handler's choice, which A31 says a client cannot rely on in general.

#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SMPLY_VALUE_MAX 64

static uint8_t value[SMPLY_VALUE_MAX];
static size_t value_len;
static uint32_t commits;
static const char read_only[] = "smply-bench";

/* Exactly `name`, not a prefix of a longer path. */
static bool is(const char *key, const char *name)
{
	const char *next = NULL;

	return key != NULL && settings_name_steq(key, name, &next) && next == NULL;
}

static int bench_get(const char *key, char *val, int val_len_max)
{
	uint8_t le[sizeof(uint32_t)];
	const void *src;
	size_t len;

	if (is(key, "v")) {
		src = value;
		len = value_len;
	} else if (is(key, "ro")) {
		src = read_only;
		len = sizeof(read_only) - 1;
	} else if (is(key, "commits")) {
		sys_put_le32(commits, le);
		src = le;
		len = sizeof(le);
	} else {
		return -ENOENT;
	}
	if (val_len_max < 0) {
		return -EINVAL;
	}
	len = MIN(len, (size_t)val_len_max);
	memcpy(val, src, len);
	return (int)len;
}

static int bench_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	ssize_t rc;

	if (is(key, "ro") || is(key, "commits")) {
		return -ENOTSUP;
	}
	if (!is(key, "v")) {
		return -ENOENT;
	}
	if (len > sizeof(value)) {
		return -ENOMEM;
	}
	rc = read_cb(cb_arg, value, len);
	if (rc < 0) {
		return (int)rc;
	}
	value_len = (size_t)rc;
	return 0;
}

static int bench_commit(void)
{
	commits++;
	return 0;
}

static int bench_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
	return export_func("smply/v", value, value_len);
}

SETTINGS_STATIC_HANDLER_DEFINE(smply_bench, "smply", bench_get, bench_set, bench_commit,
			       bench_export);

/*
 * The sample never initialises the settings subsystem, and without it no
 * storage back-end is registered: every MCUmgr save then fails with
 * SAVE_NOT_SUPPORTED (settings_save() returns -ENOENT with no destination),
 * observed on the bench before this existed. A product would do this at boot.
 * Deliberately no settings_load() here: the MCUmgr `load` command stays the
 * only way a stored value reaches the handler, which is what lets the HIL case
 * tell a load from a value that was never lost.
 */
static int smply_bench_settings_init(void)
{
	return settings_subsys_init();
}

SYS_INIT(smply_bench_settings_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
