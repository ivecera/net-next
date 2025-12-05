// SPDX-License-Identifier: GPL-2.0
/*
 * DPLL support for netdevsim
 * Copyright (c) 2025 Red Hat, Inc.
 *
 * This allows netdevsim ports to attach to dpllsim devices as
 * SyncE recovered clock sources.
 */

#include <linux/debugfs.h>
#include <linux/dpll.h>
#include <linux/netdevice.h>

#include "netdevsim.h"

/* Recovered clock pin frequency - typical SyncE recovered clock */
static struct dpll_pin_frequency nsim_dpll_rclk_freq[] = {
	DPLL_PIN_FREQUENCY_RANGE(25000000, 25000000),  /* 25 MHz */
};

static int nsim_dpll_rclk_frequency_get(const struct dpll_pin *pin,
					void *pin_priv,
					const struct dpll_device *dpll,
					void *dpll_priv,
					u64 *frequency,
					struct netlink_ext_ack *extack)
{
	*frequency = 25000000;  /* 25 MHz */
	return 0;
}

static int nsim_dpll_rclk_direction_get(const struct dpll_pin *pin,
					void *pin_priv,
					const struct dpll_device *dpll,
					void *dpll_priv,
					enum dpll_pin_direction *direction,
					struct netlink_ext_ack *extack)
{
	*direction = DPLL_PIN_DIRECTION_INPUT;
	return 0;
}

static int nsim_dpll_rclk_state_on_pin_get(const struct dpll_pin *pin,
					   void *pin_priv,
					   const struct dpll_pin *parent_pin,
					   void *parent_pin_priv,
					   enum dpll_pin_state *state,
					   struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = pin_priv;

	/* Report connected if we're attached */
	if (ns->dpll.attached)
		*state = DPLL_PIN_STATE_CONNECTED;
	else
		*state = DPLL_PIN_STATE_DISCONNECTED;
	return 0;
}

static const struct dpll_pin_ops nsim_dpll_rclk_ops = {
	.frequency_get = nsim_dpll_rclk_frequency_get,
	.direction_get = nsim_dpll_rclk_direction_get,
	.state_on_pin_get = nsim_dpll_rclk_state_on_pin_get,
};

static int nsim_dpll_attach(struct netdevsim *ns, u64 clock_id, u32 parent_pin_id)
{
	struct dpll_pin_properties rclk_prop = {
		.board_label = ns->netdev->name,
		.type = DPLL_PIN_TYPE_SYNCE_ETH_PORT,
		.capabilities = 0,
		.freq_supported = nsim_dpll_rclk_freq,
		.freq_supported_num = ARRAY_SIZE(nsim_dpll_rclk_freq),
	};
	struct dpll_pin_properties parent_prop = {
		.type = DPLL_PIN_TYPE_MUX,
	};
	struct dpll_pin *parent_pin;
	struct dpll_pin *pin;
	int err;

	if (ns->dpll.attached)
		return -EBUSY;

	/* Get the parent MUX pin by clock_id and pin index */
	parent_pin = dpll_pin_get(clock_id, parent_pin_id, THIS_MODULE,
				  &parent_prop, NULL, &ns->dpll.parent_tracker);
	if (IS_ERR(parent_pin)) {
		netdev_err(ns->netdev,
			   "Failed to get parent MUX pin %u on clock_id %llu\n",
			   parent_pin_id, clock_id);
		return PTR_ERR(parent_pin);
	}

	/* Create a recovered clock pin for our netdev */
	pin = dpll_pin_get(clock_id, ns->netdev->ifindex + 0x10000, THIS_MODULE,
			   &rclk_prop, NULL, &ns->dpll.tracker);
	if (IS_ERR(pin)) {
		err = PTR_ERR(pin);
		netdev_err(ns->netdev, "Failed to get DPLL pin: %d\n", err);
		goto err_put_parent;
	}

	/* Register the recovered clock pin on the parent MUX pin */
	err = dpll_pin_on_pin_register(parent_pin, pin, &nsim_dpll_rclk_ops, ns);
	if (err) {
		netdev_err(ns->netdev, "Failed to register pin on parent MUX: %d\n", err);
		goto err_put_pin;
	}

	/* Link the pin to our netdev */
	dpll_netdev_pin_set(ns->netdev, pin);

	ns->dpll.pin = pin;
	ns->dpll.parent_pin = parent_pin;
	ns->dpll.clock_id = clock_id;
	ns->dpll.parent_pin_id = parent_pin_id;
	ns->dpll.attached = true;

	netdev_info(ns->netdev,
		    "Attached to DPLL clock_id %llu, parent MUX pin %u\n",
		    clock_id, parent_pin_id);
	return 0;

err_put_pin:
	dpll_pin_put(pin, &ns->dpll.tracker);
err_put_parent:
	dpll_pin_put(parent_pin, &ns->dpll.parent_tracker);
	return err;
}

static void nsim_dpll_detach(struct netdevsim *ns)
{
	if (!ns->dpll.attached)
		return;

	dpll_netdev_pin_clear(ns->netdev);

	/* Unregister recovered clock pin from parent MUX pin */
	dpll_pin_on_pin_unregister(ns->dpll.parent_pin, ns->dpll.pin,
				   &nsim_dpll_rclk_ops, ns);

	dpll_pin_put(ns->dpll.pin, &ns->dpll.tracker);
	dpll_pin_put(ns->dpll.parent_pin, &ns->dpll.parent_tracker);

	netdev_info(ns->netdev, "Detached from DPLL clock_id %llu, parent MUX pin %u\n",
		    ns->dpll.clock_id, ns->dpll.parent_pin_id);

	ns->dpll.pin = NULL;
	ns->dpll.parent_pin = NULL;
	ns->dpll.clock_id = 0;
	ns->dpll.parent_pin_id = 0;
	ns->dpll.attached = false;
}

/*
 * debugfs interface
 *
 * The 'attach' file accepts: "clock_id parent_pin_id" or "none"
 * Example: echo "12345 0" > attach   # Attach to clock_id=12345, parent MUX pin 0
 *          echo "none" > attach      # Detach
 */
static ssize_t nsim_dpll_attach_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct netdevsim *ns = file->private_data;
	char tmp[48];
	int len;

	if (ns->dpll.attached)
		len = scnprintf(tmp, sizeof(tmp), "%llu %u\n",
				ns->dpll.clock_id, ns->dpll.parent_pin_id);
	else
		len = scnprintf(tmp, sizeof(tmp), "none\n");

	return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t nsim_dpll_attach_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct netdevsim *ns = file->private_data;
	char tmp[48];
	u64 clock_id;
	u32 parent_pin_id;
	int ret;
	int err;

	if (count >= sizeof(tmp))
		return -EINVAL;

	if (copy_from_user(tmp, buf, count))
		return -EFAULT;
	tmp[count] = '\0';

	/* "none" detaches */
	if (strncmp(tmp, "none", 4) == 0) {
		nsim_dpll_detach(ns);
		return count;
	}

	/* Parse "clock_id parent_pin_id" */
	ret = sscanf(tmp, "%llu %u", &clock_id, &parent_pin_id);
	if (ret != 2)
		return -EINVAL;

	if (clock_id == 0) {
		nsim_dpll_detach(ns);
		return count;
	}

	/* Detach from current DPLL if attached */
	if (ns->dpll.attached)
		nsim_dpll_detach(ns);

	err = nsim_dpll_attach(ns, clock_id, parent_pin_id);
	if (err)
		return err;

	return count;
}

static const struct file_operations nsim_dpll_attach_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = nsim_dpll_attach_read,
	.write = nsim_dpll_attach_write,
};

int nsim_dpll_init(struct netdevsim *ns)
{
	struct dentry *ddir;

	ns->dpll.attached = false;
	ns->dpll.pin = NULL;
	ns->dpll.parent_pin = NULL;
	ns->dpll.clock_id = 0;
	ns->dpll.parent_pin_id = 0;

	/* Create debugfs directory */
	ddir = debugfs_create_dir("dpll", ns->nsim_dev_port->ddir);
	if (IS_ERR(ddir))
		return PTR_ERR(ddir);

	ns->dpll.ddir = ddir;

	/* Create attach file - accepts "clock_id parent_pin_id" */
	debugfs_create_file("attach", 0644, ddir, ns,
			    &nsim_dpll_attach_fops);

	return 0;
}

void nsim_dpll_exit(struct netdevsim *ns)
{
	if (ns->dpll.attached)
		nsim_dpll_detach(ns);

	debugfs_remove_recursive(ns->dpll.ddir);
}
