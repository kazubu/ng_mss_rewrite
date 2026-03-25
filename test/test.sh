#!/bin/sh
#
# Test script for ng_mss_rewrite
# Usage: ./test.sh <interface> <target_ip> [port]
#

if [ $# -lt 2 ]; then
    echo "Usage: $0 <interface> <target_ip> [port]"
    echo "Example: $0 em0 192.168.1.1 80"
    exit 1
fi

INTERFACE="$1"
TARGET_IP="$2"
PORT="${3:-80}"
NODE_NAME="${INTERFACE}_mss"

# Check if running as root
if [ "$(id -u)" != "0" ]; then
    echo "This script must be run as root"
    exit 1
fi

# Check if node exists
if ! ngctl list | grep -q "$NODE_NAME"; then
    echo "Error: Node $NODE_NAME not found"
    echo "Run setup.sh first"
    exit 1
fi

echo "=== ng_mss_rewrite Test ==="
echo ""
echo "Interface: $INTERFACE"
echo "Target: $TARGET_IP:$PORT"
echo "Node: $NODE_NAME"
echo ""

# Get initial statistics
echo "Initial statistics:"
ngctl msg ${NODE_NAME}: getstats
echo ""

# Start tcpdump in background
TCPDUMP_FILE="/tmp/mss_test_$$.pcap"
echo "Starting packet capture (${TCPDUMP_FILE})..."
tcpdump -i ${INTERFACE} -w ${TCPDUMP_FILE} "tcp and host ${TARGET_IP} and port ${PORT}" &
TCPDUMP_PID=$!
sleep 2

# Generate TCP connection
echo "Generating TCP SYN packet to ${TARGET_IP}:${PORT}..."
nc -w 1 -z ${TARGET_IP} ${PORT} 2>/dev/null
sleep 1

# Stop tcpdump
echo "Stopping packet capture..."
kill ${TCPDUMP_PID} 2>/dev/null
wait ${TCPDUMP_PID} 2>/dev/null

# Display captured packets
echo ""
echo "Captured packets with MSS option:"
tcpdump -r ${TCPDUMP_FILE} -vv 'tcp[tcpflags] & tcp-syn != 0' 2>/dev/null | grep -E '(mss|Flags \[S\])'
echo ""

# Get final statistics
echo "Final statistics:"
ngctl msg ${NODE_NAME}: getstats
echo ""

# Get current MSS configuration
echo "Current MSS configuration:"
ngctl msg ${NODE_NAME}: getmss
echo ""

# Cleanup
rm -f ${TCPDUMP_FILE}

echo "Test complete!"
echo ""
echo "If packets_processed increased, the node is working."
echo "If packets_rewritten increased, MSS values were rewritten."
echo ""
echo "To manually inspect packets, run:"
echo "  tcpdump -i ${INTERFACE} -vv 'tcp[tcpflags] & tcp-syn != 0' | grep mss"
