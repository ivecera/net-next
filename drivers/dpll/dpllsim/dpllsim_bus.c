// SPDX-License-Identifier: GPL-2.0-only
/*
 * DPLL simulator - bus and device management
 * Copyright (c) 2025 Red Hat, Inc.
 */

#include <linux/device.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "dpllsim.h"

static DEFINE_IDA(dpllsim_dev_ids);
static DEFINE_MUTEX(dpllsim_dev_list_lock);
static LIST_HEAD(dpllsim_dev_list);

/* Bus enable/disable for safe module unload */
static bool dpllsim_bus_enable;

struct dpllsim_bus_dev {
	struct device dev;
	struct dpllsim_dev *dpll;
	struct list_head list;
	u64 clock_id;
	bool init;
	bool deployed;  /* true = registered to DPLL subsystem */
};

static struct dpllsim_bus_dev *to_dpllsim_bus_dev(struct device *dev)
{
	return container_of(dev, struct dpllsim_bus_dev, dev);
}

static void dpllsim_bus_dev_release(struct device *dev)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);

	kfree(bus_dev);
}

static struct bus_type dpllsim_bus = {
	.name = "dpllsim",
};

/* Sysfs attribute: deployed - RO status */
static ssize_t deployed_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);

	return sprintf(buf, "%d\n", bus_dev->deployed ? 1 : 0);
}
static DEVICE_ATTR_RO(deployed);

/* Sysfs attribute: deploy - deploy/undeploy device */
static ssize_t deploy_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);
	unsigned int val;
	int ret;

	if (!smp_load_acquire(&bus_dev->init))
		return -EBUSY;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	if (val > 1)
		return -EINVAL;

	if (val == 1 && !bus_dev->deployed) {
		/* Deploy - register to DPLL subsystem */
		pr_info("dpllsim: deploying device %s\n", dev_name(dev));
		ret = dpllsim_dev_deploy(bus_dev->dpll);
		if (ret) {
			pr_err("dpllsim: deploy failed for %s: %d\n", dev_name(dev), ret);
			return ret;
		}
		bus_dev->deployed = true;
		pr_info("dpllsim: device %s deployed successfully\n", dev_name(dev));
	} else if (val == 0 && bus_dev->deployed) {
		/* Undeploy - unregister from DPLL subsystem */
		pr_info("dpllsim: undeploying device %s\n", dev_name(dev));
		dpllsim_dev_undeploy(bus_dev->dpll);
		bus_dev->deployed = false;
		pr_info("dpllsim: device %s undeployed\n", dev_name(dev));
	}

	return count;
}
static DEVICE_ATTR_WO(deploy);


/* Sysfs attribute: add_pin
 * Syntax: "type freq dir [prio] [parents:1,2,3]"
 * Example: "3 25000000 0 10"
 * Example with parents: "1 1953125 0 255 parents:0,1,2,3"
 */
static ssize_t add_pin_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);
	unsigned int type, direction;
	u32 frequency, prio = 128;  /* Default priority */
	int parent_ids[16], num_parents = 0;
	char *parents_str, *token;
	char buf_copy[256];
	int ret;

	if (!smp_load_acquire(&bus_dev->init))
		return -EBUSY;

	/* Can only add pins before deployment */
	if (bus_dev->deployed)
		return -EBUSY;

	if (count >= sizeof(buf_copy))
		return -EINVAL;

	strscpy(buf_copy, buf, sizeof(buf_copy));

	/* Check for parents: syntax */
	parents_str = strstr(buf_copy, "parents:");
	if (parents_str) {
		/* Parse parent IDs */
		*parents_str = '\0';  /* Terminate main string */
		parents_str += 8;  /* Skip "parents:" */

		/* Parse comma-separated parent IDs using strsep */
		while ((token = strsep(&parents_str, ",")) && num_parents < 16) {
			ret = kstrtoint(token, 10, &parent_ids[num_parents]);
			if (ret)
				return ret;
			num_parents++;
		}
	}

	/* Parse basic parameters: type freq dir [prio] */
	ret = sscanf(buf_copy, "%u %u %u %u", &type, &frequency, &direction, &prio);
	if (ret < 3)
		return -EINVAL;
	/* If only 3 params, prio keeps default value of 128 */

	/* Validate type: must be valid enum dpll_pin_type (1-5) */
	if (type < DPLL_PIN_TYPE_MUX || type > DPLL_PIN_TYPE_GNSS) {
		dev_err(dev, "Invalid pin type %u (must be 1-5)\n", type);
		return -EINVAL;
	}

	/* Validate frequency: must be non-zero */
	if (frequency == 0) {
		dev_err(dev, "Invalid pin frequency 0 (must be non-zero)\n");
		return -EINVAL;
	}

	/* Validate direction: must be DPLL_PIN_DIRECTION_INPUT (1) or OUTPUT (2) */
	if (direction != DPLL_PIN_DIRECTION_INPUT &&
	    direction != DPLL_PIN_DIRECTION_OUTPUT) {
		dev_err(dev, "Invalid pin direction %u (must be 1=INPUT or 2=OUTPUT)\n",
			direction);
		return -EINVAL;
	}

	/* Validate priority: reasonable range 0-255 */
	if (prio > 255) {
		dev_err(dev, "Invalid pin priority %u (must be 0-255)\n", prio);
		return -EINVAL;
	}

	ret = dpllsim_pin_add(bus_dev->dpll, type, frequency, direction, prio,
			     num_parents > 0 ? parent_ids : NULL, num_parents);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(add_pin);

/* Sysfs attribute: del_pin */
static ssize_t del_pin_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);
	unsigned int pin_id;
	int ret;

	if (!smp_load_acquire(&bus_dev->init))
		return -EBUSY;

	/* Can only delete pins before deployment */
	if (bus_dev->deployed)
		return -EBUSY;

	ret = kstrtouint(buf, 10, &pin_id);
	if (ret)
		return ret;

	ret = dpllsim_pin_del(bus_dev->dpll, pin_id);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(del_pin);

/* Sysfs attribute: lock_time_ms */
static ssize_t lock_time_ms_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);

	if (!bus_dev->dpll)
		return -ENODEV;

	return sprintf(buf, "%u\n", bus_dev->dpll->lock_time_ms);
}

static ssize_t lock_time_ms_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);
	unsigned int val;
	int ret;

	if (!smp_load_acquire(&bus_dev->init))
		return -EBUSY;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	bus_dev->dpll->lock_time_ms = val;
	return count;
}
static DEVICE_ATTR_RW(lock_time_ms);

/* Sysfs attribute: update_interval_ms */
static ssize_t update_interval_ms_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);

	if (!bus_dev->dpll)
		return -ENODEV;

	return sprintf(buf, "%u\n", bus_dev->dpll->update_interval_ms);
}

static ssize_t update_interval_ms_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);
	unsigned int val;
	int ret;

	if (!smp_load_acquire(&bus_dev->init))
		return -EBUSY;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	if (val == 0 || val > 10000)
		return -EINVAL;

	bus_dev->dpll->update_interval_ms = val;

	/* Reschedule workqueue with new interval */
	cancel_delayed_work_sync(&bus_dev->dpll->update_work);
	schedule_delayed_work(&bus_dev->dpll->update_work,
			     msecs_to_jiffies(val));

	return count;
}
static DEVICE_ATTR_RW(update_interval_ms);

/* Sysfs attribute: media_down_prob */
static ssize_t media_down_prob_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);

	if (!bus_dev->dpll)
		return -ENODEV;

	return sprintf(buf, "%u\n", bus_dev->dpll->media_down_prob);
}

static ssize_t media_down_prob_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);
	unsigned int val;
	int ret;

	if (!smp_load_acquire(&bus_dev->init))
		return -EBUSY;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	bus_dev->dpll->media_down_prob = val;
	return count;
}
static DEVICE_ATTR_RW(media_down_prob);

/* Sysfs attribute: media_recovery_prob */
static ssize_t media_recovery_prob_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);

	if (!bus_dev->dpll)
		return -ENODEV;

	return sprintf(buf, "%u\n", bus_dev->dpll->media_recovery_prob);
}

static ssize_t media_recovery_prob_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev = to_dpllsim_bus_dev(dev);
	unsigned int val;
	int ret;

	if (!smp_load_acquire(&bus_dev->init))
		return -EBUSY;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	bus_dev->dpll->media_recovery_prob = val;
	return count;
}
static DEVICE_ATTR_RW(media_recovery_prob);

static struct attribute *dpllsim_dev_attrs[] = {
	&dev_attr_deployed.attr,
	&dev_attr_deploy.attr,
	&dev_attr_add_pin.attr,
	&dev_attr_del_pin.attr,
	&dev_attr_lock_time_ms.attr,
	&dev_attr_update_interval_ms.attr,
	&dev_attr_media_down_prob.attr,
	&dev_attr_media_recovery_prob.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dpllsim_dev);

/* Bus-level sysfs attribute: new_device */
static ssize_t new_device_store(const struct bus_type *bus,
			       const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev, *tmp;
	u64 clock_id;
	int id, err;

	if (!smp_load_acquire(&dpllsim_bus_enable)) {
		pr_err("dpllsim: bus is disabled\n");
		return -EBUSY;
	}

	err = kstrtou64(buf, 10, &clock_id);
	if (err) {
		pr_err("dpllsim: invalid clock_id '%s': %d\n", buf, err);
		return err;
	}

	/* Check if clock_id is already in use */
	mutex_lock(&dpllsim_dev_list_lock);
	list_for_each_entry(tmp, &dpllsim_dev_list, list) {
		if (tmp->clock_id == clock_id) {
			mutex_unlock(&dpllsim_dev_list_lock);
			pr_err("dpllsim: clock_id %llu already exists\n", clock_id);
			return -EEXIST;
		}
	}
	mutex_unlock(&dpllsim_dev_list_lock);

	pr_info("dpllsim: creating device with clock_id=%llu\n", clock_id);

	bus_dev = kzalloc(sizeof(*bus_dev), GFP_KERNEL);
	if (!bus_dev) {
		pr_err("dpllsim: failed to allocate bus_dev\n");
		return -ENOMEM;
	}

	id = ida_alloc(&dpllsim_dev_ids, GFP_KERNEL);
	if (id < 0) {
		pr_err("dpllsim: failed to allocate device ID: %d\n", id);
		err = id;
		goto err_free;
	}

	pr_debug("dpllsim: allocated device ID %d\n", id);

	bus_dev->dev.bus = &dpllsim_bus;
	bus_dev->dev.id = id;
	bus_dev->dev.release = dpllsim_bus_dev_release;
	bus_dev->dev.groups = dpllsim_dev_groups;
	bus_dev->clock_id = clock_id;

	dev_set_name(&bus_dev->dev, "dpllsim%u", id);

	err = device_register(&bus_dev->dev);
	if (err) {
		pr_err("dpllsim: device_register failed for dpllsim%u: %d\n", id, err);
		goto err_free_id;
	}

	pr_debug("dpllsim: device dpllsim%u registered\n", id);

	/* Create DPLL device */
	bus_dev->dpll = dpllsim_dev_create(clock_id);
	if (IS_ERR(bus_dev->dpll)) {
		err = PTR_ERR(bus_dev->dpll);
		pr_err("dpllsim: dpllsim_dev_create failed for dpllsim%u: %d\n", id, err);
		goto err_unreg_dev;
	}

	/* Link device to DPLL structure for dev_* logging */
	bus_dev->dpll->dev = &bus_dev->dev;

	/* Add to list */
	mutex_lock(&dpllsim_dev_list_lock);
	list_add(&bus_dev->list, &dpllsim_dev_list);
	mutex_unlock(&dpllsim_dev_list_lock);

	/* Mark as initialized */
	smp_store_release(&bus_dev->init, true);

	pr_info("dpllsim: device dpllsim%u created successfully (clock_id=%llu)\n",
		id, clock_id);

	return count;

err_unreg_dev:
	device_unregister(&bus_dev->dev);
	return err;
err_free_id:
	ida_free(&dpllsim_dev_ids, id);
err_free:
	kfree(bus_dev);
	return err;
}
static BUS_ATTR_WO(new_device);

/* Bus-level sysfs attribute: del_device
 * Usage: echo <id> > /sys/bus/dpllsim/del_device
 */
static ssize_t del_device_store(const struct bus_type *bus,
				const char *buf, size_t count)
{
	struct dpllsim_bus_dev *bus_dev, *tmp;
	unsigned int id;
	int err;

	err = kstrtouint(buf, 10, &id);
	if (err) {
		pr_err("dpllsim: invalid device id\n");
		return err;
	}

	err = -ENOENT;
	mutex_lock(&dpllsim_dev_list_lock);

	if (!smp_load_acquire(&dpllsim_bus_enable)) {
		mutex_unlock(&dpllsim_dev_list_lock);
		return -EBUSY;
	}

	list_for_each_entry_safe(bus_dev, tmp, &dpllsim_dev_list, list) {
		if (bus_dev->dev.id != id)
			continue;

		/* Cannot delete deployed device */
		if (bus_dev->deployed) {
			pr_err("dpllsim: cannot delete dpllsim%u while deployed\n", id);
			mutex_unlock(&dpllsim_dev_list_lock);
			return -EBUSY;
		}

		/* Mark as deleted and remove from list */
		smp_store_release(&bus_dev->init, false);
		list_del(&bus_dev->list);
		mutex_unlock(&dpllsim_dev_list_lock);

		/* Cleanup and unregister */
		dpllsim_dev_destroy(bus_dev->dpll);
		bus_dev->dpll = NULL;
		ida_free(&dpllsim_dev_ids, id);
		device_unregister(&bus_dev->dev);

		return count;
	}

	mutex_unlock(&dpllsim_dev_list_lock);
	pr_err("dpllsim: device dpllsim%u not found\n", id);
	return err;
}
static BUS_ATTR_WO(del_device);

static struct attribute *dpllsim_bus_attrs[] = {
	&bus_attr_new_device.attr,
	&bus_attr_del_device.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dpllsim_bus);

static struct device_driver dpllsim_driver = {
	.name	= "dpllsim",
	.bus	= &dpllsim_bus,
	.owner	= THIS_MODULE,
};

int dpllsim_bus_init(void)
{
	int err;

	pr_info("dpllsim: initializing bus\n");

	dpllsim_bus.bus_groups = dpllsim_bus_groups;

	err = bus_register(&dpllsim_bus);
	if (err) {
		pr_err("dpllsim: bus_register failed: %d\n", err);
		return err;
	}

	err = driver_register(&dpllsim_driver);
	if (err) {
		pr_err("dpllsim: driver_register failed: %d\n", err);
		goto err_bus_unreg;
	}

	/* Enable bus usage */
	smp_store_release(&dpllsim_bus_enable, true);

	pr_info("dpllsim: bus initialized successfully\n");

	return 0;

err_bus_unreg:
	bus_unregister(&dpllsim_bus);
	return err;
}

void dpllsim_bus_exit(void)
{
	struct dpllsim_bus_dev *bus_dev, *tmp;
	int count = 0;

	pr_info("dpllsim: exiting bus\n");

	/* Disable new operations */
	smp_store_release(&dpllsim_bus_enable, false);

	/* Remove all devices */
	mutex_lock(&dpllsim_dev_list_lock);
	list_for_each_entry_safe(bus_dev, tmp, &dpllsim_dev_list, list) {
		pr_info("dpllsim: removing device dpllsim%d\n", bus_dev->dev.id);
		list_del(&bus_dev->list);
		smp_store_release(&bus_dev->init, false);

		if (bus_dev->dpll) {
			/* Undeploy if deployed to ensure clean DPLL subsystem unregister */
			if (bus_dev->deployed) {
				pr_info("dpllsim: undeploying device dpllsim%d during module exit\n",
					bus_dev->dev.id);
				dpllsim_dev_undeploy(bus_dev->dpll);
				bus_dev->deployed = false;
			}

			dpllsim_dev_destroy(bus_dev->dpll);
			bus_dev->dpll = NULL;
		}

		ida_free(&dpllsim_dev_ids, bus_dev->dev.id);
		device_unregister(&bus_dev->dev);
		count++;
	}
	mutex_unlock(&dpllsim_dev_list_lock);

	pr_info("dpllsim: removed %d devices\n", count);

	driver_unregister(&dpllsim_driver);
	bus_unregister(&dpllsim_bus);

	pr_info("dpllsim: bus exit complete\n");
}
