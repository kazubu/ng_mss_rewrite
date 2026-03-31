#!/bin/sh
#
# Test ng_mss_rewrite upper→lower direction
#
# This tests the critical aspects of upper hook processing:
# 1. Packets with CSUM_TCP flags should be skipped
# 2. Packets with CSUM_TSO flags should be skipped
# 3. Direction filtering (enable_upper/enable_lower)
# 4. Normal packets without offload flags are processed
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
	# Kill builder process if running
	if [ -n "$BUILDER_PID" ]; then
		kill $BUILDER_PID 2>/dev/null || true
		wait $BUILDER_PID 2>/dev/null || true
	fi
	# Shutdown nodes in reverse order
	ngctl shutdown mbuf_upper_hole: 2>/dev/null || true
	ngctl shutdown mbuf_upper_mss: 2>/dev/null || true
	ngctl shutdown mbuf_upper_inject: 2>/dev/null || true
	# Kill any stray processes
	pkill -f "ng_builder_generic mbuf_upper" 2>/dev/null || true
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

skip_test() {
	TESTS_TOTAL=$((TESTS_TOTAL + 1))
	printf "${YELLOW}[SKIP]${NC} %s\n" "$1"
}

# Helper function to run inject test and check results
# Usage: run_inject_test "params" expected_processed expected_rewritten "test_name"
run_inject_test() {
	local params=$1
	local expected_proc=$2
	local expected_rewr=$3
	local test_name=$4

	# Reset stats
	ngctl msg mbuf_upper_mss: resetstats >/dev/null 2>&1

	# Get inject stats before
	INJECT_BEFORE=$(ngctl msg mbuf_upper_inject: getstats 2>&1)
	SENT_BEFORE=$(echo "$INJECT_BEFORE" | grep -o 'packets_sent=[0-9]*' | cut -d= -f2)
	[ -z "$SENT_BEFORE" ] && SENT_BEFORE=0

	# Execute inject command
	ngctl msg mbuf_upper_inject: inject_single "$params" >/dev/null 2>&1

	# Poll for packet to be sent
	if ! wait_for_packet_sent mbuf_upper_inject: $SENT_BEFORE 1; then
		fail_test "$test_name (packet injection timed out)"
		return 1
	fi

	# Get stats after
	STATS=$(ngctl msg mbuf_upper_mss: getstats 2>&1)
	INJECT_AFTER=$(ngctl msg mbuf_upper_inject: getstats 2>&1)
	SENT_AFTER=$(echo "$INJECT_AFTER" | grep -o 'packets_sent=[0-9]*' | cut -d= -f2)
	[ -z "$SENT_AFTER" ] && SENT_AFTER=0

	# Verify packet was sent
	PACKETS_SENT=$((SENT_AFTER - SENT_BEFORE))
	if [ "$PACKETS_SENT" -ne 1 ]; then
		fail_test "$test_name (packet not sent: $PACKETS_SENT)"
		return 1
	fi

	# Parse counters
	PROCESSED=$(echo "$STATS" | grep -o 'packets_processed=[0-9]*' | cut -d= -f2)
	REWRITTEN=$(echo "$STATS" | grep -o 'packets_rewritten=[0-9]*' | cut -d= -f2)
	[ -z "$PROCESSED" ] && PROCESSED=0
	[ -z "$REWRITTEN" ] && REWRITTEN=0

	# Check results
	if [ "$PROCESSED" -eq "$expected_proc" ] && [ "$REWRITTEN" -eq "$expected_rewr" ]; then
		pass_test "$test_name (proc=$PROCESSED, rewr=$REWRITTEN)"
		return 0
	else
		fail_test "$test_name (expected proc=$expected_proc rewr=$expected_rewr, got proc=$PROCESSED rewr=$REWRITTEN)"
		return 1
	fi
}

echo "========================================"
echo " ng_mss_rewrite Upper Direction Tests"
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

# Load ng_mbuf_inject
if ! kldstat | grep -q ng_mbuf_inject; then
	echo "Loading ng_mbuf_inject..."
	if ! kldload ./ng_mbuf_inject.ko; then
		echo "ERROR: Failed to load ng_mbuf_inject"
		exit 1
	fi
fi

echo "Modules loaded successfully"
echo ""

# Build topology: mbuf_inject -> mss_rewrite:upper -> mss:lower -> hole
echo "Building upper direction test topology..."
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
echo "Starting topology builder (mbuf_upper)..."
./ng_builder_generic mbuf_upper &
BUILDER_PID=$!

# Wait for topology to be created
if ! wait_for_nodes 5 mbuf_upper_inject: mbuf_upper_mss: mbuf_upper_hole:; then
	echo "ERROR: Timeout waiting for topology nodes to be created"
	kill $BUILDER_PID 2>/dev/null || true
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
ngctl show mbuf_upper_inject: 2>&1 | head -10
echo ""

# Configure mss_rewrite
echo "Configuring mss_rewrite (MSS=1300/1280)..."
ngctl msg mbuf_upper_mss: setmss "{ mss_ipv4=1300 mss_ipv6=1280 }"

# Enable statistics
ngctl msg mbuf_upper_mss: setstatsmode "{ mode=1 }"

# Enable both directions for testing
ngctl msg mbuf_upper_mss: setdirection "{ enable_lower=1 enable_upper=1 }"

echo "Configuration complete!"
echo ""

# ==================================
# Test 1: Basic Upper Direction Processing
# ==================================
echo "=========================================="
echo "  Test 1: Basic Upper Direction (no offload)"
echo "=========================================="

# Normal packet without offload flags - should be processed and rewritten
run_inject_test "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" 1 1 "MSS=1460, no offload flags: processed and rewritten"

# Packet with MSS below threshold - should be processed but not rewritten
run_inject_test "{ mss=1200 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" 1 0 "MSS=1200, no offload flags: processed but not rewritten"

echo ""

# ==================================
# Test 2: CSUM_TCP Offload Flag
# ==================================
echo "=========================================="
echo "  Test 2: CSUM_TCP Offload (should skip)"
echo "=========================================="

# Packet with CSUM_IP_TCP flag (0x0004) - should be processed but not rewritten on upper hook
# FreeBSD defines: CSUM_IP_TCP = 0x00000004
# Note: packets_processed increments before offload check, so proc=1 is expected
run_inject_test "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0x0004 ext_type=0 }" 1 0 "MSS=1460 with CSUM_IP_TCP: processed but not rewritten (offload detected)"

echo ""

# ==================================
# Test 3: CSUM_TSO Offload Flag
# ==================================
echo "=========================================="
echo "  Test 3: CSUM_TSO Offload (should skip)"
echo "=========================================="

# Packet with CSUM_IP_TSO flag (0x0010) - should be processed but not rewritten on upper hook
# FreeBSD defines: CSUM_IP_TSO = 0x00000010
run_inject_test "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0x0010 ext_type=0 }" 1 0 "MSS=1460 with CSUM_IP_TSO: processed but not rewritten (offload detected)"

# Combined flags (CSUM_IP_TCP | CSUM_IP_TSO) - should also be processed but not rewritten
run_inject_test "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0x0014 ext_type=0 }" 1 0 "MSS=1460 with CSUM_IP_TCP|TSO: processed but not rewritten (offload detected)"

echo ""

# ==================================
# Test 4: Direction Filtering
# ==================================
echo "=========================================="
echo "  Test 4: Direction Filtering"
echo "=========================================="

# Disable upper direction - packets should be skipped
ngctl msg mbuf_upper_mss: setdirection "{ enable_lower=1 enable_upper=0 }"
run_inject_test "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" 0 0 "Upper disabled: packet skipped"

# Enable only upper direction - packets should be processed
ngctl msg mbuf_upper_mss: setdirection "{ enable_lower=0 enable_upper=1 }"
run_inject_test "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" 1 1 "Only upper enabled: packet processed"

# Disable both directions - packets should be skipped
ngctl msg mbuf_upper_mss: setdirection "{ enable_lower=0 enable_upper=0 }"
run_inject_test "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" 0 0 "Both disabled: packet skipped"

# Re-enable both for remaining tests
ngctl msg mbuf_upper_mss: setdirection "{ enable_lower=1 enable_upper=1 }"

echo ""

# ==================================
# Test 5: IPv6 Upper Direction
# ==================================
echo "=========================================="
echo "  Test 5: IPv6 Upper Direction"
echo "=========================================="

# IPv6 packet without offload flags
run_inject_test "{ mss=1460 ipv6=1 split_offset=0 csum_flags=0 ext_type=0 }" 1 1 "IPv6 MSS=1460, no offload: processed and rewritten"

# IPv6 packet with CSUM_IP6_TCP flag (0x0400) - should be processed but not rewritten
# FreeBSD defines: CSUM_IP6_TCP = 0x00000400
run_inject_test "{ mss=1460 ipv6=1 split_offset=0 csum_flags=0x0400 ext_type=0 }" 1 0 "IPv6 MSS=1460 with CSUM_IP6_TCP: processed but not rewritten (offload detected)"

echo ""

# ==================================
# Summary
# ==================================
echo "=========================================="
echo "  Test Summary"
echo "=========================================="
printf "Total:  $TESTS_TOTAL\n"
printf "Passed: ${GREEN}$TESTS_PASSED${NC}\n"
printf "Failed: ${RED}$TESTS_FAILED${NC}\n"
echo ""

if [ "$TESTS_FAILED" -eq 0 ]; then
	printf "${GREEN}All tests passed!${NC}\n"
	exit 0
else
	printf "${RED}Some tests failed!${NC}\n"
	exit 1
fi
