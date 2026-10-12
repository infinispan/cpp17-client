#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"

using namespace hotrod;
using namespace hotrod::test;

/**
 * REMOVE_WITH_VERSION (removeIfUnmodified) Integration Tests - No Authentication
 *
 * Version-based conditional remove (CAS). Uses PUT + getWithMetadata to obtain
 * the entry version, then removeWithVersion to conditionally delete it.
 *
 * Reference:
 * - ROADMAP Step 10: metadata / version-based operations
 * - Java: org.infinispan.client.hotrod.impl.operations.RemoveIfUnmodifiedOperation
 */

namespace {

// Helper: Create cache via CLI (same approach as the other integration tests)
void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

} // anonymous namespace

// Test 1: matching version -> entry is removed
TEST(RemoveWithVersionIntegrationTest, MatchingVersionRemoves) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'w', 'v', '1'};
    ByteArray value = {'v', 'a', 'l', '1'};
    cache.put(key, value).get();

    // Obtain the current version via getWithMetadata.
    auto entry = cache.getWithMetadata(key).get();
    ASSERT_TRUE(entry.has_value());
    int64_t version = entry->metadata.version;
    EXPECT_NE(0, version);

    bool removed = cache.removeWithVersion(key, version).get();
    EXPECT_TRUE(removed);

    // Entry is gone.
    auto after = cache.get(key).get();
    EXPECT_FALSE(after.has_value());

    cache.disconnect();
}

// Test 2: stale version (entry modified after read) -> not removed
TEST(RemoveWithVersionIntegrationTest, StaleVersionDoesNotRemove) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'w', 'v', '2'};
    ByteArray value1 = {'f', 'i', 'r', 's', 't'};
    cache.put(key, value1).get();

    // Read version v1, then overwrite the entry -> server bumps the version.
    auto entry1 = cache.getWithMetadata(key).get();
    ASSERT_TRUE(entry1.has_value());
    int64_t staleVersion = entry1->metadata.version;

    ByteArray value2 = {'s', 'e', 'c', 'o', 'n', 'd'};
    cache.put(key, value2).get();

    // Removing with the stale version must fail (version no longer matches).
    bool removed = cache.removeWithVersion(key, staleVersion).get();
    EXPECT_FALSE(removed);

    // Entry still present with the updated value.
    auto after = cache.get(key).get();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(value2, after.value());

    cache.disconnect();
}

// Test 3: missing key -> not removed
TEST(RemoveWithVersionIntegrationTest, MissingKeyDoesNotRemove) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'w', 'v', 'n', 'o', 'p', 'e'};
    bool removed = cache.removeWithVersion(key, 123456789LL).get();
    EXPECT_FALSE(removed);

    cache.disconnect();
}

// Test 4: re-using a matched version after removal -> not removed (no such key)
TEST(RemoveWithVersionIntegrationTest, SecondRemoveIsNoop) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'w', 'v', '4'};
    ByteArray value = {'x'};
    cache.put(key, value).get();

    auto entry = cache.getWithMetadata(key).get();
    ASSERT_TRUE(entry.has_value());
    int64_t version = entry->metadata.version;

    EXPECT_TRUE(cache.removeWithVersion(key, version).get());
    // Second attempt: key no longer exists -> false.
    EXPECT_FALSE(cache.removeWithVersion(key, version).get());

    cache.disconnect();
}

// Test 5: removeWithVersion after disconnect should fail
TEST(RemoveWithVersionIntegrationTest, RemoveWithVersionAfterDisconnect) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();
    cache.disconnect();

    ByteArray key = {'t', 'e', 's', 't'};
    EXPECT_THROW({
        cache.removeWithVersion(key, 1);
    }, std::runtime_error);
}

// Main function - registers the global environment
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());
    return RUN_ALL_TESTS();
}
