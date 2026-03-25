# Quick Start Guide

## 1. Build the Module

```bash
cd /home/kazubu/tcpmss-rewrite
make clean
make

# For maximum performance (no statistics):
# make CFLAGS="-DENABLE_STATS=0"
```

## 2. Load the Module

```bash
sudo kldload ./ng_mss_rewrite.ko
```

Verify:
```bash
kldstat | grep mss_rewrite
```

## 3. Setup (Automated)

Replace `em0` with your physical interface:

```bash
sudo ./setup.sh em0 1400 1380
```

This will:
- Create the mss_rewrite node
- Connect it between the physical interface and kernel stack
- Set MSS to 1400 (IPv4) and 1380 (IPv6)

## 4. Verify Configuration

```bash
# Check node exists
ngctl list | grep mss

# Check MSS values
ngctl msg em0_mss: getmss

# Check statistics
ngctl msg em0_mss: getstats
```

## 5. Test

Generate some TCP traffic and check if MSS is being rewritten:

```bash
# Monitor TCP SYN packets
sudo tcpdump -i em0 -vv 'tcp[tcpflags] & tcp-syn != 0' | grep mss
```

In another terminal:
```bash
# Generate traffic (replace with actual IP)
curl http://example.com
nc -z 8.8.8.8 80
```

You should see MSS values limited to 1400 (IPv4) or 1380 (IPv6).

Check statistics:
```bash
ngctl msg em0_mss: getstats
```

## 6. Performance Tuning (Optional)

For maximum performance in production, disable statistics at runtime:

```bash
# Disable statistics (5-10% faster)
ngctl msg em0_mss: setstatsmode "{ mode=0 }"

# Check current mode
ngctl msg em0_mss: getstatsmode
```

**Statistics modes:**
- **0**: DISABLED (fastest, no overhead)
- **1**: PERCPU (minimal overhead ~1-2%, default)

**Note**: Switching modes does not reset counters. You can freely switch between modes without losing data.

## 7. Manual Setup (Alternative)

If you prefer manual setup:

```bash
# Create node
ngctl mkpeer em0: mss_rewrite lower lower

# Name it
ngctl name em0:lower em0_mss

# Connect upper hook
ngctl connect em0_mss: em0: upper upper

# Set MSS values
ngctl msg em0_mss: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"

# Verify
ngctl show em0_mss:
```

## 8. Remove

To remove the node:

```bash
ngctl shutdown em0_mss:
```

To unload the module:

```bash
sudo kldunload ng_mss_rewrite
```

## Common Issues

### "Failed to create node"
- Check if the interface name is correct: `ifconfig`
- Verify the module is loaded: `kldstat | grep mss_rewrite`

### "No packets being processed"
- Verify hooks are connected: `ngctl show em0_mss:`
- Check if traffic is flowing: `tcpdump -i em0 -c 10`

### Build errors
- Ensure kernel sources are installed
- Check FreeBSD version compatibility (requires 14.x+)

## Performance Tips

- This module only processes TCP SYN packets
- Non-SYN packets pass through with minimal overhead
- Typical CPU usage: <1% even on 10Gbps links
- For multiple physical interfaces, create separate nodes for each

## See Also

- Full documentation: [README.md](README.md)
- Test script: `./test/test.sh em0 <target_ip>`
