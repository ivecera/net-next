#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# dpll_lib.sh - DPLL subsystem selftest library
#
# This library provides helper functions for testing DPLL devices and pins
# using the dpll netlink interface via iproute2's dpll utility.
#
# Copyright (c) 2025 Red Hat, Inc.

##############################################################################
# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

##############################################################################
# DPLL device type detection (hardware vs dpllsim)

# DPLL_DEV_ID can be set by user to force specific device ID

# DPLL_MODULE_NAME can be set to select device by module name
# If not set, auto-detect: prefer hardware, fall back to dpllsim
if [[ ! -v DPLL_DEV_ID ]]; then
	# Try to find any DPLL device - use id as identifier
	DPLL_DEV_ID=$(dpll device show -j 2>/dev/null | jq -r '.device[0].id // empty' 2>/dev/null)

	if [ -z "$DPLL_DEV_ID" ]; then
		# No DPLL device found - will try dpllsim later
		DPLL_USE_SIM=1
		DPLL_MODULE_NAME=""
	else
		# Found a device, get its module name
		DPLL_MODULE_NAME=$(dpll device show id $DPLL_DEV_ID -j 2>/dev/null | jq -r '.["module-name"] // empty' 2>/dev/null)

		# Check if it's dpllsim
		if [ "$DPLL_MODULE_NAME" == "dpllsim" ]; then
			DPLL_USE_SIM=1
		else
			DPLL_USE_SIM=0
		fi
	fi
elif [[ ! -z "$DPLL_DEV_ID" ]]; then
	# User specified DPLL_DEV_ID, validate it exists
	dpll device show id "$DPLL_DEV_ID" &> /dev/null
	if [ $? -ne 0 ]; then
		echo "SKIP: DPLL device ID \"$DPLL_DEV_ID\" not found"
		exit $ksft_skip
	fi

	# Get module name
	DPLL_MODULE_NAME=$(dpll device show id $DPLL_DEV_ID -j 2>/dev/null | jq -r '.["module-name"] // empty' 2>/dev/null)

	if [ "$DPLL_MODULE_NAME" == "dpllsim" ]; then
		DPLL_USE_SIM=1
	else
		DPLL_USE_SIM=0
	fi
fi

##############################################################################
# Sanity checks

# Check if dpll tool is available
if ! command -v dpll &> /dev/null; then
	echo "SKIP: dpll tool not found (install iproute2 with DPLL support)"
	exit $ksft_skip
fi

# Check if dpll tool supports required commands (device show)
# The command should either succeed or show usage help
if ! dpll device show &> /dev/null; then
	# Command failed, check if it at least has usage help for device show
	if ! dpll device help 2>&1 | grep -q "show"; then
		echo "SKIP: dpll tool too old, missing 'device show' command"
		exit $ksft_skip
	fi
fi

##############################################################################
# Device type detection helpers

# dpll_using_sim - Check if using dpllsim
# Returns: 0 if using dpllsim, 1 if using hardware
dpll_using_sim()
{
	[ "$DPLL_USE_SIM" -eq 1 ]
	return $?
}

# dpll_require_sim - Require dpllsim for this test
# Skips test if not using dpllsim
dpll_require_sim()
{
	if ! dpll_using_sim; then
		echo "SKIP: This test requires dpllsim"
		exit $ksft_skip
	fi
}

# dpll_get_module_name - Get the module name being used
# Returns: module name (e.g., "dpllsim", "ice", "mlx5_core")
dpll_get_module_name()
{
	echo "$DPLL_MODULE_NAME"
}

# dpll_get_default_id - Get the default device device ID
# Returns: device device ID
dpll_get_default_id()
{
	echo "$DPLL_DEV_ID"
}

# dpll_info - Print information about DPLL device being tested
dpll_info()
{
	if dpll_using_sim; then
		echo "INFO: Using dpllsim (virtual DPLL device) - ID: $DPLL_DEV_ID"
	else
		echo "INFO: Using hardware DPLL device: module=$DPLL_MODULE_NAME, ID=$DPLL_DEV_ID"
	fi
}

##############################################################################
# DPLL device discovery and validation

# dpll_device_exists - Check if a DPLL device exists
# $1: dev_id - DPLL device ID
# Returns: 0 if exists, 1 otherwise
dpll_device_exists()
{
	local dev_id=$1

	dpll device show id $dev_id &> /dev/null
	return $?
}

# dpll_device_get_count - Get number of DPLL devices
# Returns: Number of DPLL devices
dpll_device_get_count()
{
	dpll device show -j 2>/dev/null | jq '.device | length'
}

# dpll_device_get_id_by_index - Get device ID by index
# $1: index - Device index (0-based)
# Returns: device ID or empty string
dpll_device_get_id_by_index()
{
	local index=$1

	dpll device show -j 2>/dev/null | jq -r ".device[$index].id // empty"
}

# dpll_device_get_clock_id - Get clock_id for a device
# $1: dev_id - DPLL device ID
# Returns: clock_id or empty string
dpll_device_get_clock_id()
{
	local dev_id=$1

	dpll device show id $dev_id -j 2>/dev/null | jq -r '.["clock-id"] // empty'
}

# dpll_device_get_mode - Get device mode
# $1: dev_id - DPLL device ID (optional, defaults to detected device)
# Returns: mode (automatic, manual, etc.)
dpll_device_get_mode()
{
	local dev_id=${1:-$DPLL_DEV_ID}

	dpll device show id $dev_id -j 2>/dev/null \
		| jq -r '.mode // empty'
}

# dpll_device_get_lock_status - Get device lock status
# $1: dev_id - DPLL device ID (optional, defaults to detected device)
# Returns: lock status (locked-ho-acq, unlocked, etc.)
dpll_device_get_lock_status()
{
	local dev_id=${1:-$DPLL_DEV_ID}

	dpll device show id $dev_id -j 2>/dev/null \
		| jq -r '.["lock-status"] // empty'
}

# dpll_device_get_temp - Get device temperature (in millidegrees C)
# $1: dev_id - DPLL device device ID (optional, defaults to detected device)
# Returns: temperature
dpll_device_get_temp()
{
	local dev_id=${1:-$DPLL_DEV_ID}

	dpll device show id $dev_id -j 2>/dev/null \
		| jq -r '.temp // empty'
}

# dpll_device_set_mode - Set device mode (if supported)
# $1: dev_id - DPLL device device ID
# $2: mode - Mode to set (automatic, manual, etc.)
# Returns: 0 on success, 1 on failure
dpll_device_set_mode()
{
	local dev_id=$1
	local mode=$2

	dpll device set id $dev_id mode $mode &> /dev/null
	return $?
}

##############################################################################
# DPLL pin discovery and queries

# dpll_pin_get_count - Get number of pins for a device
# $1: dev_id - DPLL device device ID
# Returns: Number of pins
dpll_pin_get_count()
{
	local dev_id=$1

	dpll pin show device $dev_id -j 2>/dev/null \
		| jq '.pin | length'
}

# dpll_pin_get_id - Get pin ID by index
# $1: dev_id - DPLL device device ID
# $2: index - Pin index (0-based)
# Returns: pin_id or empty string
dpll_pin_get_id()
{
	local dev_id=$1
	local index=$2

	dpll pin show device $dev_id -j 2>/dev/null \
		| jq -r ".pin[$index].id // empty"
}

# dpll_pin_get_type - Get pin type
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# Returns: pin type (mux, ext, synce-eth-port, etc.)
dpll_pin_get_type()
{
	local dev_id=$1
	local pin_id=$2

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r '.type // empty'
}

# dpll_pin_get_direction - Get pin direction
# $1: dev_id - DPLL device ID
# $2: pin_id - Pin ID
# Returns: direction (input, output)
# Note: direction is in parent-device array, not top-level
dpll_pin_get_direction()
{
	local dev_id=$1
	local pin_id=$2

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r '.["parent-device"][0].direction // empty'
}

# dpll_pin_get_frequency - Get pin frequency
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# Returns: frequency in Hz
dpll_pin_get_frequency()
{
	local dev_id=$1
	local pin_id=$2

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r '.frequency // empty'
}

# dpll_pin_get_state - Get pin state on device
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# Returns: state (connected, disconnected, selectable)
dpll_pin_get_state()
{
	local dev_id=$1
	local pin_id=$2

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r '.["parent-device"][0].state // empty'
}

# dpll_pin_get_prio - Get pin priority on device
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# Returns: priority value
dpll_pin_get_prio()
{
	local dev_id=$1
	local pin_id=$2

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r '.["parent-device"][0].prio // empty'
}

# dpll_pin_set_state - Set pin state
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# $3: state - State to set (connected, disconnected)
# Returns: 0 on success, 1 on failure
dpll_pin_set_state()
{
	local dev_id=$1
	local pin_id=$2
	local state=$3

	dpll pin set id $pin_id parent-device $dev_id state $state &> /dev/null
	return $?
}

# dpll_pin_set_prio - Set pin priority
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# $3: prio - Priority value
# Returns: 0 on success, 1 on failure
dpll_pin_set_prio()
{
	local dev_id=$1
	local pin_id=$2
	local prio=$3

	dpll pin set id $pin_id parent-device $dev_id prio $prio &> /dev/null
	return $?
}

##############################################################################
# MUX pin helpers

# dpll_pin_get_parent_count - Get number of parent pins for a MUX pin
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# Returns: Number of parent pins
dpll_pin_get_parent_count()
{
	local dev_id=$1
	local pin_id=$2

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r '.["parent-pin"] // [] | length'
}

# dpll_pin_get_parent_id - Get parent pin ID by index
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID (child)
# $3: index - Parent index (0-based)
# Returns: parent pin_id or empty string
dpll_pin_get_parent_id()
{
	local dev_id=$1
	local pin_id=$2
	local index=$3

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r ".\"parent-pin\"[$index].\"parent-id\" // empty"
}

# dpll_pin_get_state_on_parent - Get MUX pin state on specific parent
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID (child)
# $3: parent_pin_id - Parent pin ID
# Returns: state (connected, disconnected, selectable)
dpll_pin_get_state_on_parent()
{
	local dev_id=$1
	local pin_id=$2
	local parent_pin_id=$3

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r ".\"parent-pin\"[] | select(.\"parent-id\" == $parent_pin_id) | .state // empty"
}

# dpll_pin_set_state_on_parent - Set MUX pin state on specific parent
# $1: pin_id - Pin ID (child)
# $2: parent_pin_id - Parent pin ID
# $3: state - State to set (connected, disconnected)
# Returns: 0 on success, 1 on failure
dpll_pin_set_state_on_parent()
{
	local pin_id=$1
	local parent_pin_id=$2
	local state=$3

	dpll pin set id $pin_id parent-pin $parent_pin_id state $state &> /dev/null
	return $?
}

##############################################################################
# Phase offset and frequency offset helpers

# dpll_pin_get_phase_offset - Get pin phase offset
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# Returns: phase offset in picoseconds
dpll_pin_get_phase_offset()
{
	local dev_id=$1
	local pin_id=$2

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r '.["parent-device"][0].phase_offset // empty'
}

# dpll_pin_get_ffo - Get pin fractional frequency offset
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# Returns: FFO in ppb (parts per billion)
dpll_pin_get_ffo()
{
	local dev_id=$1
	local pin_id=$2

	dpll pin show device $dev_id id $pin_id -j 2>/dev/null \
		| jq -r '.["parent-device"][0].fractional-frequency-offset // empty'
}

##############################################################################
# Test assertion helpers

# dpll_assert_device_exists - Assert that device exists
# $1: dev_id - DPLL device device ID
# $2: test_name - Test name for error message
dpll_assert_device_exists()
{
	local dev_id=$1
	local test_name=$2

	if ! dpll_device_exists $dev_id; then
		echo "FAIL: $test_name - Device ID=$dev_id does not exist"
		return 1
	fi
	return 0
}

# dpll_assert_pin_count - Assert expected pin count
# $1: dev_id - DPLL device device ID
# $2: expected_count - Expected number of pins
# $3: test_name - Test name for error message
dpll_assert_pin_count()
{
	local dev_id=$1
	local expected_count=$2
	local test_name=$3
	local actual_count

	actual_count=$(dpll_pin_get_count $dev_id)
	if [ "$actual_count" != "$expected_count" ]; then
		echo "FAIL: $test_name - Expected $expected_count pins, got $actual_count"
		return 1
	fi
	return 0
}

# dpll_assert_pin_state - Assert pin state
# $1: dev_id - DPLL device device ID
# $2: pin_id - Pin ID
# $3: expected_state - Expected state
# $4: test_name - Test name for error message
dpll_assert_pin_state()
{
	local dev_id=$1
	local pin_id=$2
	local expected_state=$3
	local test_name=$4
	local actual_state

	actual_state=$(dpll_pin_get_state $dev_id $pin_id)
	if [ "$actual_state" != "$expected_state" ]; then
		echo "FAIL: $test_name - Pin $pin_id expected state '$expected_state', got '$actual_state'"
		return 1
	fi
	return 0
}

# dpll_assert_device_mode - Assert device mode
# $1: dev_id - DPLL device device ID
# $2: expected_mode - Expected mode
# $3: test_name - Test name for error message
dpll_assert_device_mode()
{
	local dev_id=$1
	local expected_mode=$2
	local test_name=$3
	local actual_mode

	actual_mode=$(dpll_device_get_mode $dev_id)
	if [ "$actual_mode" != "$expected_mode" ]; then
		echo "FAIL: $test_name - Device expected mode '$expected_mode', got '$actual_mode'"
		return 1
	fi
	return 0
}

##############################################################################
# dpllsim-specific helpers (for testing with dpllsim module)

# dpllsim_bus_path - Get dpllsim bus sysfs path
dpllsim_bus_path()
{
	echo "/sys/bus/dpllsim"
}

# dpllsim_device_path - Get dpllsim device sysfs path
# $1: device_name - Device name (e.g., dpllsim0)
dpllsim_device_path()
{
	local device_name=$1
	echo "/sys/bus/dpllsim/devices/$device_name"
}

# dpllsim_module_loaded - Check if dpllsim module is loaded
# Returns: 0 if loaded, 1 otherwise
dpllsim_module_loaded()
{
	lsmod | grep -q "^dpllsim"
	return $?
}

# dpllsim_check_module - Check and load dpllsim module if needed
# Returns: 0 on success, exits with ksft_skip if module unavailable
dpllsim_check_module()
{
	if ! dpllsim_module_loaded; then
		echo "dpllsim module not loaded, attempting to load..."
		modprobe dpllsim &> /dev/null
		if [ $? -ne 0 ]; then
			echo "SKIP: dpllsim module not available"
			exit $ksft_skip
		fi
	fi

	if [ ! -d "$(dpllsim_bus_path)" ]; then
		echo "SKIP: dpllsim bus not found at $(dpllsim_bus_path)"
		exit $ksft_skip
	fi
}

# dpllsim_create_device - Create a dpllsim device
# $1: dev_id - Clock ID for the device
# Returns: 0 on success, 1 on failure
dpllsim_create_device()
{
	local dev_id=$1
	local bus_path=$(dpllsim_bus_path)
	local dev_path=$(dpllsim_device_path "dpllsim$dev_id")

	# Check if already exists
	if [ -d "$dev_path" ]; then
		return 0
	fi

	echo $dev_id > "$bus_path/new_device" 2>/dev/null
	if [ $? -ne 0 ]; then
		return 1
	fi

	# Wait for device to be created
	for retry in $(seq 1 10); do
		if [ -d "$dev_path" ]; then
			return 0
		fi
		sleep 0.1
	done

	return 1
}

# dpllsim_delete_device - Delete a dpllsim device
# $1: device_name - Device name (e.g., dpllsim0)
# Returns: 0 on success, 1 on failure
dpllsim_delete_device()
{
	local device_name=$1
	local dev_path=$(dpllsim_device_path "$device_name")
	local bus_path=$(dpllsim_bus_path)

	if [ ! -d "$dev_path" ]; then
		return 0
	fi

	# Extract device ID from name (e.g., "dpllsim0" -> "0")
	local dev_id="${device_name#dpllsim}"

	# Undeploy first if deployed
	if [ -f "$dev_path/deploy" ]; then
		echo 0 > "$dev_path/deploy" 2>/dev/null
		sleep 0.2
	fi

	# Delete via bus-level attribute
	echo "$dev_id" > "$bus_path/del_device" 2>/dev/null
	return $?
}

# dpllsim_add_pin - Add a pin to dpllsim device
# $1: device_name - Device name (e.g., dpllsim0)
# $2: type - Pin type
# $3: frequency - Pin frequency
# $4: direction - Pin direction (0=input, 1=output)
# $5: prio - Pin priority (optional, default 128)
# $6: parents - Parent pin IDs comma-separated (optional, for MUX pins)
# Returns: 0 on success, 1 on failure
dpllsim_add_pin()
{
	local device_name=$1
	local type=$2
	local frequency=$3
	local direction=$4
	local prio=${5:-128}
	local parents=$6
	local dev_path=$(dpllsim_device_path "$device_name")
	local cmd

	if [ ! -f "$dev_path/add_pin" ]; then
		return 1
	fi

	cmd="$type $frequency $direction $prio"
	if [ -n "$parents" ]; then
		cmd="$cmd parents:$parents"
	fi

	echo "$cmd" > "$dev_path/add_pin" 2>/dev/null
	return $?
}

# dpllsim_deploy_device - Deploy dpllsim device to DPLL subsystem
# $1: device_name - Device name (e.g., dpllsim0)
# Returns: 0 on success, 1 on failure
dpllsim_deploy_device()
{
	local device_name=$1
	local dev_path=$(dpllsim_device_path "$device_name")

	if [ ! -f "$dev_path/deploy" ]; then
		return 1
	fi

	echo 1 > "$dev_path/deploy" 2>/dev/null
	return $?
}

# dpllsim_cleanup_all - Clean up all dpllsim devices
dpllsim_cleanup_all()
{
	local devices_path="/sys/bus/dpllsim/devices"

	if [ ! -d "$devices_path" ]; then
		return
	fi

	for dev in "$devices_path"/dpllsim*; do
		if [ -d "$dev" ]; then
			local device_name=$(basename "$dev")
			dpllsim_delete_device "$device_name" 2>/dev/null
		fi
	done
}

##############################################################################
# Utility functions

# dpll_wait_for_lock - Wait for device to achieve lock
# $1: dev_id - DPLL device device ID
# $2: timeout_sec - Timeout in seconds (default 30)
# Returns: 0 if locked, 1 on timeout
dpll_wait_for_lock()
{
	local dev_id=$1
	local timeout_sec=${2:-30}
	local elapsed=0
	local status

	while [ $elapsed -lt $timeout_sec ]; do
		status=$(dpll_device_get_lock_status $dev_id)
		case "$status" in
			locked*)
				return 0
				;;
		esac
		sleep 1
		elapsed=$((elapsed + 1))
	done

	return 1
}

# dpll_dump_device_info - Dump device information for debugging
# $1: dev_id - DPLL device device ID (optional, defaults to detected device)
dpll_dump_device_info()
{
	local dev_id=${1:-$DPLL_DEV_ID}

	echo "=== DPLL Device ID=$dev_id ==="
	dpll device show id $dev_id 2>/dev/null
	echo ""
	echo "=== Pins ==="
	dpll pin show device $dev_id 2>/dev/null
	echo ""
}
