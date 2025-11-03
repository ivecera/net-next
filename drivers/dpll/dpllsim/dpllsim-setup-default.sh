#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# dpllsim-setup-default.sh - Configure dpllsim with default topology
#
# This script configures dpllsim to match the original platform_driver
# configuration: 2 DPLL devices with the default pin setup.
#
# Copyright (c) 2025 Red Hat, Inc.

set -e

# Check if running as root
if [ "$EUID" -ne 0 ]; then
	echo "This script must be run as root (use sudo)"
	exit 1
fi

DPLLSIM_BUS="/sys/bus/dpllsim"
DPLLSIM_DEVICES="/sys/bus/dpllsim/devices"
NUM_DEVICES=2

# Pin types (from dpll.h)
DPLL_PIN_TYPE_MUX=1
DPLL_PIN_TYPE_EXT=2
DPLL_PIN_TYPE_SYNCE_ETH_PORT=3
DPLL_PIN_TYPE_INT_OSCILLATOR=4
DPLL_PIN_TYPE_GNSS=5

# Pin directions (from dpll.h UAPI)
DPLL_PIN_DIRECTION_INPUT=1
DPLL_PIN_DIRECTION_OUTPUT=2

echo "Setting up dpllsim with default configuration..."

# Check if module is loaded
if [ ! -d "$DPLLSIM_BUS" ]; then
	echo "Error: dpllsim module not loaded (bus not found at $DPLLSIM_BUS)"
	echo "Load the module with: sudo modprobe dpllsim"
	exit 1
fi

# Create devices
for i in $(seq 0 $((NUM_DEVICES - 1))); do
	DEV_PATH="$DPLLSIM_DEVICES/dpllsim$i"

	# Skip if device already exists
	if [ -d "$DEV_PATH" ]; then
		echo "Device dpllsim$i already exists, skipping creation"
		continue
	fi

	echo "Creating dpllsim$i (clock_id=$i)..."
	echo $i > "$DPLLSIM_BUS/new_device"

	# Wait for device to be created
	for retry in $(seq 1 10); do
		if [ -d "$DEV_PATH" ]; then
			break
		fi
		sleep 0.1
	done

	if [ ! -d "$DEV_PATH" ]; then
		echo "Error: Device dpllsim$i was not created"
		exit 1
	fi

	# Check if add_pin exists
	if [ ! -f "$DEV_PATH/add_pin" ]; then
		echo "Error: add_pin sysfs attribute not found for dpllsim$i"
		exit 1
	fi

	# Add input pins (11 total: REF0-REF10)
	echo "  Adding input pins..."

	# Pin 0: GNSS 1PPS (MUX type for use as parent)
	echo "$DPLL_PIN_TYPE_MUX 1 $DPLL_PIN_DIRECTION_INPUT 0" > "$DEV_PATH/add_pin"

	# Pin 1: 10 MHz bidirectional (SMA1) (MUX type for use as parent)
	echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 1" > "$DEV_PATH/add_pin"

	# Pins 2-5: SyncE ports (MUX type for recovered clock)
	for j in $(seq 2 5); do
		prio=$j
		echo "$DPLL_PIN_TYPE_MUX 25000000 $DPLL_PIN_DIRECTION_INPUT $prio" > "$DEV_PATH/add_pin"
	done

	# Pin 6-7: Bidirectional SMA2, SMA3 (MUX type for use as parents)
	echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 6" > "$DEV_PATH/add_pin"
	echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 7" > "$DEV_PATH/add_pin"

	# Pins 8-9: Additional external references (MUX type for use as parents)
	echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 8" > "$DEV_PATH/add_pin"
	echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 9" > "$DEV_PATH/add_pin"

	# Pin 10: Internal oscillator (MUX type for use as parent)
	echo "$DPLL_PIN_TYPE_MUX 25000000 $DPLL_PIN_DIRECTION_INPUT 10" > "$DEV_PATH/add_pin"

	# Pin 11: Recovered clock child pin with MUX parents (SyncE pins 2-5, MUX type for hierarchical use)
	echo "  Adding MUX pins with parents..."
	echo "$DPLL_PIN_TYPE_MUX 1953125 $DPLL_PIN_DIRECTION_INPUT 255 parents:2,3,4,5" > "$DEV_PATH/add_pin"

	# Pin 12: MUX pin selecting between GNSS and SMA references (pins 0,1,6,7, MUX type for hierarchical use)
	echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 255 parents:0,1,6,7" > "$DEV_PATH/add_pin"

	# Pin 13: MUX pin selecting between internal oscillator and external refs (pins 8,9,10, MUX type for hierarchical use)
	echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 255 parents:8,9,10" > "$DEV_PATH/add_pin"

	# Pin 14: Two-level MUX hierarchy - selects between other MUX pins (pins 11,12,13)
	echo "  Adding hierarchical MUX pin (MUX of MUXes)..."
	echo "$DPLL_PIN_TYPE_EXT 10000000 $DPLL_PIN_DIRECTION_INPUT 255 parents:11,12,13" > "$DEV_PATH/add_pin"

	# Pin 15: MUX with single parent (degenerate case)
	echo "$DPLL_PIN_TYPE_EXT 25000000 $DPLL_PIN_DIRECTION_INPUT 255 parents:2" > "$DEV_PATH/add_pin"

	# Add output pins (20 total: OUT0P/N - OUT9P/N, 10 differential pairs)
	echo "  Adding output pins..."

	# Pairs 0-3: SyncE Ethernet outputs (25 MHz)
	for pair in $(seq 0 3); do
		echo "$DPLL_PIN_TYPE_SYNCE_ETH_PORT 25000000 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"
		echo "$DPLL_PIN_TYPE_SYNCE_ETH_PORT 25000000 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"
	done

	# Pairs 4-5: 1PPS timing outputs
	for pair in $(seq 4 5); do
		echo "$DPLL_PIN_TYPE_EXT 1 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"
		echo "$DPLL_PIN_TYPE_EXT 1 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"
	done

	# Pairs 6-9: General external outputs (10 MHz)
	for pair in $(seq 6 9); do
		echo "$DPLL_PIN_TYPE_EXT 10000000 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"
		echo "$DPLL_PIN_TYPE_EXT 10000000 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"
	done

	# Deploy device to DPLL subsystem
	echo "  Deploying dpllsim$i..."
	echo 1 > "$DEV_PATH/deploy"

	echo "Device dpllsim$i configured and deployed successfully"
done

echo ""
echo "Default configuration complete!"
echo "Created $NUM_DEVICES DPLL devices with complex topology:"
echo ""
echo "Input pins per device:"
echo "  - 11 regular input pins (GNSS, SMA, SyncE, internal oscillator)"
echo "  - 5 MUX pins demonstrating various hierarchies:"
echo "    * Pin 11: SyncE recovered clock (4 parents: pins 2-5)"
echo "    * Pin 12: Reference selector (4 parents: pins 0,1,6,7)"
echo "    * Pin 13: Oscillator selector (3 parents: pins 8,9,10)"
echo "    * Pin 14: Hierarchical MUX (3 MUX parents: pins 11,12,13)"
echo "    * Pin 15: Single-parent MUX (1 parent: pin 2)"
echo ""
echo "Output pins per device:"
echo "  - 20 output pins (10 differential pairs)"
echo "    * Pairs 0-3: SyncE Ethernet (25 MHz)"
echo "    * Pairs 4-5: 1PPS timing"
echo "    * Pairs 6-9: General external (10 MHz)"
echo ""
echo "Use 'dpll get' to view the complete topology"
