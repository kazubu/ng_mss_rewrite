# ng_mss_rewrite - Netgraph TCP MSS Rewriting Module

FreeBSD netgraph node for rewriting TCP MSS (Maximum Segment Size) option in SYN packets.

## Features

- Rewrites TCP MSS option in SYN packets
- Supports IPv4 (all cases) and IPv6 (simple cases only, see limitations below)
- Handles VLAN-tagged frames (single, TPID 0x8100 only)
- Only rewrites if existing MSS > configured MSS (never increases MSS)
- High performance (processes packets at line rate)
- Works at L2 level on physical interface
- Automatically applies to all VLANs/bridges/tunnels
- Validates packet headers and skips fragmented packets
- **Directional filtering for optimal performance:**
  - **Default**: Process only incoming packets (interface→kernel)
  - **Configurable**: Enable/disable per direction at runtime
  - Reduces CPU load by ~50% in typical deployments
  - **Checksum offload aware**: Skips rewrite when TSO/checksum offload is active (upper direction)
- **Two statistics modes for optimal performance:**
  - **Disabled**: Zero overhead (default)
  - **Per-CPU**: Minimal overhead (~1-2%)
  - Runtime switchable without data loss
- **Safe mbuf handling:**
  - Uses `m_copydata()` for header parsing (handles fragmented mbuf chains)
  - Only modifies mbuf when rewrite is actually needed
  - Zero pullup overhead for packets that don't need rewriting

## Limitations

### IPv6 Support

**Currently supports IPv6 packets where TCP immediately follows the IPv6 base header only.**

- ✅ Works: IPv6 base header → TCP header
- ❌ Does NOT work: IPv6 with extension headers (Hop-by-Hop, Routing, Fragment, etc.)

This is a known limitation. Most IPv6 TCP SYN packets do not use extension headers, so this covers the majority of real-world traffic.

### Other Limitations

- Fragmented IPv4 packets are skipped (SYN packets are rarely fragmented)
- Statistics counter `packets_processed` counts TCP SYN packets only, not all packets

## Requirements

- FreeBSD 14.x or later
- Kernel sources installed
- Root privileges

## Building

```bash
# Build the kernel module (with statistics enabled by default)
make

# Build without statistics support (maximum performance)
make CFLAGS="-DENABLE_STATS=0"

# Install the module (optional)
sudo make install
```

### Build Options

- `ENABLE_STATS=1` (default): Enable statistics support
- `ENABLE_STATS=0`: Disable all statistics at compile time (5-20% faster)

## Loading

```bash
# Load the module
sudo kldload ./ng_mss_rewrite.ko

# Or if installed:
sudo kldload ng_mss_rewrite

# Verify it's loaded
kldstat | grep mss_rewrite
```

## Configuration

### Basic Setup

This example shows how to insert the MSS rewrite node between a physical interface (em0) and the kernel stack:

```bash
# 1. Create the mss_rewrite node
ngctl mkpeer em0: mss_rewrite lower lower

# 2. Name the node for easier reference
ngctl name em0:lower mss0

# 3. Connect the upper hook to the kernel stack
ngctl connect mss0: em0: upper upper

# 4. (Optional) Set custom MSS values
ngctl msg mss0: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"
```

### Understanding the Packet Flow

```
Physical NIC (em0)
      ↕
[ng_ether node]
      ↕ lower hook
[mss_rewrite node] ← Rewrites MSS here
      ↕ upper hook
[ng_ether node]
      ↕
Kernel Stack (VLAN/bridge/gif processing)
```

### Default MSS Values

- IPv4: 1400 bytes
- IPv6: 1380 bytes

These defaults are suitable for most scenarios including PPPoE, GRE, and other encapsulation protocols.

## Control Commands

### Set MSS Values

```bash
ngctl msg mss0: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"
```

### Get Current MSS Values

```bash
ngctl msg mss0: getmss
```

### Get Statistics

```bash
ngctl msg mss0: getstats
```

Output:
```
Rec'd response "getstats" (4) from "mss0:":
Args:   { packets_processed=12345 packets_rewritten=678 }
```

### Reset Statistics

```bash
ngctl msg mss0: resetstats
```

### Statistics Mode Control

#### Get Current Statistics Mode

```bash
ngctl msg mss0: getstatsmode
```

Output:
```
Rec'd response "getstatsmode" (6) from "mss0:":
Args:   { mode=1 }
```

Modes:
- `0`: Disabled (no statistics collection, default)
- `1`: Per-CPU (minimal overhead, best performance when statistics are needed)

#### Statistics Mode Control

Statistics mode can be changed at runtime without losing accumulated data.

**Get Current Mode:**
```bash
ngctl msg mss0: getstatsmode
```

Output: `{ mode=0 }` (DISABLED) or `{ mode=1 }` (PERCPU)

**Set Statistics Mode:**
```bash
# Disable statistics (maximum performance)
ngctl msg mss0: setstatsmode "{ mode=0 }"

# Enable per-CPU statistics (minimal overhead, recommended)
ngctl msg mss0: setstatsmode "{ mode=1 }"
```

**Performance Impact:**
- **DISABLED (0)**: Zero overhead (default)
- **PERCPU (1)**: Minimal overhead (~1-2%)

**Important Notes:**
- Switching modes does NOT reset counters (baseline method)
- Per-CPU array allocated once, never freed until node shutdown
- Safe to switch modes at any time during operation
- Use DISABLED mode in production for maximum performance

### Directional Filtering

Control which traffic directions are processed for optimal performance.

**Get Current Direction Settings:**
```bash
ngctl msg mss0: getdirection
```

Output: `{ enable_lower=1 enable_upper=0 }`

**Set Direction:**
```bash
# Process only incoming packets (default, recommended)
ngctl msg mss0: setdirection "{ enable_lower=1 enable_upper=0 }"

# Process both directions
ngctl msg mss0: setdirection "{ enable_lower=1 enable_upper=1 }"

# Process only outgoing packets (rare use case)
ngctl msg mss0: setdirection "{ enable_lower=0 enable_upper=1 }"
```

**Direction Modes:**
- **enable_lower=1**: Process packets from physical interface to kernel (incoming)
- **enable_upper=1**: Process packets from kernel to physical interface (outgoing)

**Default:** `enable_lower=1, enable_upper=0` (incoming only)

**Rationale:**
TCP MSS negotiation uses the minimum MSS from both sides. Processing only
one direction is usually sufficient, reducing CPU load by ~50%.

**Performance Impact:**
- Single direction: ~50% reduction in processed packets
- Zero overhead for disabled direction (simple flag check)
- Configurable at runtime without service interruption

## Verification

### Check MSS Values in Packets

```bash
# Capture TCP SYN packets and check MSS
sudo tcpdump -i em0 -vv 'tcp[tcpflags] & tcp-syn != 0' -c 10 | grep mss
```

You should see MSS values limited to 1400 (IPv4) or 1380 (IPv6).

### Monitor Statistics

```bash
# Before generating traffic
ngctl msg mss0: getstats

# Generate TCP connections
# (e.g., curl, nc, iperf3, etc.)

# After generating traffic
ngctl msg mss0: getstats
```

## Removal

```bash
# Shutdown the node (this will automatically remove hooks)
ngctl shutdown mss0:

# Unload the module
sudo kldunload ng_mss_rewrite
```

## How It Works

1. **Packet Reception**: All packets from the physical interface pass through the `lower` hook
2. **Packet Analysis**:
   - Parses Ethernet header
   - Handles VLAN tags (802.1Q)
   - Identifies IPv4/IPv6 packets
   - Locates TCP header
   - Checks for SYN flag
3. **MSS Rewriting**:
   - Searches for TCP MSS option (option kind 2)
   - Compares existing MSS with configured limit
   - If existing MSS > limit, rewrites to limit
   - Recalculates TCP checksum
4. **Forwarding**: Passes packet to kernel stack via `upper` hook

## Use Cases

### PPPoE/DSL Connections

```bash
# em0 is your physical interface connected to DSL modem
ngctl mkpeer em0: mss_rewrite lower lower
ngctl name em0:lower mss0
ngctl connect mss0: em0: upper upper
ngctl msg mss0: setmss "{ mss_ipv4=1452 mss_ipv6=1432 }"
```

### GRE/IPIP Tunnels

```bash
# Reduce MSS to account for tunnel overhead
ngctl mkpeer em0: mss_rewrite lower lower
ngctl name em0:lower mss0
ngctl connect mss0: em0: upper upper
ngctl msg mss0: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"
```

### Multiple VLANs with Tunnels

```bash
# Single configuration at physical interface level
# Automatically applies to all VLANs (em0.100, em0.200, etc.)
ngctl mkpeer em0: mss_rewrite lower lower
ngctl name em0:lower mss0
ngctl connect mss0: em0: upper upper
```

## Performance Benchmarking

Synthetic benchmarks using netgraph without real NICs:

### Quick Benchmark

```bash
cd test
sudo ./bench.sh 100000
```

This creates a test topology: `ng_source -> mss_rewrite -> ng_sink`

The script generates TCP SYN packets with MSS=1460 and measures throughput.

### Comprehensive Benchmark

```bash
cd test
sudo ./bench_scenarios.sh
```

This runs multiple scenarios:
- TCP SYN with MSS > limit (rewrite needed)
- TCP SYN with MSS < limit (no rewrite)
- TCP non-SYN packets (fast path)
- UDP packets (fastest path)

Each scenario is tested with different configurations:
- Statistics enabled vs disabled
- Processing enabled vs disabled (directional filtering)

Expected results:
- Non-TCP packets: highest throughput (minimal processing)
- Non-SYN TCP: high throughput (SYN check only)
- SYN without rewrite: medium throughput (option parsing)
- SYN with rewrite: lower throughput (full processing)
- Stats disabled should be 1-2% faster than enabled

### Interpreting Results

Typical performance on modern hardware (example):
- Non-TCP: 1-2 Mpps (million packets per second)
- Non-SYN TCP: 800k-1.5 Mpps
- SYN with rewrite: 500k-1 Mpps

Actual performance depends on:
- CPU speed and architecture
- Memory bandwidth
- Kernel configuration
- System load

## Troubleshooting

### Module fails to load

Check kernel message buffer:
```bash
dmesg | tail -20
```

### Packets not being rewritten

1. Verify node is connected:
```bash
ngctl show mss0:
```

2. Check statistics:
```bash
ngctl msg mss0: getstats
```

If `packets_processed` is 0, the node is not receiving traffic.

3. Verify hooks are correct:
```bash
ngctl list
ngctl show em0:
```

### Performance issues

This module processes packets in the kernel with minimal overhead. If you experience performance issues:

1. Check system resources (CPU, memory)
2. Verify hardware capabilities
3. Check for other bottlenecks (disk I/O, network congestion)

## Performance

- **Throughput**: Designed to handle 10Gbps+ line rate
- **Latency**: Minimal overhead (<1μs per packet)
- **CPU Usage**: Only processes TCP SYN packets (typically <1% of total traffic)
- **Non-SYN packets**: Pass through with near-zero overhead

## Security Considerations

- This module only reads and potentially modifies TCP MSS option
- It does not log packet contents
- No sensitive data is stored
- Statistics counters are reset on node shutdown

## License

BSD 2-Clause License

## Author

Kazuki Shimizu

## References

- FreeBSD netgraph(4) manual: `man 4 netgraph`
- ngctl(8) manual: `man 8 ngctl`
- TCP MSS (RFC 793, RFC 879)
