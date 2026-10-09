#include "hotrod/RemoteCache.h"
#include "hotrod/RetryView.h"
#include "hotrod/Codec.h"
#include "hotrod/HotRodClientException.h"
#include "hotrod/ServerSelection.h"
#include "hotrod/HotRodURI.h"
#include <stdexcept>
#include <set>
#include <algorithm>

namespace hotrod
{
   namespace
   {
      // Rethrow a failed Response's error, enriching a HotRodClientException with
      // the union of nodes tried across this and prior attempts so a caller can
      // feed e.triedNodes back as the next attempt's exclusion set. Non-HotRod
      // exceptions propagate unchanged. Always throws.
      [[noreturn]] void rethrowWithTriedNodes(std::exception_ptr eptr,
                                              const std::vector<ServerAddress> &tried,
                                              const std::vector<ServerAddress> &excluded)
      {
         try
         {
            std::rethrow_exception(eptr);
         }
         catch (HotRodClientException &e)
         {
            e.triedNodes = unionNodes(unionNodes(excluded, tried), e.triedNodes);
            throw;
         }
      }
   } // namespace

   RemoteCache::RemoteCache(const std::string &host, uint16_t port)
       : host_(host), port_(port), cacheName_(""),
         protocolVersion_(Protocol::VERSION_41),
         clientIntelligence_(ClientIntelligence::BASIC)
   {
      // Create MultiplexedConnection with topology callback
      auto topologyCallback = [this](const TopologyInfo &topo, const std::string &cacheName)
      {
         this->handleTopologyUpdate(topo, cacheName);
      };
      connection_ = std::make_shared<MultiplexedConnection>(host, port, topologyCallback);
   }

   RemoteCache::RemoteCache(const std::string &host, uint16_t port, const std::string &cacheName)
       : host_(host), port_(port), cacheName_(cacheName),
         protocolVersion_(Protocol::VERSION_41),
         clientIntelligence_(ClientIntelligence::BASIC)
   {
      // Create MultiplexedConnection with topology callback
      auto topologyCallback = [this](const TopologyInfo &topo, const std::string &cacheName)
      {
         this->handleTopologyUpdate(topo, cacheName);
      };
      connection_ = std::make_shared<MultiplexedConnection>(host, port, topologyCallback);
   }

   std::unique_ptr<RemoteCache> RemoteCache::fromUri(const std::string &uri,
                                                     const std::string &cacheName)
   {
      // Pure parse + validation (throws on anything malformed or unsupported).
      const HotRodURI parsed = HotRodURI::parse(uri);

      // v1: the first host is the seed; topology discovery finds the rest (#10).
      const ServerAddress &seed = parsed.seed();
      auto cache = std::make_unique<RemoteCache>(seed.host, seed.port, cacheName);

      if (parsed.clientIntelligence)
      {
         cache->setClientIntelligence(*parsed.clientIntelligence);
      }
      if (parsed.protocolVersion)
      {
         cache->setProtocolVersion(*parsed.protocolVersion);
      }
      if (parsed.username)
      {
         // Credentials present -> enable SCRAM. Mechanism from the query if given,
         // else the setAuthentication default (SCRAM-SHA-256). Non-SCRAM mechanisms
         // are rejected later at connect().
         const std::string password = parsed.password.value_or("");
         if (parsed.saslMechanism)
         {
            cache->setAuthentication(*parsed.username, password, "default",
                                     "infinispan", *parsed.saslMechanism);
         }
         else
         {
            cache->setAuthentication(*parsed.username, password);
         }
      }

      return cache;
   }

   RemoteCache::~RemoteCache()
   {
      if (isConnected())
      {
         disconnect();
      }
   }

   void RemoteCache::connect()
   {
      connection_->setProtocolVersion(protocolVersion_);
      connection_->setClientIntelligence(clientIntelligence_);
      connection_->setAuthentication(auth_);
      connection_->connect();
   }

   void RemoteCache::disconnect()
   {
      connection_->close();
   }

   bool RemoteCache::isConnected() const
   {
      return connection_ && connection_->isConnected();
   }

   void RemoteCache::handleTopologyUpdate(const TopologyInfo &topo, const std::string & /*cacheName*/)
   {
      // Runs on a connection's read-loop thread. Guard the routing/pool state
      // against user/retry threads in selectServerForKey (slice 4). cleanup below
      // runs with the lock held (it must not re-lock).
      std::lock_guard<std::mutex> lock(stateMutex_);

      // Update topology
      topology_ = topo;

      // If hash topology is present, update consistent hash
      if (clientIntelligence_ == ClientIntelligence::HASH_DISTRIBUTION_AWARE)
      {
         consistentHash_.updateFromTopology(topo);
      }

      // Clean up connections to servers no longer in topology
      cleanupStaleConnections();
   }

   std::future<void> RemoteCache::ping()
   {
      return pingImpl(RetryContext{});
   }

   std::future<void> RemoteCache::pingImpl(const RetryContext &ctx)
   {
      // Build request body (empty for PING)
      ByteArray requestBody;

      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status != 0x00)
         {
            throw HotRodClientException("PING failed with status: " + std::to_string(status),
                                        FailurePhase::ServerError, status);
         }
         // PING response body (Protocol 3.0+):
         // - key_type (media_type)
         // - value_type (media_type)
         // - server_version (u1)
         // - op_count (vint)
         // - supported_opcodes (u2 array)
         //
         // We need to consume the ENTIRE response body to clear the socket buffer!

         // Read key_type media_type (at minimum: type_indicator=1 byte)
         ByteArray keyType = conn->receive(1);
         uint8_t keyTypeIndicator = keyType[0];

         // If not NONE (0), read param_count and params
         if (keyTypeIndicator != 0)
         {
            // Read predefined_id or custom_string based on indicator
            if (keyTypeIndicator == 1)
            { // predefined
               // Read vint predefined_id
               while (true)
               {
                  ByteArray b = conn->receive(1);
                  if ((b[0] & 0x80) == 0)
                     break;
               }
            }
            else if (keyTypeIndicator == 2)
            { // custom
               // Read lp_string (vint length + bytes)
               VInt len = 0;
               while (true)
               {
                  ByteArray b = conn->receive(1);
                  if ((b[0] & 0x80) == 0)
                  {
                     len = b[0]; // simplified
                     break;
                  }
               }
               if (len > 0)
                  conn->receive(len);
            }
            // Read param_count (vint)
            VInt paramCount = 0;
            while (true)
            {
               ByteArray b = conn->receive(1);
               if ((b[0] & 0x80) == 0)
               {
                  paramCount = b[0]; // simplified
                  break;
               }
            }
            (void)paramCount; // TODO: Read params if paramCount > 0
         }

         // Read value_type media_type (same structure as key_type)
         ByteArray valType = conn->receive(1);
         uint8_t valTypeIndicator = valType[0];
         if (valTypeIndicator != 0)
         {
            if (valTypeIndicator == 1)
            {
               while (true)
               {
                  ByteArray b = conn->receive(1);
                  if ((b[0] & 0x80) == 0)
                     break;
               }
            }
            else if (valTypeIndicator == 2)
            {
               VInt len = 0;
               while (true)
               {
                  ByteArray b = conn->receive(1);
                  if ((b[0] & 0x80) == 0)
                  {
                     len = b[0];
                     break;
                  }
               }
               if (len > 0)
                  conn->receive(len);
            }
            VInt paramCount = 0;
            while (true)
            {
               ByteArray b = conn->receive(1);
               if ((b[0] & 0x80) == 0)
               {
                  paramCount = b[0];
                  break;
               }
            }
            (void)paramCount; // TODO: Read params if paramCount > 0
         }

         // Read server_version (1 byte)
         conn->receive(1);

         // Read op_count (vint)
         VInt opCount = 0;
         while (true)
         {
            ByteArray b = conn->receive(1);
            opCount = (opCount << 7) | (b[0] & 0x7F);
            if ((b[0] & 0x80) == 0)
               break;
         }

         // Read supported_opcodes array (opCount * 2 bytes each = u2)
         if (opCount > 0)
         {
            conn->receive(opCount * 2);
         }

         return {}; // PING doesn't return data
      };

      // PING is keyless: select any reachable server (auto before-send failover
      // across the whole cluster, minus ctx.excludeNodes), recording tried nodes.
      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectAnyServer(ctx, &tried);

      auto responseFuture = conn->execute(requestBody, 0x17, 0x18, bodyParser, cacheName_);

      // Transform Response → void. On failure enrich the exception with the union
      // of excluded + tried nodes so a caller can feed it back via excluding(e).
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable
                        {
                           Response resp = responseFuture.get();
                           if (resp.error)
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           // Success - return void
                        });
   }

   std::future<std::optional<ByteArray>> RemoteCache::get(const ByteArray &key)
   {
      return getImpl(key, RetryContext{});
   }

   std::future<std::optional<ByteArray>> RemoteCache::getImpl(const ByteArray &key, const RetryContext &ctx)
   {
      // Build request body (just the key)
      ByteArray requestBody;
      Codec::writeByteArray(requestBody, key);

      // Define body parser (runs in read thread)
      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status == 0x01 || status == 0x02)
         {
            // Key not found
            return {};
         }
         if (status != 0x00)
         {
            throw HotRodClientException("GET failed with status: " + std::to_string(status),
                                        FailurePhase::ServerError, status);
         }

         // Read value (lp_bytes: vInt length + bytes)
         return conn->receiveByteArray();
      };

      // Select connection (hash-aware routing or default), recording tried nodes
      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);

      // Execute and get Response future
      auto responseFuture = conn->execute(
          requestBody,
          0x03, // GET_REQUEST
          0x04, // GET_RESPONSE
          bodyParser,
          cacheName_);

      // Transform Response → std::optional<ByteArray>
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> std::optional<ByteArray>
                        {
                           Response resp = responseFuture.get();

                           if (resp.error)
                           {
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           }

                           if (resp.status == 0x00 && resp.body.has_value())
                           {
                              return std::any_cast<ByteArray>(resp.body);
                           }

                           return std::nullopt;
                        });
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::getWithMetadata(const ByteArray &key)
   {
      return getWithMetadataImpl(key, RetryContext{});
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::getWithMetadataImpl(const ByteArray &key, const RetryContext &ctx)
   {
      // Build request body (just the key) - identical to GET
      ByteArray requestBody;
      Codec::writeByteArray(requestBody, key);

      // Define body parser (runs in read thread)
      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status == 0x01 || status == 0x02)
         {
            // Key not found
            return {};
         }
         if (status != 0x00)
         {
            throw HotRodClientException("GET_WITH_METADATA failed with status: " + std::to_string(status),
                                        FailurePhase::ServerError, status);
         }

         // On success the body is: entry_metadata (flag + optional expiration + version)
         // followed by the value as lp_bytes.
         EntryWithMetadata entry;
         entry.metadata = conn->receiveMetadata();
         entry.value = conn->receiveByteArray();
         return entry;
      };

      // Select connection (hash-aware routing or default), recording tried nodes
      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);

      // Execute and get Response future
      auto responseFuture = conn->execute(
          requestBody,
          0x1B, // GET_WITH_METADATA_REQUEST
          0x1C, // GET_WITH_METADATA_RESPONSE
          bodyParser,
          cacheName_);

      // Transform Response → std::optional<EntryWithMetadata>
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> std::optional<EntryWithMetadata>
                        {
                           Response resp = responseFuture.get();

                           if (resp.error)
                           {
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           }

                           if (resp.status == 0x00 && resp.body.has_value())
                           {
                              return std::any_cast<EntryWithMetadata>(resp.body);
                           }

                           return std::nullopt;
                        });
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::put(const ByteArray &key, const ByteArray &value,
                                                          uint64_t lifespan, uint64_t maxIdle, bool previousValue)
   {
      return putImpl(key, value, lifespan, maxIdle, previousValue, RetryContext{});
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::putImpl(const ByteArray &key, const ByteArray &value,
                                                          uint64_t lifespan, uint64_t maxIdle, bool previousValue,
                                                          const RetryContext &ctx)
   {
      // Build PUT request body
      // Encode request header + key + expiration + value
      ByteArray requestBody;
      // Write key as lp_bytes (vInt length + bytes)
      Codec::writeByteArray(requestBody, key);

      // Write expiration parameters (time_units byte + optional lifespan/maxIdle)
      // Time units encoding (per Protocol 3.0+):
      // - High nibble (bits 4-7): lifespan time unit
      // - Low nibble (bits 0-3): maxIdle time unit
      // - 0x00 = SECONDS, 0x07 = DEFAULT (infinite/server default)
      // - If unit < 0x07, the duration (vLong) follows

      uint8_t timeUnits = 0;
      if (lifespan == 0)
      {
         timeUnits |= (0x07 << 4); // DEFAULT (infinite)
      }
      else
      {
         timeUnits |= (0x00 << 4); // SECONDS
      }
      if (maxIdle == 0)
      {
         timeUnits |= 0x07; // DEFAULT (infinite)
      }
      else
      {
         timeUnits |= 0x00; // SECONDS
      }
      requestBody.push_back(timeUnits);

      // Write lifespan if not default
      if (lifespan > 0)
      {
         Codec::writeVLong(requestBody, lifespan);
      }

      // Write maxIdle if not default
      if (maxIdle > 0)
      {
         Codec::writeVLong(requestBody, maxIdle);
      }

      // Write value as lp_bytes (vInt length + bytes)
      Codec::writeByteArray(requestBody, value);

      // Define body parser (runs in read thread)
      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t protocolVersion, int32_t /*flags*/) -> std::any
      {
         if (status == 0x03)
         {
            EntryWithMetadata entry;
            // SUCCESS_WITH_PREVIOUS - Protocol 4.0+ returns metadata + value
            if (protocolVersion >= Protocol::VERSION_40)
            {
               entry.metadata = conn->receiveMetadata();
            }  // Skip metadata for now
            entry.value = conn->receiveByteArray();  // Return just the value
            return entry;
         }
         // Status 0x00 (success, no previous) or other - no body
         return {};
      };
      // Execute
      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);
      auto responseFuture = conn->execute(requestBody, 0x01, 0x02, bodyParser, cacheName_, (previousValue ? 1 : 0));
      // Transform Response → std::optional<ByteArray>
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> std::optional<EntryWithMetadata>
                        {
                           Response resp = responseFuture.get();
                           if (resp.error)
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           if (resp.status != 0x00 && resp.status != 0x03)
                              throw HotRodClientException("PUT failed with status: " + std::to_string(resp.status),
                                                         FailurePhase::ServerError, resp.status,
                                                         unionNodes(excluded, tried));
                           if (resp.status == 0x03 && resp.body.has_value()) {
                              return std::any_cast<EntryWithMetadata>(resp.body);
                           }
                           return std::nullopt;
                        });
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::remove(const ByteArray &key, bool previousValue)
   {
      return removeImpl(key, previousValue, RetryContext{});
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::removeImpl(const ByteArray &key, bool previousValue,
                                                          const RetryContext &ctx)
   {
      // Build REMOVE request body
      ByteArray requestBody;

      // Write key as lp_bytes (vInt length + bytes)
      Codec::writeByteArray(requestBody, key);

      // Status codes:
      // 0x00 = SUCCESS (key existed, no previous value in response)
      // 0x01 = NOT_EXECUTED (operation not executed)
      // 0x02 = KEY_DOES_NOT_EXIST (key didn't exist)
      // 0x03 = SUCCESS_WITH_PREVIOUS (key existed, previous value in response)
      // 0x04 = NOT_EXECUTED_WITH_PREVIOUS

      // Define body parser (runs in read thread)
      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status == 0x03 || status == 0x04)
         {
            // SUCCESS_WITH_PREVIOUS - Protocol 4.0+ returns metadata + value
            EntryWithMetadata entry;
            entry.metadata = conn->receiveMetadata();
            entry.value = conn->receiveByteArray();
            return entry;
         }
         if (status == 0x00 || status == 0x01 || status == 0x02)
         {
            return {};
         }
         throw HotRodClientException("REMOVE failed with status: " + std::to_string(status),
                                     FailurePhase::ServerError, status);
      };

      // Execute
      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);
      auto responseFuture = conn->execute(requestBody, 0x0B, 0x0C, bodyParser, cacheName_, (previousValue ? 1 : 0));

      // Transform Response → std::optional<ByteArray>
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> std::optional<EntryWithMetadata>
                        {
                           Response resp = responseFuture.get();
                           if (resp.error)
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           if (resp.status == 0x03 && resp.body.has_value())
                              return std::any_cast<EntryWithMetadata>(resp.body);
                           return std::nullopt;
                        });
   }

   std::future<bool> RemoteCache::removeWithVersion(const ByteArray &key, int64_t version)
   {
      return removeWithVersionImpl(key, version, RetryContext{});
   }

   std::future<bool> RemoteCache::removeWithVersionImpl(const ByteArray &key, int64_t version,
                                                        const RetryContext &ctx)
   {
      // Build REMOVE_WITH_VERSION request body: key (lp_bytes) + version (8-byte
      // big-endian s8). Mirrors Java RemoveIfUnmodifiedOperation which appends
      // buf.writeLong(version) after the key.
      ByteArray requestBody;
      Codec::writeByteArray(requestBody, key);
      Codec::writeLong(requestBody, version);

      // Status codes (see Java VersionedOperationResponse.RspCode):
      // 0x00 = SUCCESS (removed)                         -> true
      // 0x01 = NOT_EXECUTED (version mismatch/modified)  -> false
      // 0x02 = KEY_DOES_NOT_EXIST                        -> false
      // 0x03 = SUCCESS_WITH_PREVIOUS                     -> true  (body present)
      // 0x04 = NOT_EXECUTED_WITH_PREVIOUS                -> false (body present)
      // The *_WITH_PREVIOUS variants only occur when FORCE_RETURN_VALUE is set
      // (not yet exposed). We drain their body defensively so the shared read
      // loop stays byte-aligned if that flag is added later.
      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status == 0x03 || status == 0x04)
         {
            EntryWithMetadata entry;
            entry.metadata = conn->receiveMetadata();
            entry.value = conn->receiveByteArray();
            return entry;
         }
         if (status == 0x00 || status == 0x01 || status == 0x02)
         {
            return {};
         }
         throw HotRodClientException("REMOVE_WITH_VERSION failed with status: " + std::to_string(status),
                                     FailurePhase::ServerError, status);
      };

      // Execute
      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);
      auto responseFuture = conn->execute(requestBody, 0x0D, 0x0E, bodyParser, cacheName_);

      // Transform Response → bool (removed?). Success is 0x00 or 0x03, matching
      // Java's HotRodConstants.isSuccess / RspCode.isUpdated().
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> bool
                        {
                           Response resp = responseFuture.get();
                           if (resp.error)
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           return resp.status == 0x00 || resp.status == 0x03;
                        });
   }

   std::future<bool> RemoteCache::replaceWithVersion(const ByteArray &key, const ByteArray &value,
                                                     int64_t version, uint64_t lifespan, uint64_t maxIdle)
   {
      return replaceWithVersionImpl(key, value, version, lifespan, maxIdle, RetryContext{});
   }

   std::future<bool> RemoteCache::replaceWithVersionImpl(const ByteArray &key, const ByteArray &value,
                                                     int64_t version, uint64_t lifespan, uint64_t maxIdle,
                                                     const RetryContext &ctx)
   {
      // Build REPLACE_WITH_VERSION request body:
      //   key (lp_bytes) + time_units (u1) + [lifespan vLong] + [maxIdle vLong]
      //   + version (8-byte big-endian s8) + value (lp_bytes)
      // Mirrors Java ReplaceIfUnmodifiedOperation, which writes the *same*
      // expiration params as PUT, then buf.writeLong(version), then the value.
      //
      // NOTE: The expiration byte packs the lifespan time unit in the HIGH
      // nibble and maxIdle in the LOW nibble (Java TimeUnitParam.encodeTimeUnits:
      // (encodedLifespan << 4) | encodedMaxIdle), identical to put(). The
      // hotrod40.ksy `replace_if_unmodified_request` has these nibbles flipped;
      // that is a schema bug — Java (authoritative) uses one shared encoder for
      // both PUT and replaceWithVersion.
      ByteArray requestBody;
      Codec::writeByteArray(requestBody, key);

      uint8_t timeUnits = 0;
      timeUnits |= (lifespan == 0 ? 0x07 : 0x00) << 4;  // high nibble: lifespan
      timeUnits |= (maxIdle == 0 ? 0x07 : 0x00);        // low nibble: maxIdle
      requestBody.push_back(timeUnits);
      if (lifespan > 0)
         Codec::writeVLong(requestBody, lifespan);
      if (maxIdle > 0)
         Codec::writeVLong(requestBody, maxIdle);

      Codec::writeLong(requestBody, version);
      Codec::writeByteArray(requestBody, value);

      // Status codes (see Java VersionedOperationResponse.RspCode):
      // 0x00 = SUCCESS (replaced)                        -> true
      // 0x01 = NOT_EXECUTED (version mismatch/modified)  -> false
      // 0x02 = KEY_DOES_NOT_EXIST                        -> false
      // 0x03 = SUCCESS_WITH_PREVIOUS                     -> true  (body present)
      // 0x04 = NOT_EXECUTED_WITH_PREVIOUS                -> false (body present)
      // The *_WITH_PREVIOUS variants only occur when FORCE_RETURN_VALUE is set
      // (not yet exposed). We drain their body defensively so the shared read
      // loop stays byte-aligned if that flag is added later.
      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status == 0x03 || status == 0x04)
         {
            EntryWithMetadata entry;
            entry.metadata = conn->receiveMetadata();
            entry.value = conn->receiveByteArray();
            return entry;
         }
         if (status == 0x00 || status == 0x01 || status == 0x02)
         {
            return {};
         }
         throw HotRodClientException("REPLACE_WITH_VERSION failed with status: " + std::to_string(status),
                                     FailurePhase::ServerError, status);
      };

      // Execute
      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);
      auto responseFuture = conn->execute(requestBody, 0x09, 0x0A, bodyParser, cacheName_);

      // Transform Response → bool (replaced?). Success is 0x00 or 0x03, matching
      // Java's HotRodConstants.isSuccess / RspCode.isUpdated().
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> bool
                        {
                           Response resp = responseFuture.get();
                           if (resp.error)
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           return resp.status == 0x00 || resp.status == 0x03;
                        });
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::putIfAbsent(const ByteArray &key, const ByteArray &value,
                                                                          uint64_t lifespan, uint64_t maxIdle, bool previousValue)
   {
      return putIfAbsentImpl(key, value, lifespan, maxIdle, previousValue, RetryContext{});
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::putIfAbsentImpl(const ByteArray &key, const ByteArray &value,
                                                                          uint64_t lifespan, uint64_t maxIdle, bool previousValue,
                                                                          const RetryContext &ctx)
   {
      // Build PUT_IF_ABSENT request body: identical layout to PUT
      //   key (lp_bytes) + time_units (u1) + [lifespan vLong] + [maxIdle vLong]
      //   + value (lp_bytes)
      // Mirrors Java PutIfAbsentOperation (extends AbstractKeyValueOperation, so
      // the same key/expiration/value encoder PUT uses).
      ByteArray requestBody;
      Codec::writeByteArray(requestBody, key);

      uint8_t timeUnits = 0;
      timeUnits |= (lifespan == 0 ? 0x07 : 0x00) << 4;  // high nibble: lifespan
      timeUnits |= (maxIdle == 0 ? 0x07 : 0x00);        // low nibble: maxIdle
      requestBody.push_back(timeUnits);
      if (lifespan > 0)
         Codec::writeVLong(requestBody, lifespan);
      if (maxIdle > 0)
         Codec::writeVLong(requestBody, maxIdle);

      Codec::writeByteArray(requestBody, value);

      // Status codes:
      // 0x00 = SUCCESS (stored, key was absent)          -> nullopt
      // 0x01 = NOT_EXECUTED (key already existed)         -> nullopt (no body)
      // 0x04 = NOT_EXECUTED_WITH_PREVIOUS                 -> existing entry (body)
      // The *_WITH_PREVIOUS variant only occurs when FORCE_RETURN_VALUE is set
      // (via previousValue). We drain 0x03/0x04 bodies defensively so the shared
      // read loop stays byte-aligned.
      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status == 0x03 || status == 0x04)
         {
            EntryWithMetadata entry;
            entry.metadata = conn->receiveMetadata();
            entry.value = conn->receiveByteArray();
            return entry;
         }
         if (status == 0x00 || status == 0x01 || status == 0x02)
         {
            return {};
         }
         throw HotRodClientException("PUT_IF_ABSENT failed with status: " + std::to_string(status),
                                     FailurePhase::ServerError, status);
      };

      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);
      auto responseFuture = conn->execute(requestBody, 0x05, 0x06, bodyParser, cacheName_, (previousValue ? 1 : 0));

      // Transform Response → optional<EntryWithMetadata>: the existing entry that
      // blocked storage, if returned; nullopt when stored (or no body present).
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> std::optional<EntryWithMetadata>
                        {
                           Response resp = responseFuture.get();
                           if (resp.error)
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           if ((resp.status == 0x03 || resp.status == 0x04) && resp.body.has_value())
                              return std::any_cast<EntryWithMetadata>(resp.body);
                           return std::nullopt;
                        });
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::replace(const ByteArray &key, const ByteArray &value,
                                                                      uint64_t lifespan, uint64_t maxIdle, bool previousValue)
   {
      return replaceImpl(key, value, lifespan, maxIdle, previousValue, RetryContext{});
   }

   std::future<std::optional<EntryWithMetadata>> RemoteCache::replaceImpl(const ByteArray &key, const ByteArray &value,
                                                                      uint64_t lifespan, uint64_t maxIdle, bool previousValue,
                                                                      const RetryContext &ctx)
   {
      // Build REPLACE request body: identical layout to PUT
      //   key (lp_bytes) + time_units (u1) + [lifespan vLong] + [maxIdle vLong]
      //   + value (lp_bytes)
      // Mirrors Java ReplaceOperation (extends AbstractKeyValueOperation).
      ByteArray requestBody;
      Codec::writeByteArray(requestBody, key);

      uint8_t timeUnits = 0;
      timeUnits |= (lifespan == 0 ? 0x07 : 0x00) << 4;  // high nibble: lifespan
      timeUnits |= (maxIdle == 0 ? 0x07 : 0x00);        // low nibble: maxIdle
      requestBody.push_back(timeUnits);
      if (lifespan > 0)
         Codec::writeVLong(requestBody, lifespan);
      if (maxIdle > 0)
         Codec::writeVLong(requestBody, maxIdle);

      Codec::writeByteArray(requestBody, value);

      // Status codes:
      // 0x00 = SUCCESS (replaced)                         -> nullopt
      // 0x01 = NOT_EXECUTED (key did not exist)           -> nullopt (no body)
      // 0x03 = SUCCESS_WITH_PREVIOUS                      -> previous entry (body)
      // The *_WITH_PREVIOUS variant only occurs when FORCE_RETURN_VALUE is set
      // (via previousValue). We drain 0x03/0x04 bodies defensively.
      auto bodyParser = [](uint8_t status, Connection *conn, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status == 0x03 || status == 0x04)
         {
            EntryWithMetadata entry;
            entry.metadata = conn->receiveMetadata();
            entry.value = conn->receiveByteArray();
            return entry;
         }
         if (status == 0x00 || status == 0x01 || status == 0x02)
         {
            return {};
         }
         throw HotRodClientException("REPLACE failed with status: " + std::to_string(status),
                                     FailurePhase::ServerError, status);
      };

      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);
      auto responseFuture = conn->execute(requestBody, 0x07, 0x08, bodyParser, cacheName_, (previousValue ? 1 : 0));

      // Transform Response → optional<EntryWithMetadata>: the replaced (previous)
      // entry, if returned; nullopt when nothing was replaced (or no body).
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> std::optional<EntryWithMetadata>
                        {
                           Response resp = responseFuture.get();
                           if (resp.error)
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           if ((resp.status == 0x03 || resp.status == 0x04) && resp.body.has_value())
                              return std::any_cast<EntryWithMetadata>(resp.body);
                           return std::nullopt;
                        });
   }

   std::future<bool> RemoteCache::containsKey(const ByteArray &key)
   {
      return containsKeyImpl(key, RetryContext{});
   }

   std::future<bool> RemoteCache::containsKeyImpl(const ByteArray &key, const RetryContext &ctx)
   {
      // Build CONTAINS_KEY request body (just the key), like GET.
      ByteArray requestBody;
      Codec::writeByteArray(requestBody, key);

      // The response carries no body — only the status. Java ContainsKeyOperation
      // returns !isNotExist(status) && isSuccess(status).
      // 0x00 = SUCCESS (key exists)     -> true
      // 0x01 = NOT_EXECUTED             -> false
      // 0x02 = KEY_DOES_NOT_EXIST       -> false
      auto bodyParser = [](uint8_t status, Connection * /*conn*/, uint8_t /*protocolVersion*/, int32_t /*flags*/) -> std::any
      {
         if (status == 0x00 || status == 0x01 || status == 0x02)
         {
            return {};
         }
         throw HotRodClientException("CONTAINS_KEY failed with status: " + std::to_string(status),
                                     FailurePhase::ServerError, status);
      };

      std::vector<ServerAddress> tried;
      std::shared_ptr<MultiplexedConnection> conn = selectServerForKey(key, ctx, &tried);
      auto responseFuture = conn->execute(requestBody, 0x0F, 0x10, bodyParser, cacheName_);

      // Transform Response → bool (key present?). Success is 0x00.
      return std::async(std::launch::deferred,
                        [conn, responseFuture = std::move(responseFuture),
                         tried = std::move(tried),
                         excluded = ctx.excludeNodes]() mutable -> bool
                        {
                           Response resp = responseFuture.get();
                           if (resp.error)
                              rethrowWithTriedNodes(resp.error, tried, excluded);
                           return resp.status == 0x00;
                        });
   }

   RetryView RemoteCache::excluding(std::vector<ServerAddress> excludeNodes)
   {
      return RetryView(*this, RetryContext{std::move(excludeNodes)});
   }

   RetryView RemoteCache::excluding(const HotRodClientException &e)
   {
      return RetryView(*this, RetryContext{e.triedNodes});
   }

   std::shared_ptr<MultiplexedConnection> RemoteCache::selectServerForKey(const ByteArray &key,
                                                          const RetryContext &ctx,
                                                          std::vector<ServerAddress> *triedOut)
   {
      // Guard the routing/pool state against concurrent topology callbacks on the
      // read-loop thread (handleTopologyUpdate). Held across connection
      // acquisition (getConnectionForServer, which may connect()); the caller runs
      // the actual request on the returned connection *after* the lock is
      // released. The shared_ptr keeps that connection alive even if a topology
      // update evicts it from the pool meanwhile (slice 4).
      std::lock_guard<std::mutex> lock(stateMutex_);

      // If hash-aware routing is enabled and hash topology is available
      if (clientIntelligence_ == ClientIntelligence::HASH_DISTRIBUTION_AWARE &&
          consistentHash_.hasHashTopology())
      {

         int segment = consistentHash_.getSegment(key);

         // Get all owners (primary + backups) for this key
         auto owners = consistentHash_.getOwners(key, topology_);

         if (!owners.empty())
         {
            // Build the ordered candidate list: owners first, then (when
            // proxyToNonOwner_ is set, D5) non-owner proxy fallback, minus any
            // excluded (already-tried) nodes. This routing decision is pure and
            // unit-tested (orderKeyCandidates); only the connection acquisition
            // below does I/O.
            std::vector<ServerAddress> ownerAddrs;
            ownerAddrs.reserve(owners.size());
            for (const auto *owner : owners)
               ownerAddrs.push_back({owner->host, owner->port});

            std::vector<ServerAddress> allAddrs;
            for (const auto &server : topology_.getServers())
               allAddrs.push_back({server.host, server.port});

            std::vector<ServerAddress> candidates =
                orderKeyCandidates(ownerAddrs, allAddrs, ctx.excludeNodes,
                                   proxyToNonOwner_);

            // Try each candidate in order, recording every node we attempt so
            // the caller can accumulate triedNodes across retries.
            std::vector<ServerAddress> tried;
            for (const auto &cand : candidates)
            {
               tried.push_back(cand);
               try
               {
                  // hashId is unused by getConnectionForServer (host:port only).
                  std::shared_ptr<MultiplexedConnection> conn =
                      getConnectionForServer(ServerInfo(cand.host, cand.port, 0));
                  if (triedOut)
                     *triedOut = tried;
                  return conn;
               }
               catch (const std::exception &e)
               {
                  fprintf(stderr, "[WARN] Failed to connect to %s:%u: %s\n",
                          cand.host.c_str(), cand.port, e.what());
                  // Continue to next candidate
               }
            }

            // No candidate was reachable. Nothing was sent (BeforeSend). An
            // owner is "exhausted" if it was tried on this attempt or excluded
            // (tried on a prior attempt); ownersExhausted holds iff that covers
            // every owner — i.e. further retry can only reach a proxy.
            auto attempted = [&](const ServerAddress &a) {
               return std::find(tried.begin(), tried.end(), a) != tried.end() ||
                      std::find(ctx.excludeNodes.begin(), ctx.excludeNodes.end(), a) !=
                          ctx.excludeNodes.end();
            };
            bool ownersExhausted = true;
            for (const auto &o : ownerAddrs)
               if (!attempted(o))
               {
                  ownersExhausted = false;
                  break;
               }

            fprintf(stderr, "[WARN] No servers available for segment %d\n", segment);
            if (triedOut)
               *triedOut = tried;
            throw HotRodClientException("No servers available in topology",
                                        FailurePhase::BeforeSend, std::nullopt,
                                        unionNodes(ctx.excludeNodes, tried),
                                        ownersExhausted);
         }
         else
         {
            fprintf(stderr, "[WARN] No owners found for key (segment %d), falling back to default connection\n", segment);
         }
      }

      // Fallback: use default connection
      return connection_;
   }

   std::shared_ptr<MultiplexedConnection> RemoteCache::selectAnyServer(const RetryContext &ctx,
                                                          std::vector<ServerAddress> *triedOut)
   {
      // Same lock/lifetime discipline as selectServerForKey (slice 4): guard the
      // routing/pool state and keep the returned connection alive past the lock.
      std::lock_guard<std::mutex> lock(stateMutex_);

      std::vector<ServerAddress> allAddrs;
      for (const auto &server : topology_.getServers())
         allAddrs.push_back({server.host, server.port});

      // Early ping: no topology received yet → nothing to route over, so use the
      // seed connection so the very first ping still works.
      if (allAddrs.empty())
      {
         if (triedOut)
            triedOut->clear();
         return connection_;
      }

      // Keyless candidates: every server minus the exclusion set, in topology
      // order. Reuses the pure, unit-tested orderKeyCandidates() with an empty
      // owner list (no key → no owners); the proxy flag is irrelevant here since
      // there are no owners, so all servers come from the allServers pass.
      std::vector<ServerAddress> candidates =
          orderKeyCandidates({}, allAddrs, ctx.excludeNodes, true);

      std::vector<ServerAddress> tried;
      for (const auto &cand : candidates)
      {
         tried.push_back(cand);
         try
         {
            std::shared_ptr<MultiplexedConnection> conn =
                getConnectionForServer(ServerInfo(cand.host, cand.port, 0));
            if (triedOut)
               *triedOut = tried;
            return conn;
         }
         catch (const std::exception &e)
         {
            fprintf(stderr, "[WARN] Failed to connect to %s:%u: %s\n",
                    cand.host.c_str(), cand.port, e.what());
            // Continue to next candidate
         }
      }

      // Topology existed but every server was excluded or unreachable. Nothing was
      // sent (BeforeSend). ownersExhausted is always false for a keyless op.
      fprintf(stderr, "[WARN] No servers available for keyless operation\n");
      if (triedOut)
         *triedOut = tried;
      throw HotRodClientException("No servers available in topology",
                                  FailurePhase::BeforeSend, std::nullopt,
                                  unionNodes(ctx.excludeNodes, tried),
                                  /*ownersExhausted=*/false);
   }

   std::shared_ptr<MultiplexedConnection> RemoteCache::getConnectionForServer(const ServerInfo &server)
   {
      // Create connection pool key
      std::string poolKey = server.host + ":" + std::to_string(server.port);

      // Check if connection already exists in pool
      auto it = connectionPool_.find(poolKey);
      if (it != connectionPool_.end())
      {
         // Connection exists, check if it's still connected
         if (it->second->isConnected())
         {
            return it->second;
         }
         else
         {
            // Connection is dead, remove it and create a new one
            connectionPool_.erase(it);
            // Fall through to create new connection
         }
      }

      // Connection doesn't exist (or was removed), create new one

      auto topologyCallback = [this](const TopologyInfo &topo, const std::string &cacheName)
      {
         this->handleTopologyUpdate(topo, cacheName);
      };

      auto newConnection = std::make_shared<MultiplexedConnection>(server.host, server.port, topologyCallback);
      newConnection->setProtocolVersion(protocolVersion_);
      newConnection->setClientIntelligence(clientIntelligence_);
      newConnection->setAuthentication(auth_);

      // connect() can throw if server is unreachable - let it propagate to caller
      newConnection->connect();

      connectionPool_[poolKey] = newConnection;

      return newConnection;
   }

   void RemoteCache::cleanupStaleConnections()
   {
      // Build set of current servers
      std::set<std::string> currentServers;
      for (const auto &server : topology_.getServers())
      {
         std::string key = server.host + ":" + std::to_string(server.port);
         currentServers.insert(key);
      }

      // Remove connections not in current topology
      auto it = connectionPool_.begin();
      while (it != connectionPool_.end())
      {
         if (currentServers.find(it->first) == currentServers.end())
         {
            it = connectionPool_.erase(it);
         }
         else
         {
            ++it;
         }
      }
   }

} // namespace hotrod
