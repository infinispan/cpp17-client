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

TEST(HotRodURITest, HotRodsSchemeThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrods://localhost:11222"), HotRodClientException);
}

TEST(HotRodURITest, UnknownParamThrows) {
    EXPECT_THROW(HotRodURI::parse("hotrod://h?connect_timeout=5000"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://h?socket_timeout=5000"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://h?use_ssl=true"), HotRodClientException);
    EXPECT_THROW(HotRodURI::parse("hotrod://h?token=abc"), HotRodClientException);
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
        HotRodURI::parse("hotrods://localhost");
        FAIL() << "expected throw";
    } catch (const HotRodClientException& e) {
        EXPECT_EQ(e.phase, FailurePhase::BeforeSend);
    }
}
