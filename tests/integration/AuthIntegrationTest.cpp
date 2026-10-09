/**
 * SASL/SCRAM Authentication Integration Tests (Docker-backed).
 *
 * Runs against an auth-enabled Infinispan server started by
 * scripts/start_infinispan_auth.sh (security realm with plain-text passwords so
 * the SCRAM family works; user testuser/testpassword). Verifies the end-to-end
 * handshake against a real server:
 *   1. Correct credentials  -> connect + put/get round-trip succeeds.
 *   2. Wrong password        -> connect() throws HotRodClientException.
 *   3. No credentials        -> operations against a secured server fail.
 *
 * Excluded from the default CI unit run via the "integration;docker;auth" label.
 */
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "hotrod/HotRodClientException.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>

using namespace hotrod;

namespace {

/**
 * Starts/stops the auth-enabled Infinispan container once for the whole suite.
 * Separate from InfinispanTestEnvironment because that one hardcodes the
 * no-auth start script; here we drive start_infinispan_auth.sh and also carry
 * the credentials it exports.
 */
class AuthServerEnvironment : public ::testing::Environment {
public:
    static std::string containerID;
    static std::string host;
    static int         port;
    static std::string user;
    static std::string pass;

    void SetUp() override {
        int result = system("./scripts/start_infinispan_auth.sh > /tmp/ispn-auth-env.sh 2>&1");
        if (result != 0) {
            throw std::runtime_error("Failed to start auth Infinispan server. Check Docker is running.");
        }

        std::ifstream env("/tmp/ispn-auth-env.sh");
        if (!env.is_open()) {
            throw std::runtime_error("Failed to open /tmp/ispn-auth-env.sh");
        }

        std::string line;
        while (std::getline(env, line)) {
            if (line.rfind("export ISPN_CONTAINER_ID=", 0) == 0) {
                containerID = line.substr(25);
            } else if (line.rfind("export ISPN_HOST=", 0) == 0) {
                host = line.substr(17);
            } else if (line.rfind("export ISPN_PORT=", 0) == 0) {
                port = std::stoi(line.substr(17));
            } else if (line.rfind("export ISPN_USER=", 0) == 0) {
                user = line.substr(17);
            } else if (line.rfind("export ISPN_PASS=", 0) == 0) {
                pass = line.substr(17);
            }
        }

        if (containerID.empty() || port == 0) {
            throw std::runtime_error("Failed to parse auth server info from startup script");
        }
    }

    void TearDown() override {
        if (!containerID.empty()) {
            std::string cmd = "./scripts/stop_infinispan.sh " + containerID + " >/dev/null";
            std::ignore = system(cmd.c_str());
        }
    }
};

std::string AuthServerEnvironment::containerID;
std::string AuthServerEnvironment::host = "localhost";
int         AuthServerEnvironment::port = 0;
std::string AuthServerEnvironment::user;
std::string AuthServerEnvironment::pass;

} // namespace

// Cache pre-defined in infinispan-auth.xml.
constexpr const char* kAuthCache = "authcache";

// 1. Correct credentials: connect, then a put/get round-trip succeeds — for each
//    mechanism in the SCRAM family the server offers.
class AuthMechanismRoundTrip : public ::testing::TestWithParam<std::string> {};

TEST_P(AuthMechanismRoundTrip, PutGet) {
    const std::string& mechanism = GetParam();

    RemoteCache cache(AuthServerEnvironment::host, AuthServerEnvironment::port, kAuthCache);
    cache.setAuthentication(AuthServerEnvironment::user, AuthServerEnvironment::pass,
                            "default", "infinispan", mechanism);
    cache.connect();

    // Key is mechanism-specific so parallel runs don't collide.
    ByteArray key(mechanism.begin(), mechanism.end());
    const ByteArray value{'v', '1'};

    cache.put(key, value).get();
    auto got = cache.get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache.disconnect();
}

INSTANTIATE_TEST_SUITE_P(
    ScramFamily, AuthMechanismRoundTrip,
    ::testing::Values("SCRAM-SHA-1", "SCRAM-SHA-256", "SCRAM-SHA-512"),
    [](const ::testing::TestParamInfo<std::string>& info) {
        std::string n = info.param;
        for (char& ch : n) if (ch == '-') ch = '_';
        return n;
    });

// 2. Wrong password: the SASL handshake fails at connect() time.
TEST(AuthIntegrationTest, WrongPasswordThrowsAtConnect) {
    RemoteCache cache(AuthServerEnvironment::host, AuthServerEnvironment::port);
    cache.setAuthentication(AuthServerEnvironment::user, "definitely-wrong-password");

    EXPECT_THROW(cache.connect(), HotRodClientException);
}

// 3. No credentials against a secured server: operations must fail.
TEST(AuthIntegrationTest, NoCredentialsFails) {
    RemoteCache cache(AuthServerEnvironment::host, AuthServerEnvironment::port, kAuthCache);
    // No setAuthentication(): the client skips the handshake; the server rejects
    // unauthenticated operations. Whether it surfaces at connect() or on the
    // first op, an exception must be thrown before a value round-trips.
    bool threw = false;
    try {
        cache.connect();
        const ByteArray key{'n', 'o', 'a', 'u', 't', 'h'};
        cache.put(key, ByteArray{'x'}).get();
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw);

    cache.disconnect();
}

// 4. Credentials carried in a Hot Rod URI authenticate end-to-end (issue #9).
//    Proves the fromUri() parse -> setAuthentication -> connect -> op chain, with
//    the SASL mechanism supplied as a query parameter.
TEST(AuthIntegrationTest, FromUriWithCredentialsAuthenticates) {
    const std::string uri =
        "hotrod://" + AuthServerEnvironment::user + ":" + AuthServerEnvironment::pass +
        "@" + AuthServerEnvironment::host + ":" + std::to_string(AuthServerEnvironment::port) +
        "?sasl_mechanism=SCRAM-SHA-256";

    auto cache = RemoteCache::fromUri(uri, kAuthCache);
    cache->connect();

    const ByteArray key{'u', 'r', 'i', 'a', 'u', 't', 'h'};
    const ByteArray value{'v', '1'};
    cache->put(key, value).get();
    auto got = cache->get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache->disconnect();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new AuthServerEnvironment());
    return RUN_ALL_TESTS();
}
