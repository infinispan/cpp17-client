#include <gtest/gtest.h>
#include "hotrod/HotRodURI.h"

using namespace hotrod;

/**
 * Unit tests for HotRodURI::parse() — the pure Hot Rod URI parser behind
 * RemoteCache::fromUri() (issue #9).
 *
 * Pure (no I/O), so the grammar, the typed query whitelist, and the explicit
 * rejection of the v1-deferred features (issue #10) are tested directly.
 *
 * Authoritative behaviour is the Java client's HotRodURI (AGENTS.md source order).
 */

// --- happy paths ------------------------------------------------------------

TEST(HotRodURITest, SingleHostDefaultPort) {
    HotRodURI u = HotRodURI::parse("hotrod://localhost");
    ASSERT_EQ(u.servers.size(), 1u);
    EXPECT_EQ(u.seed().host, "localhost");
    EXPECT_EQ(u.seed().port, 11222);
    EXPECT_FALSE(u.username.has_value());
    EXPECT_FALSE(u.clientIntelligence.has_value());
    EXPECT_FALSE(u.protocolVersion.has_value());
}

TEST(HotRodURITest, HostWithExplicitPort) {
    HotRodURI u = HotRodURI::parse("hotrod://192.168.1.10:11322");
    EXPECT_EQ(u.seed().host, "192.168.1.10");
    EXPECT_EQ(u.seed().port, 11322);
}

TEST(HotRodURITest, MultipleHostsParsedInOrder) {
    HotRodURI u = HotRodURI::parse("hotrod://a:11222,b:11322,c");
    ASSERT_EQ(u.servers.size(), 3u);
    EXPECT_EQ(u.servers[0].host, "a");
    EXPECT_EQ(u.servers[0].port, 11222);
    EXPECT_EQ(u.servers[1].host, "b");
    EXPECT_EQ(u.servers[1].port, 11322);
    EXPECT_EQ(u.servers[2].host, "c");
    EXPECT_EQ(u.servers[2].port, 11222);  // default
    EXPECT_EQ(&u.seed(), &u.servers[0]);  // seed is the first host
}

TEST(HotRodURITest, UserAndPassword) {
    HotRodURI u = HotRodURI::parse("hotrod://admin:secret@localhost:11222");
    ASSERT_TRUE(u.username.has_value());
    ASSERT_TRUE(u.password.has_value());
    EXPECT_EQ(*u.username, "admin");
    EXPECT_EQ(*u.password, "secret");
    EXPECT_EQ(u.seed().host, "localhost");
}

TEST(HotRodURITest, UserWithoutPassword) {
    HotRodURI u = HotRodURI::parse("hotrod://admin@localhost");
    ASSERT_TRUE(u.username.has_value());
    EXPECT_EQ(*u.username, "admin");
    EXPECT_FALSE(u.password.has_value());
}

TEST(HotRodURITest, TrailingPathIsIgnored) {
    HotRodURI u = HotRodURI::parse("hotrod://localhost:11222/");
    EXPECT_EQ(u.seed().host, "localhost");
    EXPECT_EQ(u.seed().port, 11222);
}

// --- percent-decoding (parity with java.net.URI) ----------------------------

// A password carrying a URI-reserved character must be percent-encoded to form a
// legal URI; the decoded bytes (not the "%40" text) must reach the client, else
// SCRAM would hash the wrong secret. Mirrors java.net.URI.getUserInfo() decoding.
TEST(HotRodURITest, PasswordPercentDecoded) {
    HotRodURI u = HotRodURI::parse("hotrod://admin:p%40ss%2Fword@localhost");
    ASSERT_TRUE(u.password.has_value());
    EXPECT_EQ(*u.password, "p@ss/word");
    EXPECT_EQ(*u.username, "admin");
}

TEST(HotRodURITest, UsernamePercentDecoded) {
    HotRodURI u = HotRodURI::parse("hotrod://ad%40min@localhost");
    ASSERT_TRUE(u.username.has_value());
    EXPECT_EQ(*u.username, "ad@min");
    EXPECT_FALSE(u.password.has_value());
}

// Splitting on the raw string before decoding means an encoded ':' stays part of
// the password rather than being treated as the user:pass delimiter.
TEST(HotRodURITest, EncodedColonInPasswordPreserved) {
    HotRodURI u = HotRodURI::parse("hotrod://user:pa%3Ass@localhost");
    EXPECT_EQ(*u.username, "user");
    ASSERT_TRUE(u.password.has_value());
    EXPECT_EQ(*u.password, "pa:ss");
}

// Lowercase hex digits decode too.
TEST(HotRodURITest, PercentDecodeLowercaseHex) {
    HotRodURI u = HotRodURI::parse("hotrod://u:a%2fb@h");
    EXPECT_EQ(*u.password, "a/b");
}

// Query values are decoded as well (java.net.URI.getQuery() is decoded).
TEST(HotRodURITest, QueryValuePercentDecoded) {
    HotRodURI u = HotRodURI::parse("hotrod://h?sasl_mechanism=SCRAM%2DSHA%2D256");
    ASSERT_TRUE(u.saslMechanism.has_value());
    EXPECT_EQ(*u.saslMechanism, "SCRAM-SHA-256");
}

// A malformed escape (non-hex digits, or a truncated '%') is rejected, matching
// java.net.URI's parse-time rejection and this parser's fail-fast stance.
TEST(HotRodURITest, MalformedPercentEscapeThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://u:a%ZZb@h"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://u:a%4@h"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://u:a%@h"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://u:ab%@h"), HotRodClientException);
}

// --- query parameters (typed whitelist) -------------------------------------

TEST(HotRodURITest, SaslMechanismParam) {
    HotRodURI u = HotRodURI::parse("hotrod://u:p@localhost?sasl_mechanism=SCRAM-SHA-512");
    ASSERT_TRUE(u.saslMechanism.has_value());
    EXPECT_EQ(*u.saslMechanism, "SCRAM-SHA-512");
}

TEST(HotRodURITest, ClientIntelligenceCaseInsensitive) {
    EXPECT_EQ(*HotRodURI::parse("hotrod://h?client_intelligence=basic").clientIntelligence,
              ClientIntelligence::BASIC);
    EXPECT_EQ(*HotRodURI::parse("hotrod://h?client_intelligence=TOPOLOGY_AWARE").clientIntelligence,
              ClientIntelligence::TOPOLOGY_AWARE);
    EXPECT_EQ(*HotRodURI::parse("hotrod://h?client_intelligence=Hash_Distribution_Aware").clientIntelligence,
              ClientIntelligence::HASH_DISTRIBUTION_AWARE);
}

TEST(HotRodURITest, ProtocolVersionJavaForm) {
    EXPECT_EQ(*HotRodURI::parse("hotrod://h?protocol_version=4.0").protocolVersion,
              Protocol::VERSION_40);
    EXPECT_EQ(*HotRodURI::parse("hotrod://h?protocol_version=4.1").protocolVersion,
              Protocol::VERSION_41);
}

TEST(HotRodURITest, MultipleQueryParams) {
    HotRodURI u = HotRodURI::parse(
        "hotrod://u:p@a,b?sasl_mechanism=SCRAM-SHA-256&client_intelligence=hash_distribution_aware&protocol_version=4.1");
    EXPECT_EQ(*u.saslMechanism, "SCRAM-SHA-256");
    EXPECT_EQ(*u.clientIntelligence, ClientIntelligence::HASH_DISTRIBUTION_AWARE);
    EXPECT_EQ(*u.protocolVersion, Protocol::VERSION_41);
    EXPECT_EQ(u.servers.size(), 2u);
}

// --- rejections: malformed grammar ------------------------------------------

TEST(HotRodURITest, MissingSchemeThrows) {
    EXPECT_THROW(HotRodURI::parse("localhost:11222"), HotRodClientException);
}

TEST(HotRodURITest, EmptyHostThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://"), HotRodClientException);
}

TEST(HotRodURITest, EmptyServerEntryThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://a,,b"), HotRodClientException);
}

TEST(HotRodURITest, InvalidPortThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://localhost:notaport"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://localhost:0"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://localhost:99999"), HotRodClientException);
}

// std::stoi stops at the first non-digit and would silently accept "8080x" as 8080;
// Java's Integer.parseInt throws. The parser must reject trailing garbage too.
TEST(HotRodURITest, PortWithTrailingGarbageThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://localhost:8080x"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://localhost:80 "), HotRodClientException);
}

TEST(HotRodURITest, QueryParamMissingEqualsThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://h?sasl_mechanism"), HotRodClientException);
}

// --- rejections: unsupported / deferred features (issue #10) -----------------

TEST(HotRodURITest, UnknownParamThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://h?connect_timeout=5000"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://h?socket_timeout=5000"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://h?token=abc"), HotRodClientException);
}

// --- TLS: hotrods:// scheme + server-auth PEM subset (issue #18) -------------

TEST(HotRodURITest, HotRodsSchemeEnablesTlsWithDefaults) {
    HotRodURI u = HotRodURI::parse("hotrods://localhost:11222");
    ASSERT_TRUE(u.tls.has_value());
    EXPECT_TRUE(u.tls->enabled);
    EXPECT_TRUE(u.tls->ca_file.empty());          // system/default trust store
    EXPECT_TRUE(u.tls->sni_host.empty());         // derived from host at connect
    EXPECT_TRUE(u.tls->hostname_validation);      // Java default
    // Host/port still parse as usual.
    EXPECT_EQ(u.seed().host, "localhost");
    EXPECT_EQ(u.seed().port, 11222);
}

TEST(HotRodURITest, PlainSchemeHasNoTls) {
    HotRodURI u = HotRodURI::parse("hotrod://localhost");
    EXPECT_FALSE(u.tls.has_value());
}

TEST(HotRodURITest, TlsParamsParsedIntoConfig) {
    HotRodURI u = HotRodURI::parse(
        "hotrods://localhost"
        "?ca_file=/etc/pki/ca.pem"
        "&sni_host_name=cache.example.com"
        "&ssl_hostname_validation=false");
    ASSERT_TRUE(u.tls.has_value());
    EXPECT_TRUE(u.tls->enabled);
    EXPECT_EQ(u.tls->ca_file, "/etc/pki/ca.pem");
    EXPECT_EQ(u.tls->sni_host, "cache.example.com");
    EXPECT_FALSE(u.tls->hostname_validation);
    // No client identity unless explicitly supplied.
    EXPECT_TRUE(u.tls->cert_file.empty());
    EXPECT_TRUE(u.tls->key_file.empty());
    EXPECT_TRUE(u.tls->key_password.empty());
}

TEST(HotRodURITest, MutualTlsClientIdentityParsedIntoConfig) {
    HotRodURI u = HotRodURI::parse(
        "hotrods://localhost"
        "?ca_file=/ca.pem"
        "&client_cert_file=/client.pem"
        "&client_key_file=/client-key.pem"
        "&client_key_password=s3cret");
    ASSERT_TRUE(u.tls.has_value());
    EXPECT_EQ(u.tls->ca_file, "/ca.pem");
    EXPECT_EQ(u.tls->cert_file, "/client.pem");
    EXPECT_EQ(u.tls->key_file, "/client-key.pem");
    EXPECT_EQ(u.tls->key_password, "s3cret");
}

TEST(HotRodURITest, TlsHostnameValidationBooleanCaseInsensitive) {
    EXPECT_TRUE(HotRodURI::parse(
        "hotrods://h?ssl_hostname_validation=TRUE").tls->hostname_validation);
    EXPECT_FALSE(HotRodURI::parse(
        "hotrods://h?ssl_hostname_validation=False").tls->hostname_validation);
}

TEST(HotRodURITest, TlsHostnameValidationBadBooleanThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrods://h?ssl_hostname_validation=yes"),
                 HotRodClientException);
}

TEST(HotRodURITest, TlsParamsOnPlainSchemeThrow) {
    // TLS params require hotrods://; silently ignoring them would hide a mistake.
    EXPECT_THROW(HotRodURI::parse("hotrod://h?sni_host_name=x"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://h?ca_file=/ca.pem"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://h?client_cert_file=/c.pem"),
                 HotRodClientException);
}

TEST(HotRodURITest, JavaKeystoreKeysRejectedWithRedirect) {
    // The Java keystore (JKS/PKCS12) TLS keys are rejected — this client uses PEM.
    // They throw regardless of scheme so a ported Java URI fails fast with a fix.
    for (const char* uri : {
             "hotrods://h?trust_store_file_name=/truststore.jks",
             "hotrods://h?trust_store_type=pkcs12",
             "hotrods://h?trust_store_password=secret",
             "hotrods://h?key_store_file_name=/client.p12",
             "hotrods://h?key_store_type=pkcs12",
             "hotrods://h?key_store_password=secret",
             "hotrods://h?key_alias=client",
         }) {
        EXPECT_THROW(HotRodURI::parse(uri), HotRodClientException) << uri;
    }
}

TEST(HotRodURITest, DeferredTlsKeysThrow) {
    // Recognized Java TLS keys we do not implement: the scheme-independent SSL
    // toggle and protocol/cipher/provider tuning are deferred (issue #10).
    for (const char* uri : {
             "hotrods://h?use_ssl=true",
             "hotrods://h?ssl_protocol=TLSv1.3",
             "hotrods://h?ssl_ciphers=TLS_AES_256_GCM_SHA384",
             "hotrods://h?ssl_named_groups=x25519",
             "hotrods://h?ssl_provider=openssl",
             "hotrods://h?ssl_context=ctx",
         }) {
        EXPECT_THROW(HotRodURI::parse(uri), HotRodClientException) << uri;
    }
}

TEST(HotRodURITest, UnsupportedProtocolVersionThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://h?protocol_version=AUTO"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://h?protocol_version=3.1"), HotRodClientException);
}

TEST(HotRodURITest, BadClientIntelligenceValueThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://h?client_intelligence=banana"), HotRodClientException);
}

// The parse error is surfaced as a BeforeSend failure (nothing was sent).
TEST(HotRodURITest, ParseErrorIsBeforeSend) {
    try {
        HotRodURI::parse("hotrod://h?connect_timeout=5000");
        FAIL() << "expected throw";
    } catch (const HotRodClientException& e) {
        EXPECT_EQ(e.phase, FailurePhase::BeforeSend);
    }
}
