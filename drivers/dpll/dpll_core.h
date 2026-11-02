/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (c) 2023 Meta Platforms, Inc. and affiliates
 *  Copyright (c) 2023 Intel and affiliates
 */

#ifndef __DPLL_CORE_H__
#define __DPLL_CORE_H__

#include <linux/dpll.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/ref_tracker.h>
#include <linux/rwsem.h>
#include "dpll_nl.h"

#define DPLL_REGISTERED		XA_MARK_1

/**
 * struct dpll_domain - group of dpll objects sharing a lock
 * @id:			index of the domain in dpll_domain_xa
 * @clock_id:		clock_id of the objects in the domain
 * @module:		module of the objects in the domain
 * @refcount:		number of dpll devices and pins in the domain
 * @lock:		serializes driver callbacks of the domain
 *
 * All dpll devices and pins created with the same (module, clock_id) pair
 * belong to one domain, in practice to one chip or card.
 *
 * Callbacks of a device run under the domain of the device and callbacks of a
 * pin under the domain of the pin, whichever driver registered them. Only one
 * domain lock is held at a time, so domains have no lock order.
 **/
struct dpll_domain {
	u32 id;
	u64 clock_id;
	struct module *module;
	refcount_t refcount;
	struct mutex lock;
};

/**
 * struct dpll_device - stores DPLL device internal data
 * @id:			unique id number for device given by dpll subsystem
 * @device_idx:		id given by dev driver
 * @clock_id:		unique identifier (clock_id) of a dpll
 * @module:		module of creator
 * @domain:		domain the device belongs to
 * @type:		type of a dpll
 * @pin_refs:		stores pins registered within a dpll
 * @refcount:		refcount
 * @refcnt_tracker:	ref_tracker directory for debugging reference leaks
 * @registration_list:	list of registered ops and priv data of dpll owners
 **/
struct dpll_device {
	u32 id;
	u32 device_idx;
	u64 clock_id;
	struct module *module;
	struct dpll_domain *domain;
	enum dpll_type type;
	struct xarray pin_refs;
	refcount_t refcount;
	struct ref_tracker_dir refcnt_tracker;
	struct list_head registration_list;
};

/**
 * struct dpll_pin - structure for a dpll pin
 * @id:			unique id number for pin given by dpll subsystem
 * @pin_idx:		index of a pin given by dev driver
 * @clock_id:		clock_id of creator
 * @module:		module of creator
 * @module_name:	module name of creator
 * @domain:		domain the pin belongs to
 * @fwnode:		optional reference to firmware node
 * @dpll_refs:		hold referencees to dplls pin was registered with
 * @parent_refs:	hold references to parent pins pin was registered with
 * @ref_sync_pins:	hold references to pins for Reference SYNC feature
 * @prop:		pin properties copied from the registerer
 * @refcount:		refcount
 * @refcnt_tracker:	ref_tracker directory for debugging reference leaks
 * @rcu:		rcu_head for kfree_rcu()
 **/
struct dpll_pin {
	u32 id;
	u32 pin_idx;
	u64 clock_id;
	struct module *module;
	char module_name[MODULE_NAME_LEN];
	struct dpll_domain *domain;
	struct fwnode_handle *fwnode;
	struct xarray dpll_refs;
	struct xarray parent_refs;
	struct xarray ref_sync_pins;
	struct dpll_pin_properties prop;
	refcount_t refcount;
	struct ref_tracker_dir refcnt_tracker;
	struct rcu_head rcu;
};

/**
 * struct dpll_pin_ref - structure for referencing either dpll or pins
 * @dpll:		pointer to a dpll
 * @pin:		pointer to a pin
 * @registration_list:	list of ops and priv data registered with the ref
 * @refcount:		refcount
 **/
struct dpll_pin_ref {
	union {
		struct dpll_device *dpll;
		struct dpll_pin *pin;
	};
	struct list_head registration_list;
	refcount_t refcount;
};

void *dpll_priv(struct dpll_device *dpll);
void *dpll_pin_on_dpll_priv(struct dpll_device *dpll, struct dpll_pin *pin);
void *dpll_pin_on_pin_priv(struct dpll_pin *parent, struct dpll_pin *pin);

const struct dpll_device_ops *dpll_device_ops(struct dpll_device *dpll);
struct dpll_device *dpll_device_get_by_id(int id);
struct dpll_pin_ref *dpll_pin_own_dpll_ref_first(struct dpll_pin *pin);
const struct dpll_pin_ops *dpll_pin_ops(struct dpll_pin_ref *ref);
struct dpll_pin_ref *dpll_xa_ref_dpll_first(struct xarray *xa_refs);
extern struct xarray dpll_device_xa;
extern struct xarray dpll_pin_xa;
extern struct rw_semaphore dpll_lock;

void dpll_device_notify(struct dpll_device *dpll, unsigned long action);
void dpll_pin_notify(struct dpll_pin *pin, u64 src_clock_id,
		     unsigned long action);

#endif
