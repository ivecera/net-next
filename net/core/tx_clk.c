// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/dpll.h>
#include <linux/netdevice.h>
#include <linux/netdev_tx_clk.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "tx_clk.h"

struct netdev_tx_clk_data *netdev_get_tx_clk_data(struct net_device *ndev)
{
	return ndev->tx_clk_data;
}

/**
 * netdev_tx_clk_register_dpll_pin - Register a DPLL pin as TX clock source
 * @ndev: network device
 * @pin: DPLL pin handle
 * @ops: driver callbacks used to control the active pin
 * @priv: driver private context passed back to @ops
 *
 * Register a DPLL pin that can be used as a TX clock source for this netdev.
 * Users can later select this pin as the active TX clock source through its
 * DPLL pin_id. The driver-provided @ops are used to apply active pin changes
 * in hardware.
 *
 * Returns: 0 on success, negative error code on failure
 */
int netdev_tx_clk_register_dpll_pin(struct net_device *ndev,
				    struct dpll_pin *pin,
				    const struct netdev_tx_clk_ops *ops,
				    void *priv)
{
	struct netdev_tx_clk_pin_node *pin_node;
	struct netdev_tx_clk_pin_node *node;
	struct netdev_tx_clk_data *clk_data;

	if (WARN_ON(!pin || !ops || !ops->set_active || !ops->get_active))
		return -EINVAL;

	clk_data = netdev_get_tx_clk_data(ndev);
	if (!clk_data) {
		/* Initialize TX clock data if not present */
		clk_data = kzalloc(sizeof(*clk_data), GFP_KERNEL);
		if (!clk_data)
			return -ENOMEM;

		mutex_init(&clk_data->lock);
		INIT_LIST_HEAD(&clk_data->pins);
		clk_data->ops = ops;
		clk_data->priv = priv;
		ndev->tx_clk_data = clk_data;
	}

	mutex_lock(&clk_data->lock);
	if (WARN_ON(clk_data->ops != ops || clk_data->priv != priv)) {
		mutex_unlock(&clk_data->lock);
		return -EINVAL;
	}
	list_for_each_entry(node, &clk_data->pins, node) {
		if (node->pin == pin) {
			mutex_unlock(&clk_data->lock);
			return -EEXIST;
		}
	}

	pin_node = kzalloc(sizeof(*pin_node), GFP_KERNEL);
	if (!pin_node) {
		mutex_unlock(&clk_data->lock);
		return -ENOMEM;
	}
	pin_node->pin = pin;
	list_add_tail(&pin_node->node, &clk_data->pins);

	mutex_unlock(&clk_data->lock);

	netdev_err(ndev, "Registered TX clock DPLL pin %p\n", pin);

	return 0;
}
EXPORT_SYMBOL_GPL(netdev_tx_clk_register_dpll_pin);

/**
 * netdev_tx_clk_unregister_dpll_pin - Unregister a DPLL pin
 * @ndev: network device
 * @pin: DPLL pin handle to unregister
 */
void netdev_tx_clk_unregister_dpll_pin(struct net_device *ndev,
				       struct dpll_pin *pin)
{
	struct netdev_tx_clk_data *clk_data = netdev_get_tx_clk_data(ndev);
	struct netdev_tx_clk_pin_node *node, *tmp;

	mutex_lock(&clk_data->lock);
	list_for_each_entry_safe(node, tmp, &clk_data->pins, node) {
		if (node->pin == pin) {
			list_del(&node->node);
			kfree(node);
			break;
		}
	}

	if (list_empty(&clk_data->pins)) {
		ndev->tx_clk_data = NULL;
		clk_data->ops = NULL;
		clk_data->priv = NULL;
		mutex_unlock(&clk_data->lock);
		mutex_destroy(&clk_data->lock);
		kfree(clk_data);
	} else {
		mutex_unlock(&clk_data->lock);
	}

	netdev_dbg(ndev, "Unregistered TX clock DPLL pin %p\n", pin);
}
EXPORT_SYMBOL_GPL(netdev_tx_clk_unregister_dpll_pin);

/**
 * netdev_tx_clk_set_active_pin - Set the active TX clock pin
 * @ndev: network device
 * @pin_id: DPLL pin ID to request as the active TX clock source
 *
 * Returns: 0 on success, negative error code on failure
 */
int netdev_tx_clk_set_active_pin(struct net_device *ndev, u32 pin_id)
{
	struct netdev_tx_clk_data *clk_data;
	struct netdev_tx_clk_pin_node *node;
	struct dpll_pin *pin;
	int ret;

	if (!pin_id)
		return -EINVAL;
	clk_data = netdev_get_tx_clk_data(ndev);
	mutex_lock(&clk_data->lock);
	list_for_each_entry(node, &clk_data->pins, node) {
		if (dpll_pin_id_get(node->pin) == pin_id) {
			pin = node->pin;
			break;
		}
	}
	if (pin)
		ret = clk_data->ops->set_active(ndev, pin, clk_data->priv);
	else
		ret = -ENOENT;
	mutex_unlock(&clk_data->lock);
	netdev_dbg(ndev, "Set active TX clock pin ID %u ret=%d\n", pin_id, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(netdev_tx_clk_set_active_pin);

/**
 * netdev_tx_clk_get_active_pin - Get the currently active TX clock pin
 * @ndev: network device
 *
 * Returns: pointer to active DPLL pin, or ERR_PTR on error
 */
struct dpll_pin *netdev_tx_clk_get_active_pin(struct net_device *ndev)
{
	struct netdev_tx_clk_data *clk_data = netdev_get_tx_clk_data(ndev);
	struct dpll_pin *active_pin_ptr;

	mutex_lock(&clk_data->lock);
	active_pin_ptr = clk_data->ops->get_active(ndev, clk_data->priv);
	mutex_unlock(&clk_data->lock);

	return active_pin_ptr;
}
EXPORT_SYMBOL_GPL(netdev_tx_clk_get_active_pin);
