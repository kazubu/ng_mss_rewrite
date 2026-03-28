#!/bin/sh
#
# Test script for wire image verification
# Verifies that MSS rewriting actually modifies packet bytes correctly
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
MODULE_DIR=$(dirname "$SCRIPT_DIR")

printf "${YELLOW}=== Wire Image Verification Test ===${NC}\n\n"

# Build ng_builder_generic if needed
if [ ! -f "${SCRIPT_DIR}/ng_builder_generic" ]; then
    printf "Building ng_builder_generic...\n"
    cd ${SCRIPT_DIR}
    make ng_builder_generic || {
        printf "${RED}Failed to build ng_builder_generic${NC}\n"
        exit 1
    }
fi

# Load modules
printf "Loading modules...\n"
kldunload ng_mbuf_inject 2>/dev/null || true
kldunload ng_mss_rewrite 2>/dev/null || true
sleep 1

if ! kldload ${MODULE_DIR}/ng_mss_rewrite.ko; then
    printf "${RED}Failed to load ng_mss_rewrite${NC}\n"
    exit 1
fi

if ! kldload ${SCRIPT_DIR}/ng_mbuf_inject.ko; then
    printf "${RED}Failed to load ng_mbuf_inject${NC}\n"
    exit 1
fi

printf "Modules loaded successfully\n"

# Cleanup function
cleanup() {
    printf "\nCleaning up...\n"
    if [ -n "$BUILDER_PID" ]; then
        kill $BUILDER_PID 2>/dev/null || true
        wait $BUILDER_PID 2>/dev/null || true
    fi
    ngctl shutdown wire_verify_inject: 2>/dev/null || true
    ngctl shutdown wire_verify_mss: 2>/dev/null || true
}
trap cleanup EXIT

# Create netgraph topology:
# inject:output -> mss_rewrite:lower -> mss_rewrite:upper -> inject:input
printf "Creating netgraph topology...\n"

${SCRIPT_DIR}/ng_builder_generic wire_verify &
BUILDER_PID=$!

# Wait for topology to be created
sleep 2

# Check if builder process is still running
if ! kill -0 $BUILDER_PID 2>/dev/null; then
    printf "${RED}ng_builder_wire_verify process died unexpectedly${NC}\n"
    wait $BUILDER_PID
    exit 1
fi

# Verify nodes exist
if ! ngctl show wire_verify_inject: >/dev/null 2>&1; then
    printf "${RED}wire_verify_inject node not found${NC}\n"
    ngctl list
    exit 1
fi

if ! ngctl show wire_verify_mss: >/dev/null 2>&1; then
    printf "${RED}wire_verify_mss node not found${NC}\n"
    ngctl list
    exit 1
fi

printf "Topology created successfully\n\n"

# Configure MSS rewrite to 1300
printf "Configuring MSS rewrite to 1300...\n"
ngctl msg wire_verify_mss: setmss '{ mss_ipv4=1300 mss_ipv6=1300 }'
ngctl msg wire_verify_mss: setstatsmode '{ mode=1 }'
ngctl msg wire_verify_mss: setdirection '{ enable_lower=1 enable_upper=1 }'

# Test function
test_mss_rewrite() {
    local orig_mss=$1
    local expected_mss=$2
    local test_name=$3

    printf "\n${YELLOW}Test: ${test_name}${NC}\n"
    printf "  Original MSS: %d, Expected MSS: %d\n" $orig_mss $expected_mss

    # Inject packet with original MSS
    ngctl msg wire_verify_inject: inject_single "{ mss=$orig_mss ipv6=0 split_offset=0 csum_flags=0 ext_type=0 }" >/dev/null 2>&1

    # Small delay to ensure packet is processed
    sleep 0.1

    # Get last received packet info
    result=$(ngctl msg wire_verify_inject: getlastpkt 2>&1)

    # Parse results (extract only the value after '=')
    valid=$(echo "$result" | grep -o 'valid=[0-9]*' | cut -d= -f2)
    received_mss=$(echo "$result" | grep -o 'mss=[0-9]*' | cut -d= -f2)
    checksum=$(echo "$result" | grep -o 'checksum=[0-9]*' | cut -d= -f2)

    if [ "$valid" != "1" ]; then
        printf "  ${RED}FAIL: No valid packet received${NC}\n"
        return 1
    fi

    printf "  Received MSS: %d\n" $received_mss
    printf "  TCP Checksum: %s\n" $checksum

    if [ "$received_mss" = "$expected_mss" ]; then
        printf "  ${GREEN}PASS: MSS correctly rewritten${NC}\n"
        return 0
    else
        printf "  ${RED}FAIL: MSS not rewritten correctly (got %d, expected %d)${NC}\n" $received_mss $expected_mss
        return 1
    fi
}

# Run tests
passed=0
failed=0

# Test 1: MSS > max_mss (should be rewritten)
if test_mss_rewrite 1460 1300 "MSS 1460 -> 1300 (should rewrite)"; then
    passed=$((passed + 1))
else
    failed=$((failed + 1))
fi

# Test 2: MSS < max_mss (should not be rewritten)
if test_mss_rewrite 1200 1200 "MSS 1200 -> 1200 (should not rewrite)"; then
    passed=$((passed + 1))
else
    failed=$((failed + 1))
fi

# Test 3: MSS == max_mss (should not be rewritten)
if test_mss_rewrite 1300 1300 "MSS 1300 -> 1300 (should not rewrite)"; then
    passed=$((passed + 1))
else
    failed=$((failed + 1))
fi

# Test 4: Very large MSS
if test_mss_rewrite 9000 1300 "MSS 9000 -> 1300 (jumbo frame)"; then
    passed=$((passed + 1))
else
    failed=$((failed + 1))
fi

# Print summary
printf "\n${YELLOW}=== Summary ===${NC}\n"
printf "Total: %d tests\n" $((passed + failed))
printf "${GREEN}Passed: %d${NC}\n" $passed
if [ $failed -gt 0 ]; then
    printf "${RED}Failed: %d${NC}\n" $failed
    exit 1
else
    printf "${GREEN}All tests passed!${NC}\n"
    exit 0
fi
