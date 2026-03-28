#!/usr/bin/env bash
#
# Comprehensive fuzzing tests for ng_mss_rewrite
# Tests random packet variations to find edge cases
#

if [ "$(id -u)" != "0" ]; then
    echo "This script must be run as root"
    exit 1
fi

# Debug mode: set to 1 to see detailed output
DEBUG=${DEBUG:-0}

# Detect script directory and project root
SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")

# Source test helper functions
. "${SCRIPT_DIR}/test_helpers.sh"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
CRASHES=0

# Test result tracking
pass_test() {
    TESTS_PASSED=$((TESTS_PASSED + 1))
    printf "${GREEN}✓${NC} %s\n" "$1"
}

fail_test() {
    TESTS_FAILED=$((TESTS_FAILED + 1))
    printf "${RED}✗${NC} %s\n" "$1"
    printf "  Expected: %s, Got: %s\n" "$2" "$3"
}

crash_detected() {
    CRASHES=$((CRASHES + 1))
    printf "${RED}💥 CRASH${NC}: %s\n" "$1"
}

# Cleanup function
cleanup() {
    if [ $DEBUG -eq 1 ]; then
        echo "Cleaning up fuzz topology..."
    fi

    # Kill ng_builder processes
    pkill -f "ng_builder_generic fuzz" 2>&1 || true; pkill -f "ng_builder fuzz" 2>/dev/null
    pkill -f ng_builder 2>/dev/null

    # Shutdown nodes in correct order
    ngctl msg fuzz_source: clrdata 2>/dev/null || true
    ngctl shutdown fuzz_source: 2>/dev/null || true
    ngctl shutdown fuzz_mss: 2>/dev/null || true
    ngctl shutdown fuzz_hole: 2>/dev/null || true

    # Verify cleanup
    if ngctl list 2>/dev/null | grep -q "fuzz_"; then
        if [ $DEBUG -eq 1 ]; then
            echo "Warning: Some fuzz nodes still exist, forcing cleanup..."
        fi

        # Force cleanup
        for node in fuzz_source fuzz_mss fuzz_hole; do
            ngctl shutdown ${node}: 2>/dev/null || true
        done
    fi
}

# Setup test environment
setup() {
    echo "Setting up fuzzing environment..."

    # Clean up any existing fuzz topology first
    cleanup

    # Force reload module to test latest binary
    echo "Reloading ng_mss_rewrite module..."
    if kldstat | grep -q ng_mss_rewrite; then
        kldunload ng_mss_rewrite || {
            echo "ERROR: Failed to unload ng_mss_rewrite (may be in use)"
            return 1
        }
        wait_for_module_unload ng_mss_rewrite 2 || true
    fi

    # Ensure dependencies are loaded
    kldload ng_source 2>/dev/null || true
    kldload ng_hole 2>/dev/null || true

    # Load the module (prefer local build)
    if [ -f "$PROJECT_ROOT/ng_mss_rewrite.ko" ]; then
        echo "Loading local module: $PROJECT_ROOT/ng_mss_rewrite.ko"
        kldload "$PROJECT_ROOT/ng_mss_rewrite.ko" || {
            echo "ERROR: Failed to load ng_mss_rewrite.ko"
            return 1
        }
    else
        echo "Loading system module: ng_mss_rewrite"
        kldload ng_mss_rewrite || {
            echo "ERROR: Failed to load ng_mss_rewrite"
            return 1
        }
    fi

    # Verify module is loaded
    if ! kldstat | grep -q ng_mss_rewrite; then
        echo "ERROR: ng_mss_rewrite not loaded after kldload"
        return 1
    fi

    echo "Module loaded successfully"

    # Build ng_builder if needed
    if [ ! -f "$SCRIPT_DIR/ng_builder_generic" ]; then
        echo "Building ng_builder_generic..."
        cd "$SCRIPT_DIR" && make || return 1
    fi

    # Start topology with "fuzz" prefix
    $SCRIPT_DIR/ng_builder_generic fuzz > /tmp/ng_builder_fuzz.log 2>&1 &
    NG_BUILDER_PID=$!

    # Wait for topology to be created
    if ! wait_for_nodes 5 fuzz_source: fuzz_mss: fuzz_hole:; then
        echo "ERROR: Timeout waiting for topology nodes to be created"
        cat /tmp/ng_builder_fuzz.log
        kill $NG_BUILDER_PID 2>/dev/null || true
        return 1
    fi

    # Verify ng_builder is still running
    if ! ps -p $NG_BUILDER_PID > /dev/null 2>&1; then
        echo "ERROR: ng_builder_generic exited unexpectedly"
        cat /tmp/ng_builder_fuzz.log
        return 1
    fi

    # Configure mss_rewrite for testing
    ngctl msg fuzz_mss: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }" || return 1
    ngctl msg fuzz_mss: setstatsmode "{ mode=1 }" || return 1
    ngctl msg fuzz_mss: setdirection "{ enable_lower=1 enable_upper=0 }" || return 1

    # Configure ng_source
    ngctl msg fuzz_source: setpps 1000000 2>/dev/null || true

    echo "Setup complete"
    return 0
}

# Send a packet and check for crashes
send_fuzz_packet() {
    local PACKET_HEX="$1"
    local TEST_NAME="$2"

    TESTS_RUN=$((TESTS_RUN + 1))

    # Check if module is still loaded
    if ! kldstat | grep -q ng_mss_rewrite; then
        crash_detected "$TEST_NAME - module unloaded"
        return 1
    fi

    # Reset stats
    ngctl msg fuzz_mss: resetstats 2>/dev/null || {
        crash_detected "$TEST_NAME - cannot reset stats"
        return 1
    }

    # Clear any queued packets
    ngctl msg fuzz_source: clrdata 2>/dev/null || true

    # Send packet (same method as test_cases.sh)
    echo "$PACKET_HEX" | perl -pe 's/(..)[ \t\n]*/chr(hex($1))/ge' | \
        nghook fuzz_source: input 2>/dev/null

    # nghook always succeeds with ng_source, no need to check return code

    # Start transmission
    ngctl msg fuzz_source: start 1 2>/dev/null

    # Poll for packet to be processed (timeout is OK for malformed packets)
    wait_for_packets_processed fuzz_mss: 1 1 || true

    # Check if module is still alive
    if ! kldstat | grep -q ng_mss_rewrite; then
        crash_detected "$TEST_NAME - module crashed"
        return 1
    fi

    # Try to get stats
    ngctl msg fuzz_mss: getstats 2>/dev/null >/dev/null || {
        crash_detected "$TEST_NAME - cannot get stats"
        return 1
    }

    pass_test "$TEST_NAME"
    return 0
}

# Generate random byte
rand_byte() {
    printf '%02x' $((RANDOM % 256))
}

# Generate random 2 bytes
rand_word() {
    printf '%02x %02x' $((RANDOM % 256)) $((RANDOM % 256))
}

echo "========================================"
echo "ng_mss_rewrite Fuzzing Tests"
echo "========================================"
echo ""

# Ensure cleanup on exit/interrupt
trap cleanup EXIT INT TERM

# Setup
if ! setup; then
    echo "Setup failed, cannot run tests"
    exit 1
fi

echo ""
printf "${BLUE}=== Phase 1: Random MSS values ===${NC}\n"
echo ""

# Test 100 random MSS values
for i in $(seq 1 100); do
    MSS_HIGH=$((RANDOM % 256))
    MSS_LOW=$((RANDOM % 256))

    send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 2c 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 00 00 00 00 02 04 $(printf '%02x %02x' $MSS_HIGH $MSS_LOW)" \
        "Random MSS=$((MSS_HIGH * 256 + MSS_LOW))"
done

echo ""
printf "${BLUE}=== Phase 2: Random TCP option lengths ===${NC}\n"
echo ""

# Test various option lengths
for opt_len in 0 1 2 3 4 5 6 7 8 12 16 20 24 28 32 36 40; do
    # Generate random options
    OPTIONS=""
    for j in $(seq 1 $opt_len); do
        OPTIONS="$OPTIONS $(rand_byte)"
    done

    # Calculate TCP header size (must be multiple of 4)
    TCP_HDR_LEN=$(((20 + opt_len + 3) / 4 * 4))
    DATA_OFF=$(printf '%x' $((TCP_HDR_LEN / 4)))
    IP_LEN=$(printf '%02x %02x' 0 $((20 + TCP_HDR_LEN)))

    send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
$IP_LEN 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 ${DATA_OFF}0 02
20 00 00 00 00 00 $OPTIONS" \
        "Random options len=$opt_len"
done

echo ""
printf "${BLUE}=== Phase 3: Random TCP data offsets ===${NC}\n"
echo ""

# Test various data offset values (0-15)
for data_off in 0 1 2 3 4 5 6 7 8 9 a b c d e f; do
    send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 ${data_off}0 02
20 00 00 00 00 00 02 04 05 b4 01 03 03 08 01 01 04 02" \
        "Data offset=$data_off"
done

echo ""
printf "${BLUE}=== Phase 4: Random IP lengths ===${NC}\n"
echo ""

# Test various IP total lengths
for ip_len in 20 28 2c 34 40 60 80 ff 00 01 02 ff ff; do
    send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 $ip_len 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 00 00 00 00 02 04 05 b4 01 03 03 08 01 01 04 02" \
        "IP len=0x$ip_len"
done

echo ""
printf "${BLUE}=== Phase 5: Random TCP flags ===${NC}\n"
echo ""

# Test various TCP flag combinations
for flags in 00 01 02 04 08 10 20 40 80 ff 3f 1f; do
    send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 2c 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 $flags
20 00 00 00 00 00 02 04 05 b4" \
        "TCP flags=0x$flags"
done

echo ""
printf "${BLUE}=== Phase 6: Malformed MSS options ===${NC}\n"
echo ""

# Test malformed MSS options
MSS_TESTS=(
    "02"                    # MSS kind only
    "02 04"                 # MSS kind + len only
    "02 04 05"              # MSS incomplete
    "02 00 05 b4"           # MSS len=0
    "02 01 05 b4"           # MSS len=1
    "02 02 05 b4"           # MSS len=2
    "02 03 05 b4"           # MSS len=3
    "02 05 05 b4 00"        # MSS len=5
    "02 ff 05 b4"           # MSS len=255
    "02 04 00 00"           # MSS=0
    "02 04 ff ff"           # MSS=65535
)

for i in "${!MSS_TESTS[@]}"; do
    send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 30 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 70 02
20 00 00 00 00 00 ${MSS_TESTS[$i]} 00 00 00 00 00 00 00 00" \
        "Malformed MSS #$i: ${MSS_TESTS[$i]}"
done

echo ""
printf "${BLUE}=== Phase 7: Random IPv6 packets ===${NC}\n"
echo ""

# Test random IPv6 packets
for i in $(seq 1 50); do
    MSS_HIGH=$((RANDOM % 256))
    MSS_LOW=$((RANDOM % 256))

    send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 86 dd 60 00
00 00 00 $(rand_byte) 06 40 $(rand_word) $(rand_word) $(rand_word) $(rand_word)
$(rand_word) $(rand_word) $(rand_word) $(rand_word) $(rand_word) $(rand_word)
$(rand_word) $(rand_word) $(rand_word) $(rand_word) 00 50 00 50 00 00
00 00 00 00 00 00 60 02 20 00 00 00 00 00 02 04 $(printf '%02x %02x' $MSS_HIGH $MSS_LOW)" \
        "IPv6 random #$i"
done

echo ""
printf "${BLUE}=== Phase 8: Random VLAN tags ===${NC}\n"
echo ""

# Test various VLAN configurations
for i in $(seq 1 30); do
    VLAN_ID=$((RANDOM % 4096))
    VLAN_HI=$(printf '%02x' $((VLAN_ID / 256)))
    VLAN_LO=$(printf '%02x' $((VLAN_ID % 256)))

    send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 81 00 $VLAN_HI $VLAN_LO
08 00 45 00 00 2c 00 01 00 00 40 06 00 00 0a 00
00 01 0a 00 00 02 00 50 00 50 00 00 00 00 00 00
00 00 60 02 20 00 00 00 00 00 02 04 05 b4" \
        "VLAN ID=$VLAN_ID"
done

echo ""
printf "${BLUE}=== Phase 9: Edge case packet sizes ===${NC}\n"
echo ""

# Test minimum size packets
send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 14" \
    "Minimum IPv4 (no TCP)"

send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 28 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 50 02
20 00 00 00" \
    "Minimum TCP (no options)"

# Test maximum reasonable packet size
LARGE_OPTIONS=""
for i in $(seq 1 40); do
    LARGE_OPTIONS="$LARGE_OPTIONS 01"  # 40 NOPs
done

send_fuzz_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 5c 00 01 00 00 40 06 00 00 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 f0 02
20 00 00 00 00 00 $LARGE_OPTIONS" \
    "Maximum TCP options (60 bytes)"

echo ""
printf "${BLUE}=== Phase 10: Completely random packets ===${NC}\n"
echo ""

# Generate completely random packets
for i in $(seq 1 100); do
    # Random packet length between 40 and 100 bytes
    PKT_LEN=$((40 + RANDOM % 61))
    RANDOM_PKT=""
    for j in $(seq 1 $PKT_LEN); do
        RANDOM_PKT="$RANDOM_PKT $(rand_byte)"
    done

    send_fuzz_packet "$RANDOM_PKT" "Completely random #$i" || true
done

echo ""
echo "========================================"
echo "Fuzzing Summary"
echo "========================================"
echo "Total tests run: $TESTS_RUN"
printf "${GREEN}Passed: %d${NC}\n" "$TESTS_PASSED"
printf "${RED}Failed: %d${NC}\n" "$TESTS_FAILED"
printf "${RED}Crashes: %d${NC}\n" "$CRASHES"
echo ""

# Cleanup
echo "Cleaning up..."
cleanup

# Final verification
REMAINING=$(ngctl list 2>/dev/null | grep -c "fuzz_" || true)
if [ $REMAINING -gt 0 ]; then
    printf "${YELLOW}Warning: %d fuzz nodes still exist after cleanup${NC}\n" "$REMAINING"
    ngctl list 2>/dev/null | grep "fuzz_"
fi

if [ $CRASHES -eq 0 ]; then
    printf "${GREEN}No crashes detected!${NC}\n"
    exit 0
else
    printf "${RED}Crashes detected!${NC}\n"
    exit 1
fi
