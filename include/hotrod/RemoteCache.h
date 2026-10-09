#pragma once

#include "Types.h"
#include "Connection.h"
#include "MultiplexedConnection.h"
#include "HeaderCodec.h"
#include "ConsistentHash.h"
#include "RetryContext.h"
#include "Authentication.h"
#include <string>
#include <memory>
#include <cstdint>
#include <map>
#include <future>
#include <optional>
#include <mutex>

namespace hotrod {

class RetryView;  // bound exclusion-aware view; defined in RetryView.h

/**
 * High-level Hot Rod client API.
 *
 * Provides operations like PING, GET, PUT against an Infinispan server.
 *
 * Reference:
 * - Java: org.infinispan.client.hotrod.RemoteCache
 */
class RemoteCache {
public:
    /**
     * Create a client connected to specified server.
     *
     * @param host Server hostname or IP
     * @param port Server port (default 11222)
     */
    RemoteCache(const std::string& host, uint16_t port = 11222);

    /**
     * Create a client with optional cache name.
     */
    RemoteCache(const std::string& host, uint16_t port, const std::string& cacheName);

    /**
     * Create a client from a Hot Rod URI (issue #9).
     *
     * Grammar: hotrod://[user:password@]host1[:port1][,host2...][?k=v&...]
     * The returned client is configured but NOT connected — call connect() on it.
     * Because RemoteCache is non-movable (it owns a mutex), this returns a
     * unique_ptr rather than a value.
     *
     * v1 scope (see HotRodURI / issue #10 for what is deferred): only the
     * hotrod:// scheme; the first host is used as the seed (topology discovery
     * finds the rest); credentials enable SCRAM; the client_intelligence,
     * protocol_version, and sasl_mechanism query parameters are honoured, any
     * other parameter (or hotrods:// / TLS) throws.
     *
     * @param uri       Hot Rod URI string
     * @param cacheName Cache to operate on (the URI path is not used for this)
     * @throws HotRodClientException on a malformed or unsupported URI
     */
    static std::unique_ptr<RemoteCache> fromUri(const std::string& uri,
                                                const std::string& cacheName = "");

    ~RemoteCache();

    /**
     * Connect to the server.
     */
    void connect();

    /**
     * PING operation - test connectivity to server.
     *
     * Opcode: 0x17 (PING_REQUEST) → 0x18 (PING_RESPONSE)
     *
     * @return Future that completes when PING response received
     * @throws std::runtime_error if PING fails (via future.get())
     */
    std::future<void> ping();

    /**
     * GET operation - retrieve value for key.
     *
     * Opcode: 0x03 (GET_REQUEST) → 0x04 (GET_RESPONSE)
     *
     * @param key The key as raw bytes
     * @return Future with optional<ByteArray>: value if found, nullopt if not found
     * @throws std::runtime_error on communication errors (via future.get())
     *
     * Usage:
     *   if (auto value = cache.get(key).get()) {
     *       // key found, use *value
     *   }
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.GetOperation
     */
    std::future<std::optional<ByteArray>> get(const ByteArray& key);

    /**
     * PUT operation - store key-value pair.
     *
     * Opcode: 0x01 (PUT_REQUEST) → 0x02 (PUT_RESPONSE)
     *
     * @param key The key as raw bytes
     * @param value The value as raw bytes
     * @param lifespan Entry lifespan in seconds (0 = immortal)
     * @param maxIdle Max idle time in seconds (0 = no max idle)
     * @param previousValue If true, request previous value with metadata (Protocol 4.0+)
     * @return Future with optional<EntryWithMetadata>: previous entry if existed, nullopt otherwise
     * @throws std::runtime_error on communication errors (via future.get())
     *
     * Usage:
     *   if (auto prevEntry = cache.put(key, value, 0, 0, true).get()) {
     *       // key already existed
     *       ByteArray& prevValue = prevEntry->value;
     *       int64_t version = prevEntry->metadata.version;
     *   }
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.PutOperation
     * - Java: org.infinispan.client.hotrod.impl.operations.AbstractKeyValueOperation
     */
    std::future<std::optional<EntryWithMetadata>> put(const ByteArray &key, const ByteArray &value,
                                                      uint64_t lifespan = 0, uint64_t maxIdle = 0, bool previousValue = false);

    /**
     * PUT_IF_ABSENT operation - store key-value pair only if the key is absent.
     *
     * Opcode: 0x05 (PUT_IF_ABSENT_REQUEST) → 0x06 (PUT_IF_ABSENT_RESPONSE)
     *
     * Stores the entry only if no value is currently associated with the key.
     * If the key already exists the entry is left untouched.
     *
     * @param key The key as raw bytes
     * @param value The value as raw bytes
     * @param lifespan Entry lifespan in seconds (0 = immortal)
     * @param maxIdle Max idle time in seconds (0 = no max idle)
     * @param previousValue If true, request previous value with metadata (Protocol 4.0+)
     * @return Future with optional<EntryWithMetadata>: the existing entry that
     *         prevented storage (when previousValue is set and the key existed);
     *         nullopt if the value was stored (key was absent)
     * @throws std::runtime_error on communication errors (via future.get())
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.PutIfAbsentOperation
     */
    std::future<std::optional<EntryWithMetadata>> putIfAbsent(const ByteArray &key, const ByteArray &value,
                                                              uint64_t lifespan = 0, uint64_t maxIdle = 0, bool previousValue = false);

    /**
     * REPLACE operation - store key-value pair only if the key is present.
     *
     * Opcode: 0x07 (REPLACE_REQUEST) → 0x08 (REPLACE_RESPONSE)
     *
     * Replaces the entry's value only if a value is currently associated with
     * the key. If the key does not exist nothing is stored.
     *
     * @param key The key as raw bytes
     * @param value The new value as raw bytes
     * @param lifespan Entry lifespan in seconds (0 = immortal)
     * @param maxIdle Max idle time in seconds (0 = no max idle)
     * @param previousValue If true, request previous value with metadata (Protocol 4.0+)
     * @return Future with optional<EntryWithMetadata>: the replaced (previous)
     *         entry (when previousValue is set and the key existed); nullopt if
     *         the key did not exist (nothing replaced)
     * @throws std::runtime_error on communication errors (via future.get())
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.ReplaceOperation
     */
    std::future<std::optional<EntryWithMetadata>> replace(const ByteArray &key, const ByteArray &value,
                                                          uint64_t lifespan = 0, uint64_t maxIdle = 0, bool previousValue = false);

    /**
     * CONTAINS_KEY operation - test whether a key exists in the cache.
     *
     * Opcode: 0x0F (CONTAINS_KEY_REQUEST) → 0x10 (CONTAINS_KEY_RESPONSE)
     *
     * @param key The key as raw bytes
     * @return Future<bool>: true if the key exists; false otherwise
     * @throws std::runtime_error on communication errors (via future.get())
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.ContainsKeyOperation
     */
    std::future<bool> containsKey(const ByteArray &key);

    /**
     * GET_WITH_METADATA operation - retrieve value plus entry metadata.
     *
     * Opcode: 0x1B (GET_WITH_METADATA_REQUEST) → 0x1C (GET_WITH_METADATA_RESPONSE)
     *
     * Unlike get(), this returns the entry version (for optimistic locking /
     * version-based CAS operations) and expiration metadata alongside the value.
     *
     * @param key The key as raw bytes
     * @return Future with optional<EntryWithMetadata>: entry if found, nullopt if not found
     * @throws std::runtime_error on communication errors (via future.get())
     *
     * Usage:
     *   if (auto entry = cache.getWithMetadata(key).get()) {
     *       ByteArray& value = entry->value;
     *       int64_t version = entry->metadata.version;  // for replaceWithVersion, etc.
     *   }
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.GetWithMetadataOperation
     */
    std::future<std::optional<EntryWithMetadata>> getWithMetadata(const ByteArray& key);

    /**
     * REMOVE operation - delete key-value pair.
     *
     * Opcode: 0x0B (REMOVE_REQUEST) → 0x0C (REMOVE_RESPONSE)
     *
     * @param key The key as raw bytes
     * @param previousValue If true, request previous value with metadata (Protocol 4.0+)
     * @return Future with optional<EntryWithMetadata>: previous entry if existed, nullopt otherwise
     * @throws std::runtime_error on communication errors (via future.get())
     *
     * Usage:
     *   if (auto prevEntry = cache.remove(key, true).get()) {
     *       // key existed and was removed
     *       ByteArray& prevValue = prevEntry->value;
     *       int64_t version = prevEntry->metadata.version;
     *   }
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.RemoveOperation
     */
    std::future<std::optional<EntryWithMetadata>> remove(const ByteArray& key, bool previousValue = false);

    /**
     * REMOVE_WITH_VERSION operation - version-based conditional remove (CAS).
     *
     * Removes the entry only if its current version matches `version`. The
     * version is obtained from a prior getWithMetadata() call.
     *
     * Opcode: 0x0D (REMOVE_WITH_VERSION_REQUEST) → 0x0E (RESPONSE)
     *
     * @param key The key as raw bytes
     * @param version Expected entry version (from getWithMetadata().metadata.version)
     * @return Future<bool>: true if the entry was removed; false if the version
     *         did not match (entry modified) or the key did not exist
     * @throws std::runtime_error on communication errors or unexpected status
     *         (via future.get())
     *
     * Usage:
     *   if (auto entry = cache.getWithMetadata(key).get()) {
     *       bool removed = cache.removeWithVersion(key, entry->metadata.version).get();
     *   }
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.RemoveIfUnmodifiedOperation
     */
    std::future<bool> removeWithVersion(const ByteArray& key, int64_t version);

    /**
     * REPLACE_WITH_VERSION operation - version-based conditional replace (CAS).
     *
     * Replaces the entry's value only if its current version matches `version`
     * (obtained from a prior getWithMetadata() call).
     *
     * Opcode: 0x09 (REPLACE_WITH_VERSION_REQUEST) → 0x0A (RESPONSE)
     *
     * @param key The key as raw bytes
     * @param value The new value as raw bytes
     * @param version Expected entry version (from getWithMetadata().metadata.version)
     * @param lifespan Lifespan in seconds (0 = server default / infinite)
     * @param maxIdle Max idle in seconds (0 = server default / infinite)
     * @return Future<bool>: true if the entry was replaced; false if the version
     *         did not match (entry modified) or the key did not exist
     * @throws std::runtime_error on communication errors or unexpected status
     *         (via future.get())
     *
     * Usage:
     *   if (auto entry = cache.getWithMetadata(key).get()) {
     *       bool replaced = cache.replaceWithVersion(key, newValue,
     *                                                entry->metadata.version).get();
     *   }
     *
     * Reference:
     * - Java: org.infinispan.client.hotrod.impl.operations.ReplaceIfUnmodifiedOperation
     */
    std::future<bool> replaceWithVersion(const ByteArray& key, const ByteArray& value,
                                         int64_t version, uint64_t lifespan = 0, uint64_t maxIdle = 0);

    /**
     * Close connection to server.
     */
    void disconnect();

    /**
     * Check if connected.
     */
    bool isConnected() const;

    /**
     * Get the cache name.
     */
    const std::string& getCacheName() const { return cacheName_; }

    /**
     * Set cache name.
     */
    void setCacheName(const std::string& name) { cacheName_ = name; }

    /**
     * Set protocol version.
     *
     * @param version Protocol version (e.g., Protocol::VERSION_40, Protocol::VERSION_41)
     */
    void setProtocolVersion(uint8_t version) {
        protocolVersion_ = version;
    }

    /**
     * Get current protocol version.
     */
    uint8_t getProtocolVersion() const {
        return protocolVersion_;
    }

    /**
     * Set client intelligence level.
     *
     * Controls what topology information the server sends in responses:
     * - BASIC (0x01): No topology updates, use configured server list only
     * - TOPOLOGY_AWARE (0x02): Receive topology updates, round-robin to servers
     * - HASH_DISTRIBUTION_AWARE (0x03): Topology + routing to primary owner (default in Java)
     *
     * @param intelligence The client intelligence level
     */
    void setClientIntelligence(ClientIntelligence intelligence) {
        clientIntelligence_ = intelligence;
    }

    /**
     * Get current client intelligence level.
     */
    ClientIntelligence getClientIntelligence() const {
        return clientIntelligence_;
    }

    /**
     * Configure SASL authentication. Must be called before connect().
     *
     * Enables the SASL handshake on every connection the client opens (the seed
     * connection and every topology-discovered cluster member). Only the SCRAM
     * family is supported — SCRAM-SHA-1 / SCRAM-SHA-256 / SCRAM-SHA-512 — any
     * other mechanism throws a HotRodClientException at connect() time.
     *
     * For SCRAM only username/password enter the exchange; realm and serverName
     * are accepted for parity with the Java client but are unused by SCRAM.
     *
     * @param username   SASL authentication username
     * @param password   SASL authentication password
     * @param realm      Security realm (default "default"; unused by SCRAM)
     * @param serverName SASL server name (default "infinispan"; unused by SCRAM)
     * @param mechanism  SASL mechanism (default "SCRAM-SHA-256")
     */
    void setAuthentication(const std::string& username,
                           const std::string& password,
                           const std::string& realm = "default",
                           const std::string& serverName = "infinispan",
                           const std::string& mechanism = "SCRAM-SHA-256") {
        auth_.enabled = true;
        auth_.username = username;
        auth_.password = password;
        auth_.realm = realm;
        auth_.serverName = serverName;
        auth_.mechanism = mechanism;
    }

    /**
     * Current authentication configuration (auth_.enabled is false by default).
     */
    const Authentication& getAuthentication() const {
        return auth_;
    }

    /**
     * Control non-owner proxy fallback for hash-aware routing (Step 11b, D5).
     *
     * When true (the default, matching the Java client), if none of a key's
     * owners can be reached, selection falls through to any other server in the
     * topology, which proxies the request to the real owner. When false,
     * selection stops once the owners are exhausted and throws a transient
     * HotRodClientException with ownersExhausted=true, letting the caller decide
     * whether to drive the proxy step itself.
     *
     * This governs the BeforeSend connection-fallback in selectServerForKey
     * only; it does not make retry automatic (retry stays user-decided, D2).
     */
    void setProxyToNonOwner(bool proxyToNonOwner) {
        proxyToNonOwner_ = proxyToNonOwner;
    }

    /**
     * Whether non-owner proxy fallback is enabled (default true).
     */
    bool getProxyToNonOwner() const {
        return proxyToNonOwner_;
    }

    /**
     * Get current topology information.
     * Only populated if client intelligence is TOPOLOGY_AWARE or HASH_DISTRIBUTION_AWARE.
     *
     * Thread-safety: returns a reference to internal state that the read-loop
     * thread mutates under stateMutex_ on topology updates. It is a
     * testing/debugging accessor and must NOT be read concurrently with live
     * operations or topology churn. Use getTopologyId() (locked, by value) for a
     * safe point-in-time read.
     */
    const TopologyInfo& getTopology() const {
        return topology_;
    }

    /**
     * Get current topology ID (thread-safe: read under stateMutex_).
     */
    VInt getTopologyId() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return topology_.getTopologyId();
    }

    /**
     * Get the ConsistentHash instance (for testing/debugging).
     *
     * Thread-safety: same caveat as getTopology() — returns a reference to state
     * the read-loop mutates under stateMutex_; not safe to read concurrently with
     * live operations or topology churn.
     */
    const ConsistentHash& getConsistentHash() const {
        return consistentHash_;
    }

    /**
     * Return a bound view of this cache that excludes the given nodes from
     * routing (Step 11b). The base operations stay retry-free; a caller opts into
     * exclusion only on the retry path, seeding it from a caught exception:
     *
     *   try { return cache.get(key).get(); }
     *   catch (const HotRodClientException& e) {
     *       if (!isTransient(e)) throw;              // + your idempotency call
     *       return cache.excluding(e).get(key).get();  // avoids e.triedNodes
     *   }
     *
     * The returned RetryView is non-owning and must not outlive this cache.
     */
    RetryView excluding(std::vector<ServerAddress> excludeNodes);

    /**
     * Convenience overload: exclude exactly the nodes a prior attempt tried,
     * as reported by the exception (uses e.triedNodes).
     */
    RetryView excluding(const HotRodClientException& e);

private:
    friend class RetryView;  // forwards to the *Impl methods below

    // Exclusion-aware implementations behind the public retry-free operations.
    // The public ops call these with an empty RetryContext; a RetryView calls
    // them with its bound context. Each surfaces failures as a
    // HotRodClientException whose triedNodes is the union of the context's
    // excludeNodes and the nodes this attempt touched (see RemoteCache.cpp).
    // Keyless operation (no routing object). Auto-fails over across all servers
    // before send (selectAnyServer); after-send/server errors surface to the
    // caller with triedNodes, retryable via a RetryView, exactly like keyed ops.
    std::future<void> pingImpl(const RetryContext& ctx);
    std::future<std::optional<ByteArray>> getImpl(const ByteArray& key, const RetryContext& ctx);
    std::future<std::optional<EntryWithMetadata>> getWithMetadataImpl(const ByteArray& key, const RetryContext& ctx);
    std::future<std::optional<EntryWithMetadata>> putImpl(const ByteArray& key, const ByteArray& value,
                                                          uint64_t lifespan, uint64_t maxIdle, bool previousValue,
                                                          const RetryContext& ctx);
    std::future<std::optional<EntryWithMetadata>> putIfAbsentImpl(const ByteArray& key, const ByteArray& value,
                                                                  uint64_t lifespan, uint64_t maxIdle, bool previousValue,
                                                                  const RetryContext& ctx);
    std::future<std::optional<EntryWithMetadata>> replaceImpl(const ByteArray& key, const ByteArray& value,
                                                              uint64_t lifespan, uint64_t maxIdle, bool previousValue,
                                                              const RetryContext& ctx);
    std::future<bool> containsKeyImpl(const ByteArray& key, const RetryContext& ctx);
    std::future<std::optional<EntryWithMetadata>> removeImpl(const ByteArray& key, bool previousValue,
                                                             const RetryContext& ctx);
    std::future<bool> removeWithVersionImpl(const ByteArray& key, int64_t version, const RetryContext& ctx);
    std::future<bool> replaceWithVersionImpl(const ByteArray& key, const ByteArray& value,
                                             int64_t version, uint64_t lifespan, uint64_t maxIdle,
                                             const RetryContext& ctx);

    std::string host_;
    uint16_t port_;
    std::string cacheName_;
    // shared_ptr (not unique_ptr) so a connection handed to an in-flight
    // operation stays alive even if a concurrent topology update evicts it from
    // the pool (see selectServerForKey / cleanupStaleConnections, Step 11b slice 4).
    std::shared_ptr<MultiplexedConnection> connection_;
    // Note: messageIdCounter_ removed - MultiplexedConnection generates IDs
    uint8_t protocolVersion_;  // Protocol version (default: VERSION_41)
    ClientIntelligence clientIntelligence_;  // Client intelligence level
    bool proxyToNonOwner_ = true;  // Owner->non-owner fallback in routing (D5)
    Authentication auth_;  // SASL auth config (disabled by default); applied at connect()

    // stateMutex_ guards the routing/pool state below (topology_, consistentHash_,
    // connectionPool_) against concurrent access by the read-loop thread (topology
    // callbacks → handleTopologyUpdate) and user/retry threads (ops →
    // selectServerForKey → getConnectionForServer). Mutable so the const
    // getTopologyId() accessor can lock it. Lock discipline: the two public entry
    // points (handleTopologyUpdate, selectServerForKey) acquire it at the top;
    // their internal helpers (cleanupStaleConnections, getConnectionForServer)
    // assume it is already held and must not re-lock.
    mutable std::mutex stateMutex_;
    TopologyInfo topology_;  // Current cluster topology
    ConsistentHash consistentHash_;  // Hash-aware routing (for HASH_DISTRIBUTION_AWARE)

    // Connection pool: one MultiplexedConnection per server
    // Key = "host:port", Value = MultiplexedConnection instance
    std::map<std::string, std::shared_ptr<MultiplexedConnection>> connectionPool_;

    /**
     * Handle topology update callback from MultiplexedConnection.
     */
    void handleTopologyUpdate(const TopologyInfo& topo, const std::string& cacheName);

    /**
     * Select server connection for a given key using hash-aware routing.
     *
     * When clientIntelligence == HASH_DISTRIBUTION_AWARE and hash topology is available:
     * - Calculates the segment for the key
     * - Finds the primary owner server for that segment
     * - Returns connection to that server (creating if needed)
     *
     * Otherwise falls back to the default connection.
     *
     * Exclusion-aware (Step 11b): nodes in @p ctx.excludeNodes are skipped, so a
     * user-driven retry avoids the nodes a prior attempt already tried. The
     * ordered candidate list (owners first, then non-owner proxy fallback, minus
     * excluded nodes) is computed by the pure orderKeyCandidates() helper.
     *
     * @param key      The key to route
     * @param ctx      Retry/exclusion context (defaulted: no exclusions)
     * @param triedOut If non-null, receives every node this call attempted, in
     *                 order — for accumulating triedNodes across retries.
     * @return Connection to use for this key (never null). Returned as a
     *         shared_ptr so it stays alive for the duration of the operation even
     *         if a concurrent topology update evicts it from the pool. Acquires
     *         stateMutex_.
     * @throws HotRodClientException (BeforeSend) if no candidate was reachable;
     *         ownersExhausted is set iff every owner was tried or excluded.
     */
    std::shared_ptr<MultiplexedConnection> selectServerForKey(const ByteArray& key,
                                              const RetryContext& ctx = {},
                                              std::vector<ServerAddress>* triedOut = nullptr);

    /**
     * Select a connection for a keyless operation (e.g. ping), with the same
     * before-send failover as selectServerForKey but over *all* servers rather
     * than a key's owners (Step 11c). Candidates are every server in the current
     * topology minus ctx.excludeNodes, in topology order (computed by
     * orderKeyCandidates with an empty owner list); the first reachable one wins.
     *
     * Ping is often one of the first calls, before any topology has been received:
     * when the topology has no servers this returns the seed connection_ so an
     * early ping still works. Once real topology exists, exhausting it (all
     * servers excluded or unreachable) throws instead of silently reusing an
     * excluded seed.
     *
     * @param ctx      Retry/exclusion context (defaulted: no exclusions)
     * @param triedOut If non-null, receives every node this call attempted.
     * @return Connection to use (never null). shared_ptr for the same lifetime
     *         reason as selectServerForKey. Acquires stateMutex_.
     * @throws HotRodClientException (BeforeSend) if topology exists but no
     *         candidate was reachable. ownersExhausted is always false (keyless).
     */
    std::shared_ptr<MultiplexedConnection> selectAnyServer(const RetryContext& ctx = {},
                                              std::vector<ServerAddress>* triedOut = nullptr);

    /**
     * Get or create connection for a specific server.
     *
     * Caller must hold stateMutex_ (invoked from selectServerForKey); this method
     * does not lock.
     *
     * @param server Server information (host, port, hashId)
     * @return MultiplexedConnection to the server (never null)
     */
    std::shared_ptr<MultiplexedConnection> getConnectionForServer(const ServerInfo& server);

    /**
     * Clean up connections to servers no longer in topology.
     * Called after topology updates. Caller must hold stateMutex_
     * (invoked from handleTopologyUpdate); this method does not lock.
     */
    void cleanupStaleConnections();
};

} // namespace hotrod

// RetryView completes the excluding()/RetryView pairing; included last so
// RemoteCache is a complete type. (Include-guarded, so the mutual include of
// RemoteCache.h from RetryView.h is a no-op here.)
#include "RetryView.h"
