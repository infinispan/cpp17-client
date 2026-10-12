#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <chrono>
#include <future>

using namespace hotrod;
using namespace hotrod::test;

/**
 * Concurrent Clients Integration Test
 *
 * Tests multiple clients (with BASIC intelligence) performing operations concurrently
 * against a single Infinispan server.
 *
 * Purpose: Verify basic concurrent access patterns without topology awareness.
 */

namespace {

// Helper: Create cache via CLI
void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

} // anonymous namespace

// Test 1: Single client with concurrent interlaced operations on different keys
TEST(ConcurrentClientsTest, SingleClientConcurrentOperations) {
    const std::string cacheName = "concurrent-single";
    createCacheViaCLI(cacheName);

    RemoteCache cache(InfinispanTestEnvironment::host,
                     InfinispanTestEnvironment::port,
                     cacheName);
    cache.setClientIntelligence(ClientIntelligence::BASIC);
    cache.connect();

    const int NUM_KEYS = 100;

    // Phase 1: Issue all PUTs concurrently (non-blocking)
    std::vector<std::future<std::optional<EntryWithMetadata>>> putFutures;
    std::vector<std::string> keys;
    std::vector<std::string> values;

    for (int i = 0; i < NUM_KEYS; i++) {
        std::string keyStr = "key-" + std::to_string(i);
        std::string valueStr = "value-" + std::to_string(i);

        keys.push_back(keyStr);
        values.push_back(valueStr);

        ByteArray key(keyStr.begin(), keyStr.end());
        ByteArray value(valueStr.begin(), valueStr.end());

        putFutures.push_back(cache.put(key, value));
    }

    // Phase 2: Issue all GETs in different order (interlaced)
    std::vector<std::future<std::optional<ByteArray>>> getFutures;
    for (int i = NUM_KEYS - 1; i >= 0; i--) {  // Reverse order
        ByteArray key(keys[i].begin(), keys[i].end());
        getFutures.push_back(cache.get(key));
    }

    // Phase 3: Issue REMOVEs for every 3rd key
    std::vector<std::future<std::optional<EntryWithMetadata>>> removeFutures;
    for (int i = 0; i < NUM_KEYS; i += 3) {
        ByteArray key(keys[i].begin(), keys[i].end());
        removeFutures.push_back(cache.remove(key));
    }

    // Wait for all PUTs
    for (auto& fut : putFutures) {
        fut.get();
    }

    // Verify all GETs (in reverse order)
    int errors = 0;
    for (int i = 0; i < NUM_KEYS; i++) {
        auto result = getFutures[i].get();
        if (!result.has_value()) {
            errors++;
            continue;
        }
        std::string retrieved(result.value().begin(), result.value().end());
        std::string expected = values[NUM_KEYS - 1 - i];  // Reverse order
        if (retrieved != expected) {
            errors++;
        }
    }

    // Wait for all REMOVEs
    for (auto& fut : removeFutures) {
        fut.get();
    }

    cache.disconnect();

    EXPECT_EQ(0, errors) << "Single client concurrent operations had errors";
}

// Test 2: 4 concurrent clients, each performing 10 PUTs and 10 GETs
TEST(ConcurrentClientsTest, FourClientsBasicIntelligence) {
    // Ensure cache exists
    const std::string cacheName = "concurrent-test";
    createCacheViaCLI(cacheName);

    const int NUM_CLIENTS = 4;
    const int OPS_PER_CLIENT = 10;

    std::vector<std::thread> threads;
    std::vector<bool> results(NUM_CLIENTS, true);

    // Lambda: Each client performs 10 PUTs then 10 GETs
    auto clientWork = [&](int clientId) {
        try {
            // Create client with BASIC intelligence (default)
            RemoteCache cache(InfinispanTestEnvironment::host,
                            InfinispanTestEnvironment::port,
                            cacheName);

            // Explicitly set BASIC intelligence (no topology awareness)
            cache.setClientIntelligence(ClientIntelligence::BASIC);

            cache.connect();

            // Perform 10 PUTs
            for (int i = 0; i < OPS_PER_CLIENT; i++) {
                std::string keyStr = "client" + std::to_string(clientId) +
                                    "-key" + std::to_string(i);
                std::string valueStr = "client" + std::to_string(clientId) +
                                      "-value" + std::to_string(i);

                ByteArray key(keyStr.begin(), keyStr.end());
                ByteArray value(valueStr.begin(), valueStr.end());

                cache.put(key, value);
            }

            // Perform 10 GETs (verify what we just wrote)
            for (int i = 0; i < OPS_PER_CLIENT; i++) {
                std::string keyStr = "client" + std::to_string(clientId) +
                                    "-key" + std::to_string(i);
                std::string expectedValueStr = "client" + std::to_string(clientId) +
                                              "-value" + std::to_string(i);

                ByteArray key(keyStr.begin(), keyStr.end());

                auto found = cache.get(key).get();
                if (!found.has_value()) {
                    results[clientId] = false;
                    return;
                }

                std::string actualValueStr(found.value().begin(), found.value().end());
                if (actualValueStr != expectedValueStr) {
                    results[clientId] = false;
                    return;
                }
            }

            cache.disconnect();

        } catch (const std::exception& e) {
            results[clientId] = false;
        }
    };

    // Launch 4 concurrent clients
    for (int i = 0; i < NUM_CLIENTS; i++) {
        threads.emplace_back(clientWork, i);
    }

    // Wait for all clients to finish
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all clients succeeded
    for (int i = 0; i < NUM_CLIENTS; i++) {
        EXPECT_TRUE(results[i]) << "Client " << i << " failed";
    }
}

// Test 3: Multiple clients with interlaced operations
TEST(ConcurrentClientsTest, MultipleClientsInterlaced) {
    const std::string cacheName = "concurrent-interlaced";
    createCacheViaCLI(cacheName);

    const int NUM_CLIENTS = 5;
    const int OPS_PER_CLIENT = 30;

    std::vector<std::thread> clientThreads;
    std::atomic<int> totalErrors{0};

    // Each client performs interlaced PUT/GET/REMOVE
    auto clientWork = [&](int clientId) {
        try {
            RemoteCache cache(InfinispanTestEnvironment::host,
                            InfinispanTestEnvironment::port,
                            cacheName);
            cache.setClientIntelligence(ClientIntelligence::BASIC);
            cache.connect();

            std::vector<std::future<std::optional<EntryWithMetadata>>> putFutures;
            std::vector<std::future<std::optional<ByteArray>>> getFutures;
            std::vector<std::future<std::optional<EntryWithMetadata>>> removeFutures;

            // Issue all operations concurrently (non-blocking)
            for (int i = 0; i < OPS_PER_CLIENT; i++) {
                std::string keyStr = "client" + std::to_string(clientId) +
                                    "-key" + std::to_string(i);
                std::string valueStr = "value-" + std::to_string(clientId) +
                                      "-" + std::to_string(i);

                ByteArray key(keyStr.begin(), keyStr.end());
                ByteArray value(valueStr.begin(), valueStr.end());

                // Interlace: PUT, GET, PUT, GET, REMOVE pattern
                putFutures.push_back(cache.put(key, value));
                getFutures.push_back(cache.get(key));

                if (i % 2 == 0) {
                    removeFutures.push_back(cache.remove(key));
                }
            }

            // Wait for all PUTs
            for (auto& fut : putFutures) {
                fut.get();
            }

            // Verify all GETs (some may not exist due to REMOVE)
            for (auto& fut : getFutures) {
                fut.get();  // Just consume the results
            }

            // Wait for all REMOVEs
            for (auto& fut : removeFutures) {
                fut.get();
            }

            cache.disconnect();

        } catch (const std::exception& e) {
            fprintf(stderr, "[Client %d ERROR] %s\n", clientId, e.what());
            totalErrors++;
        }
    };

    // Launch clients
    for (int i = 0; i < NUM_CLIENTS; i++) {
        clientThreads.emplace_back(clientWork, i);
    }

    // Wait for all
    for (auto& t : clientThreads) {
        t.join();
    }

    EXPECT_EQ(0, totalErrors.load()) << "Multiple clients interlaced operations had errors";
}

// Test 4: Stress test with many concurrent operations on single connection (different keys)
TEST(ConcurrentClientsTest, StressTestSingleConnection) {
    const std::string cacheName = "concurrent-stress";
    createCacheViaCLI(cacheName);

    RemoteCache cache(InfinispanTestEnvironment::host,
                     InfinispanTestEnvironment::port,
                     cacheName);
    cache.setClientIntelligence(ClientIntelligence::BASIC);
    cache.connect();

    const int NUM_OPERATIONS = 200;

    std::vector<std::string> keys;
    std::vector<std::string> values;

    auto startTime = std::chrono::steady_clock::now();

    // Phase 1: Issue all PUTs (non-blocking)
    std::vector<std::future<std::optional<EntryWithMetadata>>> putFutures;
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        std::string keyStr = "stress-key-" + std::to_string(i);
        std::string valueStr = "stress-value-" + std::to_string(i);

        keys.push_back(keyStr);
        values.push_back(valueStr);

        ByteArray key(keyStr.begin(), keyStr.end());
        ByteArray value(valueStr.begin(), valueStr.end());

        putFutures.push_back(cache.put(key, value));
    }

    // Phase 2: Immediately issue all GETs (before PUTs complete, testing interlacing)
    std::vector<std::future<std::optional<ByteArray>>> getFutures;
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        ByteArray key(keys[i].begin(), keys[i].end());
        getFutures.push_back(cache.get(key));
    }

    // Phase 3: Wait for all PUTs
    for (auto& fut : putFutures) {
        fut.get();
    }

    // Phase 4: Verify all GETs
    int completedOps = 0;
    int errors = 0;

    for (int i = 0; i < NUM_OPERATIONS; i++) {
        auto result = getFutures[i].get();

        if (result.has_value()) {
            std::string retrieved(result.value().begin(), result.value().end());
            if (retrieved == values[i]) {
                completedOps++;
            } else {
                errors++;
            }
        } else {
            errors++;
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    cache.disconnect();

    fprintf(stderr, "[STRESS] Completed %d/%d operations in %ld ms (%.2f ops/sec)\n",
            completedOps, NUM_OPERATIONS, duration.count(),
            (NUM_OPERATIONS * 2 * 1000.0) / duration.count());  // 2x for PUT+GET

    EXPECT_EQ(NUM_OPERATIONS, completedOps) << "Stress test: some operations failed";
    EXPECT_EQ(0, errors) << "Stress test had errors";
}

// Main function
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    // Register single-server test environment
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());

    return RUN_ALL_TESTS();
}
