#include <gtest/gtest.h>
#include "TopologyTestFixture.h"
#include "hotrod/RemoteCache.h"
#include "hotrod/HotRodClientException.h"
#include <thread>
#include <chrono>
#include <optional>
#include <string>

using namespace hotrod;
using namespace hotrod::test;

/**
 * Multi-server seed list — runtime failover (issue #12).
 *
 * Startup failover (a dead first seed at connect()) is covered against the
 * single-server fixture in FromUriIntegrationTest. This suite closes the runtime
 * half: a client seeded with the whole cluster keeps serving after the server it
 * is currently talking to dies, by failing over to another seed.
 *
 * BASIC intelligence is used deliberately — it never receives a topology update,
 * so the connection pool stays empty and the client only ever routes through a
 * seed. That isolates the seed-failover path (tryConnectSeeds) from topology
 * discovery: the recovery here can only have come from the seed list.
 */
class MultiSeedIntegrationTest : public TopologyTest {
protected:
    void SetUp() override {
        TopologyTest::SetUp();
        resetCluster(2);     // DIST_SYNC numOwners=2 → both nodes own every key
        clearTestCache();
    }
};

// Seed the client with BOTH cluster nodes in BASIC mode, connect (lands on the
// first seed), store a key, then kill that node. The next op must fail over to
// the second seed and still read the value back (it is replicated to both owners).
TEST_F(MultiSeedIntegrationTest, RuntimeFailoverAcrossSeedsBasic) {
    const auto& n1 = MultiServerTestEnvironment::getServer(0);
    const auto& n2 = MultiServerTestEnvironment::getServer(1);

    const std::string uri = "hotrod://" + n1.host + ":" + std::to_string(n1.port) +
                            "," + n2.host + ":" + std::to_string(n2.port);
    auto cache = RemoteCache::fromUri(uri, TEST_CACHE);
    cache->setClientIntelligence(ClientIntelligence::BASIC);  // default; explicit for intent
    cache->connect();
    ASSERT_TRUE(cache->isConnected());

    const std::string keyStr = "multiseed-runtime";
    const std::string valueStr = "multiseed-runtime-value";
    ByteArray key(keyStr.begin(), keyStr.end());
    ByteArray value(valueStr.begin(), valueStr.end());

    cache->put(key, value).get();
    std::this_thread::sleep_for(std::chrono::seconds(2));  // replicate to the other owner

    // Kill node 1 — the bootstrap seed connect() landed on (seeds are tried in URI
    // order, so the first live seed is node 1).
    fprintf(stderr, "[TEST] killing seed node 1 (%s:%d)\n", n1.host.c_str(), n1.port);
    removeNode(1);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // The next op must fail over to seed node 2. GET is idempotent, so a short
    // retry loop safely absorbs an attempt that races the node going down.
    std::optional<ByteArray> result;
    for (int attempt = 0; attempt < 5 && !result.has_value(); ++attempt) {
        try {
            result = cache->get(key).get();
        } catch (const std::exception& e) {
            fprintf(stderr, "[TEST] attempt %d failed, retrying: %s\n", attempt, e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    ASSERT_TRUE(result.has_value())
        << "runtime failover to the second seed must recover the value";
    EXPECT_EQ(value, *result);

    cache->disconnect();

    // Restore the cluster for any subsequent tests.
    addNode(1);
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

// Topology-aware variant: a HASH_DISTRIBUTION_AWARE client built from a multi-seed
// URI whose FIRST seed is dead must (1) fail over at connect() to a live seed,
// (2) learn the cluster topology from it, and (3) hash-route keyed ops — i.e. the
// multi-seed URI path works end-to-end with topology, not just in BASIC mode. (The
// pure "all topology members unreachable -> seed" corner is not staged here: the
// seeds ARE the cluster nodes, so killing every topology member also kills every
// seed; BASIC-mode RuntimeFailoverAcrossSeedsBasic above isolates the seed tier.)
TEST_F(MultiSeedIntegrationTest, TopologyAwareStartupFailoverAndHashRouting) {
    const auto& n1 = MultiServerTestEnvironment::getServer(0);
    const auto& n2 = MultiServerTestEnvironment::getServer(1);

    const std::string uri = "hotrod://127.0.0.1:1," +
        n1.host + ":" + std::to_string(n1.port) + "," +
        n2.host + ":" + std::to_string(n2.port) +
        "?client_intelligence=hash_distribution_aware";
    auto cache = RemoteCache::fromUri(uri, TEST_CACHE);
    cache->connect();  // must fail over past the dead first seed
    ASSERT_TRUE(cache->isConnected());

    const std::string keyStr = "multiseed-topo";
    const std::string valueStr = "multiseed-topo-value";
    ByteArray key(keyStr.begin(), keyStr.end());
    ByteArray value(valueStr.begin(), valueStr.end());

    // Drive ops until the client has learned the hash topology from a seed.
    bool hashReady = false;
    for (int i = 0; i < 20 && !hashReady; ++i) {
        cache->put(key, value).get();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        hashReady = cache->getConsistentHash().hasHashTopology();
    }
    ASSERT_TRUE(hashReady) << "a URI-seeded client must learn hash topology from a seed";
    ASSERT_GE(cache->getTopology().getServers().size(), 2u)
        << "topology discovery must find both cluster nodes via the seed";

    // Hash-routed read round-trips.
    auto got = cache->get(key).get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(value, *got);

    cache->disconnect();
}

// Main — 2-node cluster; both nodes own every key under DIST_SYNC numOwners=2.
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    MultiServerTestEnvironment::setInitialServerCount(2);
    ::testing::AddGlobalTestEnvironment(new MultiServerTestEnvironment());
    return RUN_ALL_TESTS();
}
