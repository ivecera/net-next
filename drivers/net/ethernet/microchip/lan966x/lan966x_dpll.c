// SPDX-License-Identifier: GPL-2.0+

#include <linux/dpll.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#include "lan966x_dpll.h"
#include "lan966x_main.h"

static int
lan966x_pin_direction_get(const struct dpll_pin *pin, void *pin_priv,
			  const struct dpll_device *dpll, void *dpll_priv,
			  enum dpll_pin_direction *direction,
			  struct netlink_ext_ack *extack)
{
	struct lan966x_port *port = pin_priv;

	netdev_info(port->dev, "%s\n", __func__);
	*direction = DPLL_PIN_DIRECTION_INPUT;

	return 0;
}

static int
lan966x_pin_state_on_pin_get(const struct dpll_pin *pin, void *pin_priv,
			     const struct dpll_pin *parent_pin,
			     void *parent_pin_priv,
			     enum dpll_pin_state *state,
			     struct netlink_ext_ack *extack)
{
	struct lan966x_port *port = pin_priv;

	netdev_info(port->dev, "%s\n", __func__);
	*state = DPLL_PIN_STATE_CONNECTED;

	return 0;
}

static struct dpll_pin_ops pin_ops = {
	.state_on_pin_get = lan966x_pin_state_on_pin_get,
	.direction_get = lan966x_pin_direction_get,
};

static struct dpll_pin_properties pin_props = {
	.board_label = "RCLK BRD",
	.panel_label = "RCLK PNL",
	.package_label = "RCLK PKG",
	.type = DPLL_PIN_TYPE_EXT,
	.freq_supported_num = 0,
};

static u64 lan966x_dpll_clock_id(struct lan966x_port *port)
{
	u64 base_mac, clock_id;

	base_mac = get_unaligned_be48(port->lan966x->base_mac);
	clock_id = ULL(0xFFFE000000);
	clock_id |= base_mac & GENMASK_U64(23, 0);
	clock_id |= (base_mac & GENMASK_U64(47, 24)) << 16;

	return clock_id;
}

static int lan966x_dpll_pin_register(struct lan966x_port *port)
{
	int err;

	if (WARN_ON(!port->dpll.mux_pin))
		return -EINVAL;

	/* Allocate our pin */
	port->dpll.rclk_pin = dpll_pin_get(lan966x_dpll_clock_id(port),
					   DPLL_PIN_IDX_UNSPEC, THIS_MODULE,
					   &pin_props, NULL);
	if (IS_ERR(port->dpll.rclk_pin)) {
		netdev_err(port->dev, "Failed to alloc dpll pin: %pe\n",
			   port->dpll.rclk_pin);
		return PTR_ERR(port->dpll.rclk_pin);
	}

	/* Register our pin on top of mux pin from the FW */
	err = dpll_pin_on_pin_register(port->dpll.mux_pin, port->dpll.rclk_pin,
				       &pin_ops, port);
	if (err) {
		netdev_err(port->dev, "Failed to register pin: %pe\n",
			   ERR_PTR(err));
		dpll_pin_put(port->dpll.rclk_pin);
		port->dpll.rclk_pin = NULL;
		return err;
	}

	/* Set our pin as recovered clock pin */
	dpll_netdev_pin_set(port->dev, port->dpll.rclk_pin);

	return 0;
}

static int lan966x_dpll_pin_unregister(struct lan966x_port *port)
{
	if (WARN_ON(!port->dpll.mux_pin))
		return -EINVAL;

	/* Clear recovered clock pin */
	dpll_netdev_pin_clear(port->dev);

	/* Unregister our pin */
	dpll_pin_on_pin_unregister(port->dpll.mux_pin, port->dpll.rclk_pin,
				   &pin_ops, port);

	/* Free our pin */
	dpll_pin_put(port->dpll.rclk_pin);
	port->dpll.rclk_pin = NULL;

	return 0;
}

static void lan966x_dpll_work(struct work_struct *work)
{
	struct lan966x_dpll *dpll = container_of(work, struct lan966x_dpll,
						 work);
	struct lan966x_port *port = container_of(dpll, struct lan966x_port,
						 dpll);
	int err;

	if (!dpll->rclk_pin) {
		/* At this point port->mux_pin should be NULL and pin referenced
		 * by port->mux_node should be registered.
		 */
		if (!WARN_ON(dpll->mux_pin)) {
			dpll->mux_pin = fwnode_dpll_pin_find(dpll->mux_node);
			if (!dpll->mux_pin) {
				netdev_warn(port->dev,
					    "Parent mux pin not registered\n");
				return;
			}
		}
		/* Register our pin on top of mux pin */
		err = lan966x_dpll_pin_register(port);
		if (err) {
			dpll_pin_put(dpll->mux_pin);
			dpll->mux_pin = NULL;
			return;
		}
	} else {
		/* At this point port->mux_pin should be non-NULL */
		if (WARN_ON(!dpll->mux_pin)) {
			netdev_warn(port->dev, "Parent mux pin is NULL\n");
			return;
		}
		/* Unregister our pin */
		lan966x_dpll_pin_unregister(port);
		/* Release parent mux pin */
		dpll_pin_put(dpll->mux_pin);
		dpll->mux_pin = NULL;
	}
}

static int lan966x_dpll_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct lan966x_port *port = container_of(nb, struct lan966x_port,
						 dpll.nb);
	struct dpll_pin_notifier_info *info = data;

	switch (action) {
	case DPLL_PIN_CREATED:
		if (port->dpll.mux_node != info->fwnode)
			return NOTIFY_DONE;
		netdev_info(port->dev, "Mux pin registered\n");
		break;
	case DPLL_PIN_DELETED:
		if (port->dpll.mux_node != info->fwnode)
			return NOTIFY_DONE;
		netdev_info(port->dev, "Mux pin unregistered\n");
		break;
	default:
		return NOTIFY_DONE;
	}

	schedule_work(&port->dpll.work);

	return NOTIFY_OK;
}

int lan966x_dpll_init(struct lan966x_port *port, struct fwnode_handle *portnp)
{
	int err;

	/* Try to get fwnode for recovered clock mux pin */
	port->dpll.mux_node = fwnode_get_dpll_pin_node(portnp, "rclk");
	if (IS_ERR(port->dpll.mux_node)) {
		netdev_info(port->dev, "No dpll pin for recovered clock\n");
		return 0; /* Not an error */
	}

	INIT_WORK(&port->dpll.work, lan966x_dpll_work);

	/* Subscribe to DPLL notifications */
	port->dpll.nb.notifier_call = lan966x_dpll_notify;
	err = register_dpll_notifier(&port->dpll.nb);
	if (err) {
		netdev_err(port->dev,
			   "Failed to subscribe to DPLL events: %pe\n",
			   ERR_PTR(err));
		fwnode_handle_put(port->dpll.mux_node);
		port->dpll.mux_node = NULL;
		return err;
	}

	/* Check if the parent mux pin is already registered */
	port->dpll.mux_pin = fwnode_dpll_pin_find(port->dpll.mux_node);
	if (!port->dpll.mux_pin) {
		netdev_info(port->dev, "Mux pin not registered yet\n");
		return 0;
	}

	/* Register our pin-on-pin pin */
	err = lan966x_dpll_pin_register(port);
	if (err) {
		dpll_pin_put(port->dpll.mux_pin);
		port->dpll.mux_pin = NULL;
		return err;
	}

	return 0;
}

void lan966x_dpll_cleanup(struct lan966x_port *port)
{
	if (IS_ERR_OR_NULL(port->dpll.mux_node))
		return;

	unregister_dpll_notifier(&port->dpll.nb);
	cancel_work_sync(&port->dpll.work);

	/* Unregister our dpll pin if it is registered */
	if (port->dpll.rclk_pin)
		lan966x_dpll_pin_unregister(port);

	/* Drop parent mux pin reference if we have it */
	if (port->dpll.mux_pin)
		dpll_pin_put(port->dpll.mux_pin);

	/* Drop DPLL pin node reference */
	fwnode_handle_put(port->dpll.mux_node);
}
