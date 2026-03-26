#!/bin/sh
# Simple test to verify topology and configuration

if [ "$(id -u)" != "0" ]; then
    echo "Must run as root"
    exit 1
fi

echo "Cleaning up..."
pkill -f ng_builder
sleep 2
ngctl shutdown mss_bench_source: 2>/dev/null
ngctl shutdown mss_bench_mss: 2>/dev/null
ngctl shutdown mss_bench_hole: 2>/dev/null
sleep 1

# Force reload module to test latest binary
echo "Reloading ng_mss_rewrite module..."
if kldstat | grep -q ng_mss_rewrite; then
    echo "Unloading existing ng_mss_rewrite module..."
    kldunload ng_mss_rewrite || {
        echo "ERROR: Failed to unload ng_mss_rewrite (may be in use)"
        exit 1
    }
    sleep 1
fi

echo "Loading ng_mss_rewrite module..."
if [ -f "../ng_mss_rewrite.ko" ]; then
    echo "Loading local module: ../ng_mss_rewrite.ko"
    kldload ../ng_mss_rewrite.ko || {
        echo "ERROR: Failed to load ng_mss_rewrite.ko"
        exit 1
    }
elif [ -f "./ng_mss_rewrite.ko" ]; then
    echo "Loading local module: ./ng_mss_rewrite.ko"
    kldload ./ng_mss_rewrite.ko || {
        echo "ERROR: Failed to load ng_mss_rewrite.ko"
        exit 1
    }
else
    echo "Loading system module: ng_mss_rewrite"
    kldload ng_mss_rewrite || {
        echo "ERROR: Failed to load ng_mss_rewrite"
        exit 1
    }
fi

# Verify module is loaded
if ! kldstat | grep -q ng_mss_rewrite; then
    echo "ERROR: ng_mss_rewrite not loaded after kldload"
    exit 1
fi

echo "Module loaded successfully"

echo "Starting ng_builder..."
./test/ng_builder &
sleep 3

echo ""
echo "Listing nodes:"
ngctl list | grep mss_bench

echo ""
echo "Showing mss_bench_mss topology:"
ngctl show mss_bench_mss:

echo ""
echo "Testing setmss command:"
ngctl msg mss_bench_mss: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"

if [ $? -eq 0 ]; then
    echo "SUCCESS: setmss command worked!"
    echo ""
    echo "Getting MSS values:"
    ngctl msg mss_bench_mss: getmss
else
    echo "FAILED: setmss command failed"
fi

echo ""
echo "Cleaning up..."
pkill -f ng_builder
sleep 1

echo "Done"
