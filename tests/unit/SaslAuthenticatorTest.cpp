/**
 * Unit tests for the SASL SCRAM handshake driver (SaslAuthenticator).
 *
 * The driver runs against a transport seam (SaslTransport). Here we supply a
 * FakeScramServer that speaks the server half of the exchange, computing the
 * server-first / server-final and validating the client proof with OpenSSL
 * *independently* of the client's SCRAM code. A green round-trip therefore
 * proves the client's generalized SCRAM (SHA-1/256/512) interoperates with a
 * correct peer — not merely that it agrees with itself.
 *
 * Coverage:
 *  - Positive round-trip, parameterized across SCRAM-SHA-1/256/512.
 *  - Wrong password  -> server rejects the proof (ERROR frame) -> throws.
 *  - Bad server signature -> client verification fails -> throws.
 *  - Mechanism not offered by the server -> throws.
 *  - Unsupported mechanism configured (e.g. PLAIN) -> throws before any I/O.
 */
#include <gtest/gtest.h>
#include "hotrod/SaslAuthenticator.h"
#include "hotrod/Authentication.h"
#include "hotrod/AuthCodec.h"
#include "hotrod/Codec.h"
#include "hotrod/HeaderCodec.h"
#include "hotrod/HotRodClientException.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <deque>
#include <sstream>
#include <stdexcept>

using namespace hotrod;

namespace {

// --- Minimal OpenSSL helpers for the *server* side of the exchange ----------

std::string b64Encode(const ByteArray& data) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, data.data(), static_cast<int>(data.size()));
    BIO_flush(bio);
    BUF_MEM* buf;
    BIO_get_mem_ptr(bio, &buf);
    std::string out(buf->data, buf->length);
    BIO_free_all(bio);
    return out;
}

ByteArray b64Decode(const std::string& enc) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* bio = BIO_new_mem_buf(enc.data(), static_cast<int>(enc.size()));
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    ByteArray out(enc.size());
    int n = BIO_read(bio, out.data(), static_cast<int>(out.size()));
    BIO_free_all(bio);
    out.resize(n < 0 ? 0 : static_cast<size_t>(n));
    return out;
}

ByteArray svPbkdf2(const std::string& pw, const ByteArray& salt, int iters, const EVP_MD* md) {
    ByteArray key(static_cast<size_t>(EVP_MD_size(md)));
    PKCS5_PBKDF2_HMAC(pw.c_str(), static_cast<int>(pw.size()),
                      salt.data(), static_cast<int>(salt.size()),
                      iters, md, EVP_MD_size(md), key.data());
    return key;
}

ByteArray svHmac(const ByteArray& key, const std::string& msg, const EVP_MD* md) {
    ByteArray out(static_cast<size_t>(EVP_MD_size(md)));
    unsigned int len = 0;
    HMAC(md, key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         out.data(), &len);
    out.resize(len);
    return out;
}

ByteArray svHash(const ByteArray& data, const EVP_MD* md) {
    ByteArray out(static_cast<size_t>(EVP_MD_size(md)));
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), out.data(), &len, md, nullptr);
    out.resize(len);
    return out;
}

// Extract the value of a "<key>=..." token from a comma-separated SCRAM message.
std::string field(const std::string& msg, char key) {
    std::istringstream ss(msg);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.size() >= 2 && tok[0] == key && tok[1] == '=') {
            return tok.substr(2);
        }
    }
    return {};
}

// --- Fake SCRAM server driven through the SaslTransport seam -----------------

struct FakeScramServer : public SaslTransport {
    // Server configuration.
    std::vector<std::string> mechs{"SCRAM-SHA-512", "SCRAM-SHA-256", "SCRAM-SHA-1", "PLAIN"};
    std::string username = "user";
    std::string password = "pencil";
    const EVP_MD* md = EVP_sha256();
    ByteArray salt = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    int iterations = 4096;
    std::string serverNonceSuffix = "3rfcNHYJY1ZVvWVs7j";

    // Fault injection.
    bool corruptServerSignature = false;

    // A pending response frame the client will read next.
    struct Frame {
        uint8_t   opcode = 0;
        ByteArray body;
        bool      isError = false;
        std::string errorMsg;
    };
    std::deque<Frame> outbox;
    ByteArray cursorBody;
    size_t    cursorOffset = 0;

    // Carried across rounds so the server can rebuild the auth message.
    std::string clientFirstBare;
    std::string serverFirst;

    enum class Phase { MechList, ClientFirst, ClientFinal, Done } phase = Phase::MechList;

    // --- SaslTransport ---
    void sendRequest(uint8_t opcode, const ByteArray& body) override {
        if (opcode == Opcode::AUTH_MECH_LIST_REQUEST) {
            Frame f;
            f.opcode = Opcode::AUTH_MECH_LIST_RESPONSE;
            Codec::writeVInt(f.body, static_cast<VInt>(mechs.size()));
            for (const auto& m : mechs) Codec::writeString(f.body, m);
            outbox.push_back(std::move(f));
            phase = Phase::ClientFirst;
            return;
        }
        ASSERT_EQ(opcode, Opcode::AUTH_REQUEST);

        size_t off = 0;
        std::string mech = Codec::readString(body, off);
        ByteArray tokenBytes = Codec::readByteArray(body, off);
        std::string token(tokenBytes.begin(), tokenBytes.end());

        if (phase == Phase::ClientFirst) {
            // token = "n,,n=<user>,r=<cnonce>"
            clientFirstBare = token.substr(3);
            std::string cnonce = field(token, 'r');
            std::string combined = cnonce + serverNonceSuffix;
            serverFirst = "r=" + combined + ",s=" + b64Encode(salt) +
                          ",i=" + std::to_string(iterations);
            Frame f;
            f.opcode = Opcode::AUTH_RESPONSE;
            f.body.push_back(0);  // completed = false
            Codec::writeByteArray(f.body, ByteArray(serverFirst.begin(), serverFirst.end()));
            outbox.push_back(std::move(f));
            phase = Phase::ClientFinal;
            return;
        }

        // Phase::ClientFinal — token = "c=biws,r=<combined>,p=<proof>"
        std::string proof = field(token, 'p');
        std::string clientFinalWithoutProof = token.substr(0, token.find(",p="));
        std::string authMessage = clientFirstBare + "," + serverFirst + "," + clientFinalWithoutProof;

        ByteArray saltedPassword = svPbkdf2(password, salt, iterations, md);
        ByteArray clientKey = svHmac(saltedPassword, "Client Key", md);
        ByteArray storedKey = svHash(clientKey, md);
        ByteArray clientSig = svHmac(storedKey, authMessage, md);

        ByteArray expectedProof(clientKey.size());
        for (size_t i = 0; i < clientKey.size(); ++i) expectedProof[i] = clientKey[i] ^ clientSig[i];

        if (b64Decode(proof) != expectedProof) {
            // Wrong password (proof mismatch) — server rejects with an ERROR frame.
            Frame f;
            f.opcode = Opcode::ERROR_RESPONSE;
            f.isError = true;
            f.errorMsg = "Authentication failed: proof mismatch";
            outbox.push_back(std::move(f));
            phase = Phase::Done;
            return;
        }

        ByteArray serverKey = svHmac(saltedPassword, "Server Key", md);
        ByteArray serverSig = svHmac(serverKey, authMessage, md);
        if (corruptServerSignature && !serverSig.empty()) serverSig[0] ^= 0xFF;
        std::string serverFinal = "v=" + b64Encode(serverSig);

        Frame f;
        f.opcode = Opcode::AUTH_RESPONSE;
        f.body.push_back(1);  // completed = true
        Codec::writeByteArray(f.body, ByteArray(serverFinal.begin(), serverFinal.end()));
        outbox.push_back(std::move(f));
        phase = Phase::Done;
    }

    uint8_t readResponseHeader() override {
        if (outbox.empty()) throw std::runtime_error("FakeScramServer: no response queued");
        Frame f = std::move(outbox.front());
        outbox.pop_front();
        if (f.isError) {
            throw HotRodClientException("Authentication failed: " + f.errorMsg,
                                        FailurePhase::ServerError, Status::SERVER_ERROR,
                                        {{"fake", 0}});
        }
        cursorBody = std::move(f.body);
        cursorOffset = 0;
        return f.opcode;
    }

    uint8_t readByte() override { return cursorBody.at(cursorOffset++); }
    VInt readVInt() override { return Codec::readVInt(cursorBody, cursorOffset); }
    std::string readString() override { return Codec::readString(cursorBody, cursorOffset); }
    ByteArray readByteArray() override { return Codec::readByteArray(cursorBody, cursorOffset); }
};

Authentication makeAuth(const std::string& mech, const std::string& user, const std::string& pass) {
    Authentication a;
    a.enabled = true;
    a.username = user;
    a.password = pass;
    a.mechanism = mech;
    return a;
}

} // namespace

// --- Positive round-trip, parameterized across the SCRAM digest family -------

struct ScramCase {
    std::string mechanism;
    const EVP_MD* md;
};

class SaslAuthenticatorFamilyTest : public ::testing::TestWithParam<ScramCase> {};

TEST_P(SaslAuthenticatorFamilyTest, SuccessfulHandshake) {
    const auto& c = GetParam();
    FakeScramServer server;
    server.md = c.md;

    SaslAuthenticator auth(makeAuth(c.mechanism, "user", "pencil"), "host", 11222);
    EXPECT_NO_THROW(auth.authenticate(server));
    EXPECT_EQ(server.phase, FakeScramServer::Phase::Done);
}

TEST_P(SaslAuthenticatorFamilyTest, WrongPasswordThrows) {
    const auto& c = GetParam();
    FakeScramServer server;
    server.md = c.md;  // server knows the real password "pencil"

    SaslAuthenticator auth(makeAuth(c.mechanism, "user", "WRONG"), "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
}

TEST_P(SaslAuthenticatorFamilyTest, BadServerSignatureThrows) {
    const auto& c = GetParam();
    FakeScramServer server;
    server.md = c.md;
    server.corruptServerSignature = true;

    SaslAuthenticator auth(makeAuth(c.mechanism, "user", "pencil"), "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
}

INSTANTIATE_TEST_SUITE_P(
    ScramFamily, SaslAuthenticatorFamilyTest,
    ::testing::Values(
        ScramCase{"SCRAM-SHA-1", EVP_sha1()},
        ScramCase{"SCRAM-SHA-256", EVP_sha256()},
        ScramCase{"SCRAM-SHA-512", EVP_sha512()}),
    [](const ::testing::TestParamInfo<ScramCase>& info) {
        std::string n = info.param.mechanism;
        for (char& ch : n) if (ch == '-') ch = '_';
        return n;
    });

// --- Failure paths independent of digest -------------------------------------

TEST(SaslAuthenticatorTest, MechanismNotOfferedThrows) {
    FakeScramServer server;
    server.mechs = {"PLAIN", "DIGEST-MD5"};  // no SCRAM offered

    SaslAuthenticator auth(makeAuth("SCRAM-SHA-256", "user", "pencil"), "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
}

TEST(SaslAuthenticatorTest, UnsupportedMechanismThrowsBeforeIO) {
    FakeScramServer server;

    // GSSAPI is not implemented by this client -> rejected before any I/O.
    SaslAuthenticator auth(makeAuth("GSSAPI", "user", "pencil"), "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
    // Nothing should have been sent to the transport.
    EXPECT_TRUE(server.outbox.empty());
    EXPECT_EQ(server.phase, FakeScramServer::Phase::MechList);
}

// --- Single-message mechanisms: PLAIN / EXTERNAL / OAUTHBEARER ----------------

namespace {

// A fake server for the single-message mechanisms. It serves the mech list, then
// captures the one AUTH request and answers with a configurable AUTH_RESPONSE
// (completed flag) or an ERROR frame. Also models the OAUTHBEARER failure
// continuation (a non-completed challenge turn followed by an ERROR).
struct FakeSimpleServer : public SaslTransport {
    std::vector<std::string> mechs{"PLAIN", "EXTERNAL", "OAUTHBEARER", "SCRAM-SHA-256"};
    bool completedFlag = true;   // AUTH_RESPONSE completed flag for the first turn
    bool errorOnFirstAuth = false;

    // Captured from the first AUTH request.
    std::string lastMech;
    ByteArray   lastInitial;
    int         authRequests = 0;

    struct Frame { uint8_t opcode = 0; ByteArray body; bool isError = false; std::string errorMsg; };
    std::deque<Frame> outbox;
    ByteArray cursorBody;
    size_t    cursorOffset = 0;

    void sendRequest(uint8_t opcode, const ByteArray& body) override {
        if (opcode == Opcode::AUTH_MECH_LIST_REQUEST) {
            Frame f;
            f.opcode = Opcode::AUTH_MECH_LIST_RESPONSE;
            Codec::writeVInt(f.body, static_cast<VInt>(mechs.size()));
            for (const auto& m : mechs) Codec::writeString(f.body, m);
            outbox.push_back(std::move(f));
            return;
        }
        ASSERT_EQ(opcode, Opcode::AUTH_REQUEST);
        ++authRequests;

        if (authRequests == 1) {
            size_t off = 0;
            lastMech = Codec::readString(body, off);
            lastInitial = Codec::readByteArray(body, off);
            if (errorOnFirstAuth) {
                outbox.push_back(Frame{Opcode::ERROR_RESPONSE, {}, true, "bad credentials"});
                return;
            }
            Frame f;
            f.opcode = Opcode::AUTH_RESPONSE;
            f.body.push_back(completedFlag ? 1 : 0);
            Codec::writeByteArray(f.body, ByteArray{});  // empty challenge
            outbox.push_back(std::move(f));
            return;
        }
        // Second AUTH turn (OAUTHBEARER failure continuation): fail with an ERROR.
        outbox.push_back(Frame{Opcode::ERROR_RESPONSE, {}, true, "oauth rejected"});
    }

    uint8_t readResponseHeader() override {
        if (outbox.empty()) throw std::runtime_error("FakeSimpleServer: no response queued");
        Frame f = std::move(outbox.front());
        outbox.pop_front();
        if (f.isError) {
            throw HotRodClientException("Authentication failed: " + f.errorMsg,
                                        FailurePhase::ServerError, Status::SERVER_ERROR,
                                        {{"fake", 0}});
        }
        cursorBody = std::move(f.body);
        cursorOffset = 0;
        return f.opcode;
    }
    uint8_t readByte() override { return cursorBody.at(cursorOffset++); }
    VInt readVInt() override { return Codec::readVInt(cursorBody, cursorOffset); }
    std::string readString() override { return Codec::readString(cursorBody, cursorOffset); }
    ByteArray readByteArray() override { return Codec::readByteArray(cursorBody, cursorOffset); }
};

}  // namespace

TEST(SaslSimpleMechanismTest, PlainSuccessSendsCorrectInitialResponse) {
    FakeSimpleServer server;
    SaslAuthenticator auth(makeAuth("PLAIN", "user", "pencil"), "host", 11222);
    EXPECT_NO_THROW(auth.authenticate(server));

    EXPECT_EQ(server.lastMech, "PLAIN");
    // authzid NUL authcid NUL passwd, authzid empty.
    const ByteArray expected{0x00, 'u','s','e','r', 0x00, 'p','e','n','c','i','l'};
    EXPECT_EQ(server.lastInitial, expected);
    EXPECT_EQ(server.authRequests, 1);
}

TEST(SaslSimpleMechanismTest, PlainRequiresUsername) {
    FakeSimpleServer server;
    SaslAuthenticator auth(makeAuth("PLAIN", "", "pencil"), "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
}

TEST(SaslSimpleMechanismTest, PlainBadCredentialsThrows) {
    FakeSimpleServer server;
    server.errorOnFirstAuth = true;
    SaslAuthenticator auth(makeAuth("PLAIN", "user", "wrong"), "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
}

TEST(SaslSimpleMechanismTest, ExternalSendsEmptyAuthzidByDefault) {
    FakeSimpleServer server;
    Authentication a;
    a.enabled = true;
    a.mechanism = "EXTERNAL";  // no username/password: identity from the client cert
    SaslAuthenticator auth(a, "host", 11222);
    EXPECT_NO_THROW(auth.authenticate(server));

    EXPECT_EQ(server.lastMech, "EXTERNAL");
    EXPECT_TRUE(server.lastInitial.empty());
}

TEST(SaslSimpleMechanismTest, OAuthBearerSendsBearerToken) {
    FakeSimpleServer server;
    Authentication a;
    a.enabled = true;
    a.mechanism = "OAUTHBEARER";
    a.token = "tok123";
    SaslAuthenticator auth(a, "host", 11222);
    EXPECT_NO_THROW(auth.authenticate(server));

    EXPECT_EQ(server.lastMech, "OAUTHBEARER");
    const std::string got(server.lastInitial.begin(), server.lastInitial.end());
    const std::string expected = std::string("n,,") + '\x01' + "auth=Bearer tok123" + '\x01' + '\x01';
    EXPECT_EQ(got, expected);
}

TEST(SaslSimpleMechanismTest, OAuthBearerRequiresToken) {
    FakeSimpleServer server;
    Authentication a;
    a.enabled = true;
    a.mechanism = "OAUTHBEARER";  // no token
    SaslAuthenticator auth(a, "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
}

TEST(SaslSimpleMechanismTest, OAuthBearerFailureContinuationThrows) {
    FakeSimpleServer server;
    server.completedFlag = false;  // server returns a non-completed error challenge
    Authentication a;
    a.enabled = true;
    a.mechanism = "OAUTHBEARER";
    a.token = "bad";
    SaslAuthenticator auth(a, "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
    // The client sent the initial token plus the RFC 7628 kvsep continuation.
    EXPECT_EQ(server.authRequests, 2);
}

TEST(SaslSimpleMechanismTest, MechanismNotOfferedThrows) {
    FakeSimpleServer server;
    server.mechs = {"SCRAM-SHA-256"};  // PLAIN not offered
    SaslAuthenticator auth(makeAuth("PLAIN", "user", "pencil"), "host", 11222);
    EXPECT_THROW(auth.authenticate(server), HotRodClientException);
}
