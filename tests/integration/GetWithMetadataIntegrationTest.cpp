#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"
#include <cstdlib>
#include <string>

using namespace hotrod;
using namespace hotrod::test;

/**
 * GET_WITH_METADATA Integration Tests - No Authentication
 *
 * Tests run against Infinispan server managed by InfinispanTestEnvironment.
 * Stores data via Hot Rod PUT, then retrieves value + metadata via
 * Hot Rod GET_WITH_METADATA (opcode 0x1B/0x1C).
 *
 * Reference:
 * - ROADMAP Step 10: Metadata operations
 * - Java: org.infinispan.client.hotrod.impl.operations.GetWithMetadataOperation
 */

namespace {

// Helper: Create cache via CLI (same approach as other integration suites)
void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

// Helper: PUT via Hot Rod protocol (immortal entry)
void putViaHotRod(const std::string& cacheName, const std::string& key, const std::string& value) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                     InfinispanTestEnvironment::port,
                     cacheName);
    cache.connect();
    ByteArray keyBytes(key.begin(), key.end());
    ByteArray valueBytes(value.begin(), value.end());
    cache.put(keyBytes, valueBytes).get();
    cache.disconnect();
}

// Helper: PUT with lifespan + maxIdle (seconds)
void putWithExpiry(const std::string& cacheName, const std::string& key,
                   const std::string& value, uint64_t lifespan, uint64_t maxIdle) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                     InfinispanTestEnvironment::port,
                     cacheName);
    cache.connect();
    ByteArray keyBytes(key.begin(), key.end());
    ByteArray valueBytes(value.begin(), value.end());
    cache.put(keyBytes, valueBytes, lifespan, maxIdle).get();
    cache.disconnect();
}

} // anonymous namespace

// Test 1: getWithMetadata on existing key returns value + a non-zero version
TEST(GetWithMetadataIntegrationTest, ExistingKeyReturnsValueAndVersion) {
    createCacheViaCLI("testcache");
    putViaHotRod("testcache", "metakey", "metavalue");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'m', 'e', 't', 'a', 'k', 'e', 'y'};
    auto entry = cache.getWithMetadata(key).get();

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ("metavalue", std::string(entry->value.begin(), entry->value.end()));
    EXPECT_NE(0, entry->metadata.version);  // server assigns a non-zero version

    cache.disconnect();
}

// Test 2: getWithMetadata on non-existent key returns nullopt
TEST(GetWithMetadataIntegrationTest, NonExistentKeyReturnsNullopt) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'n', 'o', 'm', 'e', 't', 'a'};
    auto entry = cache.getWithMetadata(key).get();

    EXPECT_FALSE(entry.has_value());

    cache.disconnect();
}

// Test 3: getWithMetadata reflects finite lifespan/maxIdle set at PUT time
TEST(GetWithMetadataIntegrationTest, FiniteExpirationMetadata) {
    createCacheViaCLI("testcache");
    putWithExpiry("testcache", "expirekey", "expirevalue", /*lifespan*/ 3600, /*maxIdle*/ 600);

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'e', 'x', 'p', 'i', 'r', 'e', 'k', 'e', 'y'};
    auto entry = cache.getWithMetadata(key).get();

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ("expirevalue", std::string(entry->value.begin(), entry->value.end()));

    // Finite expiration => lifespan/maxIdle present with the values we set
    ASSERT_TRUE(entry->metadata.lifespan.has_value());
    EXPECT_EQ(3600u, entry->metadata.lifespan.value());
    ASSERT_TRUE(entry->metadata.maxIdle.has_value());
    EXPECT_EQ(600u, entry->metadata.maxIdle.value());
    ASSERT_TRUE(entry->metadata.created.has_value());
    ASSERT_TRUE(entry->metadata.lastUsed.has_value());

    cache.disconnect();
}

// Test 4: immortal entry reports infinite expiration (lifespan/maxIdle absent)
TEST(GetWithMetadataIntegrationTest, InfiniteExpirationMetadata) {
    createCacheViaCLI("testcache");
    putViaHotRod("testcache", "immortal", "forever");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'i', 'm', 'm', 'o', 'r', 't', 'a', 'l'};
    auto entry = cache.getWithMetadata(key).get();

    ASSERT_TRUE(entry.has_value());
    EXPECT_FALSE(entry->metadata.lifespan.has_value());
    EXPECT_FALSE(entry->metadata.maxIdle.has_value());

    cache.disconnect();
}

// Test 5: large value round-trips with metadata intact
TEST(GetWithMetadataIntegrationTest, LargeValueWithMetadata) {
    std::string largeValueStr(1000, 'Z');
    createCacheViaCLI("testcache");
    putViaHotRod("testcache", "bigmeta", largeValueStr);

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'b', 'i', 'g', 'm', 'e', 't', 'a'};
    auto entry = cache.getWithMetadata(key).get();

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(1000u, entry->value.size());
    EXPECT_EQ('Z', entry->value[0]);
    EXPECT_EQ('Z', entry->value[999]);
    EXPECT_NE(0, entry->metadata.version);

    cache.disconnect();
}

// Test 6: overwriting a key yields a different version
TEST(GetWithMetadataIntegrationTest, VersionChangesOnOverwrite) {
    createCacheViaCLI("testcache");
    putViaHotRod("testcache", "verkey", "v1");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'v', 'e', 'r', 'k', 'e', 'y'};
    auto first = cache.getWithMetadata(key).get();
    ASSERT_TRUE(first.has_value());

    // Overwrite and read again
    ByteArray newValue = {'v', '2'};
    cache.put(key, newValue).get();
    auto second = cache.getWithMetadata(key).get();
    ASSERT_TRUE(second.has_value());

    EXPECT_EQ("v2", std::string(second->value.begin(), second->value.end()));
    EXPECT_NE(first->metadata.version, second->metadata.version);

    cache.disconnect();
}

// Test 7: getWithMetadata after disconnect should fail
TEST(GetWithMetadataIntegrationTest, GetWithMetadataAfterDisconnect) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();
    cache.disconnect();

    ByteArray key = {'t', 'e', 's', 't'};

    EXPECT_THROW({
        cache.getWithMetadata(key).get();
    }, std::runtime_error);
}

// Main function - registers the global environment
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());
    return RUN_ALL_TESTS();
}
