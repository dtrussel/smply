// SPDX-License-Identifier: Apache-2.0
//
// Bench-only BLE link logging for the BL54L15 peer (tests/hil/README.md).
//
// BLE upload throughput on this bench depends on the update's direction
// (roadmap backlog). The central decides the connection parameters, the PHY
// and, with the peripheral, the data length and ATT MTU, and nothing on the
// client side reports them. This logs each as it is set or changed, as one
// "smply-bench:" line on the console, which uart_log.py already captures for
// every HIL case. It changes nothing: it only observes.

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(smply_bench, LOG_LEVEL_INF);

static void log_conn(struct bt_conn *conn, const char *what)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0) {
		return;
	}
	LOG_INF("smply-bench: %s interval_us=%u latency=%u timeout_10ms=%u tx_phy=%u rx_phy=%u "
		"tx_len=%u rx_len=%u mtu=%u",
		what, info.le.interval_us, info.le.latency, info.le.timeout,
		info.le.phy->tx_phy, info.le.phy->rx_phy, info.le.data_len->tx_max_len,
		info.le.data_len->rx_max_len, bt_gatt_get_mtu(conn));
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err == 0) {
		log_conn(conn, "connected");
	}
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(interval);
	ARG_UNUSED(latency);
	ARG_UNUSED(timeout);
	log_conn(conn, "param");
}

static void le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	ARG_UNUSED(param);
	log_conn(conn, "phy");
}

static void le_data_len_updated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
	ARG_UNUSED(info);
	log_conn(conn, "data_len");
}

BT_CONN_CB_DEFINE(smply_bench_conn_cb) = {
	.connected = connected,
	.le_param_updated = le_param_updated,
	.le_phy_updated = le_phy_updated,
	.le_data_len_updated = le_data_len_updated,
};

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(tx);
	ARG_UNUSED(rx);
	log_conn(conn, "mtu");
}

static struct bt_gatt_cb smply_bench_gatt_cb = {
	.att_mtu_updated = att_mtu_updated,
};

static int smply_bench_ble_log_init(void)
{
	bt_gatt_cb_register(&smply_bench_gatt_cb);
	return 0;
}

SYS_INIT(smply_bench_ble_log_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
