#!/bin/sh
#
# Performance benchmark for ng_mss_rewrite using ng_source
# This test generates synthetic TCP SYN packets and measures throughput
#

if [ "$(id -u)" != "0" ]; then
    echo "This script must be run as root"
    exit 1
fi

# Load required modules
kldload ng_ether 2>/dev/null || true
kldload ng_source 2>/dev/null || true
kldload ng_hole 2>/dev/null || true
kldload ./ng_mss_rewrite.ko 2>/dev/null || kldload ng_mss_rewrite

# Configuration
NODE_NAME="mss_bench"
PACKET_COUNT=${1:-100000}
PACKET_SIZE=64
MSS_IPV4=1400

echo "=== ng_mss_rewrite Performance Benchmark ==="
echo "Packet count: $PACKET_COUNT"
echo "Packet size: $PACKET_SIZE bytes"
echo ""

# Cleanup function
cleanup() {
    ngctl shutdown ${NODE_NAME}_source: 2>/dev/null
    ngctl shutdown ${NODE_NAME}_mss: 2>/dev/null
    ngctl shutdown ${NODE_NAME}_hole: 2>/dev/null
}

trap cleanup EXIT

# Clean up any existing nodes
cleanup

# Create test topology:
# ng_source -> mss_rewrite -> ng_hole

echo "Setting up test topology..."

# Create ng_source node
ngctl mkpeer ng_source: mss_rewrite output lower
ngctl name ng_source:output ${NODE_NAME}_mss
ngctl mkpeer ${NODE_NAME}_mss: hole upper data
ngctl name ${NODE_NAME}_mss:upper ${NODE_NAME}_hole

# Configure mss_rewrite
ngctl msg ${NODE_NAME}_mss: setmss "{ mss_ipv4=${MSS_IPV4} mss_ipv6=1380 }"

# Enable per-CPU statistics for benchmarking
ngctl msg ${NODE_NAME}_mss: setstatsmode "{ mode=1 }"

# Reset stats
ngctl msg ${NODE_NAME}_mss: resetstats

# Create a TCP SYN packet with MSS option
# Ethernet + IPv4 + TCP with MSS=1460
PACKET_HEX="ffffffffffff000000000000080045000028000100004006f97f0a0000010a000002"
PACKET_HEX="${PACKET_HEX}00500050000000000000000050022000e45e00000204"
PACKET_HEX="${PACKET_HEX}05b4"  # MSS option: kind=2, len=4, MSS=1460

# Configure ng_source
ngctl msg ${NODE_NAME}_mss:lower setpkt "{ length=${PACKET_SIZE} data=0x${PACKET_HEX} }"
ngctl msg ${NODE_NAME}_mss:lower setconfig "{ packets=${PACKET_COUNT} }"

echo "Starting benchmark..."
START_TIME=$(date +%s)

# Start packet transmission
ngctl msg ${NODE_NAME}_mss:lower start

# Wait for completion (poll ng_source stats)
while true; do
    STATS=$(ngctl msg ${NODE_NAME}_mss:lower getstats 2>/dev/null | grep -o "packets=[0-9]*" | cut -d= -f2)
    if [ "$STATS" = "$PACKET_COUNT" ] || [ -z "$STATS" ]; then
        break
    fi
    sleep 0.1
done

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo "=== Benchmark Results ==="
echo "Elapsed time: ${ELAPSED} seconds"

if [ $ELAPSED -gt 0 ]; then
    PPS=$((PACKET_COUNT / ELAPSED))
    MBPS=$((PACKET_COUNT * PACKET_SIZE * 8 / ELAPSED / 1000000))
    echo "Throughput: ${PPS} packets/sec"
    echo "Bandwidth: ${MBPS} Mbps"
fi

echo ""
echo "=== MSS Rewrite Statistics ==="
ngctl msg ${NODE_NAME}_mss: getstats

echo ""
echo "=== Configuration ==="
ngctl msg ${NODE_NAME}_mss: getmss
ngctl msg ${NODE_NAME}_mss: getdirection
ngctl msg ${NODE_NAME}_mss: getstatsmode

echo ""
echo "Benchmark complete!"
