#pragma once

#include <tuple>
#include <gtest/gtest.h>
#include "MultiServerTestEnvironment.h"
#include "hotrod/RemoteCache.h"
#include <memory>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>

namespace hotrod {
namespace test {

/**
 * Test fixture for topology integration tests.
 *
 * Provides helper methods for:
 * - Creating clients connected to specific servers
 * - Waiting for topology updates
 * - Verifying cluster state
 * - Cleaning up test data
 */
class TopologyTest : public ::testing::Test {
protected:
    // Test data cache name
    static constexpr const char* TEST_CACHE = "topology-test";

    void SetUp() override {
        // Reset to initial cluster size (3 nodes by default)
        resetCluster(MultiServerTestEnvironment::initialServerCount);

        // Create test cache (only needs to be done on one node)
        createTestCache();

        // Clear test cache on all active nodes
        clearTestCache();
    }

    void TearDown() override {
        // Clean up any created clients
        for (auto& client : clients) {
            if (client) {
                client->disconnect();
                client.reset();
            }
        }
        clients.clear();

        // Clear test data
        clearTestCache();
    }

    /**
     * Reset cluster to specified size.
     * Ensures exactly N nodes are running.
     */
    void resetCluster(int targetSize) {
        if (targetSize < 2 || targetSize > 4) {
            throw std::invalid_argument("Target cluster size must be between 2 and 4");
        }

        int currentSize = MultiServerTestEnvironment::numServers;

        // Add nodes if needed
        while (currentSize < targetSize) {
            currentSize++;
            if (!MultiServerTestEnvironment::isNodeActive(currentSize)) {
                MultiServerTestEnvironment::addNode(currentSize);
                MultiServerTestEnvironment::waitForClusterSize(currentSize);
            }
        }

        // Remove nodes if needed
        while (currentSize > targetSize) {
            if (MultiServerTestEnvironment::isNodeActive(currentSize)) {
                MultiServerTestEnvironment::removeNode(currentSize);
                MultiServerTestEnvironment::waitForClusterSize(targetSize);
            }
            currentSize--;
        }
    }

    /**
     * Create a RemoteCache client connected to specific server.
     * @param serverIndex Server index (0-based)
     * @return Pointer to RemoteCache (ownership transferred to test)
     */
    RemoteCache* createClient(int serverIndex, const std::string& cacheName = TEST_CACHE) {
        if (serverIndex < 0 || serverIndex >= MultiServerTestEnvironment::numServers) {
            throw std::out_of_range("Server index out of range: " + std::to_string(serverIndex));
        }

        const auto& server = MultiServerTestEnvironment::getServer(serverIndex);

        auto client = std::make_unique<RemoteCache>(server.host, server.port, cacheName);

        // Enable hash-distribution awareness for topology tests
        client->setClientIntelligence(ClientIntelligence::HASH_DISTRIBUTION_AWARE);

        client->connect();

        RemoteCache* ptr = client.get();
        // Tests may call createClient() concurrently from several threads (e.g.
        // ConcurrentMultiServerTest.MultipleClientsInterlaced), so guard the
        // shared vector: std::vector::push_back is not safe for concurrent
        // callers (a reallocation moves the buffer out from under another
        // thread). This lock keeps TSan runs clean so real production races stay
        // visible.
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            clients.push_back(std::move(client));
        }

        return ptr;
    }

    /**
     * Create client connected to first server (node 1).
     */
    RemoteCache* createDefaultClient(const std::string& cacheName = TEST_CACHE) {
        return createClient(0, cacheName);
    }

    /**
     * Wait for topology update by performing operations.
     * The client receives topology updates during operations (PUT, GET, etc.).
     *
     * @param expectedServerCount Expected number of servers in topology
     * @param maxAttempts Maximum number of operation attempts
     */
    void waitForTopologyUpdate(RemoteCache* cache, int /*expectedServerCount*/, int maxAttempts = 20) {
        // Perform operations to trigger topology update
        // (expectedServerCount will be used when topology API is added to RemoteCache)
        for (int i = 0; i < maxAttempts; i++) {
            std::string key = "topo-probe-" + std::to_string(i);
            ByteArray keyBytes(key.begin(), key.end());
            ByteArray valueBytes = {1, 2, 3};

            cache->put(keyBytes, valueBytes);

            // Check if topology has updated
            // (In future: add getTopologySize() method to RemoteCache)
            // For now: rely on waitForClusterSize on server side

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    /**
     * Get current cluster size from server perspective.
     * Uses REST API to query cluster status.
     */
    int getServerClusterSize() {
        if (MultiServerTestEnvironment::servers.empty()) {
            return 0;
        }

        const auto& server = MultiServerTestEnvironment::servers[0];

        // Use curl to query cluster size
        std::string cmd = "curl -s --digest -u admin:password "
                         "'http://" + server.host + ":" + std::to_string(server.port) +
                         "/rest/v3/cluster/_distribution' 2>/dev/null | "
                         "grep -o 'node_name' | wc -l";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return -1;
        }

        char buffer[128];
        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        pclose(pipe);

        try {
            return std::stoi(result);
        } catch (...) {
            return -1;
        }
    }

    /**
     * Create test cache on the first node (cluster will sync to others).
     */
    void createTestCache() {
        if (MultiServerTestEnvironment::servers.empty()) {
            return;
        }

        const auto& server = MultiServerTestEnvironment::servers[0];

        // Anonymous mode - no authentication
        std::string cmd = "docker exec " + server.containerID +
                         " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " +
                         std::string(TEST_CACHE) +
                         "' | /opt/infinispan/bin/cli.sh -c http://localhost:11222 -f -\" >/dev/null 2>&1";
        std::ignore = system(cmd.c_str());
    }

    /**
     * Clear test cache on all servers via REST API.
     */
    void clearTestCache() {
        for (const auto& server : MultiServerTestEnvironment::servers) {
            if (server.port == 0) continue;  // Skip inactive nodes

            std::string cmd = "curl -s --digest -u admin:password -X POST "
                             "'http://" + server.host + ":" + std::to_string(server.port) +
                             "/rest/v3/caches/" + std::string(TEST_CACHE) + "/_clear' "
                             "2>/dev/null";
            std::ignore = system(cmd.c_str());
        }
    }

    /**
     * Add a node to the cluster and wait for it to join.
     */
    void addNode(int nodeNumber) {
        MultiServerTestEnvironment::addNode(nodeNumber);
        MultiServerTestEnvironment::waitForClusterSize(MultiServerTestEnvironment::numServers);
    }

    /**
     * Remove a node from the cluster and wait for rebalance.
     */
    void removeNode(int nodeNumber) {
        int currentSize = MultiServerTestEnvironment::numServers;
        MultiServerTestEnvironment::removeNode(nodeNumber);
        MultiServerTestEnvironment::waitForClusterSize(currentSize - 1);
    }

    // Storage for created clients (auto-cleanup in TearDown).
    // Guarded by clientsMutex because createClient() can be called concurrently;
    // TearDown runs single-threaded after all worker threads have joined.
    std::vector<std::unique_ptr<RemoteCache>> clients;
    std::mutex clientsMutex;
};

} // namespace test
} // namespace hotrod
