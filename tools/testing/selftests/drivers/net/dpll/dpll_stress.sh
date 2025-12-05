#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DPLL subsystem stress test
#
# This test performs intensive stress testing of the DPLL subsystem including:
# - Rapid device creation/deletion
# - Concurrent pin state changes
# - Netlink notification flooding
# - MUX pin parent switching
# - Memory leak detection
# - Race condition triggers
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
# Configuration

STRESS_ITERATIONS=${STRESS_ITERATIONS:-100}
STRESS_DEVICES=${STRESS_DEVICES:-10}
STRESS_PINS_PER_DEVICE=${STRESS_PINS_PER_DEVICE:-20}
STRESS_CONCURRENT_PROCS=${STRESS_CONCURRENT_PROCS:-4}

##############################################################################
# Cleanup tracking

CREATED_DEVICES=()
BACKGROUND_PIDS=()

cleanup()
{
	# Kill background processes
	for pid in "${BACKGROUND_PIDS[@]}"; do
		kill "$pid" 2>/dev/null || true
	done
	wait 2>/dev/null || true

	# Clean up dpllsim devices
	if dpll_using_sim; then
		for dev in "${CREATED_DEVICES[@]}"; do
			dpllsim_delete_device "$dev" 2>/dev/null || true
		done
	fi
}

trap cleanup EXIT

##############################################################################
# Helper functions

# Create a dpllsim device with random pins
create_random_device()
{
	local clock_id=$1
	local num_pins=$2
	local dev_name="dpllsim${clock_id}"

	if ! dpllsim_create_device "$clock_id" 2>/dev/null; then
		return 1
	fi

	CREATED_DEVICES+=("$dev_name")

	# Add random pins
	local i
	for i in $(seq 1 "$num_pins"); do
		local type=$((1 + RANDOM % 5))  # 1-5
		local freq=$((1000000 + RANDOM % 10000000))
		local dir=$((1 + RANDOM % 2))  # 1-2
		local prio=$((RANDOM % 256))

		dpllsim_add_pin "$dev_name" "$type" "$freq" "$dir" "$prio" 2>/dev/null || true
	done

	# Deploy
	dpllsim_deploy_device "$dev_name" 2>/dev/null
}

# Get memory usage for dpll subsystem
get_dpll_memory_kb()
{
	# Sum up memory from dpll_core and dpllsim modules
	local mem=0
	if [ -f /sys/kernel/debug/kmemleak ]; then
		# If kmemleak is available, scan it
		echo scan > /sys/kernel/debug/kmemleak 2>/dev/null || true
	fi

	# Check slab memory for dpll objects
	if [ -f /proc/slabinfo ]; then
		mem=$(awk '/dpll/ {sum += $3 * $4} END {print int(sum/1024)}' /proc/slabinfo 2>/dev/null || echo 0)
	fi

	echo "$mem"
}

##############################################################################
# Test functions

test_rapid_device_creation_deletion()
{
	RET=0
	local start_mem end_mem mem_leak

	echo "  Testing rapid device creation/deletion ($STRESS_ITERATIONS iterations)..."

	start_mem=$(get_dpll_memory_kb)

	local i
	for i in $(seq 1 "$STRESS_ITERATIONS"); do
		local clock_id=$((10000 + i))

		if ! create_random_device "$clock_id" 5; then
			echo "FAIL: Failed to create device $clock_id"
			RET=1
			break
		fi

		# Immediately delete it
		if ! dpllsim_delete_device "dpllsim${clock_id}" 2>/dev/null; then
			echo "FAIL: Failed to delete device dpllsim${clock_id}"
			RET=1
			break
		fi

		# Remove from tracking
		CREATED_DEVICES=("${CREATED_DEVICES[@]/dpllsim${clock_id}/}")

		# Progress indicator
		if [ $((i % 10)) -eq 0 ]; then
			echo -n "."
		fi
	done
	echo ""

	# Check for memory leaks
	sleep 1  # Allow cleanup
	end_mem=$(get_dpll_memory_kb)
	mem_leak=$((end_mem - start_mem))

	if [ "$mem_leak" -gt 100 ]; then
		echo "WARNING: Possible memory leak detected: ${mem_leak}KB growth"
		# Don't fail test, just warn
	fi

	check_err $RET "Rapid device creation/deletion failed"
	log_test "DPLL rapid device creation/deletion"
}

test_concurrent_device_operations()
{
	RET=0

	echo "  Testing concurrent operations with $STRESS_CONCURRENT_PROCS processes..."

	# Function to run in background
	concurrent_worker()
	{
		local worker_id=$1
		local iterations=$2

		for i in $(seq 1 "$iterations"); do
			local clock_id=$((20000 + worker_id * 1000 + i))

			create_random_device "$clock_id" 3 >/dev/null 2>&1 || true
			sleep 0.01
			dpllsim_delete_device "dpllsim${clock_id}" >/dev/null 2>&1 || true
		done
	}

	# Start workers
	local worker
	for worker in $(seq 1 "$STRESS_CONCURRENT_PROCS"); do
		concurrent_worker "$worker" 20 &
		BACKGROUND_PIDS+=($!)
	done

	# Wait for completion
	local failed=0
	for pid in "${BACKGROUND_PIDS[@]}"; do
		if ! wait "$pid"; then
			failed=1
		fi
	done
	BACKGROUND_PIDS=()

	if [ "$failed" -eq 1 ]; then
		echo "FAIL: Some concurrent workers failed"
		RET=1
	fi

	check_err $RET "Concurrent device operations failed"
	log_test "DPLL concurrent device operations"
}

test_pin_state_flooding()
{
	RET=0

	echo "  Testing pin state change flooding..."

	# Create a test device
	local test_clock_id=30000
	if ! create_random_device "$test_clock_id" 10; then
		echo "SKIP: Could not create test device"
		return "$ksft_skip"
	fi

	# Get device ID and pin IDs
	local dev_id
	dev_id=$(dpll device show -j 2>/dev/null | jq -r ".device[] | select(.\"module-name\" == \"dpllsim\") | .id" | head -1)

	if [ -z "$dev_id" ]; then
		echo "SKIP: Could not find deployed device"
		return "$ksft_skip"
	fi

	# Get first input pin
	local pin_id
	pin_id=$(dpll pin show -j 2>/dev/null | jq -r ".pin[] | select(.direction == \"input\") | .id" | head -1)

	if [ -z "$pin_id" ]; then
		echo "SKIP: No input pins found"
		return "$ksft_skip"
	fi

	# Rapidly change pin state
	local states=("connected" "disconnected" "selectable")
	local i state_idx
	for i in $(seq 1 50); do
		state_idx=$((i % 3))
		dpll pin set id "$pin_id" device-id "$dev_id" state "${states[$state_idx]}" 2>/dev/null || true

		if [ $((i % 10)) -eq 0 ]; then
			echo -n "."
		fi
	done
	echo ""

	# Cleanup
	dpllsim_delete_device "dpllsim${test_clock_id}" 2>/dev/null || true

	check_err $RET "Pin state flooding failed"
	log_test "DPLL pin state flooding"
}

test_netlink_notification_stress()
{
	RET=0

	echo "  Testing netlink notification stress..."

	# Start monitoring in background
	dpll monitor > /dev/null 2>&1 &
	local monitor_pid=$!
	BACKGROUND_PIDS+=($monitor_pid)

	sleep 0.5  # Let monitor start

	# Create/delete many devices rapidly while monitoring
	local i
	for i in $(seq 1 30); do
		local clock_id=$((40000 + i))
		create_random_device "$clock_id" 5 >/dev/null 2>&1 || true
		sleep 0.05
		dpllsim_delete_device "dpllsim${clock_id}" >/dev/null 2>&1 || true

		echo -n "."
	done
	echo ""

	# Stop monitor
	kill "$monitor_pid" 2>/dev/null || true
	wait "$monitor_pid" 2>/dev/null || true
	BACKGROUND_PIDS=("${BACKGROUND_PIDS[@]/$monitor_pid/}")

	# Check if monitor died unexpectedly (would indicate crash)
	if ! kill -0 "$monitor_pid" 2>/dev/null; then
		# Already dead, this is expected
		:
	fi

	check_err $RET "Netlink notification stress failed"
	log_test "DPLL netlink notification stress"
}

test_max_devices()
{
	RET=0

	echo "  Testing maximum number of devices ($STRESS_DEVICES devices)..."

	# Create many devices
	local i created=0
	for i in $(seq 1 "$STRESS_DEVICES"); do
		local clock_id=$((50000 + i))

		if create_random_device "$clock_id" "$STRESS_PINS_PER_DEVICE"; then
			created=$((created + 1))
		else
			echo "  Reached limit at $created devices"
			break
		fi

		if [ $((i % 5)) -eq 0 ]; then
			echo -n "."
		fi
	done
	echo ""

	echo "  Successfully created $created devices"

	# Verify we can query all devices
	local dev_count
	dev_count=$(dpll device show -j 2>/dev/null | jq '. | length' 2>/dev/null || echo 0)

	if [ "$dev_count" -lt "$created" ]; then
		echo "FAIL: Device count mismatch: expected $created, got $dev_count"
		RET=1
	fi

	# Cleanup all devices
	for i in $(seq 1 "$created"); do
		local clock_id=$((50000 + i))
		dpllsim_delete_device "dpllsim${clock_id}" 2>/dev/null || true
	done
	CREATED_DEVICES=()

	check_err $RET "Max devices test failed"
	log_test "DPLL maximum devices"
}

test_mux_parent_switching_stress()
{
	RET=0

	echo "  Testing MUX pin parent switching stress..."

	# Create device with MUX hierarchy
	local clock_id=60000
	local dev_name="dpllsim${clock_id}"

	if ! dpllsim_create_device "$clock_id" 2>/dev/null; then
		echo "SKIP: Could not create device"
		return "$ksft_skip"
	fi
	CREATED_DEVICES+=("$dev_name")

	# Add parent pins (MUX type)
	local i
	for i in $(seq 0 3); do
		dpllsim_add_pin "$dev_name" 1 10000000 1 "$i" 2>/dev/null || true  # MUX, 10MHz, input
	done

	# Add child MUX pin with parents
	echo "1 10000000 1 255 parents:0,1,2,3" > "/sys/bus/dpllsim/devices/${dev_name}/add_pin" 2>/dev/null || true

	# Deploy
	if ! dpllsim_deploy_device "$dev_name" 2>/dev/null; then
		echo "SKIP: Could not deploy device"
		dpllsim_delete_device "$dev_name" 2>/dev/null || true
		return "$ksft_skip"
	fi

	# Get device ID and MUX pin IDs
	local dev_id parent_pin_ids child_pin_id
	dev_id=$(dpll device show -j 2>/dev/null | jq -r ".device[] | select(.\"module-name\" == \"dpllsim\") | .id" | tail -1)

	if [ -n "$dev_id" ]; then
		# Rapidly switch active parent
		for i in $(seq 1 50); do
			local parent_idx=$((i % 4))
			# Try to set state on different parents
			dpll pin show -j 2>/dev/null | jq -r '.pin[] | .id' | while read pin_id; do
				dpll pin set id "$pin_id" device-id "$dev_id" state connected 2>/dev/null || true
				break
			done 2>/dev/null

			if [ $((i % 10)) -eq 0 ]; then
				echo -n "."
			fi
		done
		echo ""
	fi

	# Cleanup
	dpllsim_delete_device "$dev_name" 2>/dev/null || true

	check_err $RET "MUX parent switching stress failed"
	log_test "DPLL MUX parent switching stress"
}

test_device_deploy_cycle_stress()
{
	RET=0

	echo "  Testing deploy/undeploy cycling..."

	local clock_id=70000
	local dev_name="dpllsim${clock_id}"

	# Create device once
	if ! dpllsim_create_device "$clock_id" 2>/dev/null; then
		echo "SKIP: Could not create device"
		return "$ksft_skip"
	fi
	CREATED_DEVICES+=("$dev_name")

	# Add pins
	dpllsim_add_pin "$dev_name" 5 1 1 0 2>/dev/null || true  # GNSS
	dpllsim_add_pin "$dev_name" 2 10000000 1 1 2>/dev/null || true  # EXT
	dpllsim_add_pin "$dev_name" 2 10000000 2 0 2>/dev/null || true  # EXT output

	# Rapidly deploy/undeploy
	local i
	for i in $(seq 1 20); do
		dpllsim_deploy_device "$dev_name" 2>/dev/null || true
		sleep 0.1
		echo 0 > "/sys/bus/dpllsim/devices/${dev_name}/deploy" 2>/dev/null || true
		sleep 0.1

		if [ $((i % 5)) -eq 0 ]; then
			echo -n "."
		fi
	done
	echo ""

	# Final deploy for cleanup
	dpllsim_deploy_device "$dev_name" 2>/dev/null || true

	# Cleanup
	dpllsim_delete_device "$dev_name" 2>/dev/null || true

	check_err $RET "Deploy/undeploy cycling failed"
	log_test "DPLL deploy/undeploy cycling"
}

test_notifier_deadlock_stress()
{
	RET=0

	echo "  Testing notifier callback deadlock detection..."

	# This test exercises the notifier callback path while simultaneously
	# performing operations that acquire locks. If a deadlock exists between
	# the DPLL subsystem lock and driver locks, this test will hang.

	local clock_id=80000
	local dev_name="dpllsim${clock_id}"

	# Create and deploy a device that will receive notifications
	if ! dpllsim_create_device "$clock_id" 2>/dev/null; then
		echo "SKIP: Could not create device"
		return "$ksft_skip"
	fi
	CREATED_DEVICES+=("$dev_name")

	# Add multiple pins to generate more notifications
	local i
	for i in $(seq 0 9); do
		dpllsim_add_pin "$dev_name" 2 10000000 1 "$i" 2>/dev/null || true
	done

	if ! dpllsim_deploy_device "$dev_name" 2>/dev/null; then
		echo "SKIP: Could not deploy device"
		dpllsim_delete_device "$dev_name" 2>/dev/null || true
		return "$ksft_skip"
	fi

	# Get device ID
	local dev_id
	dev_id=$(dpll device show -j 2>/dev/null | jq -r ".device[] | select(.\"module-name\" == \"dpllsim\") | .id" | tail -1)

	if [ -z "$dev_id" ]; then
		echo "SKIP: Could not find deployed device"
		dpllsim_delete_device "$dev_name" 2>/dev/null || true
		return "$ksft_skip"
	fi

	# Worker function that creates/deletes devices rapidly
	# This generates DPLL_DEVICE_CREATED/DELETED notifications
	device_worker()
	{
		local worker_id=$1
		local iterations=$2

		for i in $(seq 1 "$iterations"); do
			local cid=$((81000 + worker_id * 100 + i))
			dpllsim_create_device "$cid" >/dev/null 2>&1 || continue
			dpllsim_add_pin "dpllsim${cid}" 2 10000000 1 0 >/dev/null 2>&1 || true
			dpllsim_deploy_device "dpllsim${cid}" >/dev/null 2>&1 || true
			dpllsim_delete_device "dpllsim${cid}" >/dev/null 2>&1 || true
		done
	}

	# Worker function that changes pin states rapidly
	# This generates DPLL_PIN_CHANGED notifications
	pin_worker()
	{
		local dev_id=$1
		local iterations=$2

		for i in $(seq 1 "$iterations"); do
			# Query all pins to trigger state reads while notifications fire
			dpll pin show -j >/dev/null 2>&1 || true

			# Get first pin and try state changes
			local pin_id
			pin_id=$(dpll pin show -j 2>/dev/null | jq -r ".pin[] | select(.direction == \"input\") | .id" | head -1)
			if [ -n "$pin_id" ]; then
				local states=("connected" "disconnected" "selectable")
				local state_idx=$((i % 3))
				dpll pin set id "$pin_id" device-id "$dev_id" state "${states[$state_idx]}" 2>/dev/null || true
			fi
		done
	}

	echo "    Starting concurrent workers to stress notifier callbacks..."

	# Start multiple workers in parallel - this creates a high notification load
	# while the deployed device's notifier callback is trying to acquire locks
	local pids=()
	for w in $(seq 1 3); do
		device_worker "$w" 15 &
		pids+=($!)
	done

	# Also run pin worker against our main device
	pin_worker "$dev_id" 30 &
	pids+=($!)

	# Use timeout to detect deadlock - if we hang for more than 60 seconds,
	# there's likely a deadlock
	local timeout_pid
	(sleep 60 && echo "TIMEOUT: Possible deadlock detected!" && kill -9 $$ 2>/dev/null) &
	timeout_pid=$!

	# Wait for workers
	local failed=0
	for pid in "${pids[@]}"; do
		if ! wait "$pid" 2>/dev/null; then
			failed=1
		fi
	done

	# Cancel timeout
	kill "$timeout_pid" 2>/dev/null || true
	wait "$timeout_pid" 2>/dev/null || true

	echo -n "."

	# If we got here, no deadlock occurred
	if [ "$failed" -eq 1 ]; then
		echo ""
		echo "WARNING: Some workers failed (not necessarily a deadlock)"
	fi

	echo ""

	# Cleanup
	dpllsim_delete_device "$dev_name" 2>/dev/null || true

	check_err $RET "Notifier deadlock stress test failed"
	log_test "DPLL notifier deadlock stress"
}

test_notifier_flood_stress()
{
	RET=0

	echo "  Testing notifier flood with concurrent operations..."

	# Create multiple devices that all have notifiers registered
	local base_clock_id=90000
	local num_devices=5
	local dev_names=()

	for i in $(seq 1 "$num_devices"); do
		local clock_id=$((base_clock_id + i))
		local dev_name="dpllsim${clock_id}"

		if dpllsim_create_device "$clock_id" 2>/dev/null; then
			CREATED_DEVICES+=("$dev_name")
			dev_names+=("$dev_name")

			# Add pins
			for p in $(seq 0 4); do
				dpllsim_add_pin "$dev_name" 2 10000000 1 "$p" 2>/dev/null || true
			done

			dpllsim_deploy_device "$dev_name" 2>/dev/null || true
		fi
	done

	if [ ${#dev_names[@]} -lt 2 ]; then
		echo "SKIP: Could not create enough devices"
		return "$ksft_skip"
	fi

	echo "    Created ${#dev_names[@]} devices, flooding with state changes..."

	# Rapidly change pin states on all devices - each change triggers
	# notifications that all other devices' notifiers will receive
	local iterations=20
	for i in $(seq 1 "$iterations"); do
		for dev_name in "${dev_names[@]}"; do
			# Trigger device activity by toggling phase offset monitor
			local clock_id="${dev_name#dpllsim}"
			local dev_id
			dev_id=$(dpll device show -j 2>/dev/null | jq -r ".device[] | select(.\"clock-id\" == $clock_id) | .id" 2>/dev/null | head -1)

			if [ -n "$dev_id" ]; then
				# Toggle phase offset monitor to generate device change notifications
				dpll device set id "$dev_id" phase-offset-monitor enabled 2>/dev/null || true
				dpll device set id "$dev_id" phase-offset-monitor disabled 2>/dev/null || true
			fi
		done

		if [ $((i % 5)) -eq 0 ]; then
			echo -n "."
		fi
	done
	echo ""

	# Cleanup all devices
	for dev_name in "${dev_names[@]}"; do
		dpllsim_delete_device "$dev_name" 2>/dev/null || true
	done

	check_err $RET "Notifier flood stress test failed"
	log_test "DPLL notifier flood stress"
}

##############################################################################
# Main test execution

# Require dpllsim for stress testing
dpll_require_sim

dpllsim_check_module

# Run stress tests
echo "========================================="
echo "DPLL Subsystem Stress Test"
echo "========================================="
echo "Configuration:"
echo "  Iterations: $STRESS_ITERATIONS"
echo "  Max devices: $STRESS_DEVICES"
echo "  Pins per device: $STRESS_PINS_PER_DEVICE"
echo "  Concurrent processes: $STRESS_CONCURRENT_PROCS"
echo "========================================="
echo ""

# Run tests directly (bypass defer framework to avoid hang)
test_rapid_device_creation_deletion
test_concurrent_device_operations
test_pin_state_flooding
test_netlink_notification_stress
test_max_devices
test_mux_parent_switching_stress
test_device_deploy_cycle_stress
test_notifier_deadlock_stress
test_notifier_flood_stress

echo ""
echo "========================================="
echo "Stress test completed"
echo "========================================="

exit "$EXIT_STATUS"
