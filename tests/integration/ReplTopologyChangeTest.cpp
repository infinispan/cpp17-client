#include <tuple>
#include <gtest/gtest.h>
#include "TopologyTestFixture.h"
#include <thread>
#include <chrono>

using namespace hotrod;
using namespace hotrod::test;

/**
 * Replication Topology Change Test (Single Client)
 *
 * Port of Java ReplTopologyChangeTest.java
 *
 * Key difference from TopologyChangeTest.cpp:
 * - Uses SINGLE client throughout all tests
 * - Client connects to ONE server initially
 * - Client discovers other servers via topology updates
 *
 * This is the simplest case for debugging topology awareness:
 * - Start with 2 servers, client connects to server2 only
 * - Client performs operations, receives topology update with both servers
 * - Add 3rd server, client discovers it via topology update
 * - Remove server, client detects the topology change
 *
 * Reference:
 * - Java: org.infinispan.client.hotrod.ReplTopologyChangeTest
 */

// Single test fixture - one client used across all tests
class ReplTopologyChangeTest : public ::testing::Test {
protected:
    static constexpr const char* TEST_CACHE = "repl-topology-test";

    // Shared client - persists across test methods
    static RemoteCache* client;
    static std::vector<RemoteCache*> allClients;

    static void SetUpTestSuite() {
        // Cluster already started by global environment
        // Just ensure we have at least 2 nodes
        if (MultiServerTestEnvironment::numServers < 2) {
            throw std::runtime_error("Need at least 2 servers for replication topology test");
        }

        // Create cache on first node
        if (!MultiServerTestEnvironment::servers.empty()) {
            const auto& server = MultiServerTestEnvironment::servers[0];
            std::string cmd = "docker exec " + server.containerID +
                            " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " +
                            std::string(TEST_CACHE) +
                            "' | /opt/infinispan/bin/cli.sh -c http://localhost:11222 -f -\" >/dev/null 2>&1";
            std::ignore = system(cmd.c_str());
        }

        // IMPORTANT: Connect to SECOND server only (like Java test line 79)
        // Client should discover first server via topology updates
        if (MultiServerTestEnvironment::servers.size() >= 2) {
            const auto& server2 = MultiServerTestEnvironment::servers[1];

            client = new RemoteCache(server2.host, server2.port, TEST_CACHE);

            // Enable topology awareness
            client->setClientIntelligence(ClientIntelligence::TOPOLOGY_AWARE);

            client->connect();
            allClients.push_back(client);
        }
    }

    static void TearDownTestSuite() {
        // Clean up client
        if (client) {
            client->disconnect();
            delete client;
            client = nullptr;
        }
        allClients.clear();

        // Cluster will be stopped by global environment
    }

    void SetUp() override {
        // Clear cache before each test
        clearTestCache();
    }

    void clearTestCache() {
        for (const auto& server : MultiServerTestEnvironment::servers) {
            if (server.port == 0) continue;

            std::string cmd = "curl -s -X DELETE "
                            "'http://" + server.host + ":" + std::to_string(server.port) +
                            "/rest/v3/caches/" + std::string(TEST_CACHE) + "/_clear"
                            "2>/dev/null";
            std::ignore = system(cmd.c_str());
        }
    }

    // Helper: Perform operations to trigger topology update
    // Java: expectTopologyChange() - does 10 PUTs to trigger updates
    void triggerTopologyUpdate(int numOps = 10) {
        for (int i = 0; i < numOps; i++) {
            std::string keyStr = "k" + std::to_string(i);
            std::string valueStr = "v" + std::to_string(i);

            ByteArray key(keyStr.begin(), keyStr.end());
            ByteArray value(valueStr.begin(), valueStr.end());

            client->put(key, value);

            // Small delay to allow topology response processing
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

// Static member initialization
RemoteCache* ReplTopologyChangeTest::client = nullptr;
std::vector<RemoteCache*> ReplTopologyChangeTest::allClients;

// Test 1: Client discovers 2 servers
// Java: testTwoMembers()
TEST_F(ReplTopologyChangeTest, TestTwoMembers) {
    ASSERT_NE(nullptr, client) << "Client should be initialized";

    // Verify 2 servers are running
    EXPECT_EQ(2, MultiServerTestEnvironment::numServers);

    // Client connected to server2, should discover server1 via topology
    triggerTopologyUpdate();

    // TODO: Add topology tracking to RemoteCache
    // Java equivalent: assertEquals(2, dispatcher.getServers().size());
    // For now, verify client can still communicate
    EXPECT_TRUE(client->ping()) << "Client should still be connected";

    // Verify data was written
    ByteArray key = {'k', '0'};
    ByteArray value;
    EXPECT_TRUE(client->get(key, value)) << "Data should be retrievable";
}

// Test 2: Add 3rd server, client discovers it
// Java: testAddNewServer()
TEST_F(ReplTopologyChangeTest, TestAddNewServer) {
    ASSERT_NE(nullptr, client) << "Client should be initialized";

    // Add 3rd node
    MultiServerTestEnvironment::addNode(3);
    MultiServerTestEnvironment::waitForClusterSize(3);

    EXPECT_EQ(3, MultiServerTestEnvironment::numServers);

    // Trigger topology update - client should discover 3rd server
    triggerTopologyUpdate();

    // TODO: Add topology tracking to RemoteCache
    // Java equivalent: assertEquals(3, dispatcher.getServers().size());
    EXPECT_TRUE(client->ping()) << "Client should still be connected";

    // Verify data is accessible with 3 servers
    for (int i = 0; i < 10; i++) {
        std::string keyStr = "k" + std::to_string(i);
        ByteArray key(keyStr.begin(), keyStr.end());
        ByteArray value;
        EXPECT_TRUE(client->get(key, value)) << "Key " << keyStr << " should be accessible";
    }
}

// Test 3: Remove server, client detects it
// Java: testDropServer()
TEST_F(ReplTopologyChangeTest, TestDropServer) {
    ASSERT_NE(nullptr, client) << "Client should be initialized";

    // Remove node 3
    MultiServerTestEnvironment::removeNode(3);
    MultiServerTestEnvironment::waitForClusterSize(2);

    EXPECT_EQ(2, MultiServerTestEnvironment::numServers);

    // Trigger topology update - client should detect server removal
    triggerTopologyUpdate();

    // TODO: Add topology tracking to RemoteCache
    // Java equivalent: assertEquals(2, dispatcher.getServers().size());
    EXPECT_TRUE(client->ping()) << "Client should still be connected";

    // Verify data is still accessible with 2 servers
    for (int i = 0; i < 10; i++) {
        std::string keyStr = "k" + std::to_string(i);
        ByteArray key(keyStr.begin(), keyStr.end());
        ByteArray value;
        EXPECT_TRUE(client->get(key, value)) << "Key " << keyStr << " should still be accessible";
    }
}

// Main function - registers the multi-server global environment
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    // Start with 2 servers (like Java test)
    MultiServerTestEnvironment::initialServerCount = 2;

    // Register multi-server test environment (starts/stops cluster)
    ::testing::AddGlobalTestEnvironment(new MultiServerTestEnvironment());

    return RUN_ALL_TESTS();
}
