#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Basic DPLL subsystem test
#
# This test validates basic DPLL device and pin operations using either
# hardware DPLL devices or dpllsim virtual devices.
#
# Copyright (c) 2025 Red Hat, Inc.

# Note: Don't use set -e because tests are allowed to fail

SCRIPTDIR=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")
LIB_DIR="${SCRIPTDIR}/../lib/sh"

# Source the DPLL library
source "${LIB_DIR}/dpll_lib.sh"

# Source kselftest lib for test reporting
source "${SCRIPTDIR}/../../../net/lib.sh"

##############################################################################
# Test functions

test_device_detection()
{
	RET=0

	dpll_info

	# Check if we have a device
	if [ -z "$DPLL_DEV_ID" ]; then
		echo "FAIL: No DPLL device detected"
		return 1
	fi

	# Verify device exists
	if ! dpll_device_exists "$DPLL_DEV_ID"; then
		echo "FAIL: Device ID $DPLL_DEV_ID does not exist"
		return 1
	fi

	log_test "DPLL device detection"
}

test_device_properties()
{
	RET=0
	local mode lock_status

	mode=$(dpll_device_get_mode)
	if [ -z "$mode" ]; then
		echo "FAIL: Could not get device mode"
		RET=1
	else
		echo "  Device mode: $mode"
	fi

	lock_status=$(dpll_device_get_lock_status)
	if [ -z "$lock_status" ]; then
		echo "FAIL: Could not get lock status"
		RET=1
	else
		echo "  Lock status: $lock_status"
	fi

	check_err $RET "Failed to get device properties"
	log_test "DPLL device properties"
}

test_pin_enumeration()
{
	RET=0
	local pin_count

	pin_count=$(dpll_pin_get_count "$DPLL_DEV_ID")
	if [ -z "$pin_count" ] || [ "$pin_count" -eq 0 ]; then
		echo "FAIL: No pins found for device $DPLL_DEV_ID"
		return 1
	fi

	echo "  Found $pin_count pins"

	# Get first pin
	local first_pin_id
	first_pin_id=$(dpll_pin_get_id "$DPLL_DEV_ID" 0)
	if [ -z "$first_pin_id" ]; then
		echo "FAIL: Could not get first pin ID"
		RET=1
	else
		echo "  First pin ID: $first_pin_id"
	fi

	check_err $RET "Failed pin enumeration"
	log_test "DPLL pin enumeration"
}

test_pin_properties()
{
	RET=0
	local pin_count first_pin_id pin_type pin_dir pin_freq

	pin_count=$(dpll_pin_get_count "$DPLL_DEV_ID")
	if [ -z "$pin_count" ] || [ "$pin_count" -eq 0 ]; then
		echo "SKIP: No pins to test"
		return "$ksft_skip"
	fi

	first_pin_id=$(dpll_pin_get_id "$DPLL_DEV_ID" 0)
	if [ -z "$first_pin_id" ]; then
		echo "FAIL: Could not get first pin ID"
		return 1
	fi

	pin_type=$(dpll_pin_get_type "$DPLL_DEV_ID" "$first_pin_id")
	if [ -z "$pin_type" ]; then
		echo "FAIL: Could not get pin type"
		RET=1
	else
		echo "  Pin $first_pin_id type: $pin_type"
	fi

	pin_dir=$(dpll_pin_get_direction "$DPLL_DEV_ID" "$first_pin_id")
	if [ -z "$pin_dir" ]; then
		echo "FAIL: Could not get pin direction"
		RET=1
	else
		echo "  Pin $first_pin_id direction: $pin_dir"
	fi

	pin_freq=$(dpll_pin_get_frequency "$DPLL_DEV_ID" "$first_pin_id")
	if [ -z "$pin_freq" ]; then
		echo "FAIL: Could not get pin frequency"
		RET=1
	else
		echo "  Pin $first_pin_id frequency: $pin_freq Hz"
	fi

	check_err $RET "Failed to get pin properties"
	log_test "DPLL pin properties"
}

test_mux_pin_detection()
{
	RET=0
	local pin_count i pin_id parent_count

	pin_count=$(dpll_pin_get_count "$DPLL_DEV_ID")
	if [ -z "$pin_count" ] || [ "$pin_count" -eq 0 ]; then
		echo "SKIP: No pins to test"
		return "$ksft_skip"
	fi

	# Look for MUX pins (pins with parents)
	local mux_found=0
	for i in $(seq 0 $((pin_count - 1))); do
		pin_id=$(dpll_pin_get_id "$DPLL_DEV_ID" "$i")
		if [ -z "$pin_id" ]; then
			continue
		fi

		parent_count=$(dpll_pin_get_parent_count "$DPLL_DEV_ID" "$pin_id")
		if [ -n "$parent_count" ] && [ "$parent_count" -gt 0 ]; then
			echo "  Found MUX pin $pin_id with $parent_count parents"
			mux_found=1
			break
		fi
	done

	if [ "$mux_found" -eq 0 ]; then
		echo "  No MUX pins found (this is OK for simple devices)"
	fi

	log_test "DPLL MUX pin detection"
}

test_pin_state_operations()
{
	RET=0
	local pin_count first_pin_id initial_state

	pin_count=$(dpll_pin_get_count "$DPLL_DEV_ID")
	if [ -z "$pin_count" ] || [ "$pin_count" -eq 0 ]; then
		echo "SKIP: No pins to test"
		return "$ksft_skip"
	fi

	# Get first input pin (skip MUX pins)
	local i pin_id pin_type
	for i in $(seq 0 $((pin_count - 1))); do
		pin_id=$(dpll_pin_get_id "$DPLL_DEV_ID" "$i")
		if [ -z "$pin_id" ]; then
			continue
		fi

		pin_type=$(dpll_pin_get_type "$DPLL_DEV_ID" "$pin_id")
		if [ "$pin_type" != "mux" ]; then
			first_pin_id=$pin_id
			break
		fi
	done

	if [ -z "$first_pin_id" ]; then
		echo "SKIP: No non-MUX pins found"
		return "$ksft_skip"
	fi

	# Get initial state
	initial_state=$(dpll_pin_get_state "$DPLL_DEV_ID" "$first_pin_id")
	if [ -n "$initial_state" ]; then
		echo "  Pin $first_pin_id initial state: $initial_state"
	else
		echo "  Pin $first_pin_id has no parent-device (expected for some pin types)"
	fi

	log_test "DPLL pin state operations"
}

test_device_json_format()
{
	RET=0
	local json

	# Verify device JSON contains expected fields
	json=$(dpll device show id "$DPLL_DEV_ID" -j 2>/dev/null)

	# Check for required fields
	if ! echo "$json" | jq -e '.mode' &>/dev/null; then
		echo "FAIL: Device JSON missing 'mode' field"
		RET=1
	fi

	if ! echo "$json" | jq -e '.["lock-status"]' &>/dev/null; then
		echo "FAIL: Device JSON missing 'lock-status' field"
		RET=1
	fi

	if ! echo "$json" | jq -e '.type' &>/dev/null; then
		echo "FAIL: Device JSON missing 'type' field"
		RET=1
	fi

	if [ $RET -eq 0 ]; then
		echo "  Device JSON format valid"
	fi

	check_err $RET "Device JSON format validation failed"
	log_test "DPLL device JSON format"
}

test_pin_json_format()
{
	RET=0
	local pin_count first_pin_id json

	pin_count=$(dpll_pin_get_count "$DPLL_DEV_ID")
	if [ -z "$pin_count" ] || [ "$pin_count" -eq 0 ]; then
		echo "SKIP: No pins to test"
		return "$ksft_skip"
	fi

	first_pin_id=$(dpll_pin_get_id "$DPLL_DEV_ID" 0)
	if [ -z "$first_pin_id" ]; then
		echo "FAIL: Could not get first pin ID"
		return 1
	fi

	# Verify pin JSON contains expected fields
	json=$(dpll pin show device "$DPLL_DEV_ID" id "$first_pin_id" -j 2>/dev/null)

	# Check for required fields
	if ! echo "$json" | jq -e '.type' &>/dev/null; then
		echo "FAIL: Pin JSON missing 'type' field"
		RET=1
	fi

	if ! echo "$json" | jq -e '.frequency' &>/dev/null; then
		echo "FAIL: Pin JSON missing 'frequency' field"
		RET=1
	fi

	# parent-device array is optional (not present for all pin types)
	if echo "$json" | jq -e '.["parent-device"]' &>/dev/null; then
		echo "  Pin has parent-device array"

		if ! echo "$json" | jq -e '.["parent-device"][0].direction' &>/dev/null; then
			echo "FAIL: Pin parent-device missing 'direction' field"
			RET=1
		fi
	else
		echo "  Pin has no parent-device (OK for some pin types)"
	fi

	if [ $RET -eq 0 ]; then
		echo "  Pin JSON format valid"
	fi

	check_err $RET "Pin JSON format validation failed"
	log_test "DPLL pin JSON format"
}

##############################################################################
# Cleanup

cleanup()
{
	# If using dpllsim, clean up created devices
	if dpll_using_sim && [ -n "$CREATED_DEVICE" ]; then
		dpllsim_delete_device "$CREATED_DEVICE" 2>/dev/null || true
	fi
}

trap cleanup EXIT

##############################################################################
# Main test execution

# If no DPLL device found or only dpllsim devices exist, create a fresh test device
if [ -z "$DPLL_DEV_ID" ] || [ "$DPLL_MODULE_NAME" == "dpllsim" ]; then
	if [ -z "$DPLL_DEV_ID" ]; then
		echo "No DPLL device found, trying to use dpllsim..."
	else
		echo "Existing dpllsim device found, creating fresh test device..."
	fi

	dpllsim_check_module

	# Clean up any existing dpllsim devices first
	for dev in /sys/bus/dpllsim/devices/dpllsim*; do
		if [ -d "$dev" ]; then
			dev_name=$(basename "$dev")
			echo "Removing existing device: $dev_name"
			dpllsim_delete_device "$dev_name" 2>/dev/null || true
		fi
	done

	# Create a simple test device
	if dpllsim_create_device 99; then
		CREATED_DEVICE="dpllsim99"
		DEV_PATH=$(dpllsim_device_path "$CREATED_DEVICE")

		# Add a few basic pins
		dpllsim_add_pin "$CREATED_DEVICE" 5 1 0 0  # GNSS, 1 PPS, input, prio 0
		dpllsim_add_pin "$CREATED_DEVICE" 2 10000000 0 1  # EXT, 10MHz, input, prio 1
		dpllsim_add_pin "$CREATED_DEVICE" 2 10000000 1 0  # EXT, 10MHz, output, prio 0

		# Deploy it
		if dpllsim_deploy_device "$CREATED_DEVICE"; then
			# Re-detect device - find the one with most pins (our fresh device)
			DPLL_DEV_ID=$(dpll device show -j 2>/dev/null | jq -r '
				.device
				| map({id: .id, module: .["module-name"]})
				| map(select(.module == "dpllsim"))
				| last
				| .id
			' 2>/dev/null)
			DPLL_MODULE_NAME="dpllsim"
			DPLL_USE_SIM=1
		else
			echo "SKIP: Failed to deploy dpllsim device"
			exit "$ksft_skip"
		fi
	else
		echo "SKIP: No DPLL device available and dpllsim failed"
		exit "$ksft_skip"
	fi
fi

# Run tests directly (bypass defer framework to avoid hang)
echo "===== DPLL Basic Tests ====="
echo ""

test_device_detection
test_device_properties
test_device_json_format
test_pin_enumeration
test_pin_properties
test_pin_json_format
test_pin_state_operations
test_mux_pin_detection

echo ""
echo "===== Test Summary ====="
exit "$EXIT_STATUS"
