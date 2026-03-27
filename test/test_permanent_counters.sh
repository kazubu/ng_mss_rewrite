#!/bin/sh
#
# Test permanent counters for production observability
# These counters should always be collected, regardless of stats_mode
#

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'  # No Color

pass_test() {
	printf "${GREEN}✓ PASS${NC}: %s\n" "$1"
}

fail_test() {
	printf "${RED}✗ FAIL${NC}: %s\n" "$1"
	FAILED=$((FAILED + 1))
}

# Check if running as root
if [ "$(id -u)" -ne 0 ]; then
	printf "${RED}Error: This script must be run as root${NC}\n"
	exit 1
fi

# Load required modules
kldload -n netgraph 2>/dev/null || true
kldload -n ng_ether 2>/dev/null || true
kldload -n ng_socket 2>/dev/null || true
kldload -n ng_source 2>/dev/null || true

# Check if ng_mbuf_inject module exists
if [ ! -f "./ng_mbuf_inject.ko" ]; then
	printf "${YELLOW}Building ng_mbuf_inject module...${NC}\n"
	make -C . ng_mbuf_inject.ko
fi

# Load test modules
kldload -n ./ng_mbuf_inject.ko 2>/dev/null || kldload ./ng_mbuf_inject.ko

# Load ng_mss_rewrite module
if [ -f "../ng_mss_rewrite.ko" ]; then
	kldload -n ../ng_mss_rewrite.ko 2>/dev/null || kldload ../ng_mss_rewrite.ko
else
	printf "${RED}Error: ng_mss_rewrite.ko not found${NC}\n"
	exit 1
fi

# Initialize counters
PASSED=0
FAILED=0
TOTAL=0

printf "${BLUE}========================================${NC}\n"
printf "${BLUE}  Permanent Counter Testing${NC}\n"
printf "${BLUE}========================================${NC}\n"
printf "\n"

# Helper function to extract counter value from stats output
get_counter() {
	local stats="$1"
	local counter="$2"
	echo "$stats" | grep -o "${counter}=[0-9]*" | cut -d= -f2 | head -1
}

# Helper function to run test
run_counter_test() {
	local test_name="$1"
	local counter_name="$2"
	local expected_value="$3"

	TOTAL=$((TOTAL + 1))

	STATS=$(ngctl msg perm_test_mss: getstats 2>&1)
	ACTUAL=$(get_counter "$STATS" "$counter_name")

	if [ -z "$ACTUAL" ]; then
		ACTUAL=0
	fi

	if [ "$ACTUAL" = "$expected_value" ]; then
		pass_test "$test_name: $counter_name=$ACTUAL"
		PASSED=$((PASSED + 1))
	else
		fail_test "$test_name: $counter_name=$ACTUAL (expected $expected_value)"
	fi
}

# Cleanup function
cleanup() {
	ngctl shutdown perm_test_src: 2>/dev/null || true
	ngctl shutdown perm_test_mss: 2>/dev/null || true
	sleep 0.1
}

# Setup test topology: ng_source -> mss_rewrite -> discard
setup_topology() {
	cleanup

	# Create nodes
	ngctl mkpeer mbuf_inject: mss_rewrite lower lower
	ngctl name mbuf_inject:lower perm_test_mss

	# Connect upper hook to nowhere (packets will be discarded)
	ngctl mkpeer perm_test_mss: hole upper in

	# Configure MSS values
	ngctl msg perm_test_mss: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"

	# Enable statistics mode
	ngctl msg perm_test_mss: setstatsmode "{ mode=1 }"

	# Reset stats to start from zero
	ngctl msg perm_test_mss: resetstats
}

printf "${BLUE}=== Test 1: Permanent counters with stats_mode=PERCPU ===${NC}\n"
setup_topology

# Test skip_non_tcp counter
printf "\nTesting skip_non_tcp counter:\n"
ngctl msg mbuf_inject: inject_udp "{ count=10 }"
sleep 0.2
run_counter_test "UDP packets" "skip_non_tcp" "10"

# Reset and test skip_fragmented_ipv4 counter
ngctl msg perm_test_mss: resetstats
printf "\nTesting skip_fragmented_ipv4 counter:\n"
ngctl msg mbuf_inject: inject_fragmented_ipv4 "{ count=5 }"
sleep 0.2
run_counter_test "Fragmented IPv4 packets" "skip_fragmented_ipv4" "5"

# Reset and test skip_ipv6_ext counter
ngctl msg perm_test_mss: resetstats
printf "\nTesting skip_ipv6_ext counter:\n"
ngctl msg mbuf_inject: inject_ipv6_ext "{ count=3 ext_type=0 }"
sleep 0.2
run_counter_test "IPv6 with extension headers" "skip_ipv6_ext" "3"

printf "\n${BLUE}=== Test 2: Permanent counters with stats_mode=DISABLED ===${NC}\n"

# Disable statistics mode
ngctl msg perm_test_mss: setstatsmode "{ mode=0 }"
ngctl msg perm_test_mss: resetstats

printf "\nWith stats_mode=DISABLED, permanent counters should still increment:\n"

# Test skip_non_tcp counter (should still work)
ngctl msg mbuf_inject: inject_udp "{ count=7 }"
sleep 0.2
run_counter_test "UDP packets (stats_mode=DISABLED)" "skip_non_tcp" "7"

# Test that packets_processed is NOT incremented (it requires PERCPU mode)
STATS=$(ngctl msg perm_test_mss: getstats 2>&1)
PROCESSED=$(get_counter "$STATS" "packets_processed")
if [ -z "$PROCESSED" ]; then
	PROCESSED=0
fi
TOTAL=$((TOTAL + 1))
if [ "$PROCESSED" = "0" ]; then
	pass_test "packets_processed=0 with stats_mode=DISABLED (expected behavior)"
	PASSED=$((PASSED + 1))
else
	fail_test "packets_processed=$PROCESSED (expected 0 with stats_mode=DISABLED)"
fi

printf "\n${BLUE}=== Test 3: Verify all permanent counters are zero initially ===${NC}\n"

cleanup
setup_topology

STATS=$(ngctl msg perm_test_mss: getstats 2>&1)
run_counter_test "Initial state" "drop_pullup_failed" "0"
run_counter_test "Initial state" "drop_unshare_failed" "0"
run_counter_test "Initial state" "skip_ipv6_ext" "0"
run_counter_test "Initial state" "skip_non_tcp" "0"
run_counter_test "Initial state" "skip_fragmented_ipv4" "0"

# Cleanup
cleanup

# Summary
printf "\n${BLUE}========================================${NC}\n"
printf "${BLUE}  Summary${NC}\n"
printf "${BLUE}========================================${NC}\n"
printf "\n"
printf "Total tests: %d\n" "$TOTAL"
printf "Passed: ${GREEN}%d${NC}\n" "$PASSED"
printf "Failed: ${RED}%d${NC}\n" "$FAILED"
printf "\n"

if [ $FAILED -eq 0 ]; then
	printf "${GREEN}Result: PASS${NC}\n"
	exit 0
else
	printf "${RED}Result: FAIL${NC}\n"
	exit 1
fi
