/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __LAN966X_DPLL_H__
#define __LAN966X_DPLL_H__

#include <linux/notifier.h>
#include <linux/workqueue_types.h>

struct dpll_pin;
struct fwnode_handle;
struct lan966x_port;

#define LAN966X_DPLL_NUM_PINS	4

enum lan966x_dpll_state {
	LAN966X_DPLL_UNREGISTERED,
	LAN966X_DPLL_REGISTERED,
};

struct lan966x_dpll {
	struct fwnode_handle	*mux_node;
	struct dpll_pin		*mux_pin;
	struct dpll_pin		*pins[LAN966X_DPLL_NUM_PINS];
	struct notifier_block	nb;
	struct work_struct	work;
	int			rclk_pin;
	enum lan966x_dpll_state	state;
};

int lan966x_dpll_init(struct lan966x_port *port, struct fwnode_handle *portnp);
void lan966x_dpll_cleanup(struct lan966x_port *port);

#endif /* __LAN966X_DPLL_H__ */
