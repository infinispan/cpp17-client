# Project Status — Hot Rod C++17 Client

> **This is the single source of truth for "where the project is."**
> If any other doc disagrees with this file, this file wins. Point-in-time
> snapshots live in [`archive/`](archive/) and are historical only.
>
> **Last updated:** 2026-11-13

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

1. **Step 12 — Bulk operations.** `GET_ALL` (0x2F), `PUT_ALL` (0x2D),
   `BULK_GET` (0x1F, iterator-style). See 📋 Backlog below.
2. **Benchmark the multiplexing path** — the async rewrite targets 5–10×
   concurrent throughput; never measured.
3. **Small cleanup:** read header "other params" when `paramCount > 0`
   (two `TODO`s in `src/operations/RemoteCache.cpp`).

_Recently shipped (detail in "Working and shipped" + [`DECISIONS.md`](DECISIONS.md)):
Docker integration coverage for mutual TLS + SASL PLAIN + SASL EXTERNAL
(2026-11-13), non-SCRAM SASL (PLAIN/EXTERNAL/OAUTHBEARER) (#10, 2026-11-12), mutual TLS (PEM
client cert/key) + PEM-honest TLS URI keys (`ca_file`,
`client_cert_file`/`client_key_file`; Java keystore keys now rejected with a
redirect) (#18, 2026-11-12), server-authenticated TLS for `hotrods://` (#18,
2026-11-06), multi-server seed list + failover (#12), Hot Rod URI `fromUri` (#9),
SASL/SCRAM auth, Step 11 error handling + user-decided retry, Step 10 metadata/CAS._

---

## 📋 Backlog (the whole list)

_Everything known-to-do, in one place. `⏭ Next steps` above is just the 1–3 you
pull from here next. Step numbers follow
[`../hotrod-foundry/ROADMAP.md`](../../hotrod-foundry/ROADMAP.md); this file
(STATUS.md) is authoritative for what's done._

**Remaining roadmap steps:**
- [ ] **Step 12 — Bulk operations.** `GET_ALL` (0x2F), `PUT_ALL` (0x2D),
  `BULK_GET` (0x1F, iterator-style).

**Other known work (not roadmap steps):**
- [ ] **OAUTHBEARER integration coverage** — the last non-SCRAM SASL mechanism
  without an end-to-end Docker test. Needs real token infra (a token-realm backed
  by Keycloak, or a static-JWT public key + a pre-minted token). PLAIN, EXTERNAL,
  and mutual TLS integration coverage shipped 2026-11-13 (see "Working and
  shipped"); OAUTHBEARER is still unit-tested against a fake server only.
- [ ] **Benchmark the multiplexing path** — the async rewrite targets 5–10×
  concurrent throughput; unmeasured so far.
- [ ] **Code cleanup:** read header "other params" when `paramCount > 0`
  (`src/operations/RemoteCache.cpp:140,182`).
- [ ] **Hot Rod URI — deferred features (issue #10):** `connect_timeout`/
  `socket_timeout`, `protocol_version=AUTO`/3.x, and GSSAPI/GS2-KRB5 SASL (needs a
  Kerberos lib). Each drops its "unsupported" throw from `HotRodURI::parse` once
  the capability lands. (Multi-server seed list — #12 — TLS incl. mutual TLS — #18 —
  and non-SCRAM SASL PLAIN/EXTERNAL/OAUTHBEARER are already shipped; see "Working
  and shipped".)
- [ ] **TLS keystore formats + tuning (deferred part of #18/#10):** Java keystore
  formats (JKS/PKCS12, `*_store_file_name`/`*_store_type`/password/`key_alias`) —
  this client is PEM-only, so these URI keys are rejected with a redirect to the
  PEM keys (`ca_file`, `client_cert_file`/`client_key_file`). Also deferred:
  `ssl_protocol`/`ssl_ciphers`/`ssl_named_groups`/`ssl_provider`/`use_ssl`.

**Longer horizon (Step 14+ advanced features):** transactions (XA), client
listeners/events, counters, Ickle queries, streaming ops (4.1+), multimap,
near caching. Not scheduled.

_Done and not repeated here (see "Working and shipped" below + dated
[`DECISIONS.md`](DECISIONS.md) entries): foundation/primitives, Protocol 4.0
headers, topology (Step 4), consistent hashing + hash-aware routing, connection
pooling (Step 13), full CRUD (Steps 6–9), async multiplexing, Step 10
metadata/CAS, Step 11 error handling + user-decided retry, SASL/SCRAM auth, Hot
Rod URI `fromUri` (#9), multi-server seed list + failover (#12)._

---

## Current state (verified 2026-11-06)

**Working and shipped:**
- **Hot Rod URI (`RemoteCache::fromUri`)** — build a configured client from a
  `hotrod[s]://[user:pass@]host[:port][,host...][?k=v]` connection string (issue #9);
  pure header-only parser `HotRodURI::parse` (`include/hotrod/HotRodURI.h`).
  `hotrod://` and `hotrods://` (TLS) schemes, SCRAM via credentials,
  `sasl_mechanism`/`client_intelligence`/`protocol_version` + the PEM TLS params
  (`ca_file`/`sni_host_name`/`ssl_hostname_validation` and mutual-TLS
  `client_cert_file`/`client_key_file`/`client_key_password`); userinfo and query
  values are percent-decoded (java.net.URI parity); unsupported schemes/params and
  the Java keystore keys throw. timeouts/non-SCRAM deferred (issue #10)
- **TLS incl. mutual TLS (`hotrods://`, issue #18)** — `RemoteCache::setTls(...)`
  (and the `hotrods://` scheme) wrap every connection the client opens (seed +
  topology-discovered) in a TLS session over OpenSSL. All certificate material is
  **PEM** (not Java JKS/PKCS12). Server-auth: PEM trust material (`ca_file`, empty =
  system store), SNI (`sni_host`, skipped for literal IPs), host-name validation
  (`hostname_validation`, default on; chain verification always on), min TLS 1.2.
  **Mutual TLS:** PEM client cert chain (`cert_file`) + private key (`key_file`) +
  optional passphrase (`key_password`), loaded with a passwd callback so an
  encrypted key never blocks on a prompt; cert/key must be set together.
  `include/hotrod/TlsConfig.h`; handshake in `src/transport/Connection.cpp`
  (host-name validation via the `X509_VERIFY_PARAM` API, so it builds clean on
  OpenSSL 1.1.0 through 4.0). **URI keys:** `ca_file`, `sni_host_name`,
  `ssl_hostname_validation`, `client_cert_file`, `client_key_file`,
  `client_key_password`; the Java keystore keys (`trust_store_file_name`,
  `key_store_*`, `*_store_type`/password, `key_alias`) are rejected with a redirect
  to the PEM key. Unit-tested against an in-process OpenSSL server (server-auth +
  mutual TLS) and end-to-end against a TLS-enabled Infinispan
  (`tests/integration/TlsIntegrationTest.cpp`, Docker). A dropped connection is
  SIGPIPE-safe per-connection (MSG_NOSIGNAL / SO_NOSIGPIPE / custom TLS BIO) — no
  process-global signal change — so it surfaces as a thrown exception, not process
  death. JKS/PKCS12 keystore formats + cipher/protocol tuning deferred (#10)
- **Multi-server seed list + failover (`fromUri`, issue #12)** — all
  comma-separated hosts become seed servers. **Startup failover:** `connect()`
  tries each seed in order; all unreachable → one `HotRodClientException` listing
  every seed. **Runtime failover:** when the learned topology is empty (incl.
  BASIC mode) or every known server is unreachable, ops fall back to the seed list
  (`tryConnectSeeds`) — prefers the live bootstrap `connection_` and adopts the
  reached seed as the new bootstrap; keyed fallback gated on `proxyToNonOwner_`.
  Excluding every known server still throws (no seed rescue of an excluded node)
- Wire primitives (vInt, vLong, strings, byte arrays)
- Protocol 4.0 headers (all conditional fields)
- **SASL authentication (end-to-end)** — `setAuthentication(...)` runs the
  AUTH_MECH_LIST + AUTH handshake on every connection (seed + topology-discovered).
  Mechanisms: SCRAM family (`SCRAM-SHA-1/256/512`, RFC 5802, two-turn), **PLAIN**
  (RFC 4616), **EXTERNAL** (RFC 4422 — identity from the mutual-TLS client cert),
  **OAUTHBEARER** (RFC 7628 — bearer `token`); single-message mechanisms share a
  generalized path, pure initial-response builders in `include/hotrod/SaslMechanisms.h`.
  URI selects via `sasl_mechanism` (+ `token`); the `Authentication` overload of
  `setAuthentication` covers token/cert mechanisms. GSSAPI/DIGEST-MD5 throw at
  connect(). All OpenSSL, no JDK SASL provider. See `documentation/topics/security.adoc`
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
  `messageId → promise` pending map, `execute()` used by all operations.
  _(The July "70% / temporary blocking" docs are obsolete — see archive note.)_
- Multi-server topology test fixtures + Docker-based integration tests
- **CI runs integration tests on Linux** (Ubuntu job) as of 2026-09-18 — the
  `ubuntu-latest` runner's host Docker daemon starts the Infinispan containers.
  Windows and Fedora stay unit-only (Fedora runs inside a container with no
  Docker daemon; Windows doesn't build the integration tests). Windows/MSVC
  portability and `-Werror` build parity also landed (Sept 2026).
- **All integration tests pull `quay.io/infinispan/server:latest`** (unpinned) —
  single-server, auth, and (as of 2026-10-19) the multi-node cluster fixture
  (`test-configs/docker-compose-cluster.yml`), which previously pinned 16.2.
  Cache setup goes through `cli.sh -c <url> -f -` (batch mode, exits on EOF) and
  each ctest integration suite has `TIMEOUT 300`, so a server/CLI-version change
  that stalls a subprocess fails fast instead of hanging the whole CI job. See
  [`DECISIONS.md`](DECISIONS.md).

**Test status:** **284 unit** (verified 2026-11-12) + **107 integration** (23
suites, verified 2026-11-13), all passing. This is the one authoritative count —
other docs describe *what* is covered rather than repeating totals. Counts read
from the built test binaries via `--gtest_list_tests`, so they match what actually
runs. (The +6 integration tests on 2026-11-13 are the mutual-TLS round-trip/URI/
no-cert-rejected cases — `MutualTlsIntegrationTests` — the SASL EXTERNAL
cert-identity round-trip + URI cases — `ExternalAuthIntegrationTests` — and a PLAIN
case added to `AuthIntegrationTests`.)
- Unit (`./build/unit_tests`, <1s) — wire primitives/headers, consistent hashing,
  server-selection/retry helpers, the SASL handshake (SCRAM family +
  PLAIN/EXTERNAL/OAUTHBEARER, against fake servers), the TLS handshake (server-auth
  + mutual TLS, in-process OpenSSL server), and the Hot Rod URI parser.
- Integration (`ctest`, spins up Docker Infinispan single-server + multi-node
  clusters; also green on Linux CI) — CRUD + metadata/CAS, topology & hash-aware
  routing, keyed/keyless retry (`RetryView`/ping), SASL against a secured server
  (SCRAM family + PLAIN via `AuthIntegrationTests`; EXTERNAL over mutual TLS via
  `ExternalAuthIntegrationTests`), server-authenticated TLS (`TlsIntegrationTests`)
  and mutual TLS (`MutualTlsIntegrationTests`), Hot Rod URI `fromUri`, and
  multi-server seed failover.
- Full run: `ctest --test-dir build --output-on-failure` covers **24 suites**
  (1 unit + 23 integration). Last full end-to-end run 2026-10-19 (100% pass at the
  then-current 21 suites); unit suite re-verified 2026-11-12 (284 pass), and the
  `Auth`/`Tls`/`MutualTls`/`ExternalAuth` integration suites verified 2026-11-13
  (7 + 5 + 3 + 2 pass) after the integration-coverage changes.
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
| Operations (CRUD, metadata/CAS, ping) | `src/operations/RemoteCache.cpp` |
| Codecs | `src/codec/`, `src/hash/`, `src/auth/`, `src/topology/` |
| Unit tests | `tests/unit/` |
| Integration tests | `tests/integration/` (see `README_TOPOLOGY.md` there) |
| Why decisions were made | [`DECISIONS.md`](DECISIONS.md) |
| Historical milestone log (frozen at Step 9) | [`archive/PROGRESS.md`](archive/PROGRESS.md) |
| Superseded snapshots | [`archive/`](archive/) |
| **Wire-format spec (protocol 4.0/4.1)** | Kaitai schema — local `../hotrod-dissector/schemas/hotrod40.ksy`, public [github.com/rigazilla/hotrod-dissector](https://github.com/rigazilla/hotrod-dissector/tree/main/schemas) |
| **Java reference client** (authoritative for behavior/semantics) | local `/home/rigazilla/git/infinispan/client/hotrod-client/`, public [github.com/infinispan/infinispan](https://github.com/infinispan/infinispan/tree/main/client/hotrod-client) |

---

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
