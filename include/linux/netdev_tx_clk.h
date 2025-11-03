/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * netdev_tx_clk.h - allow net_device TX clock control via DPLL pins
 * Author: Arkadiusz Kubalewski <arkadiusz.kubalewski@intel.com>
 */

#ifndef __NETDEV_TX_CLK_H
#define __NETDEV_TX_CLK_H

#include <linux/errno.h>
#include <linux/types.h>

struct net_device;
struct dpll_pin;
struct netdev_tx_clk_data;

/**
 * struct netdev_tx_clk_ops - driver callbacks for TX clock control
 * @set_active: switch hardware to the requested DPLL pin
 * @get_active: report the currently active DPLL pin
 */
struct netdev_tx_clk_ops {
	int (*set_active)(struct net_device *ndev, struct dpll_pin *pin,
			  void *priv);
	struct dpll_pin * (*get_active)(struct net_device *ndev, void *priv);
};

#if IS_ENABLED(CONFIG_NET_TX_CLK)

int netdev_tx_clk_register_dpll_pin(struct net_device *ndev,
			    struct dpll_pin *pin,
			    const struct netdev_tx_clk_ops *ops,
			    void *priv);
void netdev_tx_clk_unregister_dpll_pin(struct net_device *ndev,
	       struct dpll_pin *pin);

int netdev_tx_clk_set_active_pin(struct net_device *ndev, u32 pin_id);
struct dpll_pin *netdev_tx_clk_get_active_pin(struct net_device *ndev);

/* Helper for core networking code */
struct netdev_tx_clk_data *netdev_get_tx_clk_data(struct net_device *ndev);

#else

static inline int
netdev_tx_clk_register_dpll_pin(struct net_device *ndev,
				struct dpll_pin *pin,
				const struct netdev_tx_clk_ops *ops,
				void *priv)
{
	return 0;
}

static inline void netdev_tx_clk_unregister_dpll_pin(struct net_device *ndev,
						     struct dpll_pin *pin)
{
}

static inline int netdev_tx_clk_set_active_pin(struct net_device *ndev,
					       u32 pin_id)
{
	return -EOPNOTSUPP;
}

static inline struct dpll_pin *
netdev_tx_clk_get_active_pin(struct net_device *ndev)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline struct netdev_tx_clk_data *
netdev_get_tx_clk_data(struct net_device *ndev)
{
	return NULL;
}

#endif
#endif /* __NETDEV_TX_CLK_H */
