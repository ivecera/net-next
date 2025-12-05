#!/bin/bash
# Quick manual test script for DPLL ref_tracker with dpllsim
#
# Usage: sudo ./test_reftracker.sh
#
# This script creates a dpllsim device with MUX pin hierarchy and
# tests the reference tracking functionality.
#
# ref_tracker debugfs files:
#   /sys/kernel/debug/ref_tracker/dpll_device@<ptr>  - device references
#   /sys/kernel/debug/ref_tracker/dpll_pin@<ptr>     - pin references
#
# When read (cat), these files show stack traces of ACTIVE (unreleased)
# references. Empty file = all references properly released.
# Non-empty file after cleanup = LEAK with stack trace showing where
# the reference was acquired but not released.
#
# Required kernel config:
#   CONFIG_REF_TRACKER=y
#   CONFIG_DPLL_REFCNT_TRACKER=y
#   CONFIG_DEBUG_FS=y
#   CONFIG_STACKTRACE=y (for meaningful stack traces)

set -e

SYSFS_BUS="/sys/bus/dpllsim"
REF_TRACKER_DIR="/sys/kernel/debug/ref_tracker"
CLOCK_ID=12345

# Pin types (from dpll.h)
DPLL_PIN_TYPE_MUX=1
DPLL_PIN_TYPE_EXT=2
DPLL_PIN_TYPE_SYNCE_ETH_PORT=3
DPLL_PIN_TYPE_INT_OSCILLATOR=4
DPLL_PIN_TYPE_GNSS=5

# Pin directions (from dpll.h UAPI)
DPLL_PIN_DIRECTION_INPUT=1
DPLL_PIN_DIRECTION_OUTPUT=2

echo "=== DPLL ref_tracker test with dpllsim ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "This script must be run as root (use sudo)"
    exit 1
fi

# Check if dpllsim module is loaded, load if not
if ! lsmod | grep -q dpllsim; then
    echo "Loading dpllsim module..."
    modprobe dpllsim || { echo "FAIL: Cannot load dpllsim"; exit 1; }
    sleep 0.3
fi

# Check if sysfs bus exists
if [ ! -d "$SYSFS_BUS" ]; then
    echo "FAIL: dpllsim bus not found at $SYSFS_BUS"
    exit 1
fi

# Check if ref_tracker is available
if [ -d "$REF_TRACKER_DIR" ]; then
    echo "ref_tracker debugfs: AVAILABLE"
    echo "  Looking for DPLL entries..."
    ls -la "$REF_TRACKER_DIR"/dpll* 2>/dev/null || echo "  (none yet)"
else
    echo "ref_tracker debugfs: NOT AVAILABLE"
    echo "  Enable CONFIG_REF_TRACKER=y and CONFIG_DPLL_REFCNT_TRACKER=y"
fi

echo ""

# Cleanup any existing test device
if [ -d "$SYSFS_BUS/devices/dpllsim0" ]; then
    echo "Cleaning up existing dpllsim0..."
    if [ "$(cat $SYSFS_BUS/devices/dpllsim0/deployed 2>/dev/null)" = "1" ]; then
        echo 0 > "$SYSFS_BUS/devices/dpllsim0/deploy"
        sleep 0.2
    fi
    echo 0 > "$SYSFS_BUS/del_device" 2>/dev/null || true
    sleep 0.2
fi

echo "Creating dpllsim device with clock_id=$CLOCK_ID..."
echo $CLOCK_ID > "$SYSFS_BUS/new_device"

# Wait for device to be created
DEV_PATH="$SYSFS_BUS/devices/dpllsim0"
for retry in $(seq 1 10); do
    if [ -d "$DEV_PATH" ]; then
        break
    fi
    sleep 0.1
done

if [ ! -d "$DEV_PATH" ]; then
    echo "FAIL: Device not created"
    exit 1
fi

echo "Device created at $DEV_PATH"
echo ""

# Setup pins - similar to dpllsim-setup-default.sh but simpler
echo "Setting up pins..."

# Parent MUX pins (must be type MUX for pin-on-pin to work)
echo "  Pin 0: MUX parent - GNSS ref (type=$DPLL_PIN_TYPE_MUX, freq=1, dir=$DPLL_PIN_DIRECTION_INPUT, prio=0)"
echo "$DPLL_PIN_TYPE_MUX 1 $DPLL_PIN_DIRECTION_INPUT 0" > "$DEV_PATH/add_pin"

echo "  Pin 1: MUX parent - SMA 10MHz (type=$DPLL_PIN_TYPE_MUX, freq=10000000, dir=$DPLL_PIN_DIRECTION_INPUT, prio=1)"
echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 1" > "$DEV_PATH/add_pin"

echo "  Pin 2: MUX parent - SyncE port 0 (type=$DPLL_PIN_TYPE_MUX, freq=25000000, dir=$DPLL_PIN_DIRECTION_INPUT, prio=2)"
echo "$DPLL_PIN_TYPE_MUX 25000000 $DPLL_PIN_DIRECTION_INPUT 2" > "$DEV_PATH/add_pin"

echo "  Pin 3: MUX parent - SyncE port 1 (type=$DPLL_PIN_TYPE_MUX, freq=25000000, dir=$DPLL_PIN_DIRECTION_INPUT, prio=3)"
echo "$DPLL_PIN_TYPE_MUX 25000000 $DPLL_PIN_DIRECTION_INPUT 3" > "$DEV_PATH/add_pin"

echo "  Pin 4: MUX parent - SyncE port 2 (type=$DPLL_PIN_TYPE_MUX, freq=25000000, dir=$DPLL_PIN_DIRECTION_INPUT, prio=4)"
echo "$DPLL_PIN_TYPE_MUX 25000000 $DPLL_PIN_DIRECTION_INPUT 4" > "$DEV_PATH/add_pin"

echo "  Pin 5: MUX parent - SyncE port 3 (type=$DPLL_PIN_TYPE_MUX, freq=25000000, dir=$DPLL_PIN_DIRECTION_INPUT, prio=5)"
echo "$DPLL_PIN_TYPE_MUX 25000000 $DPLL_PIN_DIRECTION_INPUT 5" > "$DEV_PATH/add_pin"

echo "  Pin 6: MUX parent - Internal osc (type=$DPLL_PIN_TYPE_MUX, freq=25000000, dir=$DPLL_PIN_DIRECTION_INPUT, prio=255)"
echo "$DPLL_PIN_TYPE_MUX 25000000 $DPLL_PIN_DIRECTION_INPUT 255" > "$DEV_PATH/add_pin"

# Child pins with parents (pin-on-pin hierarchy)
echo "  Pin 7: Child MUX with parents 2,3,4,5 - SyncE recovered clock"
echo "$DPLL_PIN_TYPE_MUX 1953125 $DPLL_PIN_DIRECTION_INPUT 100 parents:2,3,4,5" > "$DEV_PATH/add_pin"

echo "  Pin 8: Child MUX with parents 0,1 - Reference selector"
echo "$DPLL_PIN_TYPE_MUX 10000000 $DPLL_PIN_DIRECTION_INPUT 110 parents:0,1" > "$DEV_PATH/add_pin"

echo "  Pin 9: Child EXT with parents 7,8,6 - Hierarchical MUX (MUX of MUXes)"
echo "$DPLL_PIN_TYPE_EXT 10000000 $DPLL_PIN_DIRECTION_INPUT 200 parents:7,8,6" > "$DEV_PATH/add_pin"

# Output pins
echo "  Pin 10: Output - SyncE Ethernet (type=$DPLL_PIN_TYPE_SYNCE_ETH_PORT)"
echo "$DPLL_PIN_TYPE_SYNCE_ETH_PORT 25000000 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"

echo "  Pin 11: Output - 1PPS timing (type=$DPLL_PIN_TYPE_EXT)"
echo "$DPLL_PIN_TYPE_EXT 1 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"

echo "  Pin 12: Output - 10MHz external (type=$DPLL_PIN_TYPE_EXT)"
echo "$DPLL_PIN_TYPE_EXT 10000000 $DPLL_PIN_DIRECTION_OUTPUT 0" > "$DEV_PATH/add_pin"

echo ""
echo "Pin setup complete: 7 parent MUX + 3 child MUX hierarchy + 3 output = 13 pins"
echo ""

echo "Deploying device..."
echo 1 > "$DEV_PATH/deploy"
sleep 0.5

echo ""
echo "Device deployed. Checking DPLL subsystem..."

# Show device info
if command -v dpll &> /dev/null; then
    dpll device show 2>/dev/null || echo "  (no devices found)"
    echo ""
    echo "Pins:"
    dpll pin show 2>/dev/null | head -80 || echo "  (no pins found)"
else
    echo "  (dpll tool not available)"
fi
echo ""

# Check ref_tracker status - files contain stack traces for active references
if [ -d "$REF_TRACKER_DIR" ]; then
    echo "ref_tracker entries after deployment:"
    echo ""
    for entry in "$REF_TRACKER_DIR"/dpll*; do
        if [ -f "$entry" ]; then
            name=$(basename "$entry")
            echo "=== $name ==="
            content=$(cat "$entry" 2>/dev/null)
            if [ -n "$content" ]; then
                echo "$content"
            else
                echo "  (no active references - all properly released)"
            fi
            echo ""
        fi
    done
    if ! ls "$REF_TRACKER_DIR"/dpll* &>/dev/null; then
        echo "  (no DPLL ref_tracker entries found)"
    fi
fi

echo ""
echo "Press Enter to undeploy and check for leaks, or Ctrl+C to keep running..."
read

echo ""
echo "Undeploying device..."
echo 0 > "$DEV_PATH/deploy"
sleep 0.3

echo "Deleting device..."
echo 0 > "$SYSFS_BUS/del_device"
sleep 0.2

echo ""
echo "=== Final ref_tracker check (after cleanup) ==="
if [ -d "$REF_TRACKER_DIR" ]; then
    found_leaks=0
    for entry in "$REF_TRACKER_DIR"/dpll*; do
        if [ -f "$entry" ]; then
            name=$(basename "$entry")
            content=$(cat "$entry" 2>/dev/null)
            if [ -n "$content" ]; then
                found_leaks=1
                echo ""
                echo "!!! LEAK DETECTED in $name !!!"
                echo "Stack traces of unreleased references:"
                echo "$content"
            fi
        fi
    done

    if [ "$found_leaks" -eq 0 ]; then
        echo ""
        echo "SUCCESS: No reference leaks detected!"
        echo "(All ref_tracker files are empty - references properly released)"
    else
        echo ""
        echo "WARNING: Reference leaks detected - check stack traces above"
    fi
else
    echo "ref_tracker not available - cannot check for leaks"
fi

echo ""
echo "Test complete."
