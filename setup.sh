#!/bin/sh
#
# Setup script for ng_mss_rewrite
# Usage: ./setup.sh <interface> [mss_ipv4] [mss_ipv6]
#

if [ $# -lt 1 ]; then
    echo "Usage: $0 <interface> [mss_ipv4] [mss_ipv6]"
    echo "Example: $0 em0 1400 1380"
    exit 1
fi

INTERFACE="$1"
MSS_IPV4="${2:-1400}"
MSS_IPV6="${3:-1380}"
NODE_NAME="${INTERFACE}_mss"

# Check if running as root
if [ "$(id -u)" != "0" ]; then
    echo "This script must be run as root"
    exit 1
fi

# Load module if not already loaded
if ! kldstat | grep -q ng_mss_rewrite; then
    echo "Loading ng_mss_rewrite module..."
    if [ -f ./ng_mss_rewrite.ko ]; then
        kldload ./ng_mss_rewrite.ko
    else
        kldload ng_mss_rewrite
    fi

    if [ $? -ne 0 ]; then
        echo "Failed to load module"
        exit 1
    fi
fi

# Load ng_ether module if not already loaded
if ! kldstat | grep -q ng_ether; then
    echo "Loading ng_ether module..."
    kldload ng_ether

    if [ $? -ne 0 ]; then
        echo "Failed to load module"
        exit 1
    fi
fi


# Check if node already exists
if ngctl list | grep -q "$NODE_NAME"; then
    echo "Node $NODE_NAME already exists. Removing..."
    ngctl shutdown "$NODE_NAME:"
    sleep 1
fi

# Create the mss_rewrite node
echo "Creating mss_rewrite node..."
ngctl mkpeer ${INTERFACE}: mss_rewrite lower lower
if [ $? -ne 0 ]; then
    echo "Failed to create node"
    exit 1
fi

# Name the node
echo "Naming node as $NODE_NAME..."
ngctl name ${INTERFACE}:lower "$NODE_NAME"
if [ $? -ne 0 ]; then
    echo "Failed to name node"
    exit 1
fi

# Connect upper hook
echo "Connecting upper hook..."
ngctl connect ${NODE_NAME}: ${INTERFACE}: upper upper
if [ $? -ne 0 ]; then
    echo "Failed to connect upper hook"
    exit 1
fi

# Set MSS values
echo "Setting MSS values (IPv4: $MSS_IPV4, IPv6: $MSS_IPV6)..."
ngctl msg ${NODE_NAME}: setmss "{ mss_ipv4=${MSS_IPV4} mss_ipv6=${MSS_IPV6} }"
if [ $? -ne 0 ]; then
    echo "Failed to set MSS values"
    exit 1
fi

# Verify configuration
echo ""
echo "Configuration complete!"
echo ""
echo "Node information:"
ngctl show ${NODE_NAME}:
echo ""
echo "Current MSS values:"
ngctl msg ${NODE_NAME}: getmss

echo ""
echo "To monitor statistics, run:"
echo "  ngctl msg ${NODE_NAME}: getstats"
echo ""
echo "To remove the node, run:"
echo "  ngctl shutdown ${NODE_NAME}:"
