# Project Status — Hot Rod C++17 Client

> **This is the single source of truth for "where the project is."**
> If any other doc disagrees with this file, this file wins. Point-in-time
> snapshots live in [`archive/`](archive/) and are historical only.
>
> **Last updated:** 2026-10-09

---

## ▶ Coming back after a break? Do this first

1. `git config core.hooksPath .githooks` (once per clone/machine — turns on the
   STATUS.md pre-commit guard; harmless to re-run)
2. `git pull` (if working across machines)
3. `git log --oneline -5` — see what the last session actually did
4. Build: `cmake --build build`  (configure first if needed: `cmake -S . -B build`)
5. Test: `ctest --test-dir build --output-on-failure`
6. Read **⏭ Next steps** below, pick one, go.

Then **before you stop**: update *⏭ Next steps* and the *Last updated* date
while the context is still fresh in your head. This 60-second habit is what
keeps this file trustworthy across long gaps. Keep this file to **current state
only** — the *why/how/when* of a change goes in
[`DECISIONS.md`](DECISIONS.md) (append-only), not here. A `pre-commit` hook
blocks source changes that don't also update this file.

_(Full per-session workflow: [`WORKFLOW.md`](WORKFLOW.md).)_

---

## ⏭ Next steps (start here)

_The 1–3 concrete things to do next. Keep this short and current._

0. **Hot Rod URI support COMPLETE (issue #9).** `RemoteCache::fromUri(uri,
   cacheName="")` builds a configured (not-yet-connected) client from a
   `hotrod://[user:password@]host1[:port1][,host2...][?k=v&...]` connection
   string; returns a `std::unique_ptr<RemoteCache>` (RemoteCache owns a mutex, so
   it is non-movable). Parsing lives in a pure, header-only `HotRodURI::parse()`
   (`include/hotrod/HotRodURI.h`, 27 unit tests). v1 scope: `hotrod://` only;
   first host is the seed (topology discovery finds the rest); credentials enable
   SCRAM; userinfo and query values are percent-decoded (RFC 3986, java.net.URI
   parity — so an `@`/`/`/`:` in a password written `%40`/`%2F`/`%3A` reaches the
   client as its real bytes); query whitelist `sasl_mechanism` /
   `client_intelligence` /
   `protocol_version` (Java `"4.0"`/`"4.1"` form, case-insensitive intelligence).
   Anything unsupported throws a clear `HotRodClientException` (BeforeSend):
   `hotrods://`/TLS, unknown params (`connect_timeout`, `socket_timeout`,
   `use_ssl`, `token`, …), `protocol_version=AUTO`/3.x. Deferred features tracked
   in **issue #10**. Rationale in [`DECISIONS.md`](DECISIONS.md) (2026-10-09).
   **Next: pick a roadmap step — Step 12 (bulk ops) or benchmark the multiplexing
   path.**
1. **SASL/SCRAM authentication COMPLETE — end-to-end for the whole SCRAM
   family.** `RemoteCache::setAuthentication(username, password, realm="default",
   serverName="infinispan", mechanism="SCRAM-SHA-256")` (flat setter, matching
   `setClientIntelligence`) enables a SASL handshake on **every** connection the
   client opens — the seed connection and each topology-discovered member — run
   inside `MultiplexedConnection::connect()` before the read loop starts. The
   `SaslAuthenticator` drives the two-turn SCRAM exchange over a testable
   `SaslTransport` seam (`ConnectionSaslTransport` in production); `SCRAM` is
   generalized across `SCRAM-SHA-1/256/512` (OpenSSL EVP, digest-derived
   lengths). It completes on the client side after verifying the server signature
   (no confirmation round; ignores the server `completed` flag — mirrors Java
   `AuthHandler`). Any non-SCRAM mechanism throws a clear `HotRodClientException`
   at connect(). New public headers: `Authentication.h`, `AuthCodec.h`,
   `SaslAuthenticator.h`. Documented in `documentation/topics/security.adoc`.
   Rationale in [`DECISIONS.md`](DECISIONS.md) (2026-09-25 SASL/SCRAM entry).
   **Next: pick a new roadmap step — Step 12 (bulk ops) or benchmark the
   multiplexing path.**
2. **Step 11c COMPLETE — keyless-op retry (ping) shipped (slices 1–3).** Keyless
   operations now mirror the keyed retry strategy: automatic before-send failover
   across all servers, user-decided after-send retry (D2). `ping()` routes through
   `selectAnyServer(ctx, triedOut)`, which orders candidates via
   `orderKeyCandidates({}, allServers, exclude)` (empty owners → all servers in
   topology order), sweeps to the next server when one is unreachable, and — when
   no topology is known yet (ping is often the very first op) — falls back to the
   seed connection. `RetryView::ping()` forwards the exclusion set, so
   `cache.excluding(e).ping()` works exactly like keyed ops; `ownersExhausted` is
   always `false` for keyless ops. (1) `pingImpl` + `selectAnyServer` in
   `RemoteCache.cpp`, 3 keyless-ordering unit tests; (2) `PingRetryIntegrationTest`
   (3 tests) — `excluding()` routes the keyless ping to another server, excluding
   all servers throws BeforeSend with `ownersExhausted=false`, and the
   catch→`excluding(e).ping()` loop recovers after the first-candidate node is
   killed; (3) `examples/quickstart/retry.cpp` gained a keyless `pingWithRetry`
   loop + README/ERROR_HANDLING docs. The implementation is generic, so future
   non-key ops (server stats/admin) reuse `selectAnyServer`. Rationale in
   [`DECISIONS.md`](DECISIONS.md) (2026-09-25 Step 11c entries). **Next: pick a new
   roadmap step — Step 12 (bulk ops) or benchmark the multiplexing path.**
2. **Step 11b COMPLETE — user-decided retry shipped (slices 1–6).** (1) `selectServerForKey` routes
   over the pure, unit-tested `orderKeyCandidates()` helper (owners → non-owner
   fallback, minus an exclusion set), takes a defaulted `RetryContext`, reports
   the nodes it tried via an out-param, and computes `ownersExhausted` honestly.
   (2) Client-level `proxyToNonOwner` flag (default `true`, D5:
   `setProxyToNonOwner`/`getProxyToNonOwner`) gates the owner→non-owner fallback;
   when `false`, exhausting owners throws a transient exception with
   `ownersExhausted=true` instead of proxying. (3) Op-level retry threading via a
   **bound-view** API — base op signatures stay retry-free; `cache.excluding(nodes)`
   / `cache.excluding(exception)` returns a `RetryView` that forwards to private
   `…Impl(args, const RetryContext&)` methods, and every op (including the plain
   first call) unions the nodes it tried into the thrown exception's `triedNodes`,
   so a caught exception feeds straight back as the next attempt's exclusion set.
   New public headers: `RetryContext.h`, `ServerSelection.h`, `RetryView.h`.
   (4) Thread-safety: a single `stateMutex_` guards `topology_` +
   `consistentHash_` + `connectionPool_` across the read-loop's topology callback
   (`handleTopologyUpdate`) and user/retry threads (`selectServerForKey` →
   `getConnectionForServer`); the connection pool holds `shared_ptr` and each op
   keeps its connection alive for the operation's duration, so a concurrent
   topology update can't free a connection mid-`execute()`.
   (5) End-to-end integration coverage: `RetryViewIntegrationTest` (3 tests)
   exercises `excluding()` dispatch — routes around an excluded owner, throws
   `ownersExhausted` when proxy is disabled and all owners are excluded, and runs
   the documented catch→`excluding(e)` retry loop after killing the primary owner.
   (6) Retry-loop example `examples/quickstart/retry.cpp` (+ README section) —
   `getWithRetry`/`putWithRetry` loops using `isTransient`/`outcomeUncertain`/
   `excluding(e)`, and the clarification that `proxyToNonOwner` does not let a
   caller skip the catch block. Retry policy/idempotency stays the user's call.
   Full design + progress checklist:
   [`ERROR_HANDLING_DESIGN.md`](ERROR_HANDLING_DESIGN.md) §5; rationale in
   [`DECISIONS.md`](DECISIONS.md) (2026-09-21, 2026-09-25 entries).
3. **Benchmark the multiplexing path** — the async rewrite targets 5–10×
   concurrent throughput; this has not been measured yet.
4. **Small cleanup:** read header "other params" when `paramCount > 0`
   (two `TODO`s in `src/operations/RemoteCache.cpp:140,182`).

---

## 📋 Backlog (the whole list)

_Everything known-to-do, in one place. `⏭ Next steps` above is just the 1–3 you
pull from here next. Step numbers follow
[`../hotrod-foundry/ROADMAP.md`](../../hotrod-foundry/ROADMAP.md); this file
(STATUS.md) is authoritative for what's done._

**Remaining roadmap steps:**
- **Step 10 — Metadata operations** (version-based CAS). Deliverables:
  - [x] `getWithMetadata` (0x1B/0x1C) — **shipped 2026-09-17** (8 unit + 7 integration tests)
  - [x] `removeWithVersion` (0x0D/0x0E) — **shipped 2026-09-19** (7 unit + 5 integration tests). Returns `future<bool>` (matches Java `boolean removeWithVersion(K, long)`); added `Codec::writeLong`/`readLong` (fixed 8-byte BE). Parser drains the `*_WITH_PREVIOUS` (0x03/0x04) body defensively though FORCE_RETURN_VALUE isn't wired yet. Byte layout cross-checked against all three legs: this client, Java `RemoveIfUnmodifiedOperation`, and `hotrod40.ksy` (`remove_if_unmodified_request` = `key: lp_bytes` + `entry_version: s8`, big-endian per `meta.endian: be`).
  - [x] `replaceWithVersion`/REPLACE_IF_UNMODIFIED (0x09/0x0A) — **shipped 2026-09-20** (5 unit + 5 integration tests). Returns `future<bool>`; request reuses PUT's expiration byte (high nibble=lifespan, low=maxIdle) + `Codec::writeLong` for the version. **Schema bug found:** `hotrod40.ksy` `replace_if_unmodified_request` has the expiration nibbles flipped vs `put_request`/`expiration_params`; Java uses one shared `writeExpirationParams` for both, so PUT's order is authoritative. Cross-checked against client + Java `ReplaceIfUnmodifiedOperation` + (corrected reading of) the schema.
  - [x] `putIfAbsent` (0x05/0x06), `replace` (0x07/0x08), `containsKey` (0x0F/0x10) — **shipped 2026-09-20** (11 unit + 12 integration tests). `putIfAbsent`/`replace` reuse PUT's request layout (`key` + expiration byte + `value`) and return `future<optional<EntryWithMetadata>>` — the previous/replaced entry when `previousValue` (FORCE_RETURN_VALUE) is set, `nullopt` otherwise; parsers drain the `*_WITH_PREVIOUS` (0x03/0x04) body defensively. `containsKey` is a key-only request returning `future<bool>` from the status (no response body). Cross-checked against Java `PutIfAbsentOperation`/`ReplaceOperation`/`ContainsKeyOperation` and `hotrod40.ksy` (put_if_absent/replace → `put_request`/`put_response`; contains_key → `key_request`, status-only response).

  Java ref: `GetWithMetadataOperation`, `ReplaceIfUnmodifiedOperation`. See the
  [step-by-step plan](#plan-metadata-operations-step-10) below.
- [x] **Step 11 — Error handling — COMPLETE (11a + 11b + 11c).** Design agreed
  2026-09-21 → [`ERROR_HANDLING_DESIGN.md`](ERROR_HANDLING_DESIGN.md). Split:
  - [x] **11a — Error surfacing** — **shipped 2026-09-25** (11 unit + 3
    integration tests). ERROR response parsing (opcode 0x50) with
    length-prefixed message drained in the read loop (keeps the stream in sync
    even for orphan responses), surfaced as a typed `HotRodClientException`
    (pure-data: `FailurePhase` {BeforeSend/AfterSend/ServerError} +
    `serverStatus` + `triedNodes` + `ownersExhausted`) replacing bare
    `std::runtime_error`, still catchable as `std::runtime_error`. Two-axis
    classification helpers: `isTransient()` (futility — library's call) and
    `outcomeUncertain()` (ambiguity — 0x86/AfterSend). `COMMAND_TIMEOUT` (0x86)
    is transient **and** outcome-uncertain (intentionally diverges from Java).
  - [x] **11b — User-decided retry** — **shipped 2026-09-25**: exclusion-aware
    `selectServerForKey`, `proxyToNonOwner` default true, a bound-view retry API
    (fork 1.b), thread-safe pool/topology. Retry policy/idempotency is the
    user's call, not automatic. **Slices 1–3:** exclusion-aware selection
    via the pure `orderKeyCandidates()` helper (`ServerSelection.h`) +
    `RetryContext` (`RetryContext.h`); client-level `proxyToNonOwner` flag
    (default `true`); op-level retry threading via `RetryView`/`cache.excluding()`
    (`RetryView.h`) with `triedNodes` unioned into every thrown exception (pure
    `unionNodes()` helper). Base op signatures stay retry-free. **Slice 4
    (thread-safety) done:** `stateMutex_` guards topology/hash/pool across the
    read-loop and user/retry threads; pool is `shared_ptr` and each op keeps its
    connection alive for its duration (no free mid-`execute()`). **Slice 5
    (integration test):** `RetryViewIntegrationTest` (3 tests) covers `excluding()`
    route-around, proxy-disabled owners-exhausted throw, and the catch→retry loop
    after killing the primary owner. **Slice 6 (example):**
    `examples/quickstart/retry.cpp` shows the retry loop and the `proxyToNonOwner`
    clarification.
  - [x] **11c — Keyless-op retry (ping)** — **shipped 2026-09-25** (3 unit + 3
    integration tests). Keyless ops mirror the keyed two-tier model: automatic
    before-send failover across all servers via `selectAnyServer` (deterministic
    topology order, minus an exclusion set; seed-connection fallback when no
    topology is known yet), user-decided after-send retry via
    `cache.excluding(e).ping()`. `ownersExhausted` is always `false` for keyless
    ops; `selectAnyServer` is generic for future non-key ops.
    `PingRetryIntegrationTest` (3 tests) + a keyless `pingWithRetry` in the retry
    example.
  Java ref: `org.infinispan.client.hotrod.exceptions.*`.
- [x] **SASL/SCRAM authentication — COMPLETE.** End-to-end auth for the whole
  SCRAM family (`SCRAM-SHA-1/256/512`). `setAuthentication(...)` (flat setter)
  runs the AUTH_MECH_LIST + two-turn SCRAM handshake on every connection (seed +
  topology-discovered), inside `MultiplexedConnection::connect()` before the read
  loop; `SaslAuthenticator` over a testable `SaslTransport` seam; `SCRAM`
  generalized across the digest family (OpenSSL EVP). Completes on the client
  side after verifying the server signature (no confirmation round). Non-SCRAM
  mechanisms throw at connect(). New headers `Authentication.h`, `AuthCodec.h`,
  `SaslAuthenticator.h`; docs `documentation/topics/security.adoc`; 19 unit +
  5 integration tests. This supersedes the earlier "SCRAM crypto exists but is
  dead code" state. Java ref: `AuthHandler` / `AuthOperation`; Kaitai `hotrod40.ksy`.
- [ ] **Step 12 — Bulk operations.** `GET_ALL` (0x2F), `PUT_ALL` (0x2D),
  `BULK_GET` (0x1F, iterator-style).

**Other known work (not roadmap steps):**
- [ ] **Benchmark the multiplexing path** — the async rewrite targets 5–10×
  concurrent throughput; unmeasured so far.
- [ ] **Code cleanup:** read header "other params" when `paramCount > 0`
  (`src/operations/RemoteCache.cpp:140,182`).
- [x] **Hot Rod URI support (`fromUri`)** — **shipped 2026-10-09** (issue #9, 27
  unit tests). See *Working and shipped*.
- [ ] **Hot Rod URI — deferred features (issue #10):** `hotrods://`/TLS +
  cert/SNI/hostname-validation params, `connect_timeout`/`socket_timeout`,
  multi-server seed list (startup failover across seeds), non-SCRAM SASL
  (PLAIN/OAUTHBEARER `token`), `protocol_version=AUTO`/3.x. Each drops its
  "unsupported" throw from `HotRodURI::parse` once the capability lands.
- [ ] **TLS/SSL support** (transport encryption). Also unblocks `hotrods://` above.

**Longer horizon (Step 14+ advanced features):** transactions (XA), client
listeners/events, counters, Ickle queries, streaming ops (4.1+), multimap,
near caching. Not scheduled.

_Done and not repeated here: foundation/primitives, Protocol 4.0 headers,
SCRAM auth (Step 3), topology (Step 4), consistent hashing + hash-aware
routing, connection pooling (Step 13), full CRUD (Steps 6–9), async
multiplexing. See "Working and shipped" below._

---

## Current state (verified against code, 2026-09-16)

**Working and shipped:**
- **Hot Rod URI (`RemoteCache::fromUri`)** — build a configured client from a
  `hotrod://[user:pass@]host[:port][,host...][?k=v]` connection string (issue #9);
  pure header-only parser `HotRodURI::parse` (`include/hotrod/HotRodURI.h`).
  `hotrod://` scheme only, first host seeds the topology, SCRAM via credentials,
  `sasl_mechanism`/`client_intelligence`/`protocol_version` params; userinfo and
  query values are percent-decoded (java.net.URI parity); unsupported
  schemes/params throw. TLS/timeouts/multi-seed/non-SCRAM deferred (issue #10)
- Wire primitives (vInt, vLong, strings, byte arrays)
- Protocol 4.0 headers (all conditional fields)
- **SASL/SCRAM authentication (end-to-end)** — `setAuthentication(...)` runs the
  AUTH_MECH_LIST + two-turn SCRAM handshake on every connection (seed +
  topology-discovered), for the whole family (`SCRAM-SHA-1/256/512`, RFC 5802,
  OpenSSL); non-SCRAM mechanisms throw at connect(). See
  `documentation/topics/security.adoc`
- Cluster topology awareness (failover, load balancing)
- Consistent hashing (MurmurHash3 x64_32, seed 9001)
- Hash-aware routing → primary owner, with automatic failover
- Connection pooling (one connection per server)
- **Full CRUD:** PING / GET / PUT / REMOVE
- **Keyless-op retry (Step 11c):** `ping()` routes through `selectAnyServer` —
  automatic before-send failover across all servers (topology order, minus an
  exclusion set), seed-connection fallback when no topology is known yet, and
  user-decided after-send retry via `cache.excluding(e).ping()`. Same strategy
  intended for all future non-key ops; `ownersExhausted` always `false`
- **getWithMetadata** (0x1B/0x1C) — value + entry version/expiration metadata,
  the entry point for version-based CAS (Step 10)
- **removeWithVersion** (0x0D/0x0E) — version-based conditional remove (CAS);
  returns `future<bool>` (removed?), version from `getWithMetadata`
- **replaceWithVersion** (0x09/0x0A) — version-based conditional replace (CAS);
  returns `future<bool>` (replaced?); request = key + expiration + version + value
- **putIfAbsent** (0x05/0x06) — store only if key absent; returns
  `future<optional<EntryWithMetadata>>` (existing entry when `previousValue` set)
- **replace** (0x07/0x08) — store only if key present; returns
  `future<optional<EntryWithMetadata>>` (replaced entry when `previousValue` set)
- **containsKey** (0x0F/0x10) — key-existence test; returns `future<bool>`
- **Typed error surfacing (Step 11a):** server ERROR responses (opcode 0x50)
  are parsed (status + length-prefixed message) and thrown as
  `HotRodClientException` — a pure-data exception carrying `FailurePhase`,
  `serverStatus`, `triedNodes`, `ownersExhausted`; still catchable as
  `std::runtime_error`. Free helpers `isTransient()` / `outcomeUncertain()`
  classify futility vs ambiguity. The ERROR body is drained in the read loop so
  the connection stays usable after a server error (no stream desync)
- **Async API:** all operations return `std::future` / `std::optional`
- **MultiplexedConnection:** true async — dedicated read-loop thread,
  `messageId → promise` pending map, `execute()` used by all four operations.
  _(The July "70% / temporary blocking" docs are obsolete — see archive note.)_
- Multi-server topology test fixtures + Docker-based integration tests
- **CI runs integration tests on Linux** (Ubuntu job) as of 2026-09-18 — the
  `ubuntu-latest` runner's host Docker daemon starts the Infinispan containers.
  Windows and Fedora stay unit-only (Fedora runs inside a container with no
  Docker daemon; Windows doesn't build the integration tests). Windows/MSVC
  portability and `-Werror` build parity also landed (Sept 2026).

**Test status (verified 2026-10-09):**
- Unit: **250/250** passing (`./build/unit_tests`, <1s) — +27 for the Hot Rod URI
  parser (`HotRodURITest`: grammar, multi-host, userinfo, the typed query
  whitelist, port trailing-garbage rejection, RFC-3986 percent-decoding of
  userinfo/query values + malformed-escape rejection, and explicit rejection of
  `hotrods://`/unknown-param/`AUTO`-3.x). The
  +19 for the auth work
  (`AuthCodecTest` body round-trips + `SaslAuthenticatorTest`, a parameterized
  SCRAM-SHA-1/256/512 handshake driven by a fake OpenSSL server, plus
  mech-not-offered / unsupported-mech / bad-server-signature failure paths); the
  earlier +17 for `ServerSelectionTest` (`orderKeyCandidates` + `unionNodes`,
  Step 11b slices 1–3; +3 keyless-ordering tests for `selectAnyServer`, Step 11c
  slice 1) still stands
- Integration: **92/92** passing across 19 suites (`ctest`, spins up Docker
  Infinispan single-server + multi-node clusters), now also green on Linux CI.
  +2 for `FromUriIntegrationTest` (issue #9: a client built via `fromUri` round-trips
  a value; a `?protocol_version=4.0` query param flows through to a working client)
  and +1 for `AuthIntegrationTest.FromUriWithCredentialsAuthenticates` (credentials
  carried in the URI authenticate end-to-end) — verified live this session. These
  prove the `fromUri` parse→setter→connect→op chain; exhaustive parsing coverage
  stays in the pure unit tests. (Hash-aware `client_intelligence` via URI is NOT
  integration-tested on the single-server fixture — the Dockerized single node
  advertises internal container addresses, so routing tests use the multi-server
  suites; see DECISIONS 2026-10-09.)
  +3 for `RetryViewIntegrationTest` (Step 11b: `excluding()` routes around an
  owner; proxy-disabled owners-exhausted throw; the catch→`excluding(e)` retry
  loop recovers after the primary owner is killed) — verified 3/3 this session.
  +3 for `PingRetryIntegrationTest` (Step 11c: keyless `excluding().ping()` routes
  to another server; excluding all servers throws BeforeSend with
  `ownersExhausted=false`; the catch→`excluding(e).ping()` loop recovers after the
  first-candidate node is killed) — verified 3/3 this session.
  +5 for `AuthIntegrationTest` (SASL/SCRAM against an auth-enabled Infinispan 16
  server started by `scripts/start_infinispan_auth.sh`): SCRAM-SHA-1/256/512
  put/get round-trips (3, parameterized), wrong-password throws at connect(), and
  no-credentials-against-a-secured-server fails — verified 5/5 live this session.
  The interlaced distributed tests (`ConcurrentMultiServerTest`) were fixed to
  tolerate a GET racing ahead of its PUT — a `nullopt` is expected, only a
  present-but-wrong value is an error.
- Full run: `ctest --test-dir build --output-on-failure` → 100% pass
  (19 ctest tests: 1 unit + 18 integration suites)
- **Concurrency validated (2026-09-25):** the concurrent suites pass 8/8
  (incl. `ConcurrentWithFailover`), and a ThreadSanitizer run (`build-tsan/`)
  reports **no races in production code** — slice-4's `stateMutex_`/`shared_ptr`
  synchronization held clean. A test-harness race in `TopologyTestFixture.h`
  `createClient()` (concurrent `push_back`) was fixed with a `clientsMutex`.
- **Local caveat:** the multi-node cluster suites bind fixed host ports
  `11222/11322/11422/11522`; free `11222` (e.g. stop the `memory-service`
  Infinispan) before running them locally, or they fail with "port is already
  allocated". CI runners have these ports free.

**Not started / open:** see [📋 Backlog](#-backlog-the-whole-list) above for the
full list (Steps 10–12, benchmarks, TLS, code TODOs).

---

## Where things live

| What | Where |
|------|-------|
| Public API | `include/hotrod/RemoteCache.h` |
| Hot Rod URI parser | `include/hotrod/HotRodURI.h` (pure); factory `RemoteCache::fromUri` |
| Authentication (SASL/SCRAM) | `include/hotrod/Authentication.h`, `AuthCodec.h`, `SaslAuthenticator.h`; `src/auth/SaslAuthenticator.cpp`, `SCRAM.cpp` |
| User guide (AsciiDoc) | `documentation/index.adoc` + `documentation/topics/` |
| Retry API (bound view) | `include/hotrod/RetryView.h`, `RetryContext.h`; routing helpers in `ServerSelection.h` |
| Async transport core | `src/transport/MultiplexedConnection.cpp` |
| Operations (PING/GET/PUT/REMOVE) | `src/operations/RemoteCache.cpp` |
| Codecs | `src/codec/`, `src/hash/`, `src/auth/`, `src/topology/` |
| Unit tests | `tests/unit/` |
| Integration tests | `tests/integration/` (see `README_TOPOLOGY.md` there) |
| Why decisions were made | [`DECISIONS.md`](DECISIONS.md) |
| Historical milestone log (frozen at Step 9) | [`archive/PROGRESS.md`](archive/PROGRESS.md) |
| Superseded snapshots | [`archive/`](archive/) |
| **Wire-format spec (protocol 4.0/4.1)** | Kaitai schema — local `../hotrod-dissector/schemas/hotrod40.ksy`, public [github.com/rigazilla/hotrod-dissector](https://github.com/rigazilla/hotrod-dissector/tree/main/schemas) |
| **Java reference client** (authoritative for behavior/semantics) | local `/home/rigazilla/git/infinispan/client/hotrod-client/`, public [github.com/infinispan/infinispan](https://github.com/infinispan/infinispan/tree/main/client/hotrod-client) |

---

## Plan: metadata operations (Step 10)

Opcodes verified against the official Hot Rod protocol reference
(infinispan.org/docs → hotrod_protocol) **and** the authoritative Kaitai
schema at [`../hotrod-dissector/schemas/hotrod40.ksy`](../../hotrod-dissector/schemas/hotrod40.ksy)
on 2026-09-16. That `.ksy` is the wire-format source of truth for protocol
4.0/4.1 — consult it (not prose docs) when in doubt about a byte layout.

> **Why the Kaitai schema is trustworthy:** it's an *independent* encoding of
> the same protocol the Java client implements, so the two act as a mutual
> cross-check. If this client, the schema, and the Java client ever disagree on
> a byte layout, one of them has a bug — investigate before shipping. Public
> copy: <https://github.com/rigazilla/hotrod-dissector/tree/main/schemas>.
>
> **Known schema bug (found 2026-09-20):** `hotrod40.ksy`
> `replace_if_unmodified_request` packs the expiration `time_units` nibbles in
> the *opposite* order from `put_request`/`expiration_params` (lifespan and
> maxIdle swapped). Java uses one shared `writeExpirationParams` for PUT and
> replaceWithVersion, so PUT's order — **high nibble = lifespan, low = maxIdle**
> — is authoritative and is what this client implements. Fix pending upstream in
> hotrod-dissector.

### Opcode table (request / response)

| Operation | Req | Resp | In code? |
|-----------|-----|------|----------|
| put | 0x01 | 0x02 | ✅ |
| get | 0x03 | 0x04 | ✅ |
| putIfAbsent | 0x05 | 0x06 | ✅ |
| replace (only if present) | 0x07 | 0x08 | ✅ |
| **replaceWithVersion** (replaceIfUnmodified) | **0x09** | **0x0A** | ✅ |
| remove | 0x0B | 0x0C | ✅ |
| **removeWithVersion** (removeIfUnmodified) | **0x0D** | **0x0E** | ✅ |
| containsKey | 0x0F | 0x10 | ✅ |
| getWithVersion | 0x11 | 0x12 | ❌ |
| **getWithMetadata** | **0x1B** | **0x1C** | ✅ |

Opcode constants are defined in `include/hotrod/HeaderCodec.h` (`Opcode`
namespace). All Step 10 opcodes above are present; add new ones there.

### Recommended order
`getWithMetadata` first (it yields the entry version), then the two
version-based writes that depend on it, then the simpler conditionals:
1. ~~**getWithMetadata** (0x1B/0x1C)~~ ✅ shipped 2026-09-17
2. ~~**removeWithVersion** (0x0D/0x0E)~~ ✅ shipped 2026-09-19
3. ~~**replaceWithVersion** (0x09/0x0A)~~ ✅ shipped 2026-09-20
4. ~~putIfAbsent (0x05/0x06), replace (0x07/0x08), containsKey (0x0F/0x10)~~ ✅ shipped 2026-09-20 — **Step 10 complete**

### Step-by-step: getWithMetadata (the entry point)

Mirrors the existing `get()` at `src/operations/RemoteCache.cpp:221`.

1. **Opcodes** — add to `HeaderCodec.h`:
   `GET_WITH_METADATA_REQUEST = 0x1B`, `GET_WITH_METADATA_RESPONSE = 0x1C`.
2. **Request encoding** — identical to GET: Protocol 4.0 header + key as
   `lp_bytes` (vInt length + bytes). Reuse the get() request builder.
3. **Response body parser** — this is the new bit. `execute()` extracts the
   header status and hands it to the parser as the `status` arg (do **not**
   read a status byte from the body — the `.ksy` puts status in the header):
   - `status == 0x01 || 0x02` → key not found → return `{}` (→ `nullopt`),
     exactly as `get()` does at `RemoteCache.cpp:221`
   - on `status == 0x00`: **reuse `Connection::receiveMetadata()`**
     (`Connection.cpp:292`) — it reads flag + optional created(s8)/lifespan(vInt)
     + optional lastUsed(s8)/maxIdle(vInt) + 8-byte version, byte-for-byte
     matching `entry_metadata` in the `.ksy` (lines 498–516)
   - read value as `lp_bytes` (`conn->receiveByteArray()`)
   - return `EntryWithMetadata{ metadata, value }`
4. **Public method** — add
   `std::future<std::optional<EntryWithMetadata>> getWithMetadata(const ByteArray& key);`
   to `RemoteCache.h`, implement via `connection_->execute(...)` with the
   parser lambda (same shape as get()).
5. **Unit test** — new `tests/unit/GetWithMetadataTest.cpp`: encode request,
   decode a crafted response with and without infinite-lifespan/maxidle flags,
   assert version + timestamps + value. Add target to `CMakeLists.txt` and to
   the `unit_tests` sources.
6. **Integration test** — PUT a key, then `getWithMetadata`, assert a non-zero
   version and the value round-trips (pattern: `GetIntegrationTest.cpp`).

**Wire format cheat-sheet (response 0x1C):**
`status(1)` · `flag(1)` [`0x01`=INFINITE_LIFESPAN, `0x02`=INFINITE_MAXIDLE] ·
`created(int64)`+`lifespan(vInt)` *(only if lifespan not infinite)* ·
`lastUsed(int64)`+`maxIdle(vInt)` *(only if maxIdle not infinite)* ·
`version(int64, 8 bytes, always)` · `valueLen(vInt)`+`value` *(on success)*.

**Then removeWithVersion / replaceWithVersion** carry the 8-byte version *in the
request*: removeWithVersion = key + version; replaceWithVersion = key + lifespan
(vInt) + maxIdle (vInt) + version + value. Responses use status `0x01` =
"not done, key was modified", `0x02` = "key does not exist".

## How this project's docs work (the process)

The full per-session workflow — start-of-session re-orientation, what to update
before you stop, the authoritative protocol sources, and how each doc fits
together — lives in **[`WORKFLOW.md`](WORKFLOW.md)**. Read that first if you're
coming back to the project.

The one habit that keeps this file trustworthy: **update `⏭ Next steps` + the
`Last updated` date when you *stop*, not when you start** — finished or not. A
`pre-commit` hook (`.githooks/pre-commit`) enforces it: a commit that changes
project source without staging this file is blocked.

Rule of thumb: **current state changes here; history/reasoning changes in
[`DECISIONS.md`](DECISIONS.md).** This file is a snapshot ("where the project is
now"); DECISIONS.md is the append-only ledger of *why/how/when*. Don't write
narrative history into this file — put it in DECISIONS.md.
