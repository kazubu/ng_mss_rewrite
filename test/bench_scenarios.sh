#!/bin/sh
#
# Comprehensive performance benchmark with multiple scenarios
#

if [ "$(id -u)" != "0" ]; then
    echo "This script must be run as root"
    exit 1
fi

# Load modules
kldload ng_source 2>/dev/null || true
kldload ng_hole 2>/dev/null || true
kldload ./ng_mss_rewrite.ko 2>/dev/null || kldload ng_mss_rewrite

PACKET_COUNT=100000

echo "=== ng_mss_rewrite Comprehensive Benchmark ==="
echo "Packet count per scenario: $PACKET_COUNT"
echo ""

# Cleanup function
cleanup() {
    ngctl shutdown bench_source: 2>/dev/null
    ngctl shutdown bench_mss: 2>/dev/null
    ngctl shutdown bench_hole: 2>/dev/null
}

trap cleanup EXIT

# Function to run a single benchmark
run_benchmark() {
    local SCENARIO="$1"
    local PACKET_HEX="$2"
    local ENABLE_LOWER="$3"
    local STATS_MODE="$4"

    echo "--- Scenario: $SCENARIO ---"

    cleanup

    # Create topology
    ngctl mkpeer ng_source: mss_rewrite output lower || return 1
    ngctl name ng_source:output bench_mss || return 1
    ngctl mkpeer bench_mss: hole upper data || return 1
    ngctl name bench_mss:upper bench_hole || return 1

    # Configure mss_rewrite
    ngctl msg bench_mss: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }" || return 1
    ngctl msg bench_mss: setdirection "{ enable_lower=${ENABLE_LOWER} enable_upper=0 }" || return 1
    ngctl msg bench_mss: setstatsmode "{ mode=${STATS_MODE} }" || return 1
    ngctl msg bench_mss: resetstats || return 1

    # Configure packet
    ngctl msg bench_mss:lower setpkt "{ length=64 data=0x${PACKET_HEX} }" || return 1
    ngctl msg bench_mss:lower setconfig "{ packets=${PACKET_COUNT} }" || return 1

    # Run benchmark
    START_TIME=$(date +%s.%N 2>/dev/null || date +%s)
    ngctl msg bench_mss:lower start || return 1

    # Wait for completion
    TIMEOUT=60
    COUNTER=0
    while [ $COUNTER -lt $TIMEOUT ]; do
        STATS=$(ngctl msg bench_mss:lower getstats 2>/dev/null | grep -o "packets=[0-9]*" | head -1 | cut -d= -f2)
        if [ "$STATS" = "$PACKET_COUNT" ]; then
            break
        fi
        sleep 0.1
        COUNTER=$((COUNTER + 1))
    done

    END_TIME=$(date +%s.%N 2>/dev/null || date +%s)

    # Calculate results
    if command -v bc >/dev/null 2>&1; then
        ELAPSED=$(echo "$END_TIME - $START_TIME" | bc)
        if [ "$(echo "$ELAPSED > 0" | bc)" = "1" ]; then
            PPS=$(echo "$PACKET_COUNT / $ELAPSED" | bc)
            echo "  Throughput: ${PPS} pps"
        fi
    else
        ELAPSED_INT=$(($(echo $END_TIME | cut -d. -f1) - $(echo $START_TIME | cut -d. -f1)))
        if [ $ELAPSED_INT -gt 0 ]; then
            PPS=$((PACKET_COUNT / ELAPSED_INT))
            echo "  Throughput: ~${PPS} pps"
        fi
    fi

    # Show stats
    ngctl msg bench_mss: getstats 2>/dev/null | grep "Args:" | sed 's/Args:/  Stats:/'
    echo ""
}

# Scenario 1: TCP SYN with MSS=1460 (needs rewrite)
echo "=== Test 1: TCP SYN with MSS=1460 (rewrite needed) ==="
SYN_MSS_1460="ffffffffffff000000000000080045000034000100004006f9730a0000010a000002"
SYN_MSS_1460="${SYN_MSS_1460}00500050000000000000000060022000d05200000204"
SYN_MSS_1460="${SYN_MSS_1460}05b40103030801010402"  # MSS=1460 + other options

run_benchmark "SYN MSS=1460, stats enabled" "$SYN_MSS_1460" 1 1
run_benchmark "SYN MSS=1460, stats disabled" "$SYN_MSS_1460" 1 0
run_benchmark "SYN MSS=1460, processing disabled" "$SYN_MSS_1460" 0 0

# Scenario 2: TCP SYN with MSS=1200 (no rewrite needed)
echo "=== Test 2: TCP SYN with MSS=1200 (no rewrite needed) ==="
SYN_MSS_1200="ffffffffffff000000000000080045000034000100004006f9730a0000010a000002"
SYN_MSS_1200="${SYN_MSS_1200}00500050000000000000000060022000d15200000204"
SYN_MSS_1200="${SYN_MSS_1200}04b00103030801010402"  # MSS=1200 + other options

run_benchmark "SYN MSS=1200, stats enabled" "$SYN_MSS_1200" 1 1
run_benchmark "SYN MSS=1200, stats disabled" "$SYN_MSS_1200" 1 0

# Scenario 3: TCP non-SYN (fast path)
echo "=== Test 3: TCP ACK packet (non-SYN, fast path) ==="
TCP_ACK="ffffffffffff000000000000080045000028000100004006f97f0a0000010a000002"
TCP_ACK="${TCP_ACK}005000500000000000000000501000003b6500000000"

run_benchmark "TCP ACK, stats enabled" "$TCP_ACK" 1 1
run_benchmark "TCP ACK, stats disabled" "$TCP_ACK" 1 0

# Scenario 4: Non-TCP packet (fastest path)
echo "=== Test 4: UDP packet (non-TCP, fastest path) ==="
UDP_PKT="ffffffffffff00000000000008004500001c000100004011f9890a0000010a000002"
UDP_PKT="${UDP_PKT}00350035000800000000"

run_benchmark "UDP, stats enabled" "$UDP_PKT" 1 1
run_benchmark "UDP, stats disabled" "$UDP_PKT" 1 0

cleanup

echo "=== Benchmark Complete ==="
echo ""
echo "Summary:"
echo "- Test 1 shows performance when MSS rewriting is needed"
echo "- Test 2 shows performance when MSS is already acceptable"
echo "- Test 3 shows fast path for non-SYN TCP packets"
echo "- Test 4 shows fastest path for non-TCP packets"
echo "- Stats disabled should be faster than stats enabled"
echo "- Processing disabled should bypass all checks"
