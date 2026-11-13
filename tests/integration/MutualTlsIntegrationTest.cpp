/**
 * Mutual TLS Integration Tests (Docker-backed), issue #10 integration coverage.
 *
 * Runs against a mutual-TLS Infinispan server started by
 * scripts/start_infinispan_mtls.sh (Hot Rod endpoint with require-ssl-client-auth
 * and a client-CA truststore, no SASL). The client trusts the server CA
 * (test-configs/tls/ca.pem) and presents the test client certificate
 * (test-configs/tls/client.pem + client.key). Verifies the real wire end-to-end:
 *   1. Programmatic setTls(ca + client cert/key) -> connect + put/get over mTLS.
 *   2. hotrods:// URI carrying ca_file + client_cert_file + client_key_file -> same.
 *   3. No client certificate -> the server rejects the TLS handshake -> connect()
 *      throws (require-ssl-client-auth enforced at the transport layer).
 *
 * The companion ExternalAuthIntegrationTest layers SASL EXTERNAL on the same
 * fixtures; this suite isolates the client's mutual-TLS cert presentation (#18)
 * with no SASL.
 *
 * Excluded from the default CI unit run via the "integration;docker;tls" label.
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

// Starts/stops the mutual-TLS Infinispan container once for the whole suite and
// carries the trust anchor + client identity the start script exports.
class MutualTlsServerEnvironment : public ::testing::Environment {
public:
    static std::string containerID;
    static std::string host;
    static int         port;
    static std::string caFile;
    static std::string clientCertFile;
    static std::string clientKeyFile;

    void SetUp() override {
        int result = system("./scripts/start_infinispan_mtls.sh > /tmp/ispn-mtls-env.sh 2>&1");
        if (result != 0) {
            throw std::runtime_error("Failed to start mutual-TLS Infinispan server. Check Docker is running.");
        }

        std::ifstream env("/tmp/ispn-mtls-env.sh");
        if (!env.is_open()) {
            throw std::runtime_error("Failed to open /tmp/ispn-mtls-env.sh");
        }

        std::string line;
        while (std::getline(env, line)) {
            if (line.rfind("export ISPN_CONTAINER_ID=", 0) == 0) {
                containerID = line.substr(25);
            } else if (line.rfind("export ISPN_HOST=", 0) == 0) {
                host = line.substr(17);
            } else if (line.rfind("export ISPN_PORT=", 0) == 0) {
                port = std::stoi(line.substr(17));
            } else if (line.rfind("export ISPN_CA_FILE=", 0) == 0) {
                caFile = line.substr(20);
            } else if (line.rfind("export ISPN_CLIENT_CERT_FILE=", 0) == 0) {
                clientCertFile = line.substr(29);
            } else if (line.rfind("export ISPN_CLIENT_KEY_FILE=", 0) == 0) {
                clientKeyFile = line.substr(28);
            }
        }

        if (containerID.empty() || port == 0 || caFile.empty() ||
            clientCertFile.empty() || clientKeyFile.empty()) {
            throw std::runtime_error("Failed to parse mutual-TLS server info from startup script");
        }
    }

    void TearDown() override {
        if (!containerID.empty()) {
            std::string cmd = "./scripts/stop_infinispan.sh " + containerID + " >/dev/null";
            std::ignore = system(cmd.c_str());
        }
    }
};

std::string MutualTlsServerEnvironment::containerID;
std::string MutualTlsServerEnvironment::host = "localhost";
int         MutualTlsServerEnvironment::port = 0;
std::string MutualTlsServerEnvironment::caFile;
std::string MutualTlsServerEnvironment::clientCertFile;
std::string MutualTlsServerEnvironment::clientKeyFile;

// Build a TlsConfig trusting the server CA and presenting the test client cert/key.
TlsConfig mutualTlsConfig() {
    TlsConfig tls;
    tls.enabled   = true;
    tls.ca_file   = MutualTlsServerEnvironment::caFile;
    tls.cert_file = MutualTlsServerEnvironment::clientCertFile;
    tls.key_file  = MutualTlsServerEnvironment::clientKeyFile;
    return tls;  // hostname_validation stays on; client dials localhost (in SAN)
}

} // namespace

// Cache pre-defined in infinispan-mtls.xml.
constexpr const char* kMtlsCache = "mtlscache";

// 1. Programmatic mutual TLS: trust the server CA, present the client cert/key,
//    connect, round-trip a value. The server verifies the client cert against its
//    truststore (require-ssl-client-auth).
TEST(MutualTlsIntegrationTest, ClientCertRoundTrips) {
    RemoteCache cache(MutualTlsServerEnvironment::host, MutualTlsServerEnvironment::port, kMtlsCache);
    cache.setTls(mutualTlsConfig());
    cache.connect();

    const ByteArray key{'m', 't', 'l', 's', '1'};
    const ByteArray value{'v', '1'};
    cache.put(key, value).get();
    auto got = cache.get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache.disconnect();
}

// 2. Same round-trip driven from a hotrods:// URI carrying the PEM trust anchor and
//    the PEM client cert/key (mutual-TLS URI keys, #18).
TEST(MutualTlsIntegrationTest, FromUriClientCertRoundTrips) {
    const std::string uri =
        "hotrods://" + MutualTlsServerEnvironment::host + ":" +
        std::to_string(MutualTlsServerEnvironment::port) +
        "?ca_file=" + MutualTlsServerEnvironment::caFile +
        "&client_cert_file=" + MutualTlsServerEnvironment::clientCertFile +
        "&client_key_file=" + MutualTlsServerEnvironment::clientKeyFile;

    auto cache = RemoteCache::fromUri(uri, kMtlsCache);
    cache->connect();

    const ByteArray key{'u', 'r', 'i', 'm', 't', 'l', 's'};
    const ByteArray value{'v', '1'};
    cache->put(key, value).get();
    auto got = cache->get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache->disconnect();
}

// 3. No client certificate against a server that requires one: the server rejects
//    the connection. Trust of the server is correct; only the missing client cert
//    differs. With require-ssl-client-auth the server aborts, but under TLS 1.3 the
//    client's SSL_connect() can return before the server's "certificate required"
//    alert arrives, so the failure may surface at connect() OR on the first
//    operation — either way an exception must be thrown before a value round-trips
//    (same pattern as AuthIntegrationTest.NoCredentialsFails).
TEST(MutualTlsIntegrationTest, NoClientCertRejected) {
    RemoteCache cache(MutualTlsServerEnvironment::host, MutualTlsServerEnvironment::port, kMtlsCache);
    cache.setTls(MutualTlsServerEnvironment::caFile);  // server-auth only, no client cert

    bool threw = false;
    try {
        cache.connect();
        const ByteArray key{'n', 'o', 'c', 'e', 'r', 't'};
        cache.put(key, ByteArray{'x'}).get();
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw);

    cache.disconnect();
}

int main(int argc, char** argv) {
    // Deliberately NO signal(SIGPIPE, SIG_IGN): the client suppresses SIGPIPE
    // per-connection, so a TLS handshake the server aborts must surface as a thrown
    // exception, not kill this process (see TlsIntegrationTest for the rationale).
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new MutualTlsServerEnvironment());
    return RUN_ALL_TESTS();
}
