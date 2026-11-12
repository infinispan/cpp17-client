#include "hotrod/SaslAuthenticator.h"
#include "hotrod/Connection.h"
#include "hotrod/HeaderCodec.h"
#include "hotrod/HotRodClientException.h"
#include "hotrod/AuthCodec.h"
#include "hotrod/SCRAM.h"
#include "hotrod/SaslMechanisms.h"
#include "hotrod/TopologyInfo.h"
#include <utility>

namespace hotrod {

// ---------------------------------------------------------------------------
// ConnectionSaslTransport — production adapter over a live blocking Connection.
// ---------------------------------------------------------------------------

ConnectionSaslTransport::ConnectionSaslTransport(Connection* conn,
                                                 uint8_t protocolVersion,
                                                 ClientIntelligence intelligence,
                                                 const std::string& host,
                                                 uint16_t port)
    : conn_(conn)
    , protocolVersion_(protocolVersion)
    , intelligence_(intelligence)
    , host_(host)
    , port_(port)
{
}

void ConnectionSaslTransport::sendRequest(uint8_t opcode, const ByteArray& body) {
    RequestHeader header;
    header.messageId = nextMessageId_++;
    header.version = protocolVersion_;
    header.opcode = opcode;
    header.cacheName = "";              // AUTH ops are not cache-scoped
    header.flags = 0;
    header.clientIntelligence = intelligence_;
    header.topologyId = 0;             // pre-topology handshake
    header.keyMediaType = 0;
    header.valueMediaType = 0;

    ByteArray frame;
    HeaderCodec::writeRequestHeader(frame, header);
    frame.insert(frame.end(), body.begin(), body.end());

    try {
        conn_->send(frame);
    } catch (const std::exception& e) {
        throw HotRodClientException(std::string("Auth send failed: ") + e.what(),
                                    FailurePhase::AfterSend, std::nullopt,
                                    {{host_, port_}});
    }
}

uint8_t ConnectionSaslTransport::readResponseHeader() {
    // Mirror MultiplexedConnection::readLoop's header parse so the byte stream
    // stays in sync (magic / messageId / opcode / status / topology marker).
    ByteArray magicByte = conn_->receive(1);
    if (magicByte[0] != Protocol::RESPONSE_MAGIC) {
        throw HotRodClientException(
            "Auth: invalid magic byte in response: " + std::to_string(magicByte[0]),
            FailurePhase::AfterSend, std::nullopt, {{host_, port_}});
    }

    (void)conn_->receiveVLong();  // messageId (handshake is strictly request/response)

    ByteArray headerTail = conn_->receive(3);
    uint8_t opcode = headerTail[0];
    uint8_t status = headerTail[1];
    uint8_t topologyMarker = headerTail[2];

    // Consume (and discard) any topology update riding along, exactly as the
    // read loop would, so the stream stays aligned for the next read.
    if (topologyMarker != 0 &&
        (intelligence_ == ClientIntelligence::TOPOLOGY_AWARE ||
         intelligence_ == ClientIntelligence::HASH_DISTRIBUTION_AWARE)) {
        TopologyInfo topo;
        uint8_t hashFunctionVersion = 0;
        VInt numSegments = 0;
        std::vector<std::vector<uint8_t>> segmentOwners;
        topo.parseTopologyInfo(conn_, intelligence_, hashFunctionVersion,
                               numSegments, segmentOwners);
    }

    // A server ERROR carries a length-prefixed message; surface it typed.
    if (opcode == Opcode::ERROR_RESPONSE) {
        std::string errorMessage = conn_->receiveString();
        throw HotRodClientException(
            "Authentication failed (status " + std::to_string(status) + "): " + errorMessage,
            FailurePhase::ServerError, status, {{host_, port_}});
    }

    if (status != Status::NO_ERROR) {
        throw HotRodClientException(
            "Authentication failed with status " + std::to_string(status),
            FailurePhase::ServerError, status, {{host_, port_}});
    }

    return opcode;
}

uint8_t ConnectionSaslTransport::readByte() {
    return conn_->receive(1)[0];
}

VInt ConnectionSaslTransport::readVInt() {
    return conn_->receiveVInt();
}

std::string ConnectionSaslTransport::readString() {
    return conn_->receiveString();
}

ByteArray ConnectionSaslTransport::readByteArray() {
    return conn_->receiveByteArray();
}

// ---------------------------------------------------------------------------
// SaslAuthenticator — the SCRAM state machine.
// ---------------------------------------------------------------------------

SaslAuthenticator::SaslAuthenticator(Authentication auth, std::string host, uint16_t port)
    : auth_(std::move(auth))
    , host_(std::move(host))
    , port_(port)
{
}

void SaslAuthenticator::authenticate(SaslTransport& transport) {
    const std::vector<ServerAddress> node{{host_, port_}};
    const std::string& mech = auth_.mechanism;

    // 1. Classify the mechanism up-front (before any I/O). SCRAM is a multi-turn
    //    exchange; PLAIN/EXTERNAL/OAUTHBEARER complete in a single client message.
    SCRAM::Digest digest{};
    bool isScram = false;
    try {
        digest = SCRAM::digestForMechanism(mech);
        isScram = true;
    } catch (const std::exception&) {
        isScram = false;
    }
    const bool isSimple =
        (mech == "PLAIN" || mech == "EXTERNAL" || mech == "OAUTHBEARER");
    if (!isScram && !isSimple) {
        throw HotRodClientException(
            "Unsupported SASL mechanism: " + mech +
            " (supported: SCRAM-SHA-1, SCRAM-SHA-256, SCRAM-SHA-512, PLAIN, "
            "EXTERNAL, OAUTHBEARER)",
            FailurePhase::BeforeSend, std::nullopt, node);
    }

    // 2. Mech-list exchange: verify the server offers our mechanism.
    transport.sendRequest(Opcode::AUTH_MECH_LIST_REQUEST, encodeAuthMechListRequestBody());
    if (transport.readResponseHeader() != Opcode::AUTH_MECH_LIST_RESPONSE) {
        throw HotRodClientException("Auth: expected AUTH_MECH_LIST_RESPONSE",
                                    FailurePhase::AfterSend, std::nullopt, node);
    }
    VInt mechCount = transport.readVInt();
    bool offered = false;
    for (VInt i = 0; i < mechCount; ++i) {
        if (transport.readString() == mech) {
            offered = true;
            // keep reading to drain the rest of the list off the wire
        }
    }
    if (!offered) {
        throw HotRodClientException(
            "SASL mechanism " + mech + " is not offered by the server",
            FailurePhase::AfterSend, std::nullopt, node);
    }

    // 3a. Single-message mechanisms (PLAIN / EXTERNAL / OAUTHBEARER): one AUTH
    //     request carrying the initial response; the server answers completed=true
    //     on success, or an ERROR frame on failure (surfaced by readResponseHeader).
    if (isSimple) {
        ByteArray initial;
        if (mech == "PLAIN") {
            if (auth_.username.empty()) {
                throw HotRodClientException("PLAIN authentication requires a username",
                                            FailurePhase::BeforeSend, std::nullopt, node);
            }
            // Empty authzid: authorize as the authentication id (the usual case).
            initial = saslPlainInitialResponse("", auth_.username, auth_.password);
        } else if (mech == "EXTERNAL") {
            // Identity comes from the mutual-TLS client cert; the authorization id
            // (auth_.username, usually empty) is the only payload.
            initial = saslExternalInitialResponse(auth_.username);
        } else {  // OAUTHBEARER
            if (auth_.token.empty()) {
                throw HotRodClientException("OAUTHBEARER authentication requires a token",
                                            FailurePhase::BeforeSend, std::nullopt, node);
            }
            initial = saslOAuthBearerInitialResponse(auth_.username, auth_.token);
        }

        transport.sendRequest(Opcode::AUTH_REQUEST, encodeAuthRequestBody(mech, initial));
        if (transport.readResponseHeader() != Opcode::AUTH_RESPONSE) {
            throw HotRodClientException("Auth: expected AUTH_RESPONSE",
                                        FailurePhase::AfterSend, std::nullopt, node);
        }
        const bool completed = transport.readByte() != 0;
        (void)transport.readByteArray();  // drain the (empty on success) challenge
        if (completed) {
            return;  // authenticated
        }

        // Not completed. Only OAUTHBEARER defines a continuation (RFC 7628): on an
        // error the server returns a result challenge and expects a lone kvsep
        // (0x01) before it fails the exchange with an ERROR frame.
        if (mech == "OAUTHBEARER") {
            transport.sendRequest(Opcode::AUTH_REQUEST,
                                  encodeAuthRequestBody(mech, ByteArray{0x01}));
            (void)transport.readResponseHeader();  // expected to throw on the ERROR frame
        }
        throw HotRodClientException(mech + " authentication did not complete",
                                    FailurePhase::ServerError, std::nullopt, node);
    }

    // 3b. SCRAM client-first (initial response carried in the first AUTH request).
    std::string nonce = SCRAM::generateNonce();
    std::string clientFirst = SCRAM::createClientFirstMessage(auth_.username, nonce);
    // client-first-message-bare = client-first without the "n,," GS2 header.
    std::string clientFirstBare = clientFirst.substr(3);

    // Reads one AUTH_RESPONSE: {completed flag, challenge bytes}.
    auto readAuth = [&](bool& completed) -> std::string {
        if (transport.readResponseHeader() != Opcode::AUTH_RESPONSE) {
            throw HotRodClientException("Auth: expected AUTH_RESPONSE",
                                        FailurePhase::AfterSend, std::nullopt, node);
        }
        completed = transport.readByte() != 0;
        ByteArray challenge = transport.readByteArray();
        return std::string(challenge.begin(), challenge.end());
    };
    auto sendAuth = [&](const std::string& token) {
        transport.sendRequest(
            Opcode::AUTH_REQUEST,
            encodeAuthRequestBody(auth_.mechanism, ByteArray(token.begin(), token.end())));
    };

    // 4. Two SCRAM client turns, mirroring the Java client (AuthHandler): the
    // exchange completes on the *client* side once it has verified the server
    // signature — the client sends no further message and does not depend on the
    // server's `completed` flag (Infinispan/Elytron sends the server-final with
    // completed=false and expects no confirmation round).
    bool completed = false;

    // Turn 1: client-first -> server-first.
    sendAuth(clientFirst);
    std::string serverFirst = readAuth(completed);
    if (completed) {
        throw HotRodClientException(
            "Auth: server completed the exchange before the client-final message",
            FailurePhase::ServerError, std::nullopt, node);
    }

    std::string combinedNonce;
    std::string salt;
    int iterations = 0;
    try {
        SCRAM::parseServerFirstMessage(serverFirst, combinedNonce, salt, iterations);
    } catch (const std::exception& e) {
        throw HotRodClientException(std::string("Auth: bad server-first message: ") + e.what(),
                                    FailurePhase::ServerError, std::nullopt, node);
    }
    // The server nonce must extend our client nonce (RFC 5802).
    if (combinedNonce.rfind(nonce, 0) != 0) {
        throw HotRodClientException("Auth: server nonce does not match client nonce",
                                    FailurePhase::ServerError, std::nullopt, node);
    }

    // Turn 2: client-final -> server-final; verify the server signature.
    std::string clientFinal = SCRAM::createClientFinalMessage(
        auth_.password, clientFirstBare, serverFirst, combinedNonce, salt, iterations, digest);
    std::string clientFinalWithoutProof = clientFinal.substr(0, clientFinal.find(",p="));
    std::string authMessage = clientFirstBare + "," + serverFirst + "," + clientFinalWithoutProof;

    sendAuth(clientFinal);
    std::string serverFinal = readAuth(completed);
    if (serverFinal.empty()) {
        throw HotRodClientException("Auth: server did not send a server-final signature",
                                    FailurePhase::ServerError, std::nullopt, node);
    }
    if (!SCRAM::verifyServerFinalMessage(serverFinal, auth_.password, authMessage,
                                         salt, iterations, digest)) {
        throw HotRodClientException("Auth: server signature verification failed",
                                    FailurePhase::ServerError, std::nullopt, node);
    }
    // Verified: authentication is complete on the client side.
}

} // namespace hotrod
