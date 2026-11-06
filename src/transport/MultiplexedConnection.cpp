#include "hotrod/MultiplexedConnection.h"
#include "hotrod/Connection.h"
#include "hotrod/HeaderCodec.h"
#include "hotrod/HotRodClientException.h"
#include "hotrod/Codec.h"
#include "hotrod/SaslAuthenticator.h"
#include <stdexcept>
#include <iostream>

namespace hotrod {

MultiplexedConnection::MultiplexedConnection(
    const std::string& host,
    uint16_t port,
    TopologyCallback onTopologyUpdate)
    : host_(host)
    , port_(port)
    , onTopologyUpdate_(std::move(onTopologyUpdate))
{
}

MultiplexedConnection::~MultiplexedConnection() {
    close();
}

void MultiplexedConnection::connect() {
    if (connected_) {
        throw std::runtime_error("Already connected");
    }

    // Create underlying TCP connection. TLS (when enabled) is negotiated inside
    // Connection::connect(), so the SASL handshake below already runs encrypted.
    connection_ = std::make_unique<Connection>(host_, port_);
    connection_->setTls(tls_);
    connection_->connect();

    // SASL handshake (synchronous, before the read thread consumes any bytes).
    // Runs only when authentication is configured; a failure here throws
    // HotRodClientException and leaves the connection cleanly closed.
    if (auth_.enabled) {
        try {
            SaslAuthenticator authenticator(auth_, host_, port_);
            ConnectionSaslTransport transport(connection_.get(), protocolVersion_,
                                              clientIntelligence_, host_, port_);
            authenticator.authenticate(transport);
        } catch (...) {
            connection_->close();
            connection_.reset();
            throw;
        }
    }

    connected_ = true;
    stopReadLoop_ = false;

    // Start read thread
    readThread_ = std::make_unique<std::thread>(&MultiplexedConnection::readLoop, this);
}

void MultiplexedConnection::close() {
    // Signal read loop to stop
    stopReadLoop_ = true;

    // Shutdown socket to interrupt blocking receive() in read thread
    if (connection_) {
        connection_->shutdown();
    }

    // Wait for read thread to finish (ALWAYS join, even if connected_ is already false)
    // The read thread might have set connected_=false itself after an error
    if (readThread_ && readThread_->joinable()) {
        readThread_->join();
    }

    // Now close the connection
    if (connection_) {
        connection_->close();
    }

    connected_ = false;

    // Close all pending requests with error (only if we haven't already)
    // The read loop may have already called closeAllPending() on error
    closeAllPending("Connection closed");
}

bool MultiplexedConnection::isConnected() const {
    return connected_;
}

void MultiplexedConnection::setProtocolVersion(uint8_t version) {
    if (connected_) {
        throw std::runtime_error("Cannot change protocol version after connect()");
    }
    protocolVersion_ = version;
}

void MultiplexedConnection::setClientIntelligence(ClientIntelligence intelligence) {
    if (connected_) {
        throw std::runtime_error("Cannot change client intelligence after connect()");
    }
    clientIntelligence_ = intelligence;
}

void MultiplexedConnection::setAuthentication(const Authentication& auth) {
    if (connected_) {
        throw std::runtime_error("Cannot change authentication after connect()");
    }
    auth_ = auth;
}

void MultiplexedConnection::setTls(const TlsConfig& tls) {
    if (connected_) {
        throw std::runtime_error("Cannot change TLS configuration after connect()");
    }
    tls_ = tls;
}

int32_t MultiplexedConnection::getTopologyId() const {
    return topologyId_.load();
}

std::future<Response> MultiplexedConnection::execute(
    const ByteArray& requestBody,
    uint8_t requestOpcode,
    uint8_t expectedResponseOpcode,
    std::function<std::any(uint8_t status, Connection*, uint8_t protocolVersion, int flags)> bodyParser,
    const std::string& cacheName,
    int32_t flags)
{
    if (!connected_) {
        // Nothing has been sent — always safe to retry (any op).
        throw HotRodClientException("Not connected", FailurePhase::BeforeSend,
                                    std::nullopt, {{host_, port_}});
    }

    // Generate unique message ID
    uint64_t msgId = nextMessageId_++;

    // Create pending entry
    auto pending = std::make_unique<PendingRequest>();
    pending->bodyParser = std::move(bodyParser);
    pending->expectedOpcode = expectedResponseOpcode;
    pending->cacheName = cacheName;
    pending->flags = flags;
    auto future = pending->promise.get_future();

    // Register pending request
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_[msgId] = std::move(pending);
    }

    // Build complete request (header + body)
    ByteArray completeRequest = buildRequest(msgId, requestOpcode, requestBody, cacheName, flags);

    // Send request (protected by write mutex)
    try {
        std::lock_guard<std::mutex> lock(writeMutex_);
        connection_->send(completeRequest);
    } catch (const std::exception& e) {
        // Remove pending on write error
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pending_.erase(msgId);
        }
        // A partial write may already have reached the server — be conservative
        // and treat this as AfterSend (ambiguous), not BeforeSend.
        throw HotRodClientException(std::string("Send failed: ") + e.what(),
                                    FailurePhase::AfterSend, std::nullopt,
                                    {{host_, port_}});
    }

    return future;
}

void MultiplexedConnection::readLoop() {
    while (!stopReadLoop_ && connected_) {
        try {
            // 1. Read response header
            // Magic byte
            ByteArray magicByte = connection_->receive(1);
            if (magicByte[0] != 0xA1) {
                throw std::runtime_error("Invalid magic byte in response: " +
                                       std::to_string(magicByte[0]));
            }

            // Message ID (vLong)
            uint64_t messageId = connection_->receiveVLong();

            // Opcode, status, topology marker
            ByteArray headerTail = connection_->receive(3);
            uint8_t opcode = headerTail[0];
            uint8_t status = headerTail[1];
            uint8_t topologyMarker = headerTail[2];

            // Parse topology update if present
            std::optional<TopologyInfo> topologyUpdate;
            if (topologyMarker != 0 &&
                (clientIntelligence_ == ClientIntelligence::TOPOLOGY_AWARE ||
                 clientIntelligence_ == ClientIntelligence::HASH_DISTRIBUTION_AWARE))
            {
                // Read topology update
                TopologyInfo topo;
                uint8_t hashFunctionVersion = 0;
                VInt numSegments = 0;
                std::vector<std::vector<uint8_t>> segmentOwners;

                topo.parseTopologyInfo(connection_.get(), clientIntelligence_,
                                      hashFunctionVersion, numSegments, segmentOwners);

                topologyId_.store(topo.getTopologyId());
                topologyUpdate = std::move(topo);
            }

            // 1b. Server ERROR response (0x50): the body is a length-prefixed
            // error message. Read it NOW — before the pending lookup — so the
            // stream stays in sync even if the request has no waiter (orphan).
            std::optional<std::string> errorMessage;
            if (opcode == Opcode::ERROR_RESPONSE) {
                errorMessage = connection_->receiveString();
            }

            // 2. Find pending request
            std::unique_ptr<PendingRequest> pending;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                auto it = pending_.find(messageId);
                if (it == pending_.end()) {
                    // Orphan response - log and continue
                    std::cerr << "[WARN] No pending request for messageId=" << messageId << std::endl;
                    continue;
                }
                pending = std::move(it->second);
                pending_.erase(it);
            }

            // Notify topology callback after we have pending request
            if (topologyUpdate && onTopologyUpdate_) {
                onTopologyUpdate_(*topologyUpdate, pending->cacheName);
            }

            // 2b. Deliver server ERROR (0x50) as a typed exception, skipping the
            // opcode/body checks below (the ERROR opcode never matches expected).
            if (errorMessage) {
                Response resp;
                resp.status = status;
                resp.error = std::make_exception_ptr(HotRodClientException(
                    "Server error (status " + std::to_string(status) + "): " + *errorMessage,
                    FailurePhase::ServerError, status, {{host_, port_}}));
                resp.topologyUpdate = std::move(topologyUpdate);
                pending->promise.set_value(std::move(resp));
                continue;
            }

            // 3. Validate opcode
            if (opcode != pending->expectedOpcode) {
                Response resp;
                // Protocol-level mismatch: we got a reply, but the wrong one.
                // Not a server ERROR status, and not worth retrying → ServerError
                // phase with no status makes isTransient() false.
                resp.error = std::make_exception_ptr(HotRodClientException(
                    "Opcode mismatch: expected " +
                        std::to_string(pending->expectedOpcode) +
                        ", got " + std::to_string(opcode),
                    FailurePhase::ServerError, std::nullopt, {{host_, port_}}));
                pending->promise.set_value(std::move(resp));
                continue;
            }

            // 4. Call body parser (runs in read thread!)
            std::any body;
            try {
                body = pending->bodyParser(status, connection_.get(), protocolVersion_, pending->flags);
            } catch (...) {
                Response resp;
                resp.error = std::current_exception();
                pending->promise.set_value(std::move(resp));
                continue;
            }

            // 5. Build and deliver response
            Response resp;
            resp.status = status;
            resp.body = std::move(body);
            resp.topologyUpdate = std::move(topologyUpdate);
            pending->promise.set_value(std::move(resp));

        } catch (const std::exception& e) {
            // Connection error - close all pending
            if (!stopReadLoop_) {
                // Unexpected error (not a normal shutdown)
                std::cerr << "[ERROR] Read loop error: " << e.what() << std::endl;
                // Mark connection as disconnected so pool knows it's dead
                connected_ = false;
            }
            // Expected shutdown - no logging needed
            closeAllPending(e.what());
            break;
        }
    }
}

ByteArray MultiplexedConnection::buildRequest(
    uint64_t messageId,
    uint8_t opcode,
    const ByteArray& body,
    const std::string& cacheName,
    uint32_t flags)
{
    // Build request header
    RequestHeader header;
    header.messageId = messageId;
    header.version = protocolVersion_;  // Use configured protocol version
    header.opcode = opcode;
    header.cacheName = cacheName;
    header.flags = flags;
    header.clientIntelligence = clientIntelligence_;
    header.topologyId = topologyId_.load();
    header.keyMediaType = 0;
    header.valueMediaType = 0;
    // otherParams empty (count = 0)

    // Encode header
    ByteArray request;
    HeaderCodec::writeRequestHeader(request, header);

    // Append body
    request.insert(request.end(), body.begin(), body.end());

    return request;
}

void MultiplexedConnection::closeAllPending(const std::string& errorMsg) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    // These requests were already sent; the connection dropped before a response
    // arrived → AfterSend (ambiguous, may have applied on the server).
    auto error = std::make_exception_ptr(HotRodClientException(
        errorMsg, FailurePhase::AfterSend, std::nullopt, {{host_, port_}}));

    for (auto& [msgId, pending] : pending_) {
        Response resp;
        resp.error = error;
        try {
            pending->promise.set_value(std::move(resp));
        } catch (const std::future_error&) {
            // Promise already satisfied, ignore
        }
    }
    pending_.clear();
}

} // namespace hotrod
