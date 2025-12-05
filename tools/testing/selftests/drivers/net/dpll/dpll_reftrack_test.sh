#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DPLL Reference Tracker Test
#
# This test validates the ref_tracker debugging infrastructure in the DPLL
# subsystem. It uses dpllsim to create devices with pin-on-pin (MUX)
# hierarchies and verifies that reference leaks are properly detected.
#
# Requirements:
# - CONFIG_DPLL_REF_TRACKER=y
# - CONFIG_REF_TRACKER=y
# - CONFIG_DEBUG_FS=y (for debugfs output)
# - dpllsim module
#
# Copyright (c) 2025 Red Hat, Inc.

SCRIPTDIR=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")
LIB_DIR="${SCRIPTDIR}/../lib/sh"

# Source the DPLL library
source "${LIB_DIR}/dpll_lib.sh"

# Source kselftest lib for test reporting
source "${SCRIPTDIR}/../../../net/lib.sh"

REF_TRACKER_DIR="/sys/kernel/debug/ref_tracker"

##############################################################################
# Helper functions

check_ref_tracker_enabled()
{
	if [ ! -d "$REF_TRACKER_DIR" ]; then
		echo "SKIP: ref_tracker debugfs not available"
		echo "      Ensure CONFIG_REF_TRACKER=y and CONFIG_DEBUG_FS=y"
		exit "$ksft_skip"
	fi
}

check_dpll_ref_tracker_enabled()
{
	# Check if DPLL ref_tracker entries exist after creating a device
	# This indicates CONFIG_DPLL_REF_TRACKER=y
	local found=0

	# Look for dpll_device or dpll_pin directories
	for dir in "$REF_TRACKER_DIR"/dpll_*; do
		if [ -d "$dir" ]; then
			found=1
			break
		fi
	done

	if [ "$found" -eq 0 ]; then
		echo "SKIP: DPLL ref_tracker not enabled"
		echo "      Ensure CONFIG_DPLL_REF_TRACKER=y"
		exit "$ksft_skip"
	fi
}

get_ref_tracker_leaks()
{
	# Get count of leaked references from ref_tracker debugfs
	local pattern=$1
	local count=0

	for dir in "$REF_TRACKER_DIR"/$pattern*; do
		if [ -d "$dir" ] && [ -f "$dir/stats" ]; then
			# Read stats file - format: "total: N, untracked: M, leaked: L"
			local leaked=$(cat "$dir/stats" 2>/dev/null | grep -o 'leaked: [0-9]*' | cut -d' ' -f2)
			if [ -n "$leaked" ] && [ "$leaked" -gt 0 ]; then
				count=$((count + leaked))
			fi
		fi
	done

	echo "$count"
}

dump_ref_tracker_info()
{
	local pattern=$1

	echo "=== ref_tracker info for $pattern ==="
	for dir in "$REF_TRACKER_DIR"/$pattern*; do
		if [ -d "$dir" ]; then
			echo "Directory: $dir"
			if [ -f "$dir/stats" ]; then
				echo "Stats: $(cat "$dir/stats" 2>/dev/null)"
			fi
			# Show stack traces if available
			if [ -f "$dir/stack" ]; then
				echo "Stack traces:"
				head -50 "$dir/stack" 2>/dev/null
			fi
			echo "---"
		fi
	done
}

##############################################################################
# Test functions

test_basic_reftrack_setup()
{
	RET=0

	echo "Testing basic ref_tracker setup..."

	# Create a simple dpllsim device
	if ! dpllsim_create_device 100; then
		echo "FAIL: Could not create dpllsim device"
		return 1
	fi

	local dev_path=$(dpllsim_device_path "dpllsim100")

	# Add some input pins
	dpllsim_add_pin "dpllsim100" 5 1 1 0        # GNSS, 1 PPS, input, prio 0
	dpllsim_add_pin "dpllsim100" 2 10000000 1 1  # EXT, 10MHz, input, prio 1
	dpllsim_add_pin "dpllsim100" 2 10000000 1 2  # EXT, 10MHz, input, prio 2

	# Add output pins
	dpllsim_add_pin "dpllsim100" 2 10000000 2 0  # EXT, 10MHz, output
	dpllsim_add_pin "dpllsim100" 2 10000000 2 0  # EXT, 10MHz, output

	# Deploy device
	if ! dpllsim_deploy_device "dpllsim100"; then
		echo "FAIL: Could not deploy dpllsim device"
		dpllsim_delete_device "dpllsim100"
		return 1
	fi

	# Wait for device to appear in DPLL subsystem
	sleep 0.5

	# Check that ref_tracker directories were created
	check_dpll_ref_tracker_enabled

	echo "  ref_tracker directories created successfully"

	# Clean undeploy
	echo 0 > "$dev_path/deploy"
	sleep 0.2

	# Delete device
	dpllsim_delete_device "dpllsim100"

	# Check for leaks after clean shutdown - should be 0
	local leaks=$(get_ref_tracker_leaks "dpll")
	if [ "$leaks" -gt 0 ]; then
		echo "WARN: Found $leaks leaked references after clean shutdown"
		dump_ref_tracker_info "dpll"
	else
		echo "  No leaks after clean shutdown - OK"
	fi

	log_test "Basic ref_tracker setup"
}

test_mux_pin_reftrack()
{
	RET=0

	echo "Testing MUX pin reference tracking..."

	# Create device with MUX pin hierarchy
	if ! dpllsim_create_device 101; then
		echo "FAIL: Could not create dpllsim device"
		return 1
	fi

	local dev_path=$(dpllsim_device_path "dpllsim101")

	# Add parent MUX pins first (these will be parents for child pins)
	# Pin 0: MUX type parent
	dpllsim_add_pin "dpllsim101" 1 1953125 1 10   # MUX, recovered clock freq, input
	# Pin 1: MUX type parent
	dpllsim_add_pin "dpllsim101" 1 1953125 1 20   # MUX, recovered clock freq, input
	# Pin 2: MUX type parent
	dpllsim_add_pin "dpllsim101" 1 1953125 1 30   # MUX, recovered clock freq, input
	# Pin 3: MUX type parent
	dpllsim_add_pin "dpllsim101" 1 1953125 1 40   # MUX, recovered clock freq, input

	# Add child pins that have multiple parents (pin-on-pin)
	# Pin 4: EXT type child with parents 0,1,2,3
	dpllsim_add_pin "dpllsim101" 2 1953125 1 100 "0,1,2,3"
	# Pin 5: EXT type child with parents 0,1
	dpllsim_add_pin "dpllsim101" 2 1953125 1 110 "0,1"

	# Add some regular output pins
	dpllsim_add_pin "dpllsim101" 2 10000000 2 0   # output
	dpllsim_add_pin "dpllsim101" 2 10000000 2 0   # output

	echo "  Created MUX hierarchy: 4 parent MUX pins, 2 child pins with parents"

	# Deploy device
	if ! dpllsim_deploy_device "dpllsim101"; then
		echo "FAIL: Could not deploy dpllsim device"
		dpllsim_delete_device "dpllsim101"
		return 1
	fi

	sleep 0.5

	# Query the device to verify MUX hierarchy
	local dpll_id=$(dpll device show -j 2>/dev/null | jq -r '
		.device
		| map(select(.["module-name"] == "dpllsim"))
		| last
		| .id
	' 2>/dev/null)

	if [ -n "$dpll_id" ]; then
		echo "  Device deployed with ID: $dpll_id"

		# Check pin count
		local pin_count=$(dpll_pin_get_count "$dpll_id")
		echo "  Total pins: $pin_count"

		# Check MUX pin parents
		local pin4_parents=$(dpll_pin_get_parent_count "$dpll_id" 4)
		echo "  Pin 4 has $pin4_parents parents"
	fi

	# Clean undeploy
	echo 0 > "$dev_path/deploy"
	sleep 0.2

	# Delete device
	dpllsim_delete_device "dpllsim101"

	# Check for leaks
	local leaks=$(get_ref_tracker_leaks "dpll")
	if [ "$leaks" -gt 0 ]; then
		echo "WARN: Found $leaks leaked references after MUX test"
		dump_ref_tracker_info "dpll"
		RET=1
	else
		echo "  No leaks after MUX pin test - OK"
	fi

	check_err $RET "MUX pin reference tracking"
	log_test "MUX pin reference tracking"
}

test_multiple_devices_reftrack()
{
	RET=0

	echo "Testing multiple device reference tracking..."

	# Create multiple devices
	for i in 200 201 202; do
		if ! dpllsim_create_device $i; then
			echo "FAIL: Could not create dpllsim device $i"
			return 1
		fi

		# Add pins
		dpllsim_add_pin "dpllsim$i" 5 1 1 0       # GNSS input
		dpllsim_add_pin "dpllsim$i" 2 10000000 1 1 # EXT input
		dpllsim_add_pin "dpllsim$i" 2 10000000 2 0 # EXT output

		# Deploy
		if ! dpllsim_deploy_device "dpllsim$i"; then
			echo "FAIL: Could not deploy dpllsim device $i"
			# Clean up created devices
			for j in 200 201 202; do
				dpllsim_delete_device "dpllsim$j" 2>/dev/null
			done
			return 1
		fi
	done

	echo "  Created and deployed 3 devices"
	sleep 0.5

	# Undeploy and delete in reverse order
	for i in 202 201 200; do
		local dev_path=$(dpllsim_device_path "dpllsim$i")
		echo 0 > "$dev_path/deploy"
		sleep 0.1
		dpllsim_delete_device "dpllsim$i"
	done

	# Check for leaks
	local leaks=$(get_ref_tracker_leaks "dpll")
	if [ "$leaks" -gt 0 ]; then
		echo "WARN: Found $leaks leaked references after multiple device test"
		dump_ref_tracker_info "dpll"
		RET=1
	else
		echo "  No leaks after multiple device test - OK"
	fi

	check_err $RET "Multiple device reference tracking"
	log_test "Multiple device reference tracking"
}

test_rapid_create_destroy()
{
	RET=0

	echo "Testing rapid create/destroy cycles..."

	for cycle in $(seq 1 5); do
		# Create device
		if ! dpllsim_create_device 300; then
			echo "FAIL: Could not create dpllsim device in cycle $cycle"
			return 1
		fi

		# Add pins
		dpllsim_add_pin "dpllsim300" 5 1 1 0
		dpllsim_add_pin "dpllsim300" 2 10000000 1 1

		# Deploy
		if ! dpllsim_deploy_device "dpllsim300"; then
			echo "FAIL: Could not deploy in cycle $cycle"
			dpllsim_delete_device "dpllsim300"
			return 1
		fi

		# Brief pause
		sleep 0.1

		# Undeploy and delete
		local dev_path=$(dpllsim_device_path "dpllsim300")
		echo 0 > "$dev_path/deploy"
		sleep 0.1
		dpllsim_delete_device "dpllsim300"
	done

	echo "  Completed 5 rapid create/destroy cycles"

	# Check for leaks
	local leaks=$(get_ref_tracker_leaks "dpll")
	if [ "$leaks" -gt 0 ]; then
		echo "WARN: Found $leaks leaked references after rapid cycles"
		dump_ref_tracker_info "dpll"
		RET=1
	else
		echo "  No leaks after rapid cycles - OK"
	fi

	check_err $RET "Rapid create/destroy cycles"
	log_test "Rapid create/destroy reference tracking"
}

##############################################################################
# Cleanup

cleanup()
{
	# Clean up any test devices that might be left
	for i in 100 101 200 201 202 300; do
		dpllsim_delete_device "dpllsim$i" 2>/dev/null || true
	done
}

trap cleanup EXIT

##############################################################################
# Main test execution

echo "===== DPLL Reference Tracker Tests ====="
echo ""

# Check prerequisites
dpllsim_check_module
check_ref_tracker_enabled

# Clean up any existing test devices
cleanup

# Run tests
test_basic_reftrack_setup
test_mux_pin_reftrack
test_multiple_devices_reftrack
test_rapid_create_destroy

echo ""
echo "===== Test Summary ====="
echo ""

# Final leak check
final_leaks=$(get_ref_tracker_leaks "dpll")
if [ "$final_leaks" -gt 0 ]; then
	echo "WARNING: Total leaked DPLL references: $final_leaks"
	dump_ref_tracker_info "dpll"
else
	echo "All ref_tracker tests passed - no leaks detected"
fi

exit "$EXIT_STATUS"
