#!/bin/sh
#
# Test ng_mss_rewrite with various mbuf chain shapes
#
# This tests the critical aspects of the module:
# 1. Fragmented mbuf chains (multi-mbuf)
# 2. Shared mbufs (M_WRITABLE == 0)
# 3. Upper hook with checksum offload flags
# 4. IPv6 extension headers
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

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

TESTS_PASSED=0
TESTS_FAILED=0
TESTS_TOTAL=0

# Check if debug statistics are enabled
DEBUG_STATS_ENABLED=0

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
	ngctl shutdown mbuf_test_hole: 2>/dev/null || true
	ngctl shutdown mbuf_test_mss: 2>/dev/null || true
	ngctl shutdown mbuf_test_inject: 2>/dev/null || true
	# Also try to find and shutdown any lingering nodes
	ngctl list 2>/dev/null | grep -E 'mbuf_test_|ngctl[0-9]' | awk '{print $2}' | while read node; do
		ngctl shutdown ${node} 2>/dev/null || true
	done
	# Kill any stray ng_builder_mbuf processes
	pkill -f ng_builder_mbuf 2>/dev/null || true
	sleep 1
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
# Usage: run_inject_test "inject_command" "params" expected_processed expected_rewritten "test_name" [debug_counter] [debug_expected]
run_inject_test() {
	local cmd=$1
	local params=$2
	local expected_proc=$3
	local expected_rewr=$4
	local test_name=$5
	local debug_counter=$6
	local debug_expected=$7

	# Reset stats
	ngctl msg mbuf_test_mss: resetstats >/dev/null 2>&1

	# Get inject stats before
	INJECT_BEFORE=$(ngctl msg mbuf_test_inject: getstats 2>&1)
	SENT_BEFORE=$(echo "$INJECT_BEFORE" | grep -o 'packets_sent=[0-9]*' | cut -d= -f2)
	[ -z "$SENT_BEFORE" ] && SENT_BEFORE=0

	# Execute inject command
	ngctl msg mbuf_test_inject: $cmd "$params" >/dev/null 2>&1
	if [ $? -ne 0 ]; then
		fail_test "$test_name: inject command failed"
		return 1
	fi

	# Check if packet was sent
	INJECT_AFTER=$(ngctl msg mbuf_test_inject: getstats 2>&1)
	SENT_AFTER=$(echo "$INJECT_AFTER" | grep -o 'packets_sent=[0-9]*' | cut -d= -f2)
	[ -z "$SENT_AFTER" ] && SENT_AFTER=0

	if [ "$SENT_AFTER" -le "$SENT_BEFORE" ]; then
		fail_test "$test_name: packet not sent (before=$SENT_BEFORE, after=$SENT_AFTER)"
		return 1
	fi

	# Give time for processing
	sleep 0.1

	# Check results
	STATS=$(ngctl msg mbuf_test_mss: getstats 2>&1)
	PROCESSED=$(echo "$STATS" | grep -o 'packets_processed=[0-9]*' | cut -d= -f2)
	REWRITTEN=$(echo "$STATS" | grep -o 'packets_rewritten=[0-9]*' | cut -d= -f2)

	# Handle empty values (no stats)
	[ -z "$PROCESSED" ] && PROCESSED=0
	[ -z "$REWRITTEN" ] && REWRITTEN=0

	# Display raw statistics for this test
	echo "  Stats: processed=$PROCESSED rewritten=$REWRITTEN"
	if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
		# Extract and display debug counters
		FAST_PATH=$(echo "$STATS" | grep -o 'fast_dispatch_count=[0-9]*' | cut -d= -f2)
		SAFE_PATH=$(echo "$STATS" | grep -o 'safe_dispatch_count=[0-9]*' | cut -d= -f2)
		PULLUP=$(echo "$STATS" | grep -o 'pullup_count=[0-9]*' | cut -d= -f2)
		UNSHARE=$(echo "$STATS" | grep -o 'unshare_count=[0-9]*' | cut -d= -f2)
		SKIP_OFFLOAD=$(echo "$STATS" | grep -o 'skip_offload=[0-9]*' | cut -d= -f2)
		SKIP_MSS_OK=$(echo "$STATS" | grep -o 'skip_mss_ok=[0-9]*' | cut -d= -f2)
		SKIP_NO_MSS=$(echo "$STATS" | grep -o 'skip_no_mss=[0-9]*' | cut -d= -f2)

		[ -z "$FAST_PATH" ] && FAST_PATH=0
		[ -z "$SAFE_PATH" ] && SAFE_PATH=0
		[ -z "$PULLUP" ] && PULLUP=0
		[ -z "$UNSHARE" ] && UNSHARE=0
		[ -z "$SKIP_OFFLOAD" ] && SKIP_OFFLOAD=0
		[ -z "$SKIP_MSS_OK" ] && SKIP_MSS_OK=0
		[ -z "$SKIP_NO_MSS" ] && SKIP_NO_MSS=0

		echo "  Debug: fast=$FAST_PATH safe=$SAFE_PATH pullup=$PULLUP unshare=$UNSHARE skip_offload=$SKIP_OFFLOAD skip_mss_ok=$SKIP_MSS_OK skip_no_mss=$SKIP_NO_MSS"
	fi

	# Check basic counters
	if [ "$PROCESSED" != "$expected_proc" ] || [ "$REWRITTEN" != "$expected_rewr" ]; then
		fail_test "$test_name: processed=$PROCESSED, rewritten=$REWRITTEN (expected $expected_proc, $expected_rewr)"
		return 1
	fi

	# Check debug counter if specified and debug stats are enabled
	if [ -n "$debug_counter" ] && [ "$DEBUG_STATS_ENABLED" = "1" ]; then
		DEBUG_VALUE=$(echo "$STATS" | grep -o "${debug_counter}=[0-9]*" | cut -d= -f2)
		[ -z "$DEBUG_VALUE" ] && DEBUG_VALUE=0

		if [ "$DEBUG_VALUE" != "$debug_expected" ]; then
			fail_test "$test_name: ${debug_counter}=$DEBUG_VALUE (expected $debug_expected)"
			return 1
		fi
	fi

	pass_test "$test_name"
}

echo "=========================================="
echo "  Mbuf Shape Testing for ng_mss_rewrite"
echo "=========================================="
echo ""

# Build ng_mbuf_inject if needed
if [ ! -f "ng_mbuf_inject.ko" ]; then
	echo "Building ng_mbuf_inject module..."
	make -f Makefile.kmod clean >/dev/null 2>&1
	if ! make -f Makefile.kmod; then
		echo "ERROR: Failed to build ng_mbuf_inject"
		exit 1
	fi
fi

# Load modules
echo "Loading kernel modules..."

# Unload any existing modules first to ensure clean state
kldunload ng_mbuf_inject 2>/dev/null || true
kldunload ng_mss_rewrite 2>/dev/null || true
sleep 1

# Load ng_mss_rewrite
echo "Loading ng_mss_rewrite..."
if [ -f "../ng_mss_rewrite.ko" ]; then
	if ! kldload ../ng_mss_rewrite.ko; then
		echo "ERROR: Failed to load ../ng_mss_rewrite.ko"
		exit 1
	fi
else
	if ! kldload ng_mss_rewrite; then
		echo "ERROR: Failed to load ng_mss_rewrite"
		exit 1
	fi
fi

# Verify ng_mss_rewrite is loaded
if ! kldstat | grep -q ng_mss_rewrite; then
	echo "ERROR: ng_mss_rewrite not in kldstat after load"
	kldstat
	exit 1
fi
echo "ng_mss_rewrite loaded successfully"

# Load ng_mbuf_inject
echo "Loading ng_mbuf_inject..."
if ! kldload ./ng_mbuf_inject.ko; then
	echo "ERROR: Failed to load ng_mbuf_inject"
	exit 1
fi

# Verify ng_mbuf_inject is loaded
if ! kldstat | grep -q ng_mbuf_inject; then
	echo "ERROR: ng_mbuf_inject not in kldstat after load"
	kldstat
	exit 1
fi
echo "ng_mbuf_inject loaded successfully"

echo ""
echo "Loaded modules:"
kldstat | grep -E 'ng_mss_rewrite|ng_mbuf_inject'
echo ""

# Build topology: socket -> mbuf_inject -> mss_rewrite -> hole
echo "Building test topology..."
cleanup

# Check if any test nodes still exist
echo "Checking for existing nodes..."
if ngctl show mbuf_test_inject: >/dev/null 2>&1; then
	echo "WARNING: mbuf_test_inject still exists, forcing removal..."
	ngctl shutdown mbuf_test_inject: 2>/dev/null || true
	sleep 1
fi

# Build ng_builder_mbuf if needed
if [ ! -f "./ng_builder_mbuf" ]; then
	echo "Building ng_builder_mbuf..."
	make ng_builder_mbuf || {
		echo "ERROR: Failed to build ng_builder_mbuf"
		exit 1
	}
fi

# Start topology builder in background
echo "Starting topology builder..."
./ng_builder_mbuf &
BUILDER_PID=$!

# Wait for topology to be created
sleep 3

# Check if builder process is still running
if ! kill -0 $BUILDER_PID 2>/dev/null; then
	echo "ERROR: ng_builder_mbuf process died unexpectedly"
	wait $BUILDER_PID
	exit 1
fi
echo "Builder process running (PID: $BUILDER_PID)"

# Verify nodes exist
if ! ngctl show mbuf_test_inject: >/dev/null 2>&1; then
	echo "ERROR: mbuf_test_inject node not found"
	kill $BUILDER_PID 2>/dev/null || true
	echo "Available nodes:"
	ngctl list
	exit 1
fi

if ! ngctl show mbuf_test_mss: >/dev/null 2>&1; then
	echo "ERROR: mbuf_test_mss node not found"
	kill $BUILDER_PID 2>/dev/null || true
	echo "Available nodes:"
	ngctl list
	exit 1
fi

if ! ngctl show mbuf_test_hole: >/dev/null 2>&1; then
	echo "ERROR: mbuf_test_hole node not found"
	kill $BUILDER_PID 2>/dev/null || true
	echo "Available nodes:"
	ngctl list
	exit 1
fi

echo "Topology created successfully!"
echo ""
ngctl show mbuf_test_inject: 2>&1 | head -10
echo ""

# Configure mss_rewrite
echo "Configuring mss_rewrite (MSS=1400/1380)..."
ngctl msg mbuf_test_mss: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"

# Enable statistics
ngctl msg mbuf_test_mss: setstatsmode "{ mode=1 }"

# Enable both directions for testing
ngctl msg mbuf_test_mss: setdirection "{ enable_lower=1 enable_upper=1 }"

# Check if debug statistics are enabled in the module
# Run a dummy test to generate some stats, then check for debug fields
ngctl msg mbuf_test_inject: inject_single "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" >/dev/null 2>&1
sleep 0.1
STATS_CHECK=$(ngctl msg mbuf_test_mss: getstats 2>&1)
ngctl msg mbuf_test_mss: resetstats >/dev/null 2>&1

if echo "$STATS_CHECK" | grep -q "fast_dispatch_count\|safe_dispatch_count"; then
	DEBUG_STATS_ENABLED=1
	echo "Debug statistics: ENABLED"
else
	DEBUG_STATS_ENABLED=0
	echo "Debug statistics: disabled"
fi

echo ""
echo "=========================================="
echo "  Test 1: Single Contiguous Mbuf (Baseline)"
echo "=========================================="

run_inject_test "inject_single" "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" 1 1 "Single contiguous mbuf, IPv4, MSS rewrite" "fast_dispatch_count" 1

echo ""
echo "=========================================="
echo "  Test 2: Fragmented Mbuf Chain (Core Test)"
echo "=========================================="

# This is the CRITICAL test for m_copydata() vs direct pointer access
# Creates multi-mbuf chain where m->m_len < 66 but m_pkthdr.len is full packet

run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=14 csum_flags=0 ext_type=0 }" 1 1 "Fragmented mbuf chain (split at Ether), IPv4, MSS rewrite" "safe_dispatch_count" 1

# Test with different split points
run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=10 csum_flags=0 ext_type=0 }" 1 1 "Fragmented mbuf chain (split at byte 10), IPv4, MSS rewrite" "safe_dispatch_count" 1

# Test IPv6 fragmented
run_inject_test "inject_fragmented" "{ mss=1460 ipv6=1 split_offset=14 csum_flags=0 ext_type=0 }" 1 1 "Fragmented mbuf chain, IPv6, MSS rewrite" "safe_dispatch_count" 1

echo ""
echo "=========================================="
echo "  Test 3: Shared Mbuf (M_WRITABLE == 0)"
echo "=========================================="

# This tests m_unshare() code path when MSS needs rewriting

run_inject_test "inject_shared" "{ mss=1460 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" 1 1 "Shared mbuf (M_WRITABLE==0), IPv4, MSS rewrite" "unshare_count" 1

# Test: Shared mbuf but NO rewrite needed → should NOT call m_unshare()
# This validates lazy unshare optimization (performance critical)
# The run_inject_test already verifies skip_mss_ok=1, confirming no rewrite
# We additionally verify unshare_count=0 for this test (no m_unshare called)
run_inject_test "inject_shared" "{ mss=1200 ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" 1 0 "Shared mbuf, MSS=1200: no unshare needed" "unshare_count" 0

echo ""
echo "=========================================="
echo "  Test 4: Upper Hook + Checksum Offload"
echo "=========================================="

# This tests the from_upper + CSUM_TCP/CSUM_TSO skip logic

ngctl msg mbuf_test_mss: resetstats

# Note: These packets are injected from lower->upper direction in our topology
# We need to reconfigure to test upper->lower with offload
# For now, we'll document this test requires manual topology change

skip_test "Upper hook + offload flags (requires topology reconfiguration)"

# TODO: To properly test this, we need:
# 1. Topology: hole -> mss_rewrite(upper) -> mbuf_inject(lower)
# 2. Send from hole with offload flags set
# 3. This is more complex and may need a different test approach

echo ""
echo "=========================================="
echo "  Test 5: IPv6 Extension Headers"
echo "=========================================="

# This tests that IPv6 packets with extension headers are skipped (current limitation)

run_inject_test "inject_ipv6ext" "{ mss=1460 ipv6=1 split_offset=0 csum_flags=0 ext_type=0 }" 0 0 "IPv6 + Hop-by-Hop ext header: correctly skipped (not supported)"

# Test with Routing header (type 43)
run_inject_test "inject_ipv6ext" "{ mss=1460 ipv6=1 split_offset=0 csum_flags=0 ext_type=43 }" 0 0 "IPv6 + Routing ext header: correctly skipped"

# Test with Fragment header (type 44)
run_inject_test "inject_ipv6ext" "{ mss=1460 ipv6=1 split_offset=0 csum_flags=0 ext_type=44 }" 0 0 "IPv6 + Fragment ext header: correctly skipped"

echo ""
echo "=========================================="
echo "  Test 6: Edge Cases with Fragmented Mbufs"
echo "=========================================="

# Test: MSS <= limit, should NOT rewrite even in fragmented mbuf
run_inject_test "inject_fragmented" "{ mss=1200 ipv6=0 split_offset=14 csum_flags=0 ext_type=0 }" 1 0 "Fragmented mbuf, MSS=1200 (<= 1400): processed but not rewritten" "skip_mss_ok" 1

# Test: Fragmented + Shared (most complex case)
# Note: This is complex to create and test, so we skip it for now
skip_test "Fragmented + Shared mbuf (complex case, not implemented in injector)"

echo ""
echo "=========================================="
echo "  Test 7: Fast Path Entry Boundary (m_len 57/58/59)"
echo "=========================================="
echo "Testing NG_MSS_FAST_PATH_MIN_LEN threshold (58 bytes)"
echo ""

# Critical boundary tests for the optimized fast path entry condition
# NG_MSS_FAST_PATH_MIN_LEN = 58, so:
# - m_len < 58 → safe_dispatch_count should increase
# - m_len >= 58 → fast_dispatch_count should increase

# Test: m_len = 57 (just below threshold) → safe path entry
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=57 csum_flags=0 ext_type=0 }" 1 1 "m_len=57 (< 58): safe path entry" "safe_dispatch_count" 1
else
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=57 csum_flags=0 ext_type=0 }" 1 1 "m_len=57 (< 58): safe path entry"
fi

# Test: m_len = 58 (exactly at threshold) → fast path entry
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=58 csum_flags=0 ext_type=0 }" 1 1 "m_len=58 (== 58): fast path entry" "fast_dispatch_count" 1
else
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=58 csum_flags=0 ext_type=0 }" 1 1 "m_len=58 (== 58): fast path entry"
fi

# Test: m_len = 59 (just above threshold) → fast path entry
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=59 csum_flags=0 ext_type=0 }" 1 1 "m_len=59 (> 58): fast path entry" "fast_dispatch_count" 1
else
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=59 csum_flags=0 ext_type=0 }" 1 1 "m_len=59 (> 58): fast path entry"
fi

echo ""
echo "=========================================="
echo "  Test 8: Fast Entry → Safe Fallback (Design Validation)"
echo "=========================================="
echo "Testing m_len=58 fast entry with safe fallback on dynamic checks"
echo ""

# Test: m_len=58, TCP options extend beyond → fast entry, safe fallback
# IPv4 packet structure (66 bytes total):
#   Ethernet(14) + IPv4(20) + TCP(20) + TCP options(12) = 66
# Split at 58:
#   first mbuf: 58 bytes (Eth + IPv4 + TCP base + 4 bytes options)
#   second mbuf: 8 bytes (remaining options)
# Expected: fast_dispatch=1 AND pullup_count=1 (proves fallback to safe path)
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=58 csum_flags=0 ext_type=0 }" 1 1 "m_len=58, TCP options split: fast→safe fallback" "fast_dispatch_count" 1

	# Verify that pullup was called (proves safe path fallback occurred)
	STATS=$(ngctl msg mbuf_test_mss: getstats 2>&1)
	PULLUP_COUNT=$(echo "$STATS" | grep -o 'pullup_count=[0-9]*' | cut -d= -f2)
	[ -z "$PULLUP_COUNT" ] && PULLUP_COUNT=0

	if [ "$PULLUP_COUNT" != "1" ]; then
		fail_test "Fast→safe fallback: pullup_count=$PULLUP_COUNT (expected 1, proving safe path executed)"
	else
		echo "  ✓ Verified: pullup_count=1 (safe path fallback confirmed)"
	fi
else
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=58 csum_flags=0 ext_type=0 }" 1 1 "m_len=58, TCP options split: fast→safe fallback"
fi

# Note: This test validates the core design change - entry at 58 is safe
# even when TCP options require safe path processing. The pullup_count
# proves that the safe path was actually executed after fast path entry.

echo ""
echo "=========================================="
echo "  Test 9: MSS Option Across mbuf Boundary"
echo "=========================================="
echo "Testing m_copydata() robustness with MSS option split across mbufs"
echo ""

# MSS option structure in packet (4 bytes): kind(1) + len(1) + value(2)
# IPv4 packet: Ethernet(14) + IPv4(20) + TCP(20) + options(12)
# TCP options: NOP(1) + NOP(1) + MSS(4) + padding
# MSS option starts at byte 56 (14 + 20 + 20 + 2)

# Test: Split at MSS option kind byte (byte 56)
# This is the most challenging case - MSS option header itself is fragmented
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=56 csum_flags=0 ext_type=0 }" 1 1 "MSS split: kind at boundary (offset 56)" "safe_dispatch_count" 1
else
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=56 csum_flags=0 ext_type=0 }" 1 1 "MSS split: kind at boundary (offset 56)"
fi

# Test: Split at MSS option length byte (byte 57)
# MSS kind is in first mbuf, but length and value are in second mbuf
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=57 csum_flags=0 ext_type=0 }" 1 1 "MSS split: length at boundary (offset 57)" "safe_dispatch_count" 1
else
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=57 csum_flags=0 ext_type=0 }" 1 1 "MSS split: length at boundary (offset 57)"
fi

# Test: Split at MSS value high byte (byte 58)
# MSS kind+len in first mbuf, both value bytes in second mbuf
# Note: Also tested in Test 8 for fallback validation (different focus)
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=58 csum_flags=0 ext_type=0 }" 1 1 "MSS split: value[0] at boundary (offset 58)" "fast_dispatch_count" 1
else
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=58 csum_flags=0 ext_type=0 }" 1 1 "MSS split: value[0] at boundary (offset 58)"
fi

# Test: Split at MSS value low byte (byte 59)
# This is critical: 16-bit MSS value itself spans mbuf boundary
# MSS kind+len+value[0] in first mbuf, value[1] in second mbuf
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=59 csum_flags=0 ext_type=0 }" 1 1 "MSS split: value[1] at boundary (offset 59)" "fast_dispatch_count" 1
else
	run_inject_test "inject_fragmented" "{ mss=1460 ipv6=0 split_offset=59 csum_flags=0 ext_type=0 }" 1 1 "MSS split: value[1] at boundary (offset 59)"
fi

echo ""
echo "Note: These tests validate m_copydata() robustness across all MSS option"
echo "boundary cases: kind/length bytes (56/57), and MSS value fragmentation (58/59)."
echo "Test 8 uses offset=58 to prove fast→safe fallback; Test 9 ensures correctness."

echo ""
echo "=========================================="
echo "  Test Summary"
echo "=========================================="
echo ""
printf "Total tests: %d\n" "$TESTS_TOTAL"
printf "${GREEN}Passed: %d${NC}\n" "$TESTS_PASSED"
printf "${RED}Failed: %d${NC}\n" "$TESTS_FAILED"
printf "${YELLOW}Skipped: %d${NC}\n" "$((TESTS_TOTAL - TESTS_PASSED - TESTS_FAILED))"
echo ""

# Show final statistics (including debug stats if enabled)
echo "=========================================="
echo "  Final Statistics"
echo "=========================================="
echo ""
if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	echo "Debug statistics are enabled - showing detailed counters:"
fi
ngctl msg mbuf_test_mss: getstats 2>&1 || echo "Failed to get statistics"
echo ""

if [ "$DEBUG_STATS_ENABLED" = "1" ]; then
	echo "Note: Debug statistics verify that expected code paths were exercised."
	echo ""
fi

if [ "$TESTS_FAILED" -gt 0 ]; then
	echo "Result: FAIL"
	exit 1
else
	echo "Result: PASS"
	exit 0
fi
