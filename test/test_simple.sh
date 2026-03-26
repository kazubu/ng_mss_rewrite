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

# Reload module
if kldstat | grep -q ng_mss_rewrite; then
    echo "Unloading existing ng_mss_rewrite module..."
    kldunload ng_mss_rewrite 2>/dev/null || true
    sleep 1
fi

echo "Loading ng_mss_rewrite module..."
kldload ./ng_mss_rewrite.ko 2>/dev/null || kldload ng_mss_rewrite || exit 1

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
