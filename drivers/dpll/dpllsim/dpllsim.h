/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DPLL simulator - internal header
 * Copyright (c) 2025 Red Hat, Inc.
 */

#ifndef _DPLLSIM_H
#define _DPLLSIM_H

#include <linux/dpll.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/notifier.h>

/* Default simulation parameters */
#define DPLLSIM_DEFAULT_LOCK_TIME_MS		2000
#define DPLLSIM_DEFAULT_UPDATE_INTERVAL_MS	500
#define DPLLSIM_DEFAULT_PHASE_DRIFT_MAX		100
#define DPLLSIM_DEFAULT_PHASE_ADJUST_GRAN	100
#define DPLLSIM_DEFAULT_TEMP_DRIFT_MAX		50
#define DPLLSIM_DEFAULT_FFO_THRESHOLD		500

/* Default failure simulation parameters */
#define DPLLSIM_DEFAULT_MEDIA_DOWN_PROB		2400
#define DPLLSIM_DEFAULT_MEDIA_RECOVERY_PROB	10
#define DPLLSIM_DEFAULT_FFO_DRIFT_PROB		100
#define DPLLSIM_DEFAULT_FFO_DRIFT_MAX		200

/* Holdover simulation parameters */
#define DPLLSIM_HOLDOVER_PHASE_DRIFT_RATE	50
#define DPLLSIM_HOLDOVER_FREQ_DRIFT_RATE	5
#define DPLLSIM_HOLDOVER_SSU_A_TIME_MS		60000
#define DPLLSIM_HOLDOVER_SSU_B_TIME_MS		120000
#define DPLLSIM_HOLDOVER_EEC1_TIME_MS		180000

/* Maximum limits */
#define DPLLSIM_MAX_PINS			128

struct dpllsim_pin {
	struct dpll_pin *dpll_pin;
	dpll_tracker tracker;
	struct dpllsim_dev *dev;

	int idx;
	enum dpll_pin_type type;
	enum dpll_pin_direction direction;
	enum dpll_pin_state state;
	u32 frequency;
	u32 prio;
	s64 phase_offset;
	s32 phase_adjust;
	s64 ffo;  /* Fractional Frequency Offset in ppb */

	/* Pin labels */
	char board_label[32];
	char panel_label[32];
	char package_label[32];

	/* ESync support */
	bool esync_control;
	u32 esync_frequency;
	u8 esync_pulse;

	/* Ref sync support */
	int ref_sync_pin_idx;  /* Index of pin this output syncs to (-1 if none) */

	/* MUX pin support */
	int num_parents;
	int parent_idx[16];
	int active_parent;

	/* Failure simulation */
	bool media_down;
	u32 failure_counter;
};

struct dpllsim_dev {
	struct dpll_device *dpll;
	dpll_tracker tracker;
	struct device *dev;

	u64 clock_id;
	enum dpll_mode mode;
	enum dpll_lock_status lock_status;
	enum dpll_lock_status_error lock_status_error;
	enum dpll_clock_quality_level clock_quality_level;
	s32 temp;
	enum dpll_feature_state phase_offset_monitor;
	u32 phase_offset_avg_factor;

	/* Dynamic parameters (was #define) */
	u32 lock_time_ms;
	u32 update_interval_ms;
	u32 phase_drift_max;
	u32 temp_drift_max;
	u32 ffo_threshold;
	u32 media_down_prob;
	u32 media_recovery_prob;
	u32 ffo_drift_prob;
	u32 ffo_drift_max;

	/* Pins - dynamically allocated */
	struct dpllsim_pin *pins;
	int num_pins;
	int num_input_pins;
	int num_output_pins;

	/* Active pin selection */
	int active_pin;
	unsigned long lock_start_time;

	/* Holdover tracking */
	unsigned long holdover_start_time;
	s64 holdover_phase_drift;
	s64 holdover_freq_drift;

	/* Workqueue for simulation */
	struct delayed_work update_work;
	struct mutex lock;

	/* Notifier for DPLL events */
	struct notifier_block dpll_notifier;
	atomic_t notify_count;
};

/* dpllsim_bus.c */
int dpllsim_bus_init(void);
void dpllsim_bus_exit(void);

/* dpllsim.c */
struct dpllsim_dev *dpllsim_dev_create(u64 clock_id);
void dpllsim_dev_destroy(struct dpllsim_dev *sim);
int dpllsim_dev_deploy(struct dpllsim_dev *sim);
void dpllsim_dev_undeploy(struct dpllsim_dev *sim);
int dpllsim_pin_add(struct dpllsim_dev *sim, unsigned int type,
		   u32 frequency, unsigned int direction, u32 prio,
		   int *parent_ids, int num_parents);
int dpllsim_pin_del(struct dpllsim_dev *sim, unsigned int pin_id);

#endif /* _DPLLSIM_H */
