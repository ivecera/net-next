/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __LAN966X_DPLL_H__
#define __LAN966X_DPLL_H__

#include <linux/notifier.h>
#include <linux/workqueue_types.h>

struct dpll_pin;
struct fwnode_handle;
struct lan966x_port;

struct lan966x_dpll {
	struct fwnode_handle	*mux_node;
	struct dpll_pin		*mux_pin;
	struct dpll_pin		*rclk_pin;
	struct notifier_block	nb;
	struct work_struct	work;
};

int lan966x_dpll_init(struct lan966x_port *port, struct fwnode_handle *portnp);
void lan966x_dpll_cleanup(struct lan966x_port *port);

#endif /* __LAN966X_DPLL_H__ */
