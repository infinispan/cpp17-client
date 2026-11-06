/**
 * Server-authenticated TLS Integration Tests (Docker-backed), issue #18.
 *
 * Runs against a TLS-enabled Infinispan server started by
 * scripts/start_infinispan_tls.sh (Hot Rod endpoint with a PKCS12 server
 * identity, SAN DNS:localhost / IP:127.0.0.1, no SASL). The client trusts the
 * test CA (test-configs/tls/ca.pem). Verifies the real wire end-to-end:
 *   1. Programmatic setTls(ca) -> connect + put/get round-trip over TLS.
 *   2. hotrods:// URI with trust_store_file_name -> same round-trip.
 *   3. Untrusted anchor (system store, no CA) -> connect() throws.
 *   4. Host-name mismatch with validation on -> throws; with validation off,
 *      the (still chain-verified) connection succeeds.
 *
 * Excluded from the default CI unit run via the "integration;docker;tls" label.
 */
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "hotrod/HotRodClientException.h"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>

using namespace hotrod;

namespace {

// Starts/stops the TLS-enabled Infinispan container once for the whole suite and
// carries the trust anchor (ca_file) the start script exports.
class TlsServerEnvironment : public ::testing::Environment {
public:
    static std::string containerID;
    static std::string host;
    static int         port;
    static std::string caFile;

    void SetUp() override {
        int result = system("./scripts/start_infinispan_tls.sh > /tmp/ispn-tls-env.sh 2>&1");
        if (result != 0) {
            throw std::runtime_error("Failed to start TLS Infinispan server. Check Docker is running.");
        }

        std::ifstream env("/tmp/ispn-tls-env.sh");
        if (!env.is_open()) {
            throw std::runtime_error("Failed to open /tmp/ispn-tls-env.sh");
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
            }
        }

        if (containerID.empty() || port == 0 || caFile.empty()) {
            throw std::runtime_error("Failed to parse TLS server info from startup script");
        }
    }

    void TearDown() override {
        if (!containerID.empty()) {
            std::string cmd = "./scripts/stop_infinispan.sh " + containerID + " >/dev/null";
            std::ignore = system(cmd.c_str());
        }
    }
};

std::string TlsServerEnvironment::containerID;
std::string TlsServerEnvironment::host = "localhost";
int         TlsServerEnvironment::port = 0;
std::string TlsServerEnvironment::caFile;

} // namespace

// Cache pre-defined in infinispan-tls.xml.
constexpr const char* kTlsCache = "tlscache";

// 1. Programmatic TLS: trust the test CA, connect, round-trip a value. The client
//    dials "localhost" (a DNS name), so host-name validation (default on) checks
//    the server cert's SAN DNS:localhost.
TEST(TlsIntegrationTest, SetTlsTrustedCaRoundTrips) {
    RemoteCache cache(TlsServerEnvironment::host, TlsServerEnvironment::port, kTlsCache);
    cache.setTls(TlsServerEnvironment::caFile);  // sni/host derived; validation on
    cache.connect();

    const ByteArray key{'t', 'l', 's', '1'};
    const ByteArray value{'v', '1'};
    cache.put(key, value).get();
    auto got = cache.get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache.disconnect();
}

// 2. Same round-trip driven from a hotrods:// URI carrying the PEM trust anchor.
TEST(TlsIntegrationTest, FromUriHotrodsRoundTrips) {
    const std::string uri =
        "hotrods://" + TlsServerEnvironment::host + ":" +
        std::to_string(TlsServerEnvironment::port) +
        "?trust_store_file_name=" + TlsServerEnvironment::caFile;

    auto cache = RemoteCache::fromUri(uri, kTlsCache);
    cache->connect();

    const ByteArray key{'u', 'r', 'i', 't', 'l', 's'};
    const ByteArray value{'v', '1'};
    cache->put(key, value).get();
    auto got = cache->get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache->disconnect();
}

// 3. Untrusted anchor: with no CA file the client uses the system trust store,
//    which does not contain the test CA -> chain verification fails at connect().
TEST(TlsIntegrationTest, UntrustedAnchorThrowsAtConnect) {
    RemoteCache cache(TlsServerEnvironment::host, TlsServerEnvironment::port, kTlsCache);
    cache.setTls("");  // empty ca_file -> system/default trust store
    EXPECT_THROW(cache.connect(), std::exception);
}

// 4a. Host-name mismatch with validation on -> connect() throws even though the
//     chain is trusted (SNI/validated name forced to a value not in the SAN).
TEST(TlsIntegrationTest, HostnameMismatchValidationOnThrows) {
    RemoteCache cache(TlsServerEnvironment::host, TlsServerEnvironment::port, kTlsCache);
    TlsConfig tls;
    tls.enabled = true;
    tls.ca_file = TlsServerEnvironment::caFile;
    tls.sni_host = "wrong.example.com";  // not in the server cert SAN
    tls.hostname_validation = true;
    cache.setTls(tls);
    EXPECT_THROW(cache.connect(), std::exception);
}

// 4b. Same mismatch, but validation off: the chain is still verified, so the
//     connection succeeds and a value round-trips.
TEST(TlsIntegrationTest, HostnameMismatchValidationOffRoundTrips) {
    RemoteCache cache(TlsServerEnvironment::host, TlsServerEnvironment::port, kTlsCache);
    TlsConfig tls;
    tls.enabled = true;
    tls.ca_file = TlsServerEnvironment::caFile;
    tls.sni_host = "wrong.example.com";
    tls.hostname_validation = false;
    cache.setTls(tls);
    cache.connect();

    const ByteArray key{'n', 'o', 'v', 'a', 'l'};
    const ByteArray value{'v', '1'};
    cache.put(key, value).get();
    auto got = cache.get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache.disconnect();
}

int main(int argc, char** argv) {
    // A dropped TLS connection can make a client-side write hit a closed peer,
    // raising SIGPIPE whose default action kills the process. Network clients
    // conventionally ignore it (the app, here the test, owns signal disposition).
    std::signal(SIGPIPE, SIG_IGN);

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new TlsServerEnvironment());
    return RUN_ALL_TESTS();
}
