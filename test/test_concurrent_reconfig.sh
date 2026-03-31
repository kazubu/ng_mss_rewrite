#!/bin/sh
#
# Test ng_mss_rewrite concurrent reconfiguration
#
# This tests the module's behavior when configuration changes happen
# while packets are being processed. Critical for production stability.
#
# Tests:
# 1. Changing MSS values while packets are flowing
# 2. Changing direction settings while packets are flowing
# 3. Changing stats mode while packets are flowing
# 4. Multiple rapid reconfigurations
# 5. Reset stats while packets are flowing
#

if [ "$(id -u)" != "0" ]; then
	echo "Must run as root"
	exit 1
fi

# Determine script directory and change to it
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
cd "$SCRIPT_DIR" || {
	echo "ERROR: Failed to change to script directory: $SCRIPT_DIR"
	exit 1
}

# Source test helper functions
. "${SCRIPT_DIR}/test_helpers.sh"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

TESTS_PASSED=0
TESTS_FAILED=0
TESTS_TOTAL=0

# Cleanup function
cleanup() {
	echo ""
	echo "Cleaning up..."
	# Kill background packet sender if running
	if [ -n "$SENDER_PID" ]; then
		kill $SENDER_PID 2>/dev/null || true
		wait $SENDER_PID 2>/dev/null || true
	fi
	# Kill builder process if running
	if [ -n "$BUILDER_PID" ]; then
		kill $BUILDER_PID 2>/dev/null || true
		wait $BUILDER_PID 2>/dev/null || true
	fi
	# Shutdown nodes
	ngctl msg reconfig_source: clrdata 2>/dev/null || true
	ngctl shutdown reconfig_source: 2>/dev/null || true
	ngctl shutdown reconfig_mss: 2>/dev/null || true
	ngctl shutdown reconfig_hole: 2>/dev/null || true
	# Kill any stray processes
	pkill -f "ng_builder_generic reconfig" 2>/dev/null || true
}

# Trap cleanup on exit
trap cleanup EXIT INT TERM

# Test result tracking
pass_test() {
	TESTS_PASSED=$((TESTS_PASSED + 1))
	TESTS_TOTAL=$((TESTS_TOTAL + 1))
	printf "${GREEN}[PASS]${NC} %s\n" "$1"
}

fail_test() {
	TESTS_FAILED=$((TESTS_FAILED + 1))
	TESTS_TOTAL=$((TESTS_TOTAL + 1))
	printf "${RED}[FAIL]${NC} %s\n" "$1"
}

echo "========================================"
echo " ng_mss_rewrite Concurrent Reconfig Tests"
echo "========================================"
echo ""

# Check if module is loaded
if ! kldstat | grep -q ng_mss_rewrite; then
	echo "Loading ng_mss_rewrite..."
	if ! kldload ../src/ng_mss_rewrite.ko; then
		echo "ERROR: Failed to load ng_mss_rewrite"
		exit 1
	fi
fi

# Load ng_source if not loaded
if ! kldstat | grep -q ng_source; then
	echo "Loading ng_source..."
	kldload ng_source 2>/dev/null || true
fi

echo "Modules loaded successfully"
echo ""

# Build topology: ng_source -> mss_rewrite:lower -> mss:upper -> hole
echo "Building reconfig test topology..."
cleanup

# Build ng_builder_generic if needed
if [ ! -f "./ng_builder_generic" ]; then
	echo "Building ng_builder_generic..."
	make ng_builder_generic || {
		echo "ERROR: Failed to build ng_builder_generic"
		exit 1
	}
fi

# Start topology builder in background
echo "Starting topology builder (reconfig)..."
./ng_builder_generic reconfig > /tmp/ng_builder_reconfig.log 2>&1 &
BUILDER_PID=$!

# Wait for topology to be created
if ! wait_for_nodes 5 reconfig_source: reconfig_mss: reconfig_hole:; then
	echo "ERROR: Timeout waiting for topology nodes to be created"
	kill $BUILDER_PID 2>/dev/null || true
	cat /tmp/ng_builder_reconfig.log
	echo "Available nodes:"
	ngctl list
	exit 1
fi

# Check if builder process is still running
if ! kill -0 $BUILDER_PID 2>/dev/null; then
	echo "ERROR: ng_builder_generic process died unexpectedly"
	wait $BUILDER_PID
	exit 1
fi
echo "Builder process running (PID: $BUILDER_PID)"

echo "Topology created successfully!"
echo ""

# Configure mss_rewrite with initial values
echo "Configuring mss_rewrite (MSS=1400/1380)..."
ngctl msg reconfig_mss: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"
ngctl msg reconfig_mss: setstatsmode "{ mode=1 }"
ngctl msg reconfig_mss: setdirection "{ enable_lower=1 enable_upper=0 }"
ngctl msg reconfig_mss: resetstats

echo "Configuration complete!"
echo ""

# ==================================
# Test 1: Change MSS while packets are flowing
# ==================================
echo "=========================================="
echo "  Test 1: Change MSS during packet flow"
echo "=========================================="

# Send 10 packets at moderate speed
ngctl msg reconfig_source: setpps 100 2>/dev/null || true

# Get test packet
TEST_PACKET="ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 2c 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 00 00 00 00 02 04 05 b4"

# Load packet into ng_source
echo "$TEST_PACKET" | perl -pe 's/(..)[ \t\n]*/chr(hex($1))/ge' | nghook reconfig_source: input 2>/dev/null

# Start sending packets in background
ngctl msg reconfig_source: start 10 2>/dev/null &
SENDER_PID=$!

# Wait a bit for packets to start flowing
sleep 0.05

# Change MSS values multiple times while packets are flowing
for new_mss in 1300 1200 1350 1450 1400; do
	ngctl msg reconfig_mss: setmss "{ mss_ipv4=$new_mss mss_ipv6=$new_mss }" 2>/dev/null
done

# Wait for packets to finish
wait $SENDER_PID 2>/dev/null
SENDER_PID=""

# Check that module is still alive
if kldstat | grep -q ng_mss_rewrite && ngctl msg reconfig_mss: getstats >/dev/null 2>&1; then
	pass_test "MSS reconfig during traffic (module stable)"
else
	fail_test "MSS reconfig during traffic (module crashed or unresponsive)"
fi

echo ""

# ==================================
# Test 2: Change direction settings during traffic
# ==================================
echo "=========================================="
echo "  Test 2: Change direction during traffic"
echo "=========================================="

ngctl msg reconfig_mss: resetstats
echo "$TEST_PACKET" | perl -pe 's/(..)[ \t\n]*/chr(hex($1))/ge' | nghook reconfig_source: input 2>/dev/null
ngctl msg reconfig_source: start 10 2>/dev/null &
SENDER_PID=$!

sleep 0.05

# Toggle direction settings  rapidly
for i in 1 2 3; do
	ngctl msg reconfig_mss: setdirection "{ enable_lower=1 enable_upper=0 }" 2>/dev/null
	ngctl msg reconfig_mss: setdirection "{ enable_lower=0 enable_upper=1 }" 2>/dev/null
	ngctl msg reconfig_mss: setdirection "{ enable_lower=1 enable_upper=1 }" 2>/dev/null
	ngctl msg reconfig_mss: setdirection "{ enable_lower=0 enable_upper=0 }" 2>/dev/null
done

wait $SENDER_PID 2>/dev/null
SENDER_PID=""

if kldstat | grep -q ng_mss_rewrite && ngctl msg reconfig_mss: getstats >/dev/null 2>&1; then
	pass_test "Direction reconfig during traffic (module stable)"
else
	fail_test "Direction reconfig during traffic (module crashed or unresponsive)"
fi

echo ""

# ==================================
# Test 3: Change stats mode during traffic
# ==================================
echo "=========================================="
echo "  Test 3: Change stats mode during traffic"
echo "=========================================="

ngctl msg reconfig_mss: resetstats
ngctl msg reconfig_mss: setdirection "{ enable_lower=1 enable_upper=0 }"
echo "$TEST_PACKET" | perl -pe 's/(..)[ \t\n]*/chr(hex($1))/ge' | nghook reconfig_source: input 2>/dev/null
ngctl msg reconfig_source: start 10 2>/dev/null &
SENDER_PID=$!

sleep 0.05

# Toggle stats mode rapidly
for i in 1 2 3; do
	ngctl msg reconfig_mss: setstatsmode "{ mode=1 }" 2>/dev/null
	ngctl msg reconfig_mss: setstatsmode "{ mode=0 }" 2>/dev/null
done

wait $SENDER_PID 2>/dev/null
SENDER_PID=""

if kldstat | grep -q ng_mss_rewrite && ngctl msg reconfig_mss: getstats >/dev/null 2>&1; then
	pass_test "Stats mode reconfig during traffic (module stable)"
else
	fail_test "Stats mode reconfig during traffic (module crashed or unresponsive)"
fi

echo ""

# ==================================
# Test 4: Reset stats during traffic
# ==================================
echo "=========================================="
echo "  Test 4: Reset stats during traffic"
echo "=========================================="

ngctl msg reconfig_mss: setdirection "{ enable_lower=1 enable_upper=0 }"
echo "$TEST_PACKET" | perl -pe 's/(..)[ \t\n]*/chr(hex($1))/ge' | nghook reconfig_source: input 2>/dev/null
ngctl msg reconfig_source: start 10 2>/dev/null &
SENDER_PID=$!

sleep 0.05

# Reset stats multiple times
for i in 1 2 3; do
	ngctl msg reconfig_mss: resetstats 2>/dev/null
done

wait $SENDER_PID 2>/dev/null
SENDER_PID=""

if kldstat | grep -q ng_mss_rewrite && ngctl msg reconfig_mss: getstats >/dev/null 2>&1; then
	pass_test "Reset stats during traffic (module stable)"
else
	fail_test "Reset stats during traffic (module crashed or unresponsive)"
fi

echo ""

# ==================================
# Test 5: All reconfigs simultaneously
# ==================================
echo "=========================================="
echo "  Test 5: All reconfigs simultaneously"
echo "=========================================="

ngctl msg reconfig_mss: resetstats
echo "$TEST_PACKET" | perl -pe 's/(..)[ \t\n]*/chr(hex($1))/ge' | nghook reconfig_source: input 2>/dev/null
ngctl msg reconfig_source: start 10 2>/dev/null &
SENDER_PID=$!

sleep 0.05

# Do everything at once
for i in 1 2 3; do
	ngctl msg reconfig_mss: setmss "{ mss_ipv4=$((1200 + i * 100)) mss_ipv6=$((1200 + i * 100)) }" 2>/dev/null
	ngctl msg reconfig_mss: setdirection "{ enable_lower=$((i % 2)) enable_upper=$(((i + 1) % 2)) }" 2>/dev/null
	ngctl msg reconfig_mss: setstatsmode "{ mode=$((i % 2)) }" 2>/dev/null
	ngctl msg reconfig_mss: resetstats 2>/dev/null
done

wait $SENDER_PID 2>/dev/null
SENDER_PID=""

if kldstat | grep -q ng_mss_rewrite && ngctl msg reconfig_mss: getstats >/dev/null 2>&1; then
	pass_test "Simultaneous reconfigs during traffic (module stable)"
else
	fail_test "Simultaneous reconfigs during traffic (module crashed or unresponsive)"
fi

echo ""

# ==================================
# Test 6: Verify data integrity after reconfigs
# ==================================
echo "=========================================="
echo "  Test 6: Verify data integrity"
echo "=========================================="

# Set known config
ngctl msg reconfig_mss: setmss "{ mss_ipv4=1300 mss_ipv6=1300 }"
ngctl msg reconfig_mss: setdirection "{ enable_lower=1 enable_upper=1 }"
ngctl msg reconfig_mss: setstatsmode "{ mode=1 }"
sleep 0.05
ngctl msg reconfig_mss: resetstats
sleep 0.05

# Send exactly 10 packets
echo "$TEST_PACKET" | perl -pe 's/(..)[ \t\n]*/chr(hex($1))/ge' | nghook reconfig_source: input 2>/dev/null
ngctl msg reconfig_source: start 10 2>/dev/null &
SENDER_PID=$!
sleep 0.1

# Wait for packets to be processed
if ! wait_for_packets_processed reconfig_mss: 10 3; then
	fail_test "Data integrity check (timeout waiting for packets)"
else
	# Get stats
	STATS=$(ngctl msg reconfig_mss: getstats 2>&1)
	PROCESSED=$(echo "$STATS" | grep -o 'packets_processed=[0-9]*' | cut -d= -f2)
	REWRITTEN=$(echo "$STATS" | grep -o 'packets_rewritten=[0-9]*' | cut -d= -f2)

	if [ "$PROCESSED" -eq 10 ] && [ "$REWRITTEN" -eq 10 ]; then
		pass_test "Data integrity check (processed=$PROCESSED, rewritten=$REWRITTEN)"
	else
		fail_test "Data integrity check (expected 10/10, got $PROCESSED/$REWRITTEN)"
	fi
fi

echo ""

# ==================================
# Summary
# ==================================
echo "=========================================="
echo "  Test Summary"
echo "=========================================="
echo "Total:  $TESTS_TOTAL"
echo "Passed: ${GREEN}$TESTS_PASSED${NC}"
echo "Failed: ${RED}$TESTS_FAILED${NC}"
echo ""

if [ "$TESTS_FAILED" -eq 0 ]; then
	echo "${GREEN}All tests passed!${NC}"
	exit 0
else
	echo "${RED}Some tests failed!${NC}"
	exit 1
fi
