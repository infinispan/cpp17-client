#pragma once

#include "Types.h"
#include "TopologyInfo.h"
#include "HeaderCodec.h"
#include "Authentication.h"
#include "TlsConfig.h"
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <future>
#include <atomic>
#include <memory>
#include <functional>
#include <optional>
#include <any>

namespace hotrod {

// Forward declaration
class Connection;

/**
 * Response from a single request.
 * Returned by execute() via future.
 */
struct Response {
    uint8_t status;                              // Response status (0x00 = success, etc.)
    std::any body;                              // Response body (operation-specific)
    std::exception_ptr error;                    // Set on communication errors
    std::optional<TopologyInfo> topologyUpdate;  // Topology update if present
};

/**
 * Pending request entry.
 * Stored in pending_ map while waiting for response.
 */
struct PendingRequest {
    std::promise<Response> promise;              // Promise to fulfill when response arrives
    std::function<std::any(uint8_t status, Connection*, uint8_t protocolVersion, int32_t flags)> bodyParser;  // How to read response body
    uint8_t expectedOpcode;                      // Expected response opcode
    std::string cacheName;                       // Cache name for topology updates
    int32_t flags;                               // Request flags (for body parser)
};

/**
 * Multiplexed connection supporting concurrent operations.
 *
 * Key features:
 * - One TCP socket shared by multiple concurrent operations
 * - Message ID matching (request → response)
 * - Dedicated read thread for async I/O
 * - Thread-safe request/response handling
 *
 * Architecture:
 * - Write mutex: Serializes request writes (multiple threads can call execute())
 * - Read thread: Continuously reads responses and dispatches to waiters
 * - Pending map: messageId → PendingRequest (tracks in-flight operations)
 *
 * Reference:
 * - infinispan-go-client/internal/connection/conn.go
 * - docs/MULTIPLEXING_DESIGN.md
 * - docs/ASYNC_API_FINAL.md
 */
class MultiplexedConnection {
public:
    using TopologyCallback = std::function<void(const TopologyInfo&, const std::string& cacheName)>;

    /**
     * Create multiplexed connection.
     *
     * @param host Server hostname or IP
     * @param port Server port (default 11222)
     * @param onTopologyUpdate Callback for topology updates (optional)
     */
    MultiplexedConnection(const std::string& host,
                          uint16_t port,
                          TopologyCallback onTopologyUpdate = nullptr);

    ~MultiplexedConnection();

    // Non-copyable, non-movable
    MultiplexedConnection(const MultiplexedConnection&) = delete;
    MultiplexedConnection& operator=(const MultiplexedConnection&) = delete;
    MultiplexedConnection(MultiplexedConnection&&) = delete;
    MultiplexedConnection& operator=(MultiplexedConnection&&) = delete;

    /**
     * Connect to server and start read thread.
     */
    void connect();

    /**
     * Close connection and stop read thread.
     */
    void close();

    /**
     * Check if connected.
     */
    bool isConnected() const;

    /**
     * Execute operation and return future with response.
     *
     * Thread-safe: Multiple threads can call this concurrently.
     *
     * @param requestBody Request body (no header - execute() builds it)
     * @param requestOpcode Request opcode (e.g., 0x03 for GET_REQUEST)
     * @param expectedResponseOpcode Expected response opcode (e.g., 0x04 for GET_RESPONSE)
     * @param bodyParser Function to parse response body (runs in read thread)
     * @param cacheName Cache name for request header
     * @param flags Request flags (vint, default 0)
     * @return Future with Response (get() blocks until response received)
     *
     * Example:
     *   auto bodyParser = [](uint8_t status, Connection* conn) -> ByteArray {
     *       if (status == 0x00) return conn->receiveByteArray();
     *       return {};
     *   };
     *   auto future = conn->execute(requestBody, 0x03, 0x04, bodyParser, "myCache");
     *   Response resp = future.get();  // Blocks until response
     */
    std::future<Response> execute(
        const ByteArray& requestBody,
        uint8_t requestOpcode,
        uint8_t expectedResponseOpcode,
        std::function<std::any(uint8_t status, Connection*, uint8_t protocolVersion, int32_t flags)> bodyParser,
        const std::string& cacheName = "",
        int32_t flags = 0
    );

    /**
     * Set protocol version.
     * Must be called before connect().
     */
    void setProtocolVersion(uint8_t version);

    /**
     * Set client intelligence level.
     * Must be called before connect().
     */
    void setClientIntelligence(ClientIntelligence intelligence);

    /**
     * Set SASL authentication configuration.
     * Must be called before connect(). When auth.enabled is true, connect() runs
     * the SASL handshake on the socket before starting the read loop.
     */
    void setAuthentication(const Authentication& auth);

    /**
     * Set TLS transport configuration.
     * Must be called before connect(). When tls.enabled is true, connect() wraps
     * the socket in a TLS session before the SASL handshake and read loop.
     */
    void setTls(const TlsConfig& tls);

    /**
     * Get current topology ID.
     */
    int32_t getTopologyId() const;

private:
    std::string host_;
    uint16_t port_;

    // Underlying TCP connection
    std::unique_ptr<Connection> connection_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopReadLoop_{false};

    // Message ID generation (atomic for thread safety)
    std::atomic<uint64_t> nextMessageId_{0};

    // Pending requests: messageId → PendingRequest
    std::mutex pendingMutex_;
    std::map<uint64_t, std::unique_ptr<PendingRequest>> pending_;

    // Write mutex (only one thread writes at a time)
    std::mutex writeMutex_;

    // Read thread
    std::unique_ptr<std::thread> readThread_;

    // Topology callback
    TopologyCallback onTopologyUpdate_;

    // Protocol configuration
    uint8_t protocolVersion_{Protocol::VERSION_41};  // Default: Protocol 4.1

    // Client intelligence and topology
    ClientIntelligence clientIntelligence_{ClientIntelligence::BASIC};
    std::atomic<int32_t> topologyId_{0};

    // SASL authentication (disabled by default). Applied during connect().
    Authentication auth_;

    // TLS transport (disabled by default). Applied to the socket during connect().
    TlsConfig tls_;

    /**
     * Read loop (runs in dedicated thread).
     * Continuously reads responses and dispatches to waiters.
     */
    void readLoop();

    /**
     * Build complete request (header + body).
     *
     * @param messageId Unique message ID
     * @param opcode Request opcode
     * @param body Request body
     * @param cacheName Cache name
     * @return Complete request bytes (header + body)
     */
    ByteArray buildRequest(uint64_t messageId,
                           uint8_t opcode,
                           const ByteArray& body,
                           const std::string& cacheName,
                           uint32_t flags);

    /**
     * Close all pending requests with error.
     * Called when connection drops.
     */
    void closeAllPending(const std::string& errorMsg);
};

} // namespace hotrod
