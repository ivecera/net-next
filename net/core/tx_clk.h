/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * netdev_tx_clk.h - allow net_device TX clock control via DPLL pins
 * Author: Arkadiusz Kubalewski <arkadiusz.kubalewski@intel.com>
 */
#ifndef _NET_TX_CLK_H
#define _NET_TX_CLK_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdev_tx_clk.h>

struct netdev_tx_clk_pin_node {
	struct dpll_pin *pin;
	struct list_head node;
};

struct netdev_tx_clk_data {
	struct mutex lock; /* per-device TX clock access protection */
	struct list_head pins;
	const struct netdev_tx_clk_ops *ops;
	void *priv;
};

#endif /* _NET_TX_CLK_H */
