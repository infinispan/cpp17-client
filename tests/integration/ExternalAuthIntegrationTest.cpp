/**
 * SASL EXTERNAL Authentication Integration Tests (Docker-backed), issue #10.
 *
 * Runs against a mutual-TLS + EXTERNAL Infinispan server started by
 * scripts/start_infinispan_mtls.sh infinispan-external.xml (require-ssl-client-auth,
 * a client-CA truststore, a truststore-realm, and SASL EXTERNAL on the Hot Rod
 * connector). The client's identity comes entirely from its TLS certificate
 * (CN=test-client); EXTERNAL carries no password. Verifies the end-to-end chain —
 * mutual TLS handshake, then the SASL EXTERNAL handshake, then an operation:
 *   1. Programmatic setTls(cert/key) + setAuthentication(EXTERNAL) -> put/get.
 *   2. hotrods:// URI carrying the client cert/key + sasl_mechanism=EXTERNAL -> same.
 *
 * Mirrors Infinispan's own AuthenticationCertIT. Excluded from the default CI unit
 * run via the "integration;docker;auth;tls" label.
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

// Starts/stops the EXTERNAL (mutual TLS + SASL) Infinispan container once for the
// whole suite and carries the trust anchor + client identity it exports.
class ExternalServerEnvironment : public ::testing::Environment {
public:
    static std::string containerID;
    static std::string host;
    static int         port;
    static std::string caFile;
    static std::string clientCertFile;
    static std::string clientKeyFile;

    void SetUp() override {
        int result = system("./scripts/start_infinispan_mtls.sh infinispan-external.xml "
                            "> /tmp/ispn-external-env.sh 2>&1");
        if (result != 0) {
            throw std::runtime_error("Failed to start EXTERNAL Infinispan server. Check Docker is running.");
        }

        std::ifstream env("/tmp/ispn-external-env.sh");
        if (!env.is_open()) {
            throw std::runtime_error("Failed to open /tmp/ispn-external-env.sh");
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
            throw std::runtime_error("Failed to parse EXTERNAL server info from startup script");
        }
    }

    void TearDown() override {
        if (!containerID.empty()) {
            std::string cmd = "./scripts/stop_infinispan.sh " + containerID + " >/dev/null";
            std::ignore = system(cmd.c_str());
        }
    }
};

std::string ExternalServerEnvironment::containerID;
std::string ExternalServerEnvironment::host = "localhost";
int         ExternalServerEnvironment::port = 0;
std::string ExternalServerEnvironment::caFile;
std::string ExternalServerEnvironment::clientCertFile;
std::string ExternalServerEnvironment::clientKeyFile;

// TlsConfig trusting the server CA and presenting the test client cert/key.
TlsConfig mutualTlsConfig() {
    TlsConfig tls;
    tls.enabled   = true;
    tls.ca_file   = ExternalServerEnvironment::caFile;
    tls.cert_file = ExternalServerEnvironment::clientCertFile;
    tls.key_file  = ExternalServerEnvironment::clientKeyFile;
    return tls;
}

} // namespace

// Cache pre-defined in infinispan-external.xml.
constexpr const char* kExternalCache = "externalcache";

// 1. Programmatic EXTERNAL: present the client cert at the TLS layer and run the
//    SASL EXTERNAL handshake (identity derived server-side from the cert). A
//    put/get then round-trips.
TEST(ExternalAuthIntegrationTest, CertIdentityRoundTrips) {
    RemoteCache cache(ExternalServerEnvironment::host, ExternalServerEnvironment::port, kExternalCache);
    cache.setTls(mutualTlsConfig());

    Authentication auth;
    auth.mechanism = "EXTERNAL";  // identity from the client cert; no password/token
    cache.setAuthentication(auth);

    cache.connect();

    const ByteArray key{'e', 'x', 't', '1'};
    const ByteArray value{'v', '1'};
    cache.put(key, value).get();
    auto got = cache.get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache.disconnect();
}

// 2. Same, driven from a hotrods:// URI: the client cert/key come from the mutual-TLS
//    URI keys and the mechanism from sasl_mechanism=EXTERNAL. Proves the
//    fromUri() parse -> setTls + setAuthentication -> connect -> op chain for a
//    cert-identity mechanism (no userinfo in the URI).
TEST(ExternalAuthIntegrationTest, FromUriExternalRoundTrips) {
    const std::string uri =
        "hotrods://" + ExternalServerEnvironment::host + ":" +
        std::to_string(ExternalServerEnvironment::port) +
        "?ca_file=" + ExternalServerEnvironment::caFile +
        "&client_cert_file=" + ExternalServerEnvironment::clientCertFile +
        "&client_key_file=" + ExternalServerEnvironment::clientKeyFile +
        "&sasl_mechanism=EXTERNAL";

    auto cache = RemoteCache::fromUri(uri, kExternalCache);
    cache->connect();

    const ByteArray key{'u', 'r', 'i', 'e', 'x', 't'};
    const ByteArray value{'v', '1'};
    cache->put(key, value).get();
    auto got = cache->get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache->disconnect();
}

int main(int argc, char** argv) {
    // No signal(SIGPIPE, SIG_IGN): the client suppresses SIGPIPE per-connection
    // (see TlsIntegrationTest for the rationale).
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new ExternalServerEnvironment());
    return RUN_ALL_TESTS();
}
