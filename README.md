# Infinispan Hot Rod Client - C++17

[![Build and Test](https://github.com/infinispan/cpp17-client/actions/workflows/build.yml/badge.svg)](https://github.com/infinispan/cpp17-client/actions/workflows/build.yml)
[![Tests](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/infinispan/cpp17-client/badges/tests.json&label=tests)](https://github.com/infinispan/cpp17-client/actions/workflows/build.yml)

A cross-platform C++17 implementation of the Infinispan Hot Rod protocol client.

🎉 **Smart client** - hash-aware routing, automatic failover, full CRUD +
metadata ops, typed error handling and user-decided retry (keyed & keyless),
and SASL/SCRAM authentication (SCRAM-SHA-1/256/512).

## Quick Start

See the complete working example in [`examples/quickstart/`](examples/quickstart/):

```bash
cd examples/quickstart
./start-server.sh        # Start Infinispan 16.2
mkdir build && cd build
cmake .. && make
./quickstart             # Run the example
```

Example output:
```
=== Hot Rod C++ Client Quickstart ===
Connecting to Infinispan server...
Connected successfully!

--- PUT Operations ---
PUT: greeting = Hello, Infinispan!
PUT: language = C++17
PUT: version = 1.0.0

--- GET Operations ---
GET: greeting = Hello, Infinispan!
GET: language = C++17
GET: version = 1.0.0

--- REMOVE Operations ---
REMOVE: language deleted successfully

=== Quickstart Complete ===
```

The quickstart demonstrates:
- Connecting to Infinispan
- PUT operations (storing key-value pairs)
- GET operations (reading values)
- REMOVE operations (deleting entries)
- Clean disconnect

A second example, `retry.cpp`, demonstrates the user-decided retry loop
(`cache.excluding(e)` on a caught `HotRodClientException`) for both keyed
operations and keyless `ping`.

See [`examples/quickstart/README.md`](examples/quickstart/README.md) for full documentation.

## Building

### Requirements

**Build Requirements**:
- **C++17** compatible compiler (GCC 7+, Clang 6+, MSVC 19.20+)
- **CMake** 3.15+
- **OpenSSL** 1.1.1+ or 3.x
- **Google Test** (for testing)

**Runtime Requirements**:
- Infinispan Server 16.0+ (tested with 16.2)
- Docker (for integration tests and quickstart example)

### Linux (Fedora/RHEL)

```bash
# Install dependencies
sudo dnf install cmake gcc-c++ openssl-devel gtest-devel

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run tests
ctest --output-on-failure
```

### Linux (Ubuntu/Debian)

```bash
# Install dependencies
sudo apt install cmake g++ libssl-dev libgtest-dev

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run tests
ctest --output-on-failure
```

### Windows (PowerShell)

```powershell
# Install dependencies via vcpkg
vcpkg install openssl gtest

# Build
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release

# Run tests
ctest -C Release --output-on-failure
```

## Testing

### Unit Tests
```bash
cd build
./unit_tests

# Or via CTest
ctest -R UnitTests --output-on-failure
```

### Integration Tests
Requires Docker to run Infinispan server:
```bash
cd build

# Run all integration tests (starts server automatically) — representative subset:
./ping_integration_tests
./get_integration_tests
./put_integration_tests
./remove_integration_tests
./topology_integration_tests            # Multi-node cluster tests
./hash_aware_routing_integration_tests  # Smart routing tests
./retryview_integration_tests           # User-decided retry (keyed)
./pingretry_integration_tests           # User-decided retry (keyless ping)

# Or run all via CTest
ctest --output-on-failure
```

**Note**: Integration tests use Docker Compose to manage multi-node Infinispan clusters (up to 4 nodes). Tests verify failover, rebalancing, and hash-aware routing.

## Status

> 📌 **Live status lives in [docs/STATUS.md](docs/STATUS.md)** — start there after any break.
> Design rationale is in [docs/DECISIONS.md](docs/DECISIONS.md).

**Current Progress**: Full CRUD (PING/GET/PUT/REMOVE) + SCRAM auth + topology
awareness + hash-aware routing + async multiplexing + metadata/version-based
operations (Step 10) + typed error handling and user-decided retry, keyed and
keyless (Step 11) — all shipped. **Next:** bulk operations (Step 12) or
multiplexing benchmarks. See [docs/STATUS.md](docs/STATUS.md) for live status.
Per-step milestone history (frozen at Step 9) is archived at
[docs/archive/PROGRESS.md](docs/archive/PROGRESS.md).

_(Step numbers follow [`../hotrod-foundry/ROADMAP.md`](../hotrod-foundry/ROADMAP.md);
[`docs/STATUS.md`](docs/STATUS.md) is authoritative for which steps are done — not
any headline count.)_

**Test Results** — see the **Tests** badge at the top of this README (CI-published
from the actual test run). Integration tests run against live Infinispan via Docker.

## Features

### Implemented ✅
- ✅ **Cross-platform support** (Linux + Windows)
- ✅ **Wire format primitives** (vInt, vLong, strings, byte arrays)
- ✅ **Protocol 4.0 headers** (complete spec with all conditional fields)
- ✅ **Client Intelligence 0x03** (HASH_DISTRIBUTION_AWARE)
- ✅ **Hash-aware routing** (keys route directly to primary owner)
- ✅ **Automatic failover** (primary → backup owners → any server)
- ✅ **Topology awareness** (cluster rebalancing, updates)
- ✅ **Consistent hashing** (MurmurHash3 x64_32, segment-based routing)
- ✅ **Connection pooling** (one connection per server)
- ✅ **Multiplexed connections** (concurrent async operations on single connection per server)
- ✅ **Async operations** (all operations return `std::future` for non-blocking execution)
- ✅ **PING operation** (server connectivity check)
- ✅ **GET operation** (read from cache with failover)
- ✅ **PUT operation** (write to cache with lifespan/maxIdle)
- ✅ **REMOVE operation** (delete from cache)
- ✅ **Metadata / version-based operations** (`getWithMetadata`, `putIfAbsent`,
  `replace`, `containsKey`, `removeWithVersion`, `replaceWithVersion`) (Step 10)
- ✅ **Typed error handling** (`HotRodClientException` with phase / server status /
  tried nodes) + `isTransient` / `outcomeUncertain` classification (Step 11a)
- ✅ **User-decided retry** (`cache.excluding(e)` bound view) for keyed ops and
  keyless `ping`, with automatic before-send failover (Step 11b/11c)
- ✅ **SASL/SCRAM authentication** (`setAuthentication(...)`) — end-to-end for the
  SCRAM family (`SCRAM-SHA-1`/`SCRAM-SHA-256`/`SCRAM-SHA-512`), applied to every
  connection; see [`documentation/topics/security.adoc`](documentation/topics/security.adoc)
- ✅ **Integration test framework** (GoogleTest + Docker + multi-node clusters)

### Full CRUD with Smart Routing and Async Operations
```cpp
// Connect with hash-aware routing
RemoteCache cache("localhost", 11222, "my-cache");
cache.setClientIntelligence(ClientIntelligence::HASH_DISTRIBUTION_AWARE);
cache.connect();

// All operations are async (return std::future) and can run concurrently
// Multiple operations share a single multiplexed connection per server

// CREATE/UPDATE - routes to primary owner
auto putFuture = cache.put(key, value, lifespan, maxIdle);

// READ - automatic failover to backup if primary fails
auto getFuture = cache.get(key);

// DELETE - with automatic failover
auto removeFuture = cache.remove(key);

// PING
auto pingFuture = cache.ping();

// Wait for results when needed
auto result = getFuture.get();  // blocks until complete
if (result.has_value()) {
    ByteArray value = result.value();
}

// Or fire-and-forget for max throughput
for (int i = 0; i < 1000; i++) {
    cache.put(keys[i], values[i]);  // returns immediately, executes concurrently
}
```

### Future

> 👉 **The full, authoritative backlog lives in
> [docs/STATUS.md → 📋 Backlog](docs/STATUS.md#-backlog-the-whole-list)**, and
> the 1–3 immediate items in
> [⏭ Next steps](docs/STATUS.md#-next-steps-start-here). This is just a summary.

- ✅ **Step 10 — Metadata / version-based operations** (shipped): `getWithMetadata`,
  `replaceWithVersion`, `removeWithVersion`, `putIfAbsent`, `replace`, `containsKey`
- ✅ **Step 11 — Error handling** (shipped): ERROR response parsing, typed exception,
  user-decided retry (keyed + keyless)
- **Step 12 — Bulk operations** (next): `GET_ALL`, `PUT_ALL`, `BULK_GET`
- Also: multiplexing benchmarks, TLS/SSL support

## Project Structure

```
cpp17-client/
├── CMakeLists.txt               # Build configuration
├── README.md                    # This file
├── LICENSE                      # Apache 2.0
├── include/hotrod/              # Public API headers
│   ├── Types.h                  # Type definitions
│   ├── Codec.h                  # Wire format primitives
│   ├── HeaderCodec.h            # Protocol 4.0 headers
│   ├── Connection.h             # TCP connection
│   ├── RemoteCache.h            # High-level API (with hash-aware routing)
│   ├── TopologyInfo.h           # Cluster tracking
│   ├── ConsistentHash.h         # Smart routing
│   └── MurmurHash3.h            # Hash function
├── src/                         # Implementation
│   ├── codec/                   # Encoding/decoding
│   ├── transport/               # Network I/O
│   ├── topology/                # Cluster tracking
│   ├── hash/                    # MurmurHash3 + consistent hashing
│   └── operations/              # Hot Rod operations (with failover)
├── tests/                       # Test suite
│   ├── unit/                    # unit tests (gtest)
│   └── integration/             # integration tests (multi-node clusters, Docker)
├── documentation/               # AsciiDoc user guide (index.adoc + topics/)
├── examples/                    # Usage examples
│   └── quickstart/              # Simple GET/PUT/REMOVE + retry + auth examples
├── scripts/                     # Test infrastructure
│   ├── start_cluster.sh         # Start multi-node cluster
│   ├── add_cluster_node.sh      # Add node dynamically
│   └── remove_cluster_node.sh   # Remove node for failover tests
└── test-configs/                # Server configurations
    ├── docker-compose-cluster.yml  # Multi-node setup
    └── infinispan-cluster.xml      # No-auth config
```

## Development

> 🔁 **Working on this project?** Read [docs/WORKFLOW.md](docs/WORKFLOW.md) — the
> per-session workflow (how to re-orient after a break, what to update before you
> stop, and which protocol sources are authoritative). It's built for
> intermittent, solo work.

This implementation follows the **hotrod-foundry** incremental approach:

1. **Test-driven development** - Test vectors → Unit tests → Implementation
2. **Java reference** - Study Java implementation for each step (MANDATORY) — it is the authoritative source for **behavior and semantics** (what each op does, statuses, edge cases)
3. **Byte-level validation** - Compare with test vectors and the Kaitai wire-format schema (authoritative for **byte layout**); if the two disagree, stop and investigate
4. **Cross-platform** - Linux + Windows support required
5. **Infrastructure-first** - Steps 0-5 (foundation), then operations

### Roadmap

See [hotrod-foundry/ROADMAP.md](../hotrod-foundry/ROADMAP.md) for the complete step-by-step plan.

### Java Reference

The **authoritative source for protocol behavior and semantics** — study it
first for each step (see workflow above).

- Local: `/home/rigazilla/git/infinispan/client/hotrod-client/`
- Public: https://github.com/infinispan/infinispan/tree/main/client/hotrod-client

## References

- **Infinispan Project**: https://infinispan.org/
- **Hot Rod Foundry**: ../hotrod-foundry/
- **Java Client Source** (authoritative for behavior/semantics): local `/home/rigazilla/git/infinispan/client/hotrod-client/` · public https://github.com/infinispan/infinispan/tree/main/client/hotrod-client
- **Protocol Specification**: https://infinispan.org/docs/stable/titles/hotrod_protocol/
- **Wire-format schema (Kaitai, protocol 4.0/4.1)**: https://github.com/rigazilla/hotrod-dissector/tree/main/schemas — an independent, machine-readable encoding of the protocol; use it to double-check byte layouts against the Java client.

## License

Apache 2.0 (matching Infinispan project)

## Contributing

**Start with [docs/WORKFLOW.md](docs/WORKFLOW.md)** — the per-session workflow.
In short, following the hotrod-foundry porting guidelines:
- One step at a time (complete step N before step N+1)
- Update [docs/STATUS.md](docs/STATUS.md) before you stop each session (next steps + date)
- Log design decisions in [docs/DECISIONS.md](docs/DECISIONS.md) as you make them
- Validate against test vectors
- Ensure cross-platform compatibility

## Implementation Milestones

- ✅ **v0.1.0** (2026-06-11): Foundation complete (Steps 0-5)
- ✅ **v0.2.0** (2026-06-12): PING operation complete (Step 6)
- ✅ **v0.3.0** (2026-06-12): GET operation complete (Step 7)
- ✅ **v0.4.0** (2026-06-12): PUT operation complete (Step 8)
- ✅ **v0.5.0** (2026-06-12): REMOVE operation complete (Step 9) - **Full CRUD!**
- ✅ **v0.6.0** (2026-06-17): Topology awareness (Step 4)
- ✅ **v0.7.0** (2026-06-17): Hash-aware routing with failover - **Smart Client!**
- ✅ **v0.8.0** (2026-09-20): Metadata / version-based operations (Step 10)
- ✅ **v0.9.0** (2026-09-25): Typed error handling + user-decided retry, keyed &
  keyless (Step 11)
- 🎯 **v0.10.0** (upcoming): Bulk operations (Step 12)

---

**Maintained by**: rigazilla  
**Status**: Active development  
**Current Step**: Error handling + user-decided retry complete (Step 11, keyed &
keyless). Next: bulk operations (Step 12). ✅
