/**
 * Unit tests for the pure SASL initial-response builders (SaslMechanisms.h):
 * PLAIN (RFC 4616), EXTERNAL (RFC 4422), OAUTHBEARER (RFC 7628). These assert the
 * exact bytes placed on the wire, independent of the transport.
 */
#include <gtest/gtest.h>
#include "hotrod/SaslMechanisms.h"

using namespace hotrod;

TEST(SaslMechanismsTest, PlainEmptyAuthzid) {
    // authzid NUL authcid NUL passwd, authzid empty.
    const ByteArray got = saslPlainInitialResponse("", "user", "pencil");
    const ByteArray expected{0x00, 'u','s','e','r', 0x00, 'p','e','n','c','i','l'};
    EXPECT_EQ(got, expected);
}

TEST(SaslMechanismsTest, PlainWithAuthzid) {
    const ByteArray got = saslPlainInitialResponse("admin", "user", "pw");
    const ByteArray expected{'a','d','m','i','n', 0x00, 'u','s','e','r', 0x00, 'p','w'};
    EXPECT_EQ(got, expected);
}

TEST(SaslMechanismsTest, PlainPasswordWithNulAndColonPreserved) {
    // The builder uses raw bytes, so a NUL/':' in the password is carried verbatim.
    const ByteArray got = saslPlainInitialResponse("", "u", std::string("a\0b", 3));
    const ByteArray expected{'u', 0x00, 'a', 0x00, 'b'};
    // leading authzid-NUL:
    ByteArray full{0x00};
    full.insert(full.end(), expected.begin(), expected.end());
    EXPECT_EQ(got, full);
}

TEST(SaslMechanismsTest, ExternalEmpty) {
    EXPECT_TRUE(saslExternalInitialResponse("").empty());
}

TEST(SaslMechanismsTest, ExternalWithAuthzid) {
    const ByteArray got = saslExternalInitialResponse("alice");
    const ByteArray expected{'a','l','i','c','e'};
    EXPECT_EQ(got, expected);
}

TEST(SaslMechanismsTest, OAuthBearerEmptyAuthzid) {
    const ByteArray got = saslOAuthBearerInitialResponse("", "tok");
    const std::string s(got.begin(), got.end());
    const std::string expected = std::string("n,,") + '\x01' + "auth=Bearer tok" + '\x01' + '\x01';
    EXPECT_EQ(s, expected);
}

TEST(SaslMechanismsTest, OAuthBearerWithAuthzid) {
    const ByteArray got = saslOAuthBearerInitialResponse("alice", "tok");
    const std::string s(got.begin(), got.end());
    const std::string expected =
        std::string("n,a=alice,") + '\x01' + "auth=Bearer tok" + '\x01' + '\x01';
    EXPECT_EQ(s, expected);
}
