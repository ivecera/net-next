// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for locking in the DPLL core.
 *
 * Each test starts with two unrelated domains:
 *
 * Domain A: devices A0 and A1
 *   pin P on A0 and A1, setting its state on A0 notifies pin S
 *   pin S on A0 and A1
 * Domain B: device B0
 *   pin BP on B0
 *
 * Tests of objects connected by a pin add these domains:
 *
 * Domain E: device E0, pin X on E0 (the owner), pin M (MUX) on E0
 * Domain F: device F0, pin X from E is also registered with F0
 * Domain G: pin C registered under pin M from E
 *
 * Requests that can block run in kthreads, so a test can report a locking bug
 * instead of blocking itself. A real deadlock cannot be undone, the stuck
 * request keeps its locks, so the teardown then hangs until the run times out.
 */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/debug_locks.h>
#include <linux/dpll.h>
#include <linux/kthread.h>
#include <linux/lockdep.h>
#include <linux/notifier.h>
#include <linux/property.h>
#include <linux/wait.h>
#include <net/genetlink.h>
#include <net/netlink.h>

#include "dpll_core.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

#define DT_CLOCK_ID_A		0xd0110a
#define DT_CLOCK_ID_B		0xd0110b
#define DT_CLOCK_ID_E		0xd0110e
#define DT_CLOCK_ID_F		0xd0110f
#define DT_CLOCK_ID_G		0xd01107

#define DT_PIN_IDX_P		0
#define DT_PIN_IDX_S		1
#define DT_PIN_IDX_BP		2
#define DT_PIN_IDX_M		3
#define DT_PIN_IDX_X		4
#define DT_PIN_IDX_C		5
#define DT_PIN_IDX_Y		6
#define DT_PIN_IDX_EXTRA	7

#define DT_MAX_EVENTS		8
#define DT_TIMEOUT		msecs_to_jiffies(5000)
#define DT_SHORT_TIMEOUT	msecs_to_jiffies(100)

struct dt_ctx;

/* Counters and blocking of the callbacks of one driver instance */
struct dt_dom {
	struct dt_ctx *ctx;
	struct completion entered;
	atomic_t cur;
	atomic_t peak;
	bool block;
};

struct dt_dev {
	struct dt_dom *dom;
	struct dpll_device *dpll;
	dpll_tracker tracker;
	struct dt_dev *notify;
	enum dpll_mode mode;
	bool block;
};

struct dt_pin {
	struct dpll_pin *pin;
	dpll_tracker tracker;
};

struct dt_reg {
	struct dt_dom *dom;
	struct dt_pin *pin;
	struct dt_dev *dev;
	struct dt_pin *parent;
	struct dt_pin *notify;
	struct dpll_domain *forbid;
	enum dpll_pin_state state;
	enum dpll_pin_direction direction;
	atomic_t freq_gets;
	u32 prio;
	bool block_prio_set;
};

struct dt_ctx {
	struct kunit *test;
	struct dt_dom dom_a, dom_b, dom_e, dom_f, dom_g;
	struct dt_dev a0, a1, b0, e0, f0;
	struct dt_pin p, s, bp, x, y, m, c, extra;
	struct dt_reg reg_p_a0, reg_p_a1, reg_s_a0, reg_s_a1, reg_bp_b0;
	struct dt_reg reg_x_e0, reg_x_f0, reg_y_e0, reg_y_f0, reg_m_e0, reg_c_m;
	struct dt_reg *atomic_regs[2];
	struct notifier_block nb;
	wait_queue_head_t wq;
	atomic_t cur_total;
	atomic_t peak_total;
	atomic_t wrong_lock;
	atomic_t bad_ntf;
	atomic_t torn;
	struct task_struct *set_task;
	void *events[DT_MAX_EVENTS];
	int num_events;
	bool record;
	bool fail_after_ntf;
	bool debug_locks_at_start;
};

struct dt_req {
	struct dt_ctx *ctx;
	int (*fn)(void *arg);
	void *arg;
	int ret;
	struct completion done;
	bool finished;
};

/* A nested attribute of a pin set request */
struct dt_nest {
	int type;
	u32 parent_id;
	int attr;
	u32 val;
};

/* Allocated with kunit_kzalloc(), a failed assertion must not free them */
struct dt_pin_set_args {
	struct dt_ctx *ctx;
	struct dpll_pin *pin;
	struct dt_nest nests[2];
	int num;
};

struct dt_mode_set_args {
	struct dpll_device *dpll;
	enum dpll_mode mode;
};

/*
 * The lock may be held by another task, so mutex_is_locked() is not enough.
 * Without lockdep no check of a held lock can fail.
 */
static bool dt_held(struct dpll_domain *domain)
{
#ifdef CONFIG_LOCKDEP
	return lockdep_is_held(&domain->lock) != 0;
#else
	return true;
#endif
}

static bool dt_surely_held(struct dpll_domain *domain)
{
#ifdef CONFIG_LOCKDEP
	return lockdep_is_held(&domain->lock) > 0;
#else
	return false;
#endif
}

static void dt_peak(atomic_t *peak, int val)
{
	int old = atomic_read(peak);

	while (val > old && !atomic_try_cmpxchg(peak, &old, val))
		;
}

static void dt_block(struct dt_dom *dom, bool *block)
{
	complete(&dom->entered);
	wait_event(dom->ctx->wq, !READ_ONCE(*block));
}

static void dt_enter(struct dt_dom *dom)
{
	struct dt_ctx *ctx = dom->ctx;

	dt_peak(&ctx->peak_total, atomic_inc_return(&ctx->cur_total));
	dt_peak(&dom->peak, atomic_inc_return(&dom->cur));

	if (READ_ONCE(dom->block))
		dt_block(dom, &dom->block);
}

static void dt_leave(struct dt_dom *dom)
{
	atomic_dec(&dom->cur);
	atomic_dec(&dom->ctx->cur_total);
}

static void dt_dev_enter(struct dt_dev *dev, const struct dpll_device *dpll)
{
	if (!dt_held(dpll->domain))
		atomic_inc(&dev->dom->ctx->wrong_lock);
	dt_enter(dev->dom);
	if (READ_ONCE(dev->block))
		dt_block(dev->dom, &dev->block);
}

/* A callback of a pin runs under the domain of the pin alone */
static void dt_pin_enter(struct dt_reg *reg, const struct dpll_pin *pin)
{
	struct dt_ctx *ctx = reg->dom->ctx;

	if (!dt_held(pin->domain) ||
	    (reg->forbid && dt_surely_held(reg->forbid)))
		atomic_inc(&ctx->wrong_lock);
	dt_enter(reg->dom);
}

static int dt_mode_get(const struct dpll_device *dpll, void *priv,
		       enum dpll_mode *mode, struct netlink_ext_ack *extack)
{
	struct dt_dev *dev = priv;

	dt_dev_enter(dev, dpll);
	*mode = dev->mode;
	dt_leave(dev->dom);

	return 0;
}

static int dt_mode_set(const struct dpll_device *dpll, void *priv,
		       enum dpll_mode mode, struct netlink_ext_ack *extack)
{
	struct dt_dev *dev = priv;

	dt_dev_enter(dev, dpll);
	dev->mode = mode;
	dt_leave(dev->dom);

	if (dev->notify)
		__dpll_device_change_ntf(dev->notify->dpll);

	return 0;
}

static int dt_supported_modes_get(const struct dpll_device *dpll, void *priv,
				  unsigned long *modes,
				  struct netlink_ext_ack *extack)
{
	struct dt_dev *dev = priv;

	dt_dev_enter(dev, dpll);
	__set_bit(DPLL_MODE_AUTOMATIC, modes);
	__set_bit(DPLL_MODE_MANUAL, modes);
	dt_leave(dev->dom);

	return 0;
}

static int dt_lock_status_get(const struct dpll_device *dpll, void *priv,
			      enum dpll_lock_status *status,
			      enum dpll_lock_status_error *status_error,
			      struct netlink_ext_ack *extack)
{
	struct dt_dev *dev = priv;

	dt_dev_enter(dev, dpll);
	*status = DPLL_LOCK_STATUS_LOCKED;
	dt_leave(dev->dom);

	return 0;
}

static const struct dpll_device_ops dt_dev_ops = {
	.mode_get = dt_mode_get,
	.mode_set = dt_mode_set,
	.supported_modes_get = dt_supported_modes_get,
	.lock_status_get = dt_lock_status_get,
};

static int dt_frequency_get(const struct dpll_pin *pin, void *pin_priv,
			    const struct dpll_device *dpll, void *dpll_priv,
			    u64 *frequency, struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	atomic_inc(&reg->freq_gets);
	*frequency = 1;
	dt_leave(reg->dom);

	return 0;
}

static int dt_state_on_dpll_get(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_device *dpll, void *dpll_priv,
				enum dpll_pin_state *state,
				struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	*state = reg->state;
	dt_leave(reg->dom);

	return 0;
}

static int dt_state_on_dpll_set(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_device *dpll, void *dpll_priv,
				const enum dpll_pin_state state,
				struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	reg->state = state;
	dt_leave(reg->dom);

	if (reg->notify)
		__dpll_pin_change_ntf(reg->notify->pin);

	return reg->dom->ctx->fail_after_ntf ? -EIO : 0;
}

static int dt_state_on_pin_get(const struct dpll_pin *pin, void *pin_priv,
			       const struct dpll_pin *parent_pin,
			       void *parent_pin_priv,
			       enum dpll_pin_state *state,
			       struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	*state = reg->state;
	dt_leave(reg->dom);

	return 0;
}

static int dt_state_on_pin_set(const struct dpll_pin *pin, void *pin_priv,
			       const struct dpll_pin *parent_pin,
			       void *parent_pin_priv,
			       const enum dpll_pin_state state,
			       struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	reg->state = state;
	dt_leave(reg->dom);

	return 0;
}

static int dt_prio_get(const struct dpll_pin *pin, void *pin_priv,
		       const struct dpll_device *dpll, void *dpll_priv,
		       u32 *prio, struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;
	struct dt_ctx *ctx = reg->dom->ctx;

	dt_pin_enter(reg, pin);
	/* Only the request doing the set may see it half done */
	if (READ_ONCE(ctx->set_task) && current != ctx->set_task &&
	    ctx->atomic_regs[0]->prio != ctx->atomic_regs[1]->prio)
		atomic_inc(&ctx->torn);
	*prio = reg->prio;
	dt_leave(reg->dom);

	return 0;
}

static int dt_prio_set(const struct dpll_pin *pin, void *pin_priv,
		       const struct dpll_device *dpll, void *dpll_priv,
		       const u32 prio, struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	if (READ_ONCE(reg->block_prio_set))
		dt_block(reg->dom, &reg->block_prio_set);
	reg->prio = prio;
	dt_leave(reg->dom);

	return 0;
}

static int dt_direction_get(const struct dpll_pin *pin, void *pin_priv,
			    const struct dpll_device *dpll, void *dpll_priv,
			    enum dpll_pin_direction *direction,
			    struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	*direction = reg->direction;
	dt_leave(reg->dom);

	return 0;
}

static int dt_direction_set(const struct dpll_pin *pin, void *pin_priv,
			    const struct dpll_device *dpll, void *dpll_priv,
			    const enum dpll_pin_direction direction,
			    struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	reg->direction = direction;
	dt_leave(reg->dom);

	return 0;
}

static int dt_ref_sync_get(const struct dpll_pin *pin, void *pin_priv,
			   const struct dpll_pin *ref_sync_pin,
			   void *ref_sync_pin_priv, enum dpll_pin_state *state,
			   struct netlink_ext_ack *extack)
{
	struct dt_reg *reg = pin_priv;

	dt_pin_enter(reg, pin);
	*state = DPLL_PIN_STATE_DISCONNECTED;
	dt_leave(reg->dom);

	return 0;
}

static const struct dpll_pin_ops dt_pin_ops = {
	.frequency_get = dt_frequency_get,
	.state_on_dpll_get = dt_state_on_dpll_get,
	.state_on_dpll_set = dt_state_on_dpll_set,
	.state_on_pin_get = dt_state_on_pin_get,
	.state_on_pin_set = dt_state_on_pin_set,
	.prio_get = dt_prio_get,
	.prio_set = dt_prio_set,
	.direction_get = dt_direction_get,
	.direction_set = dt_direction_set,
	.ref_sync_get = dt_ref_sync_get,
};

static const struct dpll_pin_properties dt_prop = {
	.type = DPLL_PIN_TYPE_EXT,
	.capabilities = DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE |
			DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE |
			DPLL_PIN_CAPABILITIES_DIRECTION_CAN_CHANGE,
};

static const struct dpll_pin_properties dt_prop_mux = {
	.type = DPLL_PIN_TYPE_MUX,
	.capabilities = DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE,
};

static int dt_notifier(struct notifier_block *nb, unsigned long action,
		       void *data)
{
	struct dt_ctx *ctx = container_of(nb, struct dt_ctx, nb);
	struct dpll_device_notifier_info *dev_info = data;
	struct dpll_pin_notifier_info *pin_info = data;
	struct dpll_domain *domain;
	void *obj;

	if (!ctx->record)
		return NOTIFY_DONE;

	switch (action) {
	case DPLL_DEVICE_CHANGED:
		obj = dev_info->dpll;
		domain = dev_info->dpll->domain;
		break;
	case DPLL_PIN_CHANGED:
		obj = pin_info->pin;
		domain = pin_info->pin->domain;
		break;
	default:
		return NOTIFY_DONE;
	}

	if (!dt_held(domain))
		atomic_inc(&ctx->bad_ntf);
	if (ctx->num_events < DT_MAX_EVENTS)
		ctx->events[ctx->num_events] = obj;
	ctx->num_events++;

	return NOTIFY_OK;
}

static void dt_final_check(void *data)
{
	struct dt_ctx *ctx = data;

	KUNIT_EXPECT_EQ(ctx->test, atomic_read(&ctx->wrong_lock), 0);
	if (ctx->debug_locks_at_start)
		KUNIT_EXPECT_TRUE(ctx->test, debug_locks);
}

static void dt_dev_release(void *data)
{
	struct dt_dev *dev = data;

	dpll_device_unregister(dev->dpll, &dt_dev_ops, dev);
	dpll_device_put(dev->dpll, &dev->tracker);
}

static void dt_dev_add(struct kunit *test, struct dt_dev *dev,
		       struct dt_dom *dom, u64 clock_id, u32 idx,
		       enum dpll_type type)
{
	int ret;

	dev->dom = dom;
	dev->mode = DPLL_MODE_AUTOMATIC;
	dev->dpll = dpll_device_get(clock_id, idx, THIS_MODULE, &dev->tracker);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev->dpll);

	ret = dpll_device_register(dev->dpll, type, &dt_dev_ops, dev);
	if (ret)
		dpll_device_put(dev->dpll, &dev->tracker);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dt_dev_release,
							dev), 0);
}

static void dt_pin_release(void *data)
{
	struct dt_pin *pin = data;

	if (!IS_ERR_OR_NULL(pin->pin))
		dpll_pin_put(pin->pin, &pin->tracker);
}

static void dt_pin_add(struct kunit *test, struct dt_pin *pin, u64 clock_id,
		       u32 idx, const struct dpll_pin_properties *prop)
{
	pin->pin = dpll_pin_get(clock_id, idx, THIS_MODULE, prop,
				&pin->tracker);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pin->pin);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dt_pin_release,
							pin), 0);
}

static void dt_reg_release(void *data)
{
	struct dt_reg *reg = data;

	if (reg->parent)
		dpll_pin_on_pin_unregister(reg->parent->pin, reg->pin->pin,
					   &dt_pin_ops, reg);
	else
		dpll_pin_unregister(reg->dev->dpll, reg->pin->pin, &dt_pin_ops,
				    reg);
}

static void dt_reg_add(struct kunit *test, struct dt_reg *reg,
		       struct dt_dom *dom, struct dt_pin *pin,
		       struct dt_dev *dev, struct dt_pin *parent)
{
	int ret;

	reg->dom = dom;
	reg->pin = pin;
	reg->dev = dev;
	reg->parent = parent;
	reg->state = DPLL_PIN_STATE_SELECTABLE;
	reg->direction = DPLL_PIN_DIRECTION_INPUT;

	if (parent)
		ret = dpll_pin_on_pin_register(parent->pin, pin->pin,
					       &dt_pin_ops, reg);
	else
		ret = dpll_pin_register(dev->dpll, pin->pin, &dt_pin_ops, reg);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dt_reg_release,
							reg), 0);
}

static void dt_fwnode_release(void *data)
{
	fwnode_remove_software_node(data);
}

/* Creates a pin with a fwnode so that it can be registered with another dpll */
static void dt_fwnode_pin_add(struct kunit *test, struct dt_pin *pin,
			      u64 clock_id, u32 idx)
{
	struct fwnode_handle *fwnode;

	fwnode = fwnode_create_software_node(NULL, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fwnode);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dt_fwnode_release,
							fwnode), 0);
	dt_pin_add(test, pin, clock_id, idx, &dt_prop);
	dpll_pin_fwnode_set(pin->pin, fwnode);
}

/* Builds domains E and F connected by pin X */
static void dt_foreign_pin_add(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;

	dt_dev_add(test, &ctx->e0, &ctx->dom_e, DT_CLOCK_ID_E, 0,
		   DPLL_TYPE_PPS);
	dt_dev_add(test, &ctx->f0, &ctx->dom_f, DT_CLOCK_ID_F, 0,
		   DPLL_TYPE_PPS);
	dt_fwnode_pin_add(test, &ctx->x, DT_CLOCK_ID_E, DT_PIN_IDX_X);
	dt_reg_add(test, &ctx->reg_x_e0, &ctx->dom_e, &ctx->x, &ctx->e0, NULL);
	dt_reg_add(test, &ctx->reg_x_f0, &ctx->dom_f, &ctx->x, &ctx->f0, NULL);

	/* No callback of the pin takes the lock of the other domain */
	ctx->reg_x_e0.forbid = ctx->f0.dpll->domain;
	ctx->reg_x_f0.forbid = ctx->f0.dpll->domain;
}

/* Builds domains E and G connected by pin C under pin M */
static void dt_on_pin_add(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;

	dt_dev_add(test, &ctx->e0, &ctx->dom_e, DT_CLOCK_ID_E, 0,
		   DPLL_TYPE_PPS);
	dt_pin_add(test, &ctx->m, DT_CLOCK_ID_E, DT_PIN_IDX_M, &dt_prop_mux);
	dt_reg_add(test, &ctx->reg_m_e0, &ctx->dom_e, &ctx->m, &ctx->e0, NULL);
	dt_pin_add(test, &ctx->c, DT_CLOCK_ID_G, DT_PIN_IDX_C, &dt_prop);
	dt_reg_add(test, &ctx->reg_c_m, &ctx->dom_g, &ctx->c, NULL, &ctx->m);

	ctx->reg_c_m.forbid = ctx->e0.dpll->domain;
	ctx->reg_m_e0.forbid = ctx->c.pin->domain;
}

static void dt_notifier_release(void *data)
{
	struct dt_ctx *ctx = data;

	unregister_dpll_notifier(&ctx->nb);
}

static void dt_dom_init(struct dt_dom *dom, struct dt_ctx *ctx)
{
	dom->ctx = ctx;
	init_completion(&dom->entered);
}

static int dt_init(struct kunit *test)
{
	struct dt_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;

	ctx->test = test;
	dt_dom_init(&ctx->dom_a, ctx);
	dt_dom_init(&ctx->dom_b, ctx);
	dt_dom_init(&ctx->dom_e, ctx);
	dt_dom_init(&ctx->dom_f, ctx);
	dt_dom_init(&ctx->dom_g, ctx);
	init_waitqueue_head(&ctx->wq);
	ctx->debug_locks_at_start = debug_locks;

	/* Runs last, after the deletion of all objects */
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dt_final_check,
							ctx), 0);

	dt_dev_add(test, &ctx->a0, &ctx->dom_a, DT_CLOCK_ID_A, 0,
		   DPLL_TYPE_PPS);
	dt_dev_add(test, &ctx->a1, &ctx->dom_a, DT_CLOCK_ID_A, 1,
		   DPLL_TYPE_EEC);
	dt_dev_add(test, &ctx->b0, &ctx->dom_b, DT_CLOCK_ID_B, 0,
		   DPLL_TYPE_PPS);
	ctx->a0.notify = &ctx->a1;

	dt_pin_add(test, &ctx->p, DT_CLOCK_ID_A, DT_PIN_IDX_P, &dt_prop);
	dt_reg_add(test, &ctx->reg_p_a0, &ctx->dom_a, &ctx->p, &ctx->a0, NULL);
	dt_reg_add(test, &ctx->reg_p_a1, &ctx->dom_a, &ctx->p, &ctx->a1, NULL);
	ctx->reg_p_a0.notify = &ctx->s;

	dt_pin_add(test, &ctx->s, DT_CLOCK_ID_A, DT_PIN_IDX_S, &dt_prop);
	dt_reg_add(test, &ctx->reg_s_a0, &ctx->dom_a, &ctx->s, &ctx->a0, NULL);
	dt_reg_add(test, &ctx->reg_s_a1, &ctx->dom_a, &ctx->s, &ctx->a1, NULL);

	dt_pin_add(test, &ctx->bp, DT_CLOCK_ID_B, DT_PIN_IDX_BP, &dt_prop);
	dt_reg_add(test, &ctx->reg_bp_b0, &ctx->dom_b, &ctx->bp, &ctx->b0,
		   NULL);

	ctx->nb.notifier_call = dt_notifier;
	KUNIT_ASSERT_EQ(test, register_dpll_notifier(&ctx->nb), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							dt_notifier_release,
							ctx), 0);

	atomic_set(&ctx->peak_total, 0);
	atomic_set(&ctx->dom_a.peak, 0);
	atomic_set(&ctx->dom_b.peak, 0);

	return 0;
}

static int dt_device_ntf(void *arg)
{
	struct dt_dev *dev = arg;

	return dpll_device_change_ntf(dev->dpll);
}

static int dt_pin_ntf(void *arg)
{
	struct dt_pin *pin = arg;

	return dpll_pin_change_ntf(pin->pin);
}

static int dt_extra_pin_get(void *arg)
{
	struct dt_pin *pin = arg;

	pin->pin = dpll_pin_get(DT_CLOCK_ID_B, DT_PIN_IDX_EXTRA, THIS_MODULE,
				&dt_prop, &pin->tracker);

	return PTR_ERR_OR_ZERO(pin->pin);
}

static void dt_unblock(struct dt_ctx *ctx)
{
	WRITE_ONCE(ctx->dom_a.block, false);
	WRITE_ONCE(ctx->dom_b.block, false);
	WRITE_ONCE(ctx->dom_e.block, false);
	WRITE_ONCE(ctx->dom_f.block, false);
	WRITE_ONCE(ctx->dom_g.block, false);
	WRITE_ONCE(ctx->f0.block, false);
	WRITE_ONCE(ctx->reg_p_a0.block_prio_set, false);
	WRITE_ONCE(ctx->reg_x_e0.block_prio_set, false);
	WRITE_ONCE(ctx->reg_x_f0.block_prio_set, false);
	wake_up_all(&ctx->wq);
}

static int dt_req_thread(void *data)
{
	struct dt_req *req = data;

	req->ret = req->fn(req->arg);
	kthread_complete_and_exit(&req->done, 0);
}

/* Runs before the objects used by the request are released */
static void dt_req_release(void *data)
{
	struct dt_req *req = data;

	dt_unblock(req->ctx);
	if (!req->finished)
		wait_for_completion(&req->done);
}

static struct dt_req *dt_req_start(struct kunit *test, int (*fn)(void *arg),
				   void *arg)
{
	struct task_struct *task;
	struct dt_req *req;

	req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);

	req->ctx = test->priv;
	req->fn = fn;
	req->arg = arg;
	init_completion(&req->done);

	task = kthread_run(dt_req_thread, req, "dpll-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, task);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dt_req_release,
							req), 0);

	return req;
}

/* Consumes the completion, so it must not be waited for again */
static bool dt_done(struct dt_req *req, unsigned long timeout)
{
	if (!req->finished && wait_for_completion_timeout(&req->done, timeout))
		req->finished = true;

	return req->finished;
}

/* Blocks a request of @dom in a callback, which holds the domain lock */
static struct dt_req *dt_block_in(struct kunit *test, struct dt_dom *dom,
				  int (*fn)(void *arg), void *arg)
{
	struct dt_req *req;

	WRITE_ONCE(dom->block, true);
	req = dt_req_start(test, fn, arg);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&dom->entered,
							    DT_TIMEOUT));

	return req;
}

/* Build a set request and pass it to the handlers genetlink would call */
static int dt_pin_set_fn(void *arg)
{
	struct dt_pin_set_args *args = arg;
	struct nlattr *attrs[DPLL_A_PIN_MAX + 1] = {};
	struct genl_info info = {};
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	struct nlattr *nest;
	int i, ret;

	if (args->ctx)
		WRITE_ONCE(args->ctx->set_task, current);

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	ret = -EMSGSIZE;
	nlh = nlmsg_put(skb, 0, 0, NLMSG_MIN_TYPE, GENL_HDRLEN, 0);
	if (!nlh || nla_put_u32(skb, DPLL_A_PIN_ID, args->pin->id))
		goto out;
	for (i = 0; i < args->num; i++) {
		const struct dt_nest *n = &args->nests[i];

		nest = nla_nest_start(skb, n->type);
		if (!nest ||
		    nla_put_u32(skb, DPLL_A_PIN_PARENT_ID, n->parent_id) ||
		    nla_put_u32(skb, n->attr, n->val))
			goto out;
		nla_nest_end(skb, nest);
	}
	nlmsg_end(skb, nlh);

	info.genlhdr = nlmsg_data(nlh);
	info.attrs = attrs;
	attrs[DPLL_A_PIN_ID] = nla_find(genlmsg_data(info.genlhdr),
					genlmsg_len(info.genlhdr),
					DPLL_A_PIN_ID);

	ret = dpll_pin_pre_doit(NULL, skb, &info);
	if (!ret) {
		ret = dpll_nl_pin_set_doit(skb, &info);
		dpll_pin_post_doit(NULL, skb, &info);
	}
out:
	nlmsg_free(skb);

	return ret;
}

static int dt_mode_set_fn(void *arg)
{
	struct dt_mode_set_args *args = arg;
	struct nlattr *attrs[DPLL_A_MAX + 1] = {};
	struct genl_info info = {};
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	int ret;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	ret = -EMSGSIZE;
	nlh = nlmsg_put(skb, 0, 0, NLMSG_MIN_TYPE, GENL_HDRLEN, 0);
	if (!nlh || nla_put_u32(skb, DPLL_A_ID, args->dpll->id) ||
	    nla_put_u32(skb, DPLL_A_MODE, args->mode))
		goto out;
	nlmsg_end(skb, nlh);

	info.genlhdr = nlmsg_data(nlh);
	info.attrs = attrs;
	attrs[DPLL_A_ID] = nla_find(genlmsg_data(info.genlhdr),
				    genlmsg_len(info.genlhdr), DPLL_A_ID);

	ret = dpll_pre_doit(NULL, skb, &info);
	if (!ret) {
		ret = dpll_nl_device_set_doit(skb, &info);
		dpll_post_doit(NULL, skb, &info);
	}
out:
	nlmsg_free(skb);

	return ret;
}

/* Runs a request that should not block in a kthread and returns its result */
static int dt_run(struct kunit *test, int (*fn)(void *arg), void *arg)
{
	struct dt_req *req = dt_req_start(test, fn, arg);

	KUNIT_ASSERT_TRUE(test, dt_done(req, DT_TIMEOUT));

	return req->ret;
}

static struct dt_pin_set_args *dt_pin_set_args_alloc(struct kunit *test,
						     struct dpll_pin *pin,
						     int num)
{
	struct dt_pin_set_args *args;

	args = kunit_kzalloc(test, sizeof(*args), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, args);
	args->pin = pin;
	args->num = num;

	return args;
}

static struct dt_pin_set_args *dt_pin_set_args_one(struct kunit *test,
						   struct dpll_pin *pin,
						   int type, u32 parent_id,
						   int attr, u32 val)
{
	struct dt_pin_set_args *args = dt_pin_set_args_alloc(test, pin, 1);

	args->nests[0].type = type;
	args->nests[0].parent_id = parent_id;
	args->nests[0].attr = attr;
	args->nests[0].val = val;

	return args;
}

static int dt_pin_set_one(struct kunit *test, struct dpll_pin *pin, int type,
			  u32 parent_id, int attr, u32 val)
{
	return dt_run(test, dt_pin_set_fn,
		      dt_pin_set_args_one(test, pin, type, parent_id, attr,
					  val));
}

static void dt_expect_events(struct kunit *test, void * const *expected,
			     int num)
{
	struct dt_ctx *ctx = test->priv;
	int i;

	KUNIT_ASSERT_EQ(test, ctx->num_events, num);
	for (i = 0; i < num; i++)
		KUNIT_EXPECT_PTR_EQ(test, ctx->events[i], expected[i]);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->bad_ntf), 0);
}

/* Objects with the same (module, clock_id) share a refcounted domain */
static void dt_test_domain_sharing(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dpll_domain *a = ctx->a0.dpll->domain;
	struct dpll_domain *b = ctx->b0.dpll->domain;

	KUNIT_EXPECT_PTR_EQ(test, ctx->a1.dpll->domain, a);
	KUNIT_EXPECT_PTR_EQ(test, ctx->p.pin->domain, a);
	KUNIT_EXPECT_PTR_EQ(test, ctx->bp.pin->domain, b);
	KUNIT_EXPECT_PTR_NE(test, a, b);

	KUNIT_EXPECT_EQ(test, refcount_read(&a->refcount), 4);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refcount), 2);

	KUNIT_ASSERT_EQ(test, dt_extra_pin_get(&ctx->extra), 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->extra.pin->domain, b);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refcount), 3);
	dpll_pin_put(ctx->extra.pin, &ctx->extra.tracker);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refcount), 2);
}

/* The same clock_id used by another module means another domain */
static void dt_test_domain_per_module(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	static char other_module;
	struct dpll_device *dpll;
	dpll_tracker tracker;

	/* Never registered, so the core does not dereference the module */
	dpll = dpll_device_get(DT_CLOCK_ID_A, 0, (struct module *)&other_module,
			       &tracker);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dpll);
	KUNIT_EXPECT_PTR_NE(test, dpll, ctx->a0.dpll);
	KUNIT_EXPECT_PTR_NE(test, dpll->domain, ctx->a0.dpll->domain);
	dpll_device_put(dpll, &tracker);
}

/* A pin on a dpll of another domain stays in its domain */
static void dt_test_foreign_registration_domain(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dpll_domain *e, *f;

	dt_foreign_pin_add(test);
	e = ctx->e0.dpll->domain;
	f = ctx->f0.dpll->domain;

	KUNIT_EXPECT_PTR_NE(test, e, f);
	KUNIT_EXPECT_PTR_EQ(test, ctx->x.pin->domain, e);
	KUNIT_EXPECT_EQ(test, refcount_read(&e->refcount), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->refcount), 1);
}

/* A pin under a parent pin of another domain stays in its domain */
static void dt_test_on_pin_registration_domain(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dpll_domain *e, *g;

	dt_on_pin_add(test);
	e = ctx->e0.dpll->domain;
	g = ctx->c.pin->domain;

	KUNIT_EXPECT_PTR_NE(test, e, g);
	KUNIT_EXPECT_PTR_EQ(test, ctx->m.pin->domain, e);
	KUNIT_EXPECT_EQ(test, refcount_read(&e->refcount), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&g->refcount), 1);
}

/* A callback blocked in one domain does not stop other domains */
static void dt_test_domains_run_in_parallel(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dt_req *blocked, *other;

	blocked = dt_block_in(test, &ctx->dom_a, dt_device_ntf, &ctx->a0);

	other = dt_req_start(test, dt_device_ntf, &ctx->b0);
	KUNIT_EXPECT_TRUE(test, dt_done(other, DT_TIMEOUT));
	KUNIT_EXPECT_FALSE(test, completion_done(&blocked->done));
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->peak_total), 2);

	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(blocked, DT_TIMEOUT));
}

/* Callbacks of one domain are serialized, across its devices too */
static void dt_test_domain_is_serialized(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dt_req *first, *second;

	first = dt_block_in(test, &ctx->dom_a, dt_device_ntf, &ctx->a0);

	second = dt_req_start(test, dt_device_ntf, &ctx->a1);
	KUNIT_EXPECT_FALSE(test, dt_done(second, DT_SHORT_TIMEOUT));

	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(first, DT_TIMEOUT));
	KUNIT_EXPECT_TRUE(test, dt_done(second, DT_TIMEOUT));
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->dom_a.peak), 1);
}

/* Callbacks of all refs of a pin run under the domain of the pin alone */
static void dt_test_foreign_get_locks(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;

	if (!IS_ENABLED(CONFIG_LOCKDEP) || !debug_locks)
		kunit_skip(test, "needs lockdep");

	dt_foreign_pin_add(test);

	KUNIT_EXPECT_EQ(test, dt_run(test, dt_pin_ntf, &ctx->x), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->wrong_lock), 0);
}

/* Setters of a foreign ref run under the domain of the pin alone */
static void dt_test_foreign_set_locks(struct kunit *test)
{
	static const struct {
		int attr;
		u32 val;
	} sets[] = {
		{ DPLL_A_PIN_STATE, DPLL_PIN_STATE_CONNECTED },
		{ DPLL_A_PIN_PRIO, 7 },
		{ DPLL_A_PIN_DIRECTION, DPLL_PIN_DIRECTION_OUTPUT },
	};
	struct dt_ctx *ctx = test->priv;
	int i;

	dt_foreign_pin_add(test);

	for (i = 0; i < ARRAY_SIZE(sets); i++) {
		int ret;

		ctx->num_events = 0;
		ctx->record = true;
		ret = dt_pin_set_one(test, ctx->x.pin, DPLL_A_PIN_PARENT_DEVICE,
				     ctx->f0.dpll->id, sets[i].attr,
				     sets[i].val);
		ctx->record = false;

		KUNIT_EXPECT_EQ(test, ret, 0);
		dt_expect_events(test, (void *[]){ ctx->x.pin }, 1);
	}
	KUNIT_EXPECT_EQ(test, ctx->reg_x_f0.state, DPLL_PIN_STATE_CONNECTED);
	KUNIT_EXPECT_EQ(test, ctx->reg_x_f0.prio, 7);
	KUNIT_EXPECT_EQ(test, ctx->reg_x_f0.direction,
			DPLL_PIN_DIRECTION_OUTPUT);
	KUNIT_EXPECT_EQ(test, ctx->reg_x_e0.prio, 0);
}

/* The callback of a foreign ref can notify objects of the domain of the pin */
static void dt_test_foreign_callback_notifies_pin(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	int ret;

	dt_foreign_pin_add(test);
	ctx->reg_x_f0.notify = &ctx->x;

	ctx->record = true;
	ret = dt_pin_set_one(test, ctx->x.pin, DPLL_A_PIN_PARENT_DEVICE,
			     ctx->f0.dpll->id, DPLL_A_PIN_STATE,
			     DPLL_PIN_STATE_CONNECTED);
	ctx->record = false;

	KUNIT_EXPECT_EQ(test, ret, 0);
	dt_expect_events(test, (void *[]){ ctx->x.pin, ctx->x.pin }, 2);
}

/* Without an owner the pin-level attributes come from the foreign ref */
static void dt_test_pin_without_owner(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;

	dt_dev_add(test, &ctx->f0, &ctx->dom_f, DT_CLOCK_ID_F, 0,
		   DPLL_TYPE_PPS);
	dt_fwnode_pin_add(test, &ctx->x, DT_CLOCK_ID_E, DT_PIN_IDX_X);
	dt_reg_add(test, &ctx->reg_x_f0, &ctx->dom_f, &ctx->x, &ctx->f0, NULL);
	KUNIT_ASSERT_PTR_NE(test, ctx->x.pin->domain, ctx->f0.dpll->domain);
	ctx->reg_x_f0.forbid = ctx->f0.dpll->domain;

	atomic_set(&ctx->reg_x_f0.freq_gets, 0);
	KUNIT_EXPECT_EQ(test, dt_run(test, dt_pin_ntf, &ctx->x), 0);
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->reg_x_f0.freq_gets), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->wrong_lock), 0);
}

/* A stuck callback of a foreign ref stops only the domain of the pin */
static void dt_test_stuck_foreign_callback(struct kunit *test)
{
	struct dt_req *blocked, *owner, *foreign, *other;
	struct dt_ctx *ctx = test->priv;

	dt_foreign_pin_add(test);

	WRITE_ONCE(ctx->reg_x_f0.block_prio_set, true);
	blocked = dt_req_start(test, dt_pin_set_fn,
			       dt_pin_set_args_one(test, ctx->x.pin,
						   DPLL_A_PIN_PARENT_DEVICE,
						   ctx->f0.dpll->id,
						   DPLL_A_PIN_PRIO, 7));
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->dom_f.entered,
							    DT_TIMEOUT));

	owner = dt_req_start(test, dt_device_ntf, &ctx->e0);
	KUNIT_EXPECT_FALSE(test, dt_done(owner, DT_SHORT_TIMEOUT));
	foreign = dt_req_start(test, dt_device_ntf, &ctx->f0);
	KUNIT_EXPECT_TRUE(test, dt_done(foreign, DT_TIMEOUT));
	other = dt_req_start(test, dt_device_ntf, &ctx->b0);
	KUNIT_EXPECT_TRUE(test, dt_done(other, DT_TIMEOUT));

	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(blocked, DT_TIMEOUT));
	KUNIT_EXPECT_TRUE(test, dt_done(owner, DT_TIMEOUT));
}

/* A stuck device callback does not stop a foreign pin registered with it */
static void dt_test_stuck_foreign_device(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dt_req *blocked, *pin;

	dt_foreign_pin_add(test);

	WRITE_ONCE(ctx->f0.block, true);
	blocked = dt_req_start(test, dt_device_ntf, &ctx->f0);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->dom_f.entered,
							    DT_TIMEOUT));

	pin = dt_req_start(test, dt_pin_ntf, &ctx->x);
	KUNIT_EXPECT_TRUE(test, dt_done(pin, DT_TIMEOUT));

	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(blocked, DT_TIMEOUT));
}

/* A stuck owner stops requests for its pins, but not the foreign device */
static void dt_test_stuck_owner_domain(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dt_req *blocked, *foreign, *pin;

	dt_foreign_pin_add(test);

	blocked = dt_block_in(test, &ctx->dom_e, dt_device_ntf, &ctx->e0);

	foreign = dt_req_start(test, dt_device_ntf, &ctx->f0);
	KUNIT_EXPECT_TRUE(test, dt_done(foreign, DT_TIMEOUT));
	pin = dt_req_start(test, dt_pin_ntf, &ctx->x);
	KUNIT_EXPECT_FALSE(test, dt_done(pin, DT_SHORT_TIMEOUT));

	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(blocked, DT_TIMEOUT));
	KUNIT_EXPECT_TRUE(test, dt_done(pin, DT_TIMEOUT));
}

/* A pin and its parent pin of another domain do not block each other */
static void dt_test_on_pin_domains_are_independent(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dt_req *blocked, *other;

	dt_on_pin_add(test);

	blocked = dt_block_in(test, &ctx->dom_e, dt_device_ntf, &ctx->e0);
	other = dt_req_start(test, dt_pin_ntf, &ctx->c);
	KUNIT_EXPECT_TRUE(test, dt_done(other, DT_TIMEOUT));
	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(blocked, DT_TIMEOUT));

	blocked = dt_block_in(test, &ctx->dom_g, dt_pin_ntf, &ctx->c);
	other = dt_req_start(test, dt_pin_ntf, &ctx->m);
	KUNIT_EXPECT_TRUE(test, dt_done(other, DT_TIMEOUT));
	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(blocked, DT_TIMEOUT));
}

/* Registrations in both directions between two domains are fine */
static void dt_test_registrations_both_directions(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;

	dt_foreign_pin_add(test);
	dt_fwnode_pin_add(test, &ctx->y, DT_CLOCK_ID_F, DT_PIN_IDX_Y);
	dt_reg_add(test, &ctx->reg_y_f0, &ctx->dom_f, &ctx->y, &ctx->f0, NULL);
	dt_reg_add(test, &ctx->reg_y_e0, &ctx->dom_e, &ctx->y, &ctx->e0, NULL);
	ctx->reg_y_f0.forbid = ctx->e0.dpll->domain;
	ctx->reg_y_e0.forbid = ctx->e0.dpll->domain;

	KUNIT_EXPECT_EQ(test, dt_run(test, dt_pin_ntf, &ctx->x), 0);
	KUNIT_EXPECT_EQ(test, dt_run(test, dt_pin_ntf, &ctx->y), 0);
}

/* A topology change waits until running requests finish */
static void dt_test_topology_change_waits(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dt_req *blocked, *writer;

	blocked = dt_block_in(test, &ctx->dom_a, dt_device_ntf, &ctx->a0);

	/* Added before the request starts, so it runs after waiting for it */
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dt_pin_release,
							&ctx->extra), 0);
	writer = dt_req_start(test, dt_extra_pin_get, &ctx->extra);
	KUNIT_EXPECT_FALSE(test, dt_done(writer, DT_SHORT_TIMEOUT));

	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(blocked, DT_TIMEOUT));
	KUNIT_EXPECT_TRUE(test, dt_done(writer, DT_TIMEOUT));
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, ctx->extra.pin);
}

/* Other requests about a pin cannot see a set of two attributes half done */
static void dt_check_request_atomic(struct kunit *test, struct dt_pin *pin,
				    struct dt_dev *dev0, struct dt_reg *reg0,
				    struct dt_dev *dev1, struct dt_reg *reg1)
{
	struct dt_pin_set_args *args = dt_pin_set_args_alloc(test, pin->pin, 2);
	struct dt_ctx *ctx = test->priv;
	struct dt_req *set, *other;
	int i;

	args->ctx = ctx;
	for (i = 0; i < args->num; i++) {
		args->nests[i].type = DPLL_A_PIN_PARENT_DEVICE;
		args->nests[i].parent_id = i ? dev1->dpll->id : dev0->dpll->id;
		args->nests[i].attr = DPLL_A_PIN_PRIO;
		args->nests[i].val = 11;
	}

	ctx->atomic_regs[0] = reg0;
	ctx->atomic_regs[1] = reg1;

	/* The other request starts to wait before the first attribute is set */
	WRITE_ONCE(reg0->block_prio_set, true);
	set = dt_req_start(test, dt_pin_set_fn, args);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&reg0->dom->entered,
							    DT_TIMEOUT));
	other = dt_req_start(test, dt_pin_ntf, pin);
	KUNIT_EXPECT_FALSE(test, dt_done(other, DT_SHORT_TIMEOUT));

	dt_unblock(ctx);
	KUNIT_EXPECT_TRUE(test, dt_done(set, DT_TIMEOUT));
	KUNIT_EXPECT_TRUE(test, dt_done(other, DT_TIMEOUT));
	KUNIT_EXPECT_EQ(test, set->ret, 0);
	KUNIT_EXPECT_EQ(test, reg0->prio, 11);
	KUNIT_EXPECT_EQ(test, reg1->prio, 11);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->torn), 0);
}

static void dt_test_request_is_atomic(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;

	dt_check_request_atomic(test, &ctx->p, &ctx->a0, &ctx->reg_p_a0,
				&ctx->a1, &ctx->reg_p_a1);
}

static void dt_test_spanning_request_is_atomic(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;

	dt_foreign_pin_add(test);
	dt_check_request_atomic(test, &ctx->x, &ctx->e0, &ctx->reg_x_e0,
				&ctx->f0, &ctx->reg_x_f0);
}

/* Notifications from a callback come first, under the domain lock */
static void dt_test_ntf_order(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	int ret;

	ctx->record = true;
	ret = dt_pin_set_one(test, ctx->p.pin, DPLL_A_PIN_PARENT_DEVICE,
			     ctx->a0.dpll->id, DPLL_A_PIN_STATE,
			     DPLL_PIN_STATE_CONNECTED);
	ctx->record = false;

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->reg_p_a0.state, DPLL_PIN_STATE_CONNECTED);
	dt_expect_events(test, (void *[]){ ctx->s.pin, ctx->p.pin }, 2);
}

/* A notification from a callback that fails is still sent */
static void dt_test_ntf_on_failure(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	int ret;

	ctx->fail_after_ntf = true;
	ctx->record = true;
	ret = dt_pin_set_one(test, ctx->p.pin, DPLL_A_PIN_PARENT_DEVICE,
			     ctx->a0.dpll->id, DPLL_A_PIN_STATE,
			     DPLL_PIN_STATE_CONNECTED);
	ctx->record = false;

	KUNIT_EXPECT_EQ(test, ret, -EIO);
	dt_expect_events(test, (void *[]){ ctx->s.pin }, 1);
}

/* A set on a pin under a parent of another domain needs only its domain */
static void dt_test_set_on_pin(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	int ret;

	dt_on_pin_add(test);

	ctx->record = true;
	ret = dt_pin_set_one(test, ctx->c.pin, DPLL_A_PIN_PARENT_PIN,
			     ctx->m.pin->id, DPLL_A_PIN_STATE,
			     DPLL_PIN_STATE_CONNECTED);
	ctx->record = false;

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->reg_c_m.state, DPLL_PIN_STATE_CONNECTED);
	dt_expect_events(test, (void *[]){ ctx->c.pin }, 1);
}

/* A device callback notifies a sibling device of its domain */
static void dt_test_device_set_ntf(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	struct dt_mode_set_args *args;
	int ret;

	args = kunit_kzalloc(test, sizeof(*args), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, args);
	args->dpll = ctx->a0.dpll;
	args->mode = DPLL_MODE_MANUAL;

	ctx->record = true;
	ret = dt_run(test, dt_mode_set_fn, args);
	ctx->record = false;

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->a0.mode, DPLL_MODE_MANUAL);
	dt_expect_events(test, (void *[]){ ctx->a1.dpll }, 1);
}

/* A change of a reference sync pair notifies under the domain of the pin */
static void dt_test_ref_sync_pair(struct kunit *test)
{
	struct dt_ctx *ctx = test->priv;
	int ret;

	ctx->record = true;
	ret = dpll_pin_ref_sync_pair_add(ctx->p.pin, ctx->s.pin);
	ctx->record = false;

	KUNIT_EXPECT_EQ(test, ret, 0);
	dt_expect_events(test, (void *[]){ ctx->p.pin }, 1);
}

static struct kunit_case dt_cases[] = {
	KUNIT_CASE(dt_test_domain_sharing),
	KUNIT_CASE(dt_test_domain_per_module),
	KUNIT_CASE(dt_test_foreign_registration_domain),
	KUNIT_CASE(dt_test_on_pin_registration_domain),
	KUNIT_CASE(dt_test_domains_run_in_parallel),
	KUNIT_CASE(dt_test_domain_is_serialized),
	KUNIT_CASE(dt_test_foreign_get_locks),
	KUNIT_CASE(dt_test_foreign_set_locks),
	KUNIT_CASE(dt_test_foreign_callback_notifies_pin),
	KUNIT_CASE(dt_test_pin_without_owner),
	KUNIT_CASE(dt_test_stuck_foreign_callback),
	KUNIT_CASE(dt_test_stuck_foreign_device),
	KUNIT_CASE(dt_test_stuck_owner_domain),
	KUNIT_CASE(dt_test_on_pin_domains_are_independent),
	KUNIT_CASE(dt_test_registrations_both_directions),
	KUNIT_CASE(dt_test_topology_change_waits),
	KUNIT_CASE(dt_test_request_is_atomic),
	KUNIT_CASE(dt_test_spanning_request_is_atomic),
	KUNIT_CASE(dt_test_ntf_order),
	KUNIT_CASE(dt_test_ntf_on_failure),
	KUNIT_CASE(dt_test_set_on_pin),
	KUNIT_CASE(dt_test_device_set_ntf),
	KUNIT_CASE(dt_test_ref_sync_pair),
	{}
};

static struct kunit_suite dt_suite = {
	.name = "dpll_core",
	.init = dt_init,
	.test_cases = dt_cases,
};
kunit_test_suite(dt_suite);

MODULE_DESCRIPTION("KUnit tests for locking in the DPLL core");
MODULE_LICENSE("GPL");
