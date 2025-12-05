#!/bin/bash
# Test script for netdevsim <-> dpllsim integration
#
# Usage: sudo ./test_nsim_dpll.sh
#
# This script demonstrates how to attach a netdevsim port to a dpllsim
# DPLL device, making the network interface appear as a SyncE source.

DPLLSIM_BUS="/sys/bus/dpllsim"
NETDEVSIM_BUS="/sys/bus/netdevsim"
CLOCK_ID=12345

# Track what we created for cleanup
NETDEVSIM_CREATED=0
DPLLSIM_CREATED=0
DPLLSIM_DEPLOYED=0
DPLL_ATTACHED=0

cleanup() {
    echo ""
    echo "Cleaning up..."

    # Detach from DPLL if attached
    if [ "$DPLL_ATTACHED" -eq 1 ]; then
        echo "  Detaching from DPLL..."
        echo "none" > "$DEBUGFS_PORT/dpll/attach" 2>/dev/null || true
    fi

    # Delete netdevsim if created
    if [ "$NETDEVSIM_CREATED" -eq 1 ]; then
        echo "  Removing netdevsim device..."
        echo 0 > "$NETDEVSIM_BUS/del_device" 2>/dev/null || true
        sleep 0.1
    fi

    # Undeploy and delete dpllsim if created
    if [ "$DPLLSIM_DEPLOYED" -eq 1 ]; then
        echo "  Undeploying dpllsim..."
        echo 0 > "$DPLLSIM_BUS/devices/dpllsim0/deploy" 2>/dev/null || true
        sleep 0.1
    fi

    if [ "$DPLLSIM_CREATED" -eq 1 ]; then
        echo "  Removing dpllsim device..."
        echo 0 > "$DPLLSIM_BUS/del_device" 2>/dev/null || true
    fi

    echo "Cleanup done."
}

# Trap for cleanup on exit (both success and failure)
trap cleanup EXIT

# Exit on error
set -e

echo "=== netdevsim <-> dpllsim integration test ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "This script must be run as root (use sudo)"
    exit 1
fi

# Load modules
echo "Loading modules..."
modprobe dpllsim 2>/dev/null || true
modprobe netdevsim 2>/dev/null || true
sleep 0.3

# Check modules
if [ ! -d "$DPLLSIM_BUS" ]; then
    echo "FAIL: dpllsim bus not found"
    exit 1
fi

if [ ! -d "$NETDEVSIM_BUS" ]; then
    echo "FAIL: netdevsim bus not found"
    exit 1
fi

echo "  dpllsim: OK"
echo "  netdevsim: OK"
echo ""

# Clean up any existing devices from previous runs
echo "Cleaning up existing devices..."
if [ -d "$DPLLSIM_BUS/devices/dpllsim0" ]; then
    if [ "$(cat $DPLLSIM_BUS/devices/dpllsim0/deployed 2>/dev/null)" = "1" ]; then
        echo 0 > "$DPLLSIM_BUS/devices/dpllsim0/deploy" 2>/dev/null || true
        sleep 0.2
    fi
    echo 0 > "$DPLLSIM_BUS/del_device" 2>/dev/null || true
    sleep 0.2
fi

echo 0 > "$NETDEVSIM_BUS/del_device" 2>/dev/null || true
sleep 0.2

# Create dpllsim device
echo ""
echo "Creating dpllsim device with clock_id=$CLOCK_ID..."
echo $CLOCK_ID > "$DPLLSIM_BUS/new_device"
DPLLSIM_CREATED=1
sleep 0.3

DPLL_DEV="$DPLLSIM_BUS/devices/dpllsim0"
if [ ! -d "$DPLL_DEV" ]; then
    echo "FAIL: dpllsim device not created"
    exit 1
fi

# Add a MUX input pin for SyncE recovered clock
echo "Adding MUX pin for SyncE recovered clock..."
# Type=1 (MUX), Freq=25MHz, Dir=1 (input), Prio=10
echo "1 25000000 1 10" > "$DPLL_DEV/add_pin"

# Deploy dpllsim
echo "Deploying dpllsim..."
echo 1 > "$DPLL_DEV/deploy"
DPLLSIM_DEPLOYED=1
sleep 0.5

echo "  dpllsim deployed, clock_id=$CLOCK_ID"
echo ""

# Create netdevsim device
echo "Creating netdevsim device..."
echo "0 1" > "$NETDEVSIM_BUS/new_device"
NETDEVSIM_CREATED=1
sleep 0.5

NSIM_DEV="$NETDEVSIM_BUS/devices/netdevsim0"
if [ ! -d "$NSIM_DEV" ]; then
    echo "FAIL: netdevsim device not created"
    exit 1
fi

# Find the network interface directly from netdevsim sysfs
# This works regardless of udev rename timing
NETDEV=$(ls /sys/bus/netdevsim/devices/netdevsim0/net/ 2>/dev/null | head -1)
if [ -z "$NETDEV" ]; then
    echo "FAIL: No netdevsim network interface found"
    echo "  Available interfaces: $(ls /sys/class/net/)"
    exit 1
fi

echo "  netdevsim created: $NETDEV"
echo ""

# Find debugfs path for the port
DEBUGFS_PORT="/sys/kernel/debug/netdevsim/netdevsim0/ports/0"
if [ ! -d "$DEBUGFS_PORT" ]; then
    echo "FAIL: debugfs port directory not found"
    echo "  Expected: $DEBUGFS_PORT"
    exit 1
fi

DPLL_ATTACH_FILE="$DEBUGFS_PORT/dpll/attach"
if [ ! -f "$DPLL_ATTACH_FILE" ]; then
    echo "FAIL: DPLL attach file not found"
    echo "  Expected: $DPLL_ATTACH_FILE"
    echo ""
    echo "Available files in debugfs:"
    find /sys/kernel/debug/netdevsim -type f 2>/dev/null | head -20
    exit 1
fi

# Read current state
echo "Current DPLL attachment:"
echo "  $(cat $DPLL_ATTACH_FILE)"
echo ""

# Parent pin ID - the MUX pin we created in dpllsim (pin index 0)
PARENT_PIN_ID=0

# Attach to dpllsim - use pin-on-pin registration
# Format: "clock_id parent_pin_id"
echo "Attaching $NETDEV to dpllsim (clock_id=$CLOCK_ID, parent_pin=$PARENT_PIN_ID)..."
echo "$CLOCK_ID $PARENT_PIN_ID" > "$DPLL_ATTACH_FILE"
DPLL_ATTACHED=1
sleep 0.3

# Verify attachment
echo ""
echo "After attachment:"
echo "  $(cat $DPLL_ATTACH_FILE)"
echo ""

# Show DPLL info
if command -v dpll &> /dev/null; then
    echo "DPLL subsystem status:"
    dpll device show 2>/dev/null || echo "  (no devices)"
    echo ""
    echo "DPLL pins:"
    dpll pin show 2>/dev/null | head -30 || echo "  (no pins)"
else
    echo "(dpll tool not available - install iproute2 with DPLL support)"
fi

echo ""
echo "Press Enter to cleanup, or Ctrl+C to exit (cleanup will run anyway)..."
read

echo ""
echo "Test complete."
