# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**ng_mss_rewrite** is a FreeBSD kernel module (netgraph node) that rewrites TCP MSS (Maximum Segment Size) options in SYN packets. It operates at Layer 2 between physical network interfaces and the kernel stack, transparently limiting MSS to handle MTU issues from encapsulation (PPPoE, GRE, tunnels, etc.).

**Target Platform**: FreeBSD 14.x+ kernel module development

## Build System

```bash
# Standard build (with statistics enabled)
make

# Debug statistics build (for development/testing)
make DEBUG_STATS=1

# Build without statistics (maximum performance)
make CFLAGS="-DENABLE_STATS=0"

# Clean build
make clean
```

**Build Options**:
- `ENABLE_STATS=0|1` - Compile-time statistics support (default: 1)
- `DEBUG_STATS=1` - Enable 15 additional debug counters for code path verification

## Testing

**CRITICAL**: All tests require root privileges and must be run on FreeBSD.

```bash
# Primary test suite - validates mbuf handling edge cases
sudo ./test/test_mbuf_shape.sh

# Basic functionality test
sudo ./test/test_simple.sh

# Comprehensive test cases (IPv4/IPv6, VLAN, etc.)
sudo ./test/test_cases.sh

# Fuzz testing
sudo ./test/test_fuzz.sh
```

**Test Infrastructure**:
- `test/ng_mbuf_inject.c` - Kernel module that injects test packets with specific mbuf shapes
- `test/ng_builder_mbuf.c` - Userspace helper to build netgraph test topology
- Tests validate: fragmented mbufs, shared mbufs, VLAN tags, IPv6 extension headers

When DEBUG_STATS=1, tests verify expected code paths:
- `fast_path_count` - Direct pointer access (m_len >= 66 bytes)
- `safe_path_count` - m_copydata() path (fragmented mbufs)
- `pullup_count` - m_pullup() calls for contiguity
- `unshare_count` - m_unshare() calls for shared mbufs

## Architecture

### Core Processing Flow

```
Physical Interface (e.g., em0)
      ↕
[ng_ether node]
      ↕ lower hook
[ng_mss_rewrite node] ← MSS rewriting happens here
      ↕ upper hook
[ng_ether node]
      ↕
Kernel Stack
```

### Dual Processing Paths

The module uses two distinct code paths for performance:

**Fast Path** (`ng_mss_rewrite_process_fast`):
- Triggered when `m->m_len >= 66` bytes (Ether + IP + TCP + options contiguous)
- Direct pointer arithmetic for header access
- No mbuf manipulation unless MSS rewrite is needed
- Most packets take this path

**Safe Path** (`ng_mss_rewrite_process_safe`):
- Triggered when `m->m_len < 66` bytes (fragmented mbuf chain)
- Uses `m_copydata()` to safely read headers across mbuf boundaries
- Calls `m_pullup()` only when actually rewriting MSS
- Handles edge cases: fragmented chains, small packets

### Critical mbuf Safety Rules

1. **Never assume contiguity** - Always check `m->m_len` before pointer access
2. **Use m_copydata() for reading** - Safe across fragmented mbufs
3. **Only pullup/unshare when writing** - Defer modifications until necessary
4. **Check M_WRITABLE()** - Call m_unshare() if mbuf is shared (refcount > 1)
5. **Preserve checksum offload** - Skip rewrite if `CSUM_TCP` or `CSUM_TSO` is set (upper direction)

### Directional Processing

- **Lower hook (interface→kernel)**: Default enabled, processes incoming SYN packets
- **Upper hook (kernel→interface)**: Default disabled, can be enabled at runtime
- Checksum offload awareness: Upper direction skips packets with `CSUM_TCP`/`CSUM_TSO`

### Statistics Architecture

**Two-tier system** for zero-overhead option:

1. **Compile-time** (`ENABLE_STATS`): Can completely remove all statistics code
2. **Runtime** (`stats_mode`): Switch between disabled/per-CPU modes without reloading module

**Per-CPU counters** (when enabled):
- Lock-free using `curcpu` indexing
- Aligned to `CACHE_LINE_SIZE` to prevent false sharing
- Atomic operations for MSS values and mode flags

## IPv6 Limitations

**Current implementation**: Only processes IPv6 packets where TCP immediately follows the base header.

**NOT supported**: IPv6 extension headers (Hop-by-Hop, Routing, Fragment, Destination Options, etc.)

**Rationale**: Most TCP SYN packets don't use extension headers. Adding support requires complex header walking and is deferred.

## Netgraph Control Messages

All runtime configuration uses netgraph messages:

```bash
# Set MSS values
ngctl msg <node>: setmss "{ mss_ipv4=1400 mss_ipv6=1380 }"

# Get current MSS
ngctl msg <node>: getmss

# Get statistics
ngctl msg <node>: getstats

# Reset statistics (uses baseline snapshot, doesn't zero counters)
ngctl msg <node>: resetstats

# Control statistics mode (0=disabled, 1=per-CPU)
ngctl msg <node>: setstatsmode "{ mode=1 }"
ngctl msg <node>: getstatsmode

# Control directional processing
ngctl msg <node>: setdirection "{ enable_lower=1 enable_upper=0 }"
ngctl msg <node>: getdirection
```

## Key Implementation Details

### TCP MSS Option Search

Fast-path optimization checks common positions first:
- Offset 0: MSS at start
- Offset 1: NOP + MSS
- Offset 2: NOP + NOP + MSS

Falls back to generic option walking only if not found in common positions.

### Checksum Incremental Update

Uses RFC 1624 algorithm for TCP checksum adjustment:
```c
tcp_checksum_adjust(old_sum, old_value, new_value)
```

Avoids full checksum recalculation - only adjusts for the changed MSS bytes.

### Statistics Reset Implementation

**Baseline method** instead of zeroing counters:
- Snapshots current totals on `resetstats`
- `getstats` subtracts baseline from current totals
- Allows runtime mode switching without data loss

## Common Development Tasks

### Adding a new debug counter

1. Add field to `ng_mss_stats_percpu` (within `#if ENABLE_DEBUG_STATS`)
2. Add matching field to `ng_mss_rewrite_stats` message structure
3. Add parse type field to `ng_mss_rewrite_stats_fields`
4. Add baseline field to `ng_mss_rewrite_private`
5. Update `getstats` aggregation loop
6. Update `resetstats` baseline capture
7. Increment counter at appropriate code location

### Modifying packet processing logic

1. Read current implementation in `ng_mss_rewrite_process_fast/safe`
2. Understand mbuf safety rules (see above)
3. Add debug counters to track new code paths
4. Update tests to verify new behavior
5. Test with `DEBUG_STATS=1` to confirm expected paths

### Debugging test failures

1. Build with `make DEBUG_STATS=1`
2. Run test with `sudo ./test/test_mbuf_shape.sh`
3. Check debug counter output shows expected code path
4. If wrong path: packet shape issue (check `test/ng_mbuf_inject.c`)
5. If right path but wrong result: logic error in processing function

## FreeBSD Kernel Development Notes

- This is a **kernel module** - bugs can panic the system
- Always test in a VM or non-production environment
- Use `kldstat` to verify module is loaded
- Use `ngctl list` to verify netgraph topology
- Check `dmesg` for kernel error messages
- Unload with `kldunload ng_mss_rewrite` before rebuilding
