// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simulated DPLL device driver
 * Copyright (c) 2025 Red Hat, Inc.
 */

#include <linux/device.h>
#include <linux/dpll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/random.h>
#include <linux/atomic.h>

#include "dpllsim.h"

#define DPLLSIM_NUM_INPUT_PINS	11  /* 10 external + 1 internal oscillator */
#define DPLLSIM_NUM_MUX_PINS	1   /* 1 recovered clock MUX pin */
#define DPLLSIM_NUM_OUTPUT_PINS	20  /* 10 differential pairs (P/N) */
#define DPLLSIM_NUM_PINS	(DPLLSIM_NUM_INPUT_PINS + DPLLSIM_NUM_MUX_PINS + DPLLSIM_NUM_OUTPUT_PINS)

/* Simulation parameters */
#define DPLLSIM_LOCK_TIME_MS		2000	/* Time to acquire lock */
#define DPLLSIM_UPDATE_INTERVAL_MS	500	/* Periodic update interval */
#define DPLLSIM_PHASE_DRIFT_MAX		100	/* Max phase drift per update */
#define DPLLSIM_PHASE_ADJUST_GRAN	100	/* Phase adjust granularity (ps) */
#define DPLLSIM_TEMP_DRIFT_MAX		50	/* Max temp drift (milli-degrees) */
#define DPLLSIM_FFO_THRESHOLD		500	/* FFO threshold for error (ppb) */

/* Failure simulation parameters */
#define DPLLSIM_MEDIA_DOWN_PROB		2400	/* 1 in N chance per update (avg ~20min per pin) */
#define DPLLSIM_MEDIA_RECOVERY_PROB	10	/* 1 in N chance per update (avg ~5s recovery) */
#define DPLLSIM_FFO_DRIFT_PROB		100	/* 1 in N chance per update */
#define DPLLSIM_FFO_DRIFT_MAX		200	/* Max FFO drift per update (ppb) */

/* Holdover simulation parameters (based on ITU-T G.8262 specs) */
#define DPLLSIM_HOLDOVER_PHASE_DRIFT_RATE	50	/* ps per update cycle */
#define DPLLSIM_HOLDOVER_FREQ_DRIFT_RATE	5	/* ppb per update cycle */
#define DPLLSIM_HOLDOVER_SSU_A_TIME_MS		60000	/* Time before degrading from SSU-A (60s) */
#define DPLLSIM_HOLDOVER_SSU_B_TIME_MS		120000	/* Time before degrading from SSU-B (120s) */
#define DPLLSIM_HOLDOVER_EEC1_TIME_MS		180000	/* Time before degrading from EEC1 (180s) */

/* Supported frequency ranges for different pin types */
static struct dpll_pin_frequency dpllsim_freq_1pps[] = {
	DPLL_PIN_FREQUENCY_RANGE(1, 1),
};

static struct dpll_pin_frequency dpllsim_freq_synce[] = {
	DPLL_PIN_FREQUENCY_RANGE(1544000, 1544000),    /* T1 */
	DPLL_PIN_FREQUENCY_RANGE(2048000, 2048000),    /* E1 */
	DPLL_PIN_FREQUENCY_RANGE(10000000, 10000000),  /* 10 MHz */
	DPLL_PIN_FREQUENCY_RANGE(25000000, 25000000),  /* 25 MHz */
};

static struct dpll_pin_frequency dpllsim_freq_ext[] = {
	DPLL_PIN_FREQUENCY_RANGE(1, 1),
	DPLL_PIN_FREQUENCY_RANGE(10000000, 10000000),
	DPLL_PIN_FREQUENCY_RANGE(19440000, 19440000),
	DPLL_PIN_FREQUENCY_RANGE(25000000, 25000000),
	DPLL_PIN_FREQUENCY_RANGE(77760000, 77760000),
	DPLL_PIN_FREQUENCY_RANGE(125000000, 125000000),
	DPLL_PIN_FREQUENCY_RANGE(156250000, 156250000),
};

static struct dpll_pin_frequency dpllsim_freq_int_osc[] = {
	DPLL_PIN_FREQUENCY_RANGE(25000000, 25000000),  /* 25 MHz internal */
};

static struct dpll_pin_frequency dpllsim_freq_rclk[] = {
	DPLL_PIN_FREQUENCY_RANGE(1953125, 1953125),  /* Recovered clock ~1.953 MHz */
};

/* Structures dpllsim_pin and dpllsim_dev are now defined in dpllsim.h */

/**
 * dpllsim_notifier_handler - Handle DPLL subsystem notifications
 * @nb: Notifier block
 * @action: Notification action (DPLL_DEVICE_*, DPLL_PIN_*)
 * @data: Notification data (dpll_device_notifier_info or dpll_pin_notifier_info)
 *
 * This callback is invoked by the DPLL subsystem when device or pin events
 * occur. It is used to test the notifier chain and detect potential deadlocks.
 *
 * The callback intentionally acquires the device mutex to stress-test locking
 * patterns and verify no deadlocks occur between the DPLL subsystem lock
 * and our device lock.
 *
 * Return: NOTIFY_OK always
 *
 * To see the debug prints, enable dynamic debug for this module.
 * For example, by adding `dpllsim_core.dyndbg=+p` to the kernel command line,
 * or by using the `echo "file dpllsim_core.c +p" >
 * /sys/kernel/debug/dynamic_debug/control` command at runtime.
 */
static int dpllsim_notifier_handler(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	struct dpllsim_dev *sim = container_of(nb, struct dpllsim_dev,
					       dpll_notifier);
	const char *action_str;
	struct dpll_pin_notifier_info *pin_info;
	struct dpll_device_notifier_info *dev_info;

	pr_debug("dpllsim: notifier handler called with action %lu\n", action);

	/* Increment notification counter atomically */
	atomic_inc(&sim->notify_count);

	switch (action) {
	case DPLL_DEVICE_CREATED:
		action_str = "DEVICE_CREATED";
		dev_info = data;
		pr_debug("dpllsim: device created: clock_id %llu type %u\n",
			 dev_info->clock_id, dev_info->type);
		break;
	case DPLL_DEVICE_DELETED:
		action_str = "DEVICE_DELETED";
		dev_info = data;
		pr_debug("dpllsim: device deleted: clock_id %llu type %u\n",
			 dev_info->clock_id, dev_info->type);
		break;
	case DPLL_DEVICE_CHANGED:
		action_str = "DEVICE_CHANGED";
		dev_info = data;
		pr_debug("dpllsim: device changed: clock_id %llu type %u\n",
			 dev_info->clock_id, dev_info->type);
		break;
	case DPLL_PIN_CREATED:
		action_str = "PIN_CREATED";
		pin_info = data;
		pr_debug("dpllsim: pin created: id %u, idx %u, clock_id %llu\n",
			 pin_info->id, pin_info->idx, pin_info->clock_id);
		break;
	case DPLL_PIN_DELETED:
		action_str = "PIN_DELETED";
		pin_info = data;
		pr_debug("dpllsim: pin deleted: id %u, idx %u, clock_id %llu\n",
			 pin_info->id, pin_info->idx, pin_info->clock_id);
		break;
	case DPLL_PIN_CHANGED:
		action_str = "PIN_CHANGED";
		pin_info = data;
		pr_debug("dpllsim: pin changed: id %u, idx %u, clock_id %llu\n",
			 pin_info->id, pin_info->idx, pin_info->clock_id);
		break;
	default:
		action_str = "UNKNOWN";
		pr_debug("dpllsim: unknown action\n");
		break;
	}

	/*
	 * Stress test: Try to acquire our device lock from within the
	 * notifier callback. This tests for deadlocks between:
	 * - DPLL subsystem's dpll_lock (held during notification dispatch)
	 * - Our device sim->lock (used in ops callbacks)
	 *
	 * If a deadlock exists, this will hang. The stress test can detect
	 * this by monitoring for timeouts.
	 */
	if (mutex_trylock(&sim->lock)) {
		/* Successfully acquired lock - no contention */
		dev_dbg(sim->dev, "Notifier: %s (count=%d, lock acquired)\n",
			action_str, atomic_read(&sim->notify_count));
		mutex_unlock(&sim->lock);
	} else {
		/* Lock is held elsewhere - this is expected under stress */
		dev_dbg(sim->dev, "Notifier: %s (count=%d, lock busy)\n",
			action_str, atomic_read(&sim->notify_count));
	}

	return NOTIFY_OK;
}

/* Select best reference pin based on priority and state */
static int dpllsim_select_best_pin(struct dpllsim_dev *sim)
{
	int best_pin = -1;
	u32 best_prio = U32_MAX;
	int i;

	for (i = 0; i < DPLLSIM_NUM_INPUT_PINS; i++) {
		struct dpllsim_pin *pin = &sim->pins[i];

		if (pin->state != DPLL_PIN_STATE_CONNECTED)
			continue;

		/* Skip pins with media down */
		if (pin->media_down)
			continue;

		if (pin->prio < best_prio) {
			best_prio = pin->prio;
			best_pin = i;
		}
	}

	return best_pin;
}

/* Periodic update workqueue handler */
static void dpllsim_update_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct dpllsim_dev *sim = container_of(dwork, struct dpllsim_dev,
						update_work);
	enum dpll_lock_status old_lock_status;
	unsigned long elapsed;
	bool notify_dpll = false;
	bool notify_pins[DPLLSIM_NUM_INPUT_PINS] = {};
	int i;

	mutex_lock(&sim->lock);

	/* Select best reference pin */
	i = dpllsim_select_best_pin(sim);
	if (i != sim->active_pin) {
		sim->active_pin = i;
		sim->lock_start_time = jiffies;
		notify_dpll = true;
	}

	old_lock_status = sim->lock_status;

	/* Check for lock errors - media down or FFO too high */
	if (sim->active_pin >= 0) {
		struct dpllsim_pin *active = &sim->pins[sim->active_pin];

		/* Check for media down on active pin */
		if (active->media_down) {
			sim->lock_status = DPLL_LOCK_STATUS_HOLDOVER;
			sim->lock_status_error = DPLL_LOCK_STATUS_ERROR_MEDIA_DOWN;
			sim->lock_start_time = jiffies; /* Restart lock attempt */
		/* Check for FFO too high */
		} else if (abs(active->ffo) > DPLLSIM_FFO_THRESHOLD) {
			sim->lock_status = DPLL_LOCK_STATUS_HOLDOVER;
			sim->lock_status_error =
				DPLL_LOCK_STATUS_ERROR_FRACTIONAL_FREQUENCY_OFFSET_TOO_HIGH;
			sim->lock_start_time = jiffies; /* Restart lock attempt */
		} else {
			sim->lock_status_error = DPLL_LOCK_STATUS_ERROR_NONE;
		}
	}

	if (sim->active_pin < 0) {
		/* No valid reference - go unlocked */
		sim->lock_status = DPLL_LOCK_STATUS_UNLOCKED;
		sim->lock_status_error = DPLL_LOCK_STATUS_ERROR_NONE;
	} else if (sim->lock_status_error == DPLL_LOCK_STATUS_ERROR_NONE) {
		/* Simulate lock acquisition (only if no error) */
		elapsed = jiffies_to_msecs(jiffies - sim->lock_start_time);

		if (elapsed < DPLLSIM_LOCK_TIME_MS / 2) {
			sim->lock_status = DPLL_LOCK_STATUS_HOLDOVER;
		} else if (elapsed < DPLLSIM_LOCK_TIME_MS) {
			sim->lock_status = DPLL_LOCK_STATUS_LOCKED_HO_ACQ;
		} else {
			sim->lock_status = DPLL_LOCK_STATUS_LOCKED;
		}
	}

	if (sim->lock_status != old_lock_status) {
		notify_dpll = true;

		/* Track when entering/exiting holdover for degradation simulation */
		if (old_lock_status != DPLL_LOCK_STATUS_HOLDOVER &&
		    sim->lock_status == DPLL_LOCK_STATUS_HOLDOVER) {
			/* Entering holdover - reset drift accumulators */
			sim->holdover_start_time = jiffies;
			sim->holdover_phase_drift = 0;
			sim->holdover_freq_drift = 0;
			dev_info(sim->dev, "Entering HOLDOVER state\n");
		} else if (old_lock_status == DPLL_LOCK_STATUS_HOLDOVER &&
			   sim->lock_status != DPLL_LOCK_STATUS_HOLDOVER) {
			/* Exiting holdover */
			unsigned long holdover_ms = jiffies_to_msecs(jiffies -
								     sim->holdover_start_time);
			dev_info(sim->dev,
				 "Exiting HOLDOVER after %lu ms (phase drift: %lld ps, freq drift: %lld ppb)\n",
				 holdover_ms, sim->holdover_phase_drift,
				 sim->holdover_freq_drift);
		}
	}

	/* Update clock quality level based on lock status and active pin type */
	if (sim->active_pin >= 0) {
		struct dpllsim_pin *active = &sim->pins[sim->active_pin];
		enum dpll_clock_quality_level old_ql = sim->clock_quality_level;

		/* Set clock quality based on pin type and lock status */
		switch (active->type) {
		case DPLL_PIN_TYPE_GNSS:
			/* GNSS provides PRTC (Primary Reference Time Clock) */
			if (sim->lock_status == DPLL_LOCK_STATUS_LOCKED ||
			    sim->lock_status == DPLL_LOCK_STATUS_LOCKED_HO_ACQ) {
				sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_PRTC;
			} else if (sim->lock_status == DPLL_LOCK_STATUS_HOLDOVER) {
				/* Time-based degradation in holdover:
				 * PRTC -> SSU-A -> SSU-B -> EEC1
				 */
				unsigned long holdover_ms = jiffies_to_msecs(jiffies -
									     sim->holdover_start_time);
				if (holdover_ms < DPLLSIM_HOLDOVER_SSU_A_TIME_MS)
					sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_SSU_A;
				else if (holdover_ms < DPLLSIM_HOLDOVER_SSU_B_TIME_MS)
					sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_SSU_B;
				else
					sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_EEC1;
			}
			break;
		case DPLL_PIN_TYPE_MUX:
			/* MUX pins (SyncE with recovered clock) provide EEC1 quality */
			if (sim->lock_status == DPLL_LOCK_STATUS_LOCKED ||
			    sim->lock_status == DPLL_LOCK_STATUS_LOCKED_HO_ACQ) {
				sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_EEC1;
			} else if (sim->lock_status == DPLL_LOCK_STATUS_HOLDOVER) {
				/* Time-based degradation: EEC1 -> SSU-B -> (unlocked) */
				unsigned long holdover_ms = jiffies_to_msecs(jiffies -
									     sim->holdover_start_time);
				if (holdover_ms < DPLLSIM_HOLDOVER_SSU_B_TIME_MS)
					sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_SSU_B;
				else
					sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_EEC1;
			}
			break;
		case DPLL_PIN_TYPE_EXT:
			/* External reference - assume SSU-A quality */
			if (sim->lock_status == DPLL_LOCK_STATUS_LOCKED ||
			    sim->lock_status == DPLL_LOCK_STATUS_LOCKED_HO_ACQ) {
				sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_SSU_A;
			} else if (sim->lock_status == DPLL_LOCK_STATUS_HOLDOVER) {
				/* Time-based degradation: SSU-A -> SSU-B -> EEC1 */
				unsigned long holdover_ms = jiffies_to_msecs(jiffies -
									     sim->holdover_start_time);
				if (holdover_ms < DPLLSIM_HOLDOVER_SSU_A_TIME_MS)
					sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_SSU_A;
				else if (holdover_ms < DPLLSIM_HOLDOVER_EEC1_TIME_MS)
					sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_SSU_B;
				else
					sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_EEC1;
			}
			break;
		case DPLL_PIN_TYPE_INT_OSCILLATOR:
			/* Internal oscillator - lowest quality, always available */
			sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_EEC1;
			/* Internal oscillator never fails, always locked */
			if (sim->lock_status_error == DPLL_LOCK_STATUS_ERROR_NONE) {
				sim->lock_status = DPLL_LOCK_STATUS_LOCKED;
			}
			break;
		default:
			sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_EEC1;
			break;
		}

		if (sim->clock_quality_level != old_ql)
			notify_dpll = true;
	} else if (sim->lock_status == DPLL_LOCK_STATUS_UNLOCKED) {
		/* No reference - degrade to lowest quality */
		enum dpll_clock_quality_level old_ql = sim->clock_quality_level;

		sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_EEC1;
		if (sim->clock_quality_level != old_ql)
			notify_dpll = true;
	}

	/* Simulate phase offset changes */
	for (i = 0; i < DPLLSIM_NUM_INPUT_PINS; i++) {
		struct dpllsim_pin *pin = &sim->pins[i];
		s64 old_phase = pin->phase_offset;
		s64 new_measurement, drift;
		u32 N = sim->phase_offset_avg_factor;

		/* Only measure phase offset for:
		 * - Active pin (always)
		 * - All pins if phase_offset_monitor is enabled
		 */
		if (i != sim->active_pin &&
		    sim->phase_offset_monitor != DPLL_FEATURE_STATE_ENABLE)
			continue;

		if (sim->lock_status == DPLL_LOCK_STATUS_LOCKED &&
		    i == sim->active_pin) {
			/* Locked - target is phase_adjust with small drift */
			get_random_bytes(&drift, sizeof(drift));
			drift = (drift % (DPLLSIM_PHASE_DRIFT_MAX / 4 + 1));
			new_measurement = pin->phase_adjust + drift;
		} else if (sim->lock_status == DPLL_LOCK_STATUS_HOLDOVER &&
			   i == sim->active_pin) {
			/* Holdover - accumulating phase drift over time */
			s64 holdover_drift = DPLLSIM_HOLDOVER_PHASE_DRIFT_RATE;
			/* Add small random component */
			get_random_bytes(&drift, sizeof(drift));
			drift = (drift % 20) - 10;  /* ±10 ps random */
			holdover_drift += drift;

			sim->holdover_phase_drift += holdover_drift;
			new_measurement = pin->phase_adjust + sim->holdover_phase_drift;
		} else {
			/* Not locked or not active pin - larger drift */
			get_random_bytes(&drift, sizeof(drift));
			drift = (drift % (2 * DPLLSIM_PHASE_DRIFT_MAX + 1)) -
				DPLLSIM_PHASE_DRIFT_MAX;
			new_measurement = pin->phase_offset + drift;
		}

		/* Apply exponential moving average:
		 * curr_avg = prev_avg * (2^N-1)/2^N + new_val * 1/2^N
		 * This smooths out phase offset measurements
		 */
		if (N > 0 && N <= 15) {
			u64 weight_prev = (1ULL << N) - 1;
			u64 weight_new = 1;
			pin->phase_offset = (pin->phase_offset * weight_prev +
					     new_measurement * weight_new) >> N;
		} else {
			pin->phase_offset = new_measurement;
		}

		if (old_phase != pin->phase_offset)
			notify_pins[i] = true;
	}

	/* Dynamic failure simulation for SyncE MUX pins */
	for (i = 0; i < DPLLSIM_NUM_INPUT_PINS; i++) {
		struct dpllsim_pin *pin = &sim->pins[i];

		/* Only simulate failures on SyncE MUX ports (pins 2-5) */
		if (pin->type != DPLL_PIN_TYPE_MUX)
			continue;

		pin->failure_counter++;

		/* Simulate random media down events */
		if (!pin->media_down && pin->failure_counter > 10) {
			if ((get_random_u32() % DPLLSIM_MEDIA_DOWN_PROB) == 0) {
				pin->media_down = true;
				pin->failure_counter = 0;
				dev_info(sim->dev, "SyncE pin %d: simulating media down\n", i);
				notify_pins[i] = true;
			}
		}

		/* Simulate media recovery */
		if (pin->media_down && pin->failure_counter > 5) {
			if ((get_random_u32() % DPLLSIM_MEDIA_RECOVERY_PROB) == 0) {
				pin->media_down = false;
				pin->failure_counter = 0;
				pin->ffo = 0; /* Reset FFO on recovery */
				dev_info(sim->dev, "SyncE pin %d: media recovered\n", i);
				notify_pins[i] = true;
			}
		}
	}

	/* Simulate FFO changes for input pins */
	for (i = 0; i < DPLLSIM_NUM_INPUT_PINS; i++) {
		struct dpllsim_pin *pin = &sim->pins[i];
		s64 drift;

		if (pin->state != DPLL_PIN_STATE_CONNECTED)
			continue;

		/* Skip if media is down */
		if (pin->media_down)
			continue;

		/* In holdover, simulate frequency drift accumulation */
		if (sim->lock_status == DPLL_LOCK_STATUS_HOLDOVER &&
		    i == sim->active_pin) {
			s64 holdover_ffo_drift = DPLLSIM_HOLDOVER_FREQ_DRIFT_RATE;
			/* Add small random component */
			get_random_bytes(&drift, sizeof(drift));
			drift = (drift % 3) - 1;  /* ±1 ppb random */
			holdover_ffo_drift += drift;

			sim->holdover_freq_drift += holdover_ffo_drift;
			pin->ffo = sim->holdover_freq_drift;

			/* Log significant drift milestones */
			if (abs(sim->holdover_freq_drift) % 100 == 0 &&
			    abs(sim->holdover_freq_drift) > 0) {
				unsigned long elapsed_ms = jiffies_to_msecs(jiffies -
									     sim->holdover_start_time);
				dev_info(sim->dev,
					 "Holdover frequency drift: %lld ppb (elapsed: %lu ms)\n",
					 sim->holdover_freq_drift, elapsed_ms);
			}
		} else {
			/* Normal operation - random FFO drift simulation */
			if ((get_random_u32() % DPLLSIM_FFO_DRIFT_PROB) == 0) {
				/* Occasional large FFO drift */
				get_random_bytes(&drift, sizeof(drift));
				drift = (drift % (2 * DPLLSIM_FFO_DRIFT_MAX + 1)) -
					DPLLSIM_FFO_DRIFT_MAX;
				pin->ffo += drift;
				dev_info(sim->dev, "Pin %d: FFO drift +%lld ppb (total %lld ppb)\n",
					 i, drift, pin->ffo);
			} else {
				/* Small random FFO drift */
				get_random_bytes(&drift, sizeof(drift));
				drift = (drift % 21) - 10;  /* -10 to +10 ppb */
				pin->ffo += drift;
			}
		}

		/* Keep FFO within reasonable bounds */
		if (pin->ffo > 1000)
			pin->ffo = 1000;
		else if (pin->ffo < -1000)
			pin->ffo = -1000;
	}

	/* Simulate temperature drift */
	if (get_random_u32() % 10 == 0) {  /* 10% chance per update */
		s32 temp_drift = (get_random_u32() % (2 * DPLLSIM_TEMP_DRIFT_MAX + 1)) -
				 DPLLSIM_TEMP_DRIFT_MAX;
		sim->temp += temp_drift;

		/* Keep temperature within reasonable range (0-50°C) */
		if (sim->temp < 0)
			sim->temp = 0;
		else if (sim->temp > 50000)
			sim->temp = 50000;

		notify_dpll = true;
	}

	/* Simulate ref_sync: synchronize output pins to their reference input pins */
	for (i = DPLLSIM_NUM_INPUT_PINS; i < DPLLSIM_NUM_PINS; i++) {
		struct dpllsim_pin *out_pin = &sim->pins[i];
		struct dpllsim_pin *ref_pin;

		/* Skip if not synced to any reference */
		if (out_pin->ref_sync_pin_idx < 0 ||
		    out_pin->ref_sync_pin_idx >= DPLLSIM_NUM_INPUT_PINS)
			continue;

		ref_pin = &sim->pins[out_pin->ref_sync_pin_idx];

		/* Copy frequency from reference pin */
		if (out_pin->frequency != ref_pin->frequency) {
			out_pin->frequency = ref_pin->frequency;
			notify_pins[i] = true;
		}

		/* Copy phase offset (with small propagation delay) */
		s64 expected_phase = ref_pin->phase_offset + out_pin->phase_adjust + 1000;
		if (out_pin->phase_offset != expected_phase) {
			out_pin->phase_offset = expected_phase;
			notify_pins[i] = true;
		}
	}

	mutex_unlock(&sim->lock);

	/* Send notifications outside of lock */
	if (notify_dpll)
		dpll_device_change_ntf(sim->dpll);

	for (i = 0; i < DPLLSIM_NUM_INPUT_PINS; i++) {
		if (notify_pins[i])
			dpll_pin_change_ntf(sim->pins[i].dpll_pin);
	}

	/* Reschedule */
	schedule_delayed_work(&sim->update_work,
			      msecs_to_jiffies(DPLLSIM_UPDATE_INTERVAL_MS));
}

/* DPLL device ops */
static int dpllsim_mode_get(const struct dpll_device *dpll, void *priv,
			    enum dpll_mode *mode,
			    struct netlink_ext_ack *extack)
{
	struct dpllsim_dev *sim = priv;

	guard(mutex)(&sim->lock);
	*mode = sim->mode;
	return 0;
}

static int dpllsim_lock_status_get(const struct dpll_device *dpll, void *priv,
				   enum dpll_lock_status *status,
				   enum dpll_lock_status_error *status_error,
				   struct netlink_ext_ack *extack)
{
	struct dpllsim_dev *sim = priv;

	guard(mutex)(&sim->lock);
	*status = sim->lock_status;
	*status_error = sim->lock_status_error;
	return 0;
}

static int dpllsim_temp_get(const struct dpll_device *dpll, void *priv,
			    s32 *temp, struct netlink_ext_ack *extack)
{
	struct dpllsim_dev *sim = priv;

	guard(mutex)(&sim->lock);
	*temp = sim->temp;
	return 0;
}

static int dpllsim_phase_offset_monitor_get(const struct dpll_device *dpll,
					     void *priv,
					     enum dpll_feature_state *state,
					     struct netlink_ext_ack *extack)
{
	struct dpllsim_dev *sim = priv;

	guard(mutex)(&sim->lock);
	*state = sim->phase_offset_monitor;
	return 0;
}

static int dpllsim_phase_offset_monitor_set(const struct dpll_device *dpll,
					     void *priv,
					     enum dpll_feature_state state,
					     struct netlink_ext_ack *extack)
{
	struct dpllsim_dev *sim = priv;

	guard(mutex)(&sim->lock);
	sim->phase_offset_monitor = state;

	/* Note: DPLL subsystem automatically sends notifications after
	 * device set operations, so we don't call dpll_device_change_ntf()
	 */
	return 0;
}

static int dpllsim_clock_quality_level_get(const struct dpll_device *dpll,
					    void *priv, unsigned long *qls,
					    struct netlink_ext_ack *extack)
{
	struct dpllsim_dev *sim = priv;

	guard(mutex)(&sim->lock);
	*qls = BIT(sim->clock_quality_level);
	return 0;
}

static int dpllsim_phase_offset_avg_factor_get(const struct dpll_device *dpll,
						void *priv, u32 *factor,
						struct netlink_ext_ack *extack)
{
	struct dpllsim_dev *sim = priv;

	guard(mutex)(&sim->lock);
	*factor = sim->phase_offset_avg_factor;
	return 0;
}

static int dpllsim_phase_offset_avg_factor_set(const struct dpll_device *dpll,
						void *priv, u32 factor,
						struct netlink_ext_ack *extack)
{
	struct dpllsim_dev *sim = priv;

	if (factor > 15) {
		NL_SET_ERR_MSG_FMT(extack,
				   "Phase offset average factor must be in range 0-15");
		return -EINVAL;
	}

	guard(mutex)(&sim->lock);
	sim->phase_offset_avg_factor = factor;

	/* Note: DPLL subsystem automatically sends notifications after
	 * device set operations, so we don't call dpll_device_change_ntf()
	 */
	return 0;
}

static const struct dpll_device_ops dpllsim_dpll_ops = {
	.mode_get = dpllsim_mode_get,
	.lock_status_get = dpllsim_lock_status_get,
	.temp_get = dpllsim_temp_get,
	.phase_offset_monitor_set = dpllsim_phase_offset_monitor_set,
	.phase_offset_monitor_get = dpllsim_phase_offset_monitor_get,
	.clock_quality_level_get = dpllsim_clock_quality_level_get,
	.phase_offset_avg_factor_get = dpllsim_phase_offset_avg_factor_get,
	.phase_offset_avg_factor_set = dpllsim_phase_offset_avg_factor_set,
};

/* Pin ops */
static int dpllsim_pin_frequency_get(const struct dpll_pin *pin, void *pin_priv,
				     const struct dpll_device *dpll,
				     void *dpll_priv, u64 *frequency,
				     struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	*frequency = sim_pin->frequency;
	return 0;
}

static int dpllsim_pin_frequency_set(const struct dpll_pin *pin, void *pin_priv,
				     const struct dpll_device *dpll,
				     void *dpll_priv, u64 frequency,
				     struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	sim_pin->frequency = frequency;
	return 0;
}

static int dpllsim_pin_direction_get(const struct dpll_pin *pin, void *pin_priv,
				     const struct dpll_device *dpll,
				     void *dpll_priv,
				     enum dpll_pin_direction *direction,
				     struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;

	*direction = sim_pin->direction;
	return 0;
}

static int dpllsim_pin_direction_set(const struct dpll_pin *pin, void *pin_priv,
				     const struct dpll_device *dpll,
				     void *dpll_priv,
				     enum dpll_pin_direction direction,
				     struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);

	/* Only pins with DIRECTION_CAN_CHANGE capability can change direction */
	sim_pin->direction = direction;

	/* When changing to input, disconnect from DPLL output
	 * When changing to output, reset state
	 */
	if (direction == DPLL_PIN_DIRECTION_INPUT) {
		sim_pin->state = DPLL_PIN_STATE_DISCONNECTED;
	} else {
		sim_pin->state = DPLL_PIN_STATE_DISCONNECTED;
		sim_pin->ref_sync_pin_idx = -1;  /* Clear any ref_sync */
	}

	return 0;
}

static int dpllsim_pin_state_get(const struct dpll_pin *pin, void *pin_priv,
				 const struct dpll_device *dpll, void *dpll_priv,
				 enum dpll_pin_state *state,
				 struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	*state = sim_pin->state;
	return 0;
}

static int dpllsim_pin_state_set(const struct dpll_pin *pin, void *pin_priv,
				 const struct dpll_device *dpll, void *dpll_priv,
				 enum dpll_pin_state state,
				 struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;
	bool is_input;

	mutex_lock(&sim->lock);
	sim_pin->state = state;
	is_input = sim_pin->idx < DPLLSIM_NUM_INPUT_PINS;
	mutex_unlock(&sim->lock);

	/* Trigger immediate workqueue update for state changes */
	if (is_input)
		mod_delayed_work(system_wq, &sim->update_work, 0);

	return 0;
}

static int dpllsim_pin_prio_get(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_device *dpll, void *dpll_priv,
				u32 *prio, struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	*prio = sim_pin->prio;
	return 0;
}

static int dpllsim_pin_prio_set(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_device *dpll, void *dpll_priv,
				u32 prio, struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;
	bool is_input;

	mutex_lock(&sim->lock);
	sim_pin->prio = prio;
	is_input = sim_pin->idx < DPLLSIM_NUM_INPUT_PINS;
	mutex_unlock(&sim->lock);

	/* Trigger immediate workqueue update for priority changes */
	if (is_input)
		mod_delayed_work(system_wq, &sim->update_work, 0);

	return 0;
}

static int dpllsim_pin_phase_offset_get(const struct dpll_pin *pin,
					void *pin_priv,
					const struct dpll_device *dpll,
					void *dpll_priv, s64 *phase_offset,
					struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	*phase_offset = sim_pin->phase_offset;
	return 0;
}

static int dpllsim_pin_phase_adjust_get(const struct dpll_pin *pin,
					void *pin_priv,
					const struct dpll_device *dpll,
					void *dpll_priv, s32 *phase_adjust,
					struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	*phase_adjust = sim_pin->phase_adjust;
	return 0;
}

static int dpllsim_pin_phase_adjust_set(const struct dpll_pin *pin,
					void *pin_priv,
					const struct dpll_device *dpll,
					void *dpll_priv, const s32 phase_adjust,
					struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	sim_pin->phase_adjust = phase_adjust;
	return 0;
}

static const struct dpll_pin_frequency esync_ranges[] = {
	DPLL_PIN_FREQUENCY_RANGE(1, 10000000),
};

static int dpllsim_pin_esync_get(const struct dpll_pin *pin, void *pin_priv,
				 const struct dpll_device *dpll,
				 void *dpll_priv,
				 struct dpll_pin_esync *esync,
				 struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	esync->freq = sim_pin->esync_frequency;
	esync->range = esync_ranges;
	esync->range_num = ARRAY_SIZE(esync_ranges);
	esync->pulse = 0;
	return 0;
}

static int dpllsim_pin_esync_set(const struct dpll_pin *pin, void *pin_priv,
				 const struct dpll_device *dpll,
				 void *dpll_priv, u64 freq,
				 struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	sim_pin->esync_frequency = freq;
	return 0;
}

static int dpllsim_input_pin_ffo_get(const struct dpll_pin *pin, void *pin_priv,
				     const struct dpll_device *dpll,
				     void *dpll_priv, s64 *ffo,
				     struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);
	*ffo = sim_pin->ffo;
	return 0;
}

/* Input pin ops */
static const struct dpll_pin_ops dpllsim_input_pin_ops = {
	.frequency_get = dpllsim_pin_frequency_get,
	.frequency_set = dpllsim_pin_frequency_set,
	.direction_get = dpllsim_pin_direction_get,
	.state_on_dpll_get = dpllsim_pin_state_get,
	.state_on_dpll_set = dpllsim_pin_state_set,
	.prio_get = dpllsim_pin_prio_get,
	.prio_set = dpllsim_pin_prio_set,
	.phase_offset_get = dpllsim_pin_phase_offset_get,
	.phase_adjust_get = dpllsim_pin_phase_adjust_get,
	.phase_adjust_set = dpllsim_pin_phase_adjust_set,
	.esync_get = dpllsim_pin_esync_get,
	.esync_set = dpllsim_pin_esync_set,
	.ffo_get = dpllsim_input_pin_ffo_get,
};

/* Output pin ops */
static int dpllsim_pin_ref_sync_get(const struct dpll_pin *pin, void *pin_priv,
				    const struct dpll_pin *ref_sync_pin,
				    void *ref_sync_pin_priv,
				    enum dpll_pin_state *state,
				    struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_pin *ref_pin = ref_sync_pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);

	/* Check if this output pin is synced to the reference pin */
	if (sim_pin->ref_sync_pin_idx == ref_pin->idx)
		*state = DPLL_PIN_STATE_CONNECTED;
	else
		*state = DPLL_PIN_STATE_DISCONNECTED;

	return 0;
}

static int dpllsim_pin_ref_sync_set(const struct dpll_pin *pin, void *pin_priv,
				    const struct dpll_pin *ref_sync_pin,
				    void *ref_sync_pin_priv,
				    const enum dpll_pin_state state,
				    struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *sim_pin = pin_priv;
	struct dpllsim_pin *ref_pin = ref_sync_pin_priv;
	struct dpllsim_dev *sim = sim_pin->dev;

	guard(mutex)(&sim->lock);

	if (state == DPLL_PIN_STATE_CONNECTED)
		sim_pin->ref_sync_pin_idx = ref_pin->idx;
	else if (state == DPLL_PIN_STATE_DISCONNECTED)
		sim_pin->ref_sync_pin_idx = -1;
	else
		return -EINVAL;

	return 0;
}

static const struct dpll_pin_ops dpllsim_output_pin_ops = {
	.frequency_get = dpllsim_pin_frequency_get,
	.frequency_set = dpllsim_pin_frequency_set,
	.direction_get = dpllsim_pin_direction_get,
	.state_on_dpll_get = dpllsim_pin_state_get,
	.phase_offset_get = dpllsim_pin_phase_offset_get,
	.phase_adjust_get = dpllsim_pin_phase_adjust_get,
	.phase_adjust_set = dpllsim_pin_phase_adjust_set,
	.esync_get = dpllsim_pin_esync_get,
	.esync_set = dpllsim_pin_esync_set,
	.ref_sync_get = dpllsim_pin_ref_sync_get,
	.ref_sync_set = dpllsim_pin_ref_sync_set,
};

/* Bidirectional pin ops (can be input or output) */
static const struct dpll_pin_ops dpllsim_bidir_pin_ops = {
	.frequency_get = dpllsim_pin_frequency_get,
	.frequency_set = dpllsim_pin_frequency_set,
	.direction_get = dpllsim_pin_direction_get,
	.direction_set = dpllsim_pin_direction_set,
	.state_on_dpll_get = dpllsim_pin_state_get,
	.state_on_dpll_set = dpllsim_pin_state_set,
	.prio_get = dpllsim_pin_prio_get,
	.prio_set = dpllsim_pin_prio_set,
	.phase_offset_get = dpllsim_pin_phase_offset_get,
	.phase_adjust_get = dpllsim_pin_phase_adjust_get,
	.phase_adjust_set = dpllsim_pin_phase_adjust_set,
	.esync_get = dpllsim_pin_esync_get,
	.esync_set = dpllsim_pin_esync_set,
	.ffo_get = dpllsim_input_pin_ffo_get,
	.ref_sync_get = dpllsim_pin_ref_sync_get,
	.ref_sync_set = dpllsim_pin_ref_sync_set,
};

/* MUX pin ops - for recovered clock aggregating multiple parent pins */
static int dpllsim_mux_state_on_pin_get(const struct dpll_pin *pin,
					void *pin_priv,
					const struct dpll_pin *parent_pin,
					void *parent_pin_priv,
					enum dpll_pin_state *state,
					struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *mux_pin = pin_priv;
	struct dpllsim_pin *parent = parent_pin_priv;
	struct dpllsim_dev *sim;

	/* Can be called during cleanup with NULL priv */
	if (!mux_pin || !parent) {
		*state = DPLL_PIN_STATE_DISCONNECTED;
		return 0;
	}

	sim = mux_pin->dev;
	if (!sim) {
		*state = DPLL_PIN_STATE_DISCONNECTED;
		return 0;
	}

	guard(mutex)(&sim->lock);

	/* MUX pin is CONNECTED to the active parent, SELECTABLE for others */
	if (mux_pin->active_parent == parent->idx)
		*state = DPLL_PIN_STATE_CONNECTED;
	else
		*state = DPLL_PIN_STATE_SELECTABLE;

	return 0;
}

static int dpllsim_mux_state_on_pin_set(const struct dpll_pin *pin,
					void *pin_priv,
					const struct dpll_pin *parent_pin,
					void *parent_pin_priv,
					enum dpll_pin_state state,
					struct netlink_ext_ack *extack)
{
	struct dpllsim_pin *mux_pin = pin_priv;
	struct dpllsim_pin *parent = parent_pin_priv;
	struct dpllsim_dev *sim = mux_pin->dev;

	guard(mutex)(&sim->lock);

	/* Switch active parent for MUX pin */
	if (state == DPLL_PIN_STATE_CONNECTED) {
		mux_pin->active_parent = parent->idx;
		/* Copy frequency from newly selected parent */
		mux_pin->frequency = parent->frequency;
	} else if (state == DPLL_PIN_STATE_SELECTABLE) {
		/* Just make it selectable, don't change active parent */
		if (mux_pin->active_parent == parent->idx)
			mux_pin->active_parent = -1;
	}

	return 0;
}

static const struct dpll_pin_ops dpllsim_mux_pin_ops = {
	.frequency_get = dpllsim_pin_frequency_get,
	.direction_get = dpllsim_pin_direction_get,
	.state_on_pin_get = dpllsim_mux_state_on_pin_get,
	.state_on_pin_set = dpllsim_mux_state_on_pin_set,
	.phase_offset_get = dpllsim_pin_phase_offset_get,
	.ffo_get = dpllsim_input_pin_ffo_get,
};

/**
 * dpllsim_get_pins() - Get pin handles for all configured pins
 * @sim: DPLL simulator device
 *
 * Creates pin handles using dpll_pin_get() for all pins that have been
 * added via sysfs. Does NOT register the pins to the DPLL device yet.
 *
 * Return: 0 on success, negative error code on failure
 */
static int dpllsim_get_pins(struct dpllsim_dev *sim)
{
	struct dpll_pin_properties prop = {};
	const struct dpll_pin_ops *ops;
	int i;

	/* No pins to get */
	if (!sim->pins || sim->num_pins == 0)
		return 0;

	/* Get handles for all pins */
	for (i = 0; i < sim->num_pins; i++) {
		struct dpllsim_pin *sim_pin = &sim->pins[i];

		/* Initialize dpll_pin_properties based on pin type */
		memset(&prop, 0, sizeof(prop));
		prop.type = sim_pin->type;
		prop.board_label = sim_pin->board_label;
		prop.panel_label = sim_pin->panel_label[0] ? sim_pin->panel_label : NULL;
		prop.package_label = sim_pin->package_label;
		prop.phase_range.min = S32_MIN;
		prop.phase_range.max = S32_MAX;
		prop.phase_gran = DPLLSIM_DEFAULT_PHASE_ADJUST_GRAN;

		/* Set frequency support based on pin type and frequency */
		switch (sim_pin->type) {
		case DPLL_PIN_TYPE_GNSS:
			prop.freq_supported = dpllsim_freq_1pps;
			prop.freq_supported_num = ARRAY_SIZE(dpllsim_freq_1pps);
			prop.capabilities = DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE;
			ops = &dpllsim_input_pin_ops;
			break;
		case DPLL_PIN_TYPE_INT_OSCILLATOR:
			prop.freq_supported = dpllsim_freq_int_osc;
			prop.freq_supported_num = ARRAY_SIZE(dpllsim_freq_int_osc);
			prop.capabilities = DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE;
			ops = &dpllsim_input_pin_ops;
			break;
		case DPLL_PIN_TYPE_MUX:
			prop.freq_supported = dpllsim_freq_synce;
			prop.freq_supported_num = ARRAY_SIZE(dpllsim_freq_synce);
			prop.capabilities = DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE |
					    DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE;
			ops = &dpllsim_input_pin_ops;
			break;
		case DPLL_PIN_TYPE_SYNCE_ETH_PORT:
			prop.freq_supported = dpllsim_freq_synce;
			prop.freq_supported_num = ARRAY_SIZE(dpllsim_freq_synce);
			prop.capabilities = DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE;
			ops = &dpllsim_output_pin_ops;
			break;
		case DPLL_PIN_TYPE_EXT:
		default:
			/* Determine freq_supported based on frequency */
			if (sim_pin->frequency == 1) {
				prop.freq_supported = dpllsim_freq_1pps;
				prop.freq_supported_num = ARRAY_SIZE(dpllsim_freq_1pps);
			} else if (sim_pin->frequency == 1953125) {
				prop.freq_supported = dpllsim_freq_rclk;
				prop.freq_supported_num = ARRAY_SIZE(dpllsim_freq_rclk);
			} else {
				prop.freq_supported = dpllsim_freq_ext;
				prop.freq_supported_num = ARRAY_SIZE(dpllsim_freq_ext);
			}

			/* Determine ops and capabilities based on direction and parents */
			if (sim_pin->num_parents > 0) {
				/* Pin with parents - uses MUX ops */
				prop.capabilities = DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE;
				ops = &dpllsim_mux_pin_ops;
			} else if (sim_pin->direction == DPLL_PIN_DIRECTION_INPUT) {
				/* Input pin - could be bidirectional */
				prop.capabilities = DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE |
						    DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE;
				/* TODO: detect bidirectional pins */
				ops = &dpllsim_input_pin_ops;
			} else {
				/* Output pin */
				prop.capabilities = DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE;
				ops = &dpllsim_output_pin_ops;
			}
			break;
		}

		/* Get DPLL pin handle */
		sim_pin->dpll_pin = dpll_pin_get(sim->clock_id, i, THIS_MODULE,
						 &prop, NULL, &sim_pin->tracker);
		if (IS_ERR(sim_pin->dpll_pin))
			return PTR_ERR(sim_pin->dpll_pin);
	}

	return 0;
}

/**
 * dpllsim_register_pins() - Register pins to DPLL device
 * @sim: DPLL simulator device
 *
 * Registers all pin handles to the DPLL device and sets up pin hierarchies.
 * This must be called AFTER dpll_device_register() so that pin events can
 * be properly sent.
 *
 * Return: 0 on success, negative error code on failure
 */
static int dpllsim_register_pins(struct dpllsim_dev *sim)
{
	const struct dpll_pin_ops *ops;
	int i, err;

	/* No pins to register */
	if (!sim->pins || sim->num_pins == 0)
		return 0;

	/* Register all regular pins (non-MUX) to DPLL device */
	for (i = 0; i < sim->num_pins; i++) {
		struct dpllsim_pin *sim_pin = &sim->pins[i];

		if (!sim_pin->dpll_pin)
			continue;

		/* Pins with parents are registered via dpll_pin_on_pin_register, skip */
		if (sim_pin->num_parents > 0)
			continue;

		/* Determine ops based on pin properties */
		if (sim_pin->type == DPLL_PIN_TYPE_MUX) {
			ops = &dpllsim_input_pin_ops;
		} else if (sim_pin->direction == DPLL_PIN_DIRECTION_INPUT) {
			ops = &dpllsim_input_pin_ops;
		} else {
			ops = &dpllsim_output_pin_ops;
		}

		/* Register pin to DPLL device */
		err = dpll_pin_register(sim->dpll, sim_pin->dpll_pin,
					ops, sim_pin);
		if (err)
			return err;
	}

	/* Register MUX pins (pins with parents) with their parent pins */
	for (i = 0; i < sim->num_pins; i++) {
		struct dpllsim_pin *mux_pin = &sim->pins[i];
		const struct dpll_pin_ops *mux_ops;
		int j;

		if (mux_pin->num_parents == 0)
			continue;

		/* Determine ops for MUX pin */
		mux_ops = &dpllsim_mux_pin_ops;

		for (j = 0; j < mux_pin->num_parents; j++) {
			int parent_idx = mux_pin->parent_idx[j];
			struct dpllsim_pin *parent_pin;

			if (parent_idx >= sim->num_pins)
				continue;

			parent_pin = &sim->pins[parent_idx];

			/* Parent pin MUST be type MUX for dpll_pin_on_pin_register */
			if (parent_pin->type != DPLL_PIN_TYPE_MUX) {
				dev_err(sim->dev,
					"Parent pin %d must be type MUX, but is type %d\n",
					parent_idx, parent_pin->type);
				return -EINVAL;
			}

			err = dpll_pin_on_pin_register(parent_pin->dpll_pin,
						       mux_pin->dpll_pin,
						       mux_ops,
						       mux_pin);
			if (err)
				return err;
		}
	}

	/* Register reference sync pairs - each output pin can sync to any input pin */
	for (i = 0; i < sim->num_pins; i++) {
		struct dpllsim_pin *out_pin = &sim->pins[i];
		int j;

		if (out_pin->direction != DPLL_PIN_DIRECTION_OUTPUT)
			continue;

		for (j = 0; j < sim->num_pins; j++) {
			struct dpllsim_pin *in_pin = &sim->pins[j];

			if (in_pin->direction != DPLL_PIN_DIRECTION_INPUT)
				continue;

			err = dpll_pin_ref_sync_pair_add(out_pin->dpll_pin,
							  in_pin->dpll_pin);
			if (err)
				return err;
		}
	}

	return 0;
}

/**
 * dpllsim_put_pins() - Release pin handles
 * @sim: DPLL simulator device
 *
 * Releases all pin handles obtained via dpll_pin_get().
 * Used during error cleanup when pins were obtained but not yet registered.
 */
static void dpllsim_put_pins(struct dpllsim_dev *sim)
{
	int i;

	if (!sim->pins || sim->num_pins == 0)
		return;

	for (i = 0; i < sim->num_pins; i++) {
		struct dpllsim_pin *sim_pin = &sim->pins[i];

		if (sim_pin->dpll_pin) {
			dpll_pin_put(sim_pin->dpll_pin, &sim_pin->tracker);
			sim_pin->dpll_pin = NULL;
		}
	}
}

/**
 * dpllsim_unregister_pins() - Unregister pins from DPLL device
 * @sim: DPLL simulator device
 *
 * Unregisters all pins from the DPLL device and releases their handles.
 * This is the cleanup counterpart to dpllsim_register_pins().
 */
static void dpllsim_unregister_pins(struct dpllsim_dev *sim)
{
	const struct dpll_pin_ops *ops;
	int i, j;

	if (!sim->pins || sim->num_pins == 0)
		return;

	/* First unregister MUX pins from their parent pins */
	for (i = 0; i < sim->num_pins; i++) {
		struct dpllsim_pin *mux_pin = &sim->pins[i];

		if (!mux_pin->dpll_pin || mux_pin->num_parents == 0)
			continue;

		for (j = 0; j < mux_pin->num_parents; j++) {
			int parent_idx = mux_pin->parent_idx[j];
			struct dpllsim_pin *parent_pin;

			if (parent_idx >= sim->num_pins)
				continue;

			parent_pin = &sim->pins[parent_idx];

			if (parent_pin->dpll_pin)
				dpll_pin_on_pin_unregister(parent_pin->dpll_pin,
							   mux_pin->dpll_pin,
							   &dpllsim_mux_pin_ops,
							   mux_pin);
		}
	}

	/* Unregister all pins from DPLL device and put them */
	for (i = 0; i < sim->num_pins; i++) {
		struct dpllsim_pin *sim_pin = &sim->pins[i];

		if (!sim_pin->dpll_pin)
			continue;

		/* Determine ops based on pin properties */
		if (sim_pin->num_parents > 0) {
			ops = &dpllsim_mux_pin_ops;
		} else if (sim_pin->direction == DPLL_PIN_DIRECTION_INPUT) {
			/* TODO: detect bidirectional pins */
			ops = &dpllsim_input_pin_ops;
		} else {
			ops = &dpllsim_output_pin_ops;
		}

		/* Pins with parents were not registered to DPLL device */
		if (sim_pin->num_parents == 0) {
			dpll_pin_unregister(sim->dpll, sim_pin->dpll_pin,
					    ops, sim_pin);
		}

		dpll_pin_put(sim_pin->dpll_pin, &sim_pin->tracker);
		sim_pin->dpll_pin = NULL;
	}
}

/* Bus interface functions */

/**
 * dpllsim_dev_create() - Create a new DPLL simulator device
 * @clock_id: Clock ID for the device
 *
 * Creates and initializes a new dpllsim_dev structure with default values.
 * The device is not yet deployed (registered to DPLL subsystem).
 *
 * Return: pointer to dpllsim_dev or ERR_PTR on error
 */
struct dpllsim_dev *dpllsim_dev_create(u64 clock_id)
{
	struct dpllsim_dev *sim;

	sim = kzalloc(sizeof(*sim), GFP_KERNEL);
	if (!sim)
		return ERR_PTR(-ENOMEM);

	mutex_init(&sim->lock);
	sim->clock_id = clock_id;
	sim->mode = DPLL_MODE_AUTOMATIC;
	sim->lock_status = DPLL_LOCK_STATUS_UNLOCKED;
	sim->lock_status_error = DPLL_LOCK_STATUS_ERROR_NONE;
	sim->temp = 25000; /* 25°C */
	sim->phase_offset_monitor = DPLL_FEATURE_STATE_DISABLE;
	sim->phase_offset_avg_factor = 4;
	sim->clock_quality_level = DPLL_CLOCK_QUALITY_LEVEL_ITU_OPT1_PRTC;
	sim->active_pin = -1;
	sim->lock_start_time = jiffies;
	sim->holdover_start_time = 0;
	sim->holdover_phase_drift = 0;
	sim->holdover_freq_drift = 0;

	/* Initialize dynamic parameters with defaults */
	sim->lock_time_ms = DPLLSIM_DEFAULT_LOCK_TIME_MS;
	sim->update_interval_ms = DPLLSIM_DEFAULT_UPDATE_INTERVAL_MS;
	sim->phase_drift_max = DPLLSIM_DEFAULT_PHASE_DRIFT_MAX;
	sim->temp_drift_max = DPLLSIM_DEFAULT_TEMP_DRIFT_MAX;
	sim->ffo_threshold = DPLLSIM_DEFAULT_FFO_THRESHOLD;
	sim->media_down_prob = DPLLSIM_DEFAULT_MEDIA_DOWN_PROB;
	sim->media_recovery_prob = DPLLSIM_DEFAULT_MEDIA_RECOVERY_PROB;
	sim->ffo_drift_prob = DPLLSIM_DEFAULT_FFO_DRIFT_PROB;
	sim->ffo_drift_max = DPLLSIM_DEFAULT_FFO_DRIFT_MAX;

	/* Pins array is NULL until pins are added */
	sim->pins = NULL;
	sim->num_pins = 0;
	sim->num_input_pins = 0;
	sim->num_output_pins = 0;

	/* Initialize workqueue */
	INIT_DELAYED_WORK(&sim->update_work, dpllsim_update_work);

	/* Initialize notifier */
	sim->dpll_notifier.notifier_call = dpllsim_notifier_handler;
	atomic_set(&sim->notify_count, 0);

	return sim;
}

/**
 * dpllsim_dev_destroy() - Destroy a DPLL simulator device
 * @sim: Device to destroy
 *
 * Frees all resources associated with the device.
 * Device must be undeployed before calling this.
 */
void dpllsim_dev_destroy(struct dpllsim_dev *sim)
{
	if (!sim)
		return;

	/* Cancel workqueue */
	cancel_delayed_work_sync(&sim->update_work);

	/* Free pins array if allocated */
	kfree(sim->pins);

	kfree(sim);
}

/**
 * dpllsim_dev_deploy() - Deploy device to DPLL subsystem
 * @sim: Device to deploy
 *
 * Registers the device and all its pins to the DPLL subsystem.
 * After deployment, pins cannot be added or removed.
 *
 * IMPORTANT: The order of operations is critical:
 * 1. Get DPLL device handle
 * 2. Get pin handles but DON'T register them yet
 * 3. Register DPLL device to subsystem
 * 4. Register pins to device
 * 5. Register MUX hierarchies (pin-on-pin)
 *
 * This order is required because dpll_pin_on_pin_register() sends events
 * that require the DPLL device to be already registered in the subsystem.
 *
 * Return: 0 on success, negative error code on failure
 */
int dpllsim_dev_deploy(struct dpllsim_dev *sim)
{
	int err;

	/* Get DPLL device */
	sim->dpll = dpll_device_get(sim->clock_id, 0, THIS_MODULE,
				    &sim->tracker);
	if (IS_ERR(sim->dpll))
		return PTR_ERR(sim->dpll);

	/* Get pin handles (but don't register them yet) */
	err = dpllsim_get_pins(sim);
	if (err)
		goto err_put_dpll;

	/* Register device to subsystem BEFORE registering pins */
	err = dpll_device_register(sim->dpll, DPLL_TYPE_PPS,
				   &dpllsim_dpll_ops, sim);
	if (err)
		goto err_put_pins;

	/* Now register pins to the device */
	err = dpllsim_register_pins(sim);
	if (err)
		goto err_unregister_device;

	/* Register notifier to receive DPLL events */
	err = register_dpll_notifier(&sim->dpll_notifier);
	if (err)
		goto err_unregister_pins;

	/* Start periodic workqueue */
	schedule_delayed_work(&sim->update_work,
			      msecs_to_jiffies(sim->update_interval_ms));

	return 0;

err_unregister_pins:
	dpllsim_unregister_pins(sim);
err_unregister_device:
	dpll_device_unregister(sim->dpll, &dpllsim_dpll_ops, sim);
err_put_pins:
	dpllsim_put_pins(sim);
err_put_dpll:
	dpll_device_put(sim->dpll, &sim->tracker);
	sim->dpll = NULL;
	return err;
}

/**
 * dpllsim_dev_undeploy() - Undeploy device from DPLL subsystem
 * @sim: Device to undeploy
 *
 * Unregisters the device and all its pins from the DPLL subsystem.
 * After undeployment, pins can be added or removed again.
 */
void dpllsim_dev_undeploy(struct dpllsim_dev *sim)
{
	if (!sim || !sim->dpll)
		return;

	/* Stop workqueue */
	cancel_delayed_work_sync(&sim->update_work);

	/* Unregister notifier */
	unregister_dpll_notifier(&sim->dpll_notifier);

	/* Unregister pins and device */
	dpllsim_unregister_pins(sim);
	dpll_device_unregister(sim->dpll, &dpllsim_dpll_ops, sim);
	dpll_device_put(sim->dpll, &sim->tracker);
	sim->dpll = NULL;

	dev_info(sim->dev, "Received %d notifications during operation\n",
		 atomic_read(&sim->notify_count));
}

/**
 * dpllsim_pin_add() - Add a pin to the device
 * @sim: Device to add pin to
 * @type: Pin type (DPLL_PIN_TYPE_*)
 * @frequency: Pin frequency in Hz
 * @direction: Pin direction (DPLL_PIN_DIRECTION_*)
 * @prio: Pin priority (lower is higher priority)
 * @parent_ids: Array of parent pin indices (for MUX pins)
 * @num_parents: Number of parent pins
 *
 * Adds a new pin to the device. Can only be called before deployment.
 *
 * Return: 0 on success, negative error code on failure
 */
int dpllsim_pin_add(struct dpllsim_dev *sim, unsigned int type,
		   u32 frequency, unsigned int direction, u32 prio,
		   int *parent_ids, int num_parents)
{
	struct dpllsim_pin *new_pins;
	struct dpllsim_pin *pin;
	int idx;

	/* Cannot add pins after deployment */
	if (sim->dpll)
		return -EBUSY;

	/* Check parent count limit */
	if (num_parents > 16)
		return -EINVAL;

	/* Reallocate pins array */
	idx = sim->num_pins;
	new_pins = krealloc(sim->pins, (idx + 1) * sizeof(*sim->pins),
			    GFP_KERNEL);
	if (!new_pins)
		return -ENOMEM;

	sim->pins = new_pins;
	pin = &sim->pins[idx];
	memset(pin, 0, sizeof(*pin));

	/* Initialize pin */
	pin->dev = sim;
	pin->idx = idx;
	pin->type = type;
	pin->direction = direction;
	pin->frequency = frequency;
	pin->prio = prio;
	pin->state = DPLL_PIN_STATE_DISCONNECTED;
	pin->phase_offset = 0;
	pin->phase_adjust = 0;
	pin->ffo = 0;
	pin->esync_control = false;
	pin->esync_frequency = 0;
	pin->ref_sync_pin_idx = -1;
	pin->num_parents = num_parents;
	pin->active_parent = -1;
	pin->media_down = false;
	pin->failure_counter = 0;

	/* Copy parent IDs */
	if (num_parents > 0 && parent_ids) {
		int i;

		for (i = 0; i < num_parents; i++)
			pin->parent_idx[i] = parent_ids[i];

		/* Set first parent as active */
		pin->active_parent = parent_ids[0];
	}

	/* Generate labels */
	snprintf(pin->board_label, sizeof(pin->board_label), "PIN%d", idx);
	snprintf(pin->package_label, sizeof(pin->package_label), "PIN%d", idx);

	/* Update counters */
	sim->num_pins++;
	if (direction == DPLL_PIN_DIRECTION_INPUT)
		sim->num_input_pins++;
	else if (direction == DPLL_PIN_DIRECTION_OUTPUT)
		sim->num_output_pins++;

	return 0;
}

/**
 * dpllsim_pin_del() - Delete a pin from the device
 * @sim: Device to delete pin from
 * @pin_id: Index of pin to delete
 *
 * Removes a pin from the device. Can only be called before deployment.
 *
 * Return: 0 on success, negative error code on failure
 */
int dpllsim_pin_del(struct dpllsim_dev *sim, unsigned int pin_id)
{
	struct dpllsim_pin *pin;
	struct dpllsim_pin *new_pins;
	int i;

	/* Cannot delete pins after deployment */
	if (sim->dpll)
		return -EBUSY;

	/* Check pin index */
	if (pin_id >= sim->num_pins)
		return -EINVAL;

	pin = &sim->pins[pin_id];

	/* Update counters */
	if (pin->direction == DPLL_PIN_DIRECTION_INPUT)
		sim->num_input_pins--;
	else if (pin->direction == DPLL_PIN_DIRECTION_OUTPUT)
		sim->num_output_pins--;

	/* Shift remaining pins down */
	for (i = pin_id; i < sim->num_pins - 1; i++) {
		sim->pins[i] = sim->pins[i + 1];
		sim->pins[i].idx = i;
	}

	sim->num_pins--;

	/* Reallocate to smaller size (or free if no pins left) */
	if (sim->num_pins == 0) {
		kfree(sim->pins);
		sim->pins = NULL;
	} else {
		new_pins = krealloc(sim->pins,
				    sim->num_pins * sizeof(*sim->pins),
				    GFP_KERNEL);
		if (new_pins)
			sim->pins = new_pins;
		/* If realloc fails, keep old pointer - not critical */
	}

	return 0;
}

static int __init dpllsim_init(void)
{
	return dpllsim_bus_init();
}

static void __exit dpllsim_exit(void)
{
	dpllsim_bus_exit();
}

module_init(dpllsim_init);
module_exit(dpllsim_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simulated DPLL device driver");
MODULE_AUTHOR("Petr Oros <poros@redhat.com>");
