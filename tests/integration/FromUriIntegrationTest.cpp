#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "hotrod/HeaderCodec.h"
#include "hotrod/HotRodClientException.h"
#include "InfinispanTestEnvironment.h"
#include <cstdlib>
#include <string>

using namespace hotrod;
using namespace hotrod::test;

/**
 * Hot Rod URI Integration Tests — RemoteCache::fromUri (issue #9).
 *
 * The URI *parsing* is exhaustively covered by the pure unit tests
 * (tests/unit/HotRodURITest.cpp). This suite closes the other half: that a client
 * built from a URI actually connects and operates end-to-end against a live
 * server — i.e. the parse -> flat-setter -> connect -> op chain works.
 *
 * Runs against the single server managed by InfinispanTestEnvironment.
 */

namespace {

// Create the target cache (same CLI approach as the other integration suites).
void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

std::string baseUri() {
    return "hotrod://" + InfinispanTestEnvironment::host + ":" +
           std::to_string(InfinispanTestEnvironment::port);
}

} // anonymous namespace

// A client built from a plain hotrod:// URI connects and round-trips a value.
TEST(FromUriIntegrationTest, BasicRoundTripViaUri) {
    const std::string cacheName = "fromuri_basic";
    createCacheViaCLI(cacheName);

    auto cache = RemoteCache::fromUri(baseUri(), cacheName);
    cache->connect();

    const ByteArray key{'u', 'r', 'i', 'k', 'e', 'y'};
    const ByteArray value{'u', 'r', 'i', 'v', 'a', 'l'};
    cache->put(key, value).get();
    auto got = cache->get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache->disconnect();
}

// A query parameter flows through to a working client: both the configured value
// and a live round-trip are asserted. protocol_version is used because it is
// routing-neutral — unlike client_intelligence, it does not change which server
// the client connects to, so it is safe against this single-server fixture
// (hash-aware routing is covered only by the multi-server suites). 4.0 is chosen
// over the 4.1 default so the assertion proves the URI set it, not the default.
TEST(FromUriIntegrationTest, ProtocolVersionParamProducesWorkingClient) {
    const std::string cacheName = "fromuri_ver";
    createCacheViaCLI(cacheName);

    const std::string uri = baseUri() + "?protocol_version=4.0";
    auto cache = RemoteCache::fromUri(uri, cacheName);
    EXPECT_EQ(cache->getProtocolVersion(), Protocol::VERSION_40);

    cache->connect();

    const ByteArray key{'v', 'e', 'r'};
    const ByteArray value{'o', 'k'};
    cache->put(key, value).get();
    auto got = cache->get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache->disconnect();
}

// Startup failover (issue #12): a dead FIRST seed followed by the live server.
// connect() must try the dead seed, fail, and establish the bootstrap connection
// via the second seed; ops then round-trip. 127.0.0.1:1 is a reserved, unused
// port, so the first attempt is refused fast rather than hanging.
TEST(FromUriIntegrationTest, StartupFailoverToSecondSeed) {
    const std::string cacheName = "fromuri_failover";
    createCacheViaCLI(cacheName);

    const std::string uri = "hotrod://127.0.0.1:1," +
        InfinispanTestEnvironment::host + ":" +
        std::to_string(InfinispanTestEnvironment::port);
    auto cache = RemoteCache::fromUri(uri, cacheName);
    cache->connect();
    EXPECT_TRUE(cache->isConnected());

    const ByteArray key{'f', 'o', 'k'};
    const ByteArray value{'f', 'o', 'v'};
    cache->put(key, value).get();
    auto got = cache->get(key).get();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    cache->disconnect();
}

// When every seed is unreachable, connect() throws a BeforeSend
// HotRodClientException whose message names each seed it tried.
TEST(FromUriIntegrationTest, StartupAllSeedsUnreachableThrows) {
    auto cache = RemoteCache::fromUri("hotrod://127.0.0.1:1,127.0.0.1:2", "nope");

    bool threw = false;
    try {
        cache->connect();
    } catch (const HotRodClientException& e) {
        threw = true;
        EXPECT_EQ(FailurePhase::BeforeSend, e.phase);
        const std::string msg = e.what();
        EXPECT_NE(msg.find("127.0.0.1:1"), std::string::npos)
            << "message must name the first seed tried: " << msg;
        EXPECT_NE(msg.find("127.0.0.1:2"), std::string::npos)
            << "message must name the second seed tried: " << msg;
    }
    EXPECT_TRUE(threw) << "all seeds unreachable must throw from connect()";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());
    return RUN_ALL_TESTS();
}
