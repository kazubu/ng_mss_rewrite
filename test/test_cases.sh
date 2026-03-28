#!/usr/bin/env bash
#
# Comprehensive unit tests for ng_mss_rewrite
# Tests various boundary cases and edge conditions
#

if [ "$(id -u)" != "0" ]; then
    echo "This script must be run as root"
    exit 1
fi

# Detect script directory and project root
SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test result tracking
pass_test() {
    TESTS_PASSED=$((TESTS_PASSED + 1))
    printf "${GREEN}✓ PASS${NC}: %s\n" "$1"
}

fail_test() {
    TESTS_FAILED=$((TESTS_FAILED + 1))
    printf "${RED}✗ FAIL${NC}: %s\n" "$1"
    printf "  Expected: %s\n" "$2"
    printf "  Got: %s\n" "$3"
}

skip_test() {
    echo "${YELLOW}⊘ SKIP${NC}: $1"
}

# Cleanup function
cleanup() {
    echo "Cleaning up test topology..."

    # Kill ng_builder processes
    pkill -f "ng_builder test" 2>/dev/null
    pkill -f "ng_builder_generic test" 2>&1 || true; pkill -f ng_builder 2>/dev/null
    sleep 1

    # Shutdown nodes in correct order (disconnect first, then shutdown)
    ngctl msg test_source: clrdata 2>/dev/null || true
    ngctl shutdown test_source: 2>/dev/null || true
    ngctl shutdown test_mss: 2>/dev/null || true
    ngctl shutdown test_hole: 2>/dev/null || true

    # Wait a bit for cleanup to complete
    sleep 1

    # Verify cleanup
    if ngctl list 2>/dev/null | grep -q "test_"; then
        echo "Warning: Some test nodes still exist:"
        ngctl list 2>/dev/null | grep "test_"

        # Force cleanup
        for node in test_source test_mss test_hole; do
            ngctl shutdown ${node}: 2>/dev/null || true
        done
        sleep 1
    fi
}

# Setup test environment
setup() {
    echo "Setting up test environment..."

    # Clean up any existing test topology first
    cleanup

    # Force reload module to test latest binary
    echo "Reloading ng_mss_rewrite module..."
    if kldstat | grep -q ng_mss_rewrite; then
        kldunload ng_mss_rewrite || {
            echo "ERROR: Failed to unload ng_mss_rewrite (may be in use)"
            return 1
        }
        sleep 1
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

    # Start topology with "test" prefix
    $SCRIPT_DIR/ng_builder_generic test > /tmp/ng_builder_test.log 2>&1 &
    NG_BUILDER_PID=$!
    sleep 2

    # Verify ng_builder is still running
    if ! ps -p $NG_BUILDER_PID > /dev/null 2>&1; then
        echo "ERROR: ng_builder_generic exited unexpectedly"
        cat /tmp/ng_builder_test.log
        return 1
    fi

    # Verify topology
    if ! ngctl list 2>/dev/null | grep -q "test_mss"; then
        echo "ERROR: Failed to create test topology"
        cat /tmp/ng_builder_test.log
        return 1
    fi

    # Configure mss_rewrite for testing
    ngctl msg test_mss: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }" || return 1
    ngctl msg test_mss: setstatsmode "{ mode=1 }" || return 1
    ngctl msg test_mss: setdirection "{ enable_lower=1 enable_upper=0 }" || return 1

    # Configure ng_source
    ngctl msg test_source: setpps 1000000 2>/dev/null || true

    echo "Setup complete"
    return 0
}

# Send a packet and check results
send_packet() {
    local PACKET_HEX="$1"
    local TEST_NAME="$2"
    local EXPECT_PROCESSED="$3"
    local EXPECT_REWRITTEN="$4"

    TESTS_RUN=$((TESTS_RUN + 1))

    # Reset stats
    ngctl msg test_mss: resetstats 2>/dev/null || true

    # Clear any queued packets
    ngctl msg test_source: clrdata 2>/dev/null || true

    # Send packet
    echo "$PACKET_HEX" | perl -pe 's/(..)[ \t\n]*/chr(hex($1))/ge' | \
        nghook test_source: input 2>/dev/null

    # Start transmission
    ngctl msg test_source: start 1 2>/dev/null
    sleep 1

    # Get stats
    STATS=$(ngctl msg test_mss: getstats 2>/dev/null)
    PROCESSED=$(echo "$STATS" | grep -o "packets_processed=[0-9]*" | cut -d= -f2)
    REWRITTEN=$(echo "$STATS" | grep -o "packets_rewritten=[0-9]*" | cut -d= -f2)

    # Default to 0 if empty
    PROCESSED=${PROCESSED:-0}
    REWRITTEN=${REWRITTEN:-0}

    # Check results
    if [ "$PROCESSED" = "$EXPECT_PROCESSED" ] && [ "$REWRITTEN" = "$EXPECT_REWRITTEN" ]; then
        pass_test "$TEST_NAME"
        return 0
    else
        fail_test "$TEST_NAME" \
            "processed=$EXPECT_PROCESSED, rewritten=$EXPECT_REWRITTEN" \
            "processed=$PROCESSED, rewritten=$REWRITTEN"
        return 1
    fi
}

echo "========================================"
echo "ng_mss_rewrite Unit Tests"
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
echo "Running test cases..."
echo ""

# Test 1: TCP SYN with MSS=1460 (should be rewritten to 1400)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d0 52 00 00 02 04 05 b4 01 03 03 08 01 01
04 02" \
    "TCP SYN with MSS=1460" "1" "1"

# Test 2: TCP SYN with MSS=1200 (should NOT be rewritten, already < limit)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d1 52 00 00 02 04 04 b0 01 03 03 08 01 01
04 02" \
    "TCP SYN with MSS=1200 (no rewrite)" "1" "0"

# Test 3: TCP SYN with MSS=1400 (exactly at limit, should NOT rewrite)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d0 f2 00 00 02 04 05 78 01 03 03 08 01 01
04 02" \
    "TCP SYN with MSS=1400 (at limit)" "1" "0"

# Test 4: TCP SYN with MSS=1401 (just over limit, should rewrite)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d0 f1 00 00 02 04 05 79 01 03 03 08 01 01
04 02" \
    "TCP SYN with MSS=1401 (just over limit)" "1" "1"

# Test 5: TCP ACK (non-SYN, should be processed but not rewritten)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 28 00 01 00 00 40 06 f9 7f 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 50 10
20 00 e4 5e 00 00" \
    "TCP ACK (non-SYN)" "0" "0"

# Test 6: UDP packet (should not be processed)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 1c 00 01 00 00 40 11 f9 89 0a 00 00 01 0a 00
00 02 00 35 00 35 00 08 00 00" \
    "UDP packet (not TCP)" "0" "0"

# Test 7: TCP SYN without MSS option (should be processed but not rewritten)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 2c 00 01 00 00 40 06 f9 7b 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d8 5e 00 00 01 01 04 02" \
    "TCP SYN without MSS option" "1" "0"

# Test 8: TCP SYN with MSS at offset 0 (no NOP padding)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 30 00 01 00 00 40 06 f9 77 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d4 52 00 00 02 04 05 b4 01 03 03 08" \
    "TCP SYN MSS at offset 0" "1" "1"

# Test 9: TCP SYN with one NOP before MSS (offset 1)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 80 02
20 00 cf 52 00 00 01 02 04 05 b4 01 03 03 08 00
00 00" \
    "TCP SYN MSS at offset 1 (one NOP)" "1" "1"

# Test 10: TCP SYN with two NOPs before MSS (offset 2)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 36 00 01 00 00 40 06 f9 71 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 70 02
20 00 ce 52 00 00 01 01 02 04 05 b4 01 03 03 08
01 01 04 02" \
    "TCP SYN MSS at offset 2 (two NOPs)" "1" "1"

# Test 11: Minimum size TCP SYN with MSS
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 2c 00 01 00 00 40 06 f9 7b 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d6 52 00 00 02 04 05 b4" \
    "Minimum TCP SYN with MSS" "1" "1"

# Test 12: VLAN tagged packet with TCP SYN MSS=1460
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 81 00 00 64
08 00 45 00 00 34 00 01 00 00 40 06 f9 73 0a 00
00 01 0a 00 00 02 00 50 00 50 00 00 00 00 00 00
00 00 60 02 20 00 d0 52 00 00 02 04 05 b4 01 03
03 08 01 01 04 02" \
    "VLAN tagged TCP SYN MSS=1460" "1" "1"

# Test 13: IPv6 TCP SYN with MSS=1460
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 86 dd 60 00
00 00 00 18 06 40 20 01 0d b8 00 00 00 00 00 00
00 00 00 00 00 01 20 01 0d b8 00 00 00 00 00 00
00 00 00 00 00 02 00 50 00 50 00 00 00 00 00 00
00 00 60 02 20 00 00 00 00 00 02 04 05 b4" \
    "IPv6 TCP SYN MSS=1460" "1" "1"

# Test 14: IPv6 TCP SYN with MSS=1200 (no rewrite)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 86 dd 60 00
00 00 00 18 06 40 20 01 0d b8 00 00 00 00 00 00
00 00 00 00 00 01 20 01 0d b8 00 00 00 00 00 00
00 00 00 00 00 02 00 50 00 50 00 00 00 00 00 00
00 00 60 02 20 00 00 00 00 00 02 04 04 b0" \
    "IPv6 TCP SYN MSS=1200 (no rewrite)" "1" "0"

# Test 15: Fragmented IPv4 packet (should be skipped)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 20 00 40 06 d9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d0 52 00 00 02 04 05 b4 01 03 03 08 01 01
04 02" \
    "Fragmented IPv4 (should skip)" "0" "0"

# Test 16: Very small packet (too small for TCP)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 14 00 01 00 00 40 06" \
    "Packet too small for TCP" "0" "0"

# Test 17: TCP with invalid header length
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 28 00 01 00 00 40 06 f9 7f 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 30 02
20 00 e4 5e 00 00" \
    "TCP with invalid header length" "0" "0"

# Test 18: ARP packet (non-IP)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 06 00 01
08 00 06 04 00 01 00 00 00 00 00 00 0a 00 00 01
00 00 00 00 00 00 0a 00 00 02" \
    "ARP packet (non-IP)" "0" "0"

# Test 19: TCP SYN-ACK with MSS=1460
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 01 60 12
20 00 d0 51 00 00 02 04 05 b4 01 03 03 08 01 01
04 02" \
    "TCP SYN-ACK MSS=1460" "1" "1"

# Test 20: TCP SYN with maximum MSS (65535)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 1a f1 00 00 02 04 ff ff 01 03 03 08 01 01
04 02" \
    "TCP SYN MSS=65535 (maximum)" "1" "1"

# Test 21: TCP SYN with invalid MSS option length (len=3 instead of 4)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 80 02
20 00 d6 52 00 00 02 03 05 b4 01 03 03 08 00 00
00 00 00 00 00 00" \
    "TCP SYN with invalid MSS len=3" "1" "0"

# Test 22: TCP SYN with invalid MSS option length (len=5)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 31 00 01 00 00 40 06 f9 76 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 70 02
20 00 d6 52 00 00 02 05 05 b4 00 01 03 03 08 00
00 00" \
    "TCP SYN with invalid MSS len=5" "1" "0"

# Test 23: TCP SYN with valid MSS followed by truncated MSS option
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 30 00 01 00 00 40 06 f9 77 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 70 02
20 00 d8 52 00 00 02 04 05 b4 01 03 03 08 02 04" \
    "TCP SYN with valid MSS + truncated MSS" "1" "1"

# Test 24: TCP SYN with options length > actual TCP header
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 30 00 01 00 00 40 06 f9 77 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 f0 02
20 00 44 52 00 00 02 04 05 b4 01 03 03 08" \
    "TCP SYN with data offset=15 (too large)" "0" "0"

# Test 25: TCP SYN with multiple MSS options (only first should be used)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 38 00 01 00 00 40 06 f9 6f 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 80 02
20 00 ca 52 00 00 02 04 05 b4 02 04 04 b0 01 03
03 08 00 00 00 00" \
    "TCP SYN with multiple MSS options" "1" "1"

# Test 26: TCP SYN with MSS=0
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d6 52 00 00 02 04 00 00 01 03 03 08 01 01
04 02" \
    "TCP SYN with MSS=0" "1" "0"

# Test 27: TCP SYN with MSS=1 (minimum possible)
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 60 02
20 00 d6 51 00 00 02 04 00 01 01 03 03 08 01 01
04 02" \
    "TCP SYN with MSS=1" "1" "0"

# Test 28: TCP with reserved bits set in flags
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 45 00
00 34 00 01 00 00 40 06 f9 73 0a 00 00 01 0a 00
00 02 00 50 00 50 00 00 00 00 00 00 00 00 6f 02
20 00 c1 52 00 00 02 04 05 b4 01 03 03 08 01 01
04 02" \
    "TCP SYN with reserved bits set" "1" "1"

# Test 29: IPv4 with IP options
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 08 00 47 00
00 38 00 01 00 00 40 06 f9 6b 0a 00 00 01 0a 00
00 02 01 01 01 01 01 01 01 01 00 50 00 50 00 00
00 00 00 00 00 00 60 02 20 00 d0 52 00 00 02 04
05 b4 01 03 03 08 01 01 04 02" \
    "TCP SYN with IPv4 options" "1" "1"

# Test 30: Double VLAN tagged packet (QinQ) - not supported
send_packet "ff ff ff ff ff ff 00 00 00 00 00 00 81 00 00 64
81 00 00 c8 08 00 45 00 00 34 00 01 00 00 40 06
f9 73 0a 00 00 01 0a 00 00 02 00 50 00 50 00 00
00 00 00 00 00 00 60 02 20 00 d0 52 00 00 02 04
05 b4 01 03 03 08 01 01 04 02" \
    "Double VLAN tagged (QinQ) - unsupported" "0" "0"

echo ""
echo "========================================"
echo "Test Summary"
echo "========================================"
echo "Total tests run: $TESTS_RUN"
printf "${GREEN}Passed: %d${NC}\n" "$TESTS_PASSED"
printf "${RED}Failed: %d${NC}\n" "$TESTS_FAILED"
echo ""

# Cleanup
echo "Cleaning up..."
cleanup

# Final verification
REMAINING=$(ngctl list 2>/dev/null | grep -c "test_" || true)
if [ $REMAINING -gt 0 ]; then
    printf "${YELLOW}Warning: %d test nodes still exist after cleanup${NC}\n" "$REMAINING"
fi

if [ $TESTS_FAILED -eq 0 ]; then
    printf "${GREEN}All tests passed!${NC}\n"
    exit 0
else
    printf "${RED}Some tests failed!${NC}\n"
    exit 1
fi
