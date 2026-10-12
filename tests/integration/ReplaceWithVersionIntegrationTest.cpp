#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"

using namespace hotrod;
using namespace hotrod::test;

/**
 * REPLACE_WITH_VERSION (replaceIfUnmodified) Integration Tests - No Auth
 *
 * Version-based conditional replace (CAS). Uses PUT + getWithMetadata to obtain
 * the entry version, then replaceWithVersion to conditionally update the value.
 *
 * Reference:
 * - ROADMAP Step 10: metadata / version-based operations
 * - Java: org.infinispan.client.hotrod.impl.operations.ReplaceIfUnmodifiedOperation
 */

namespace {

void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

} // anonymous namespace

// Test 1: matching version -> value is replaced
TEST(ReplaceWithVersionIntegrationTest, MatchingVersionReplaces) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'p', 'v', '1'};
    ByteArray value1 = {'o', 'l', 'd'};
    ByteArray value2 = {'n', 'e', 'w'};
    cache.put(key, value1).get();

    auto entry = cache.getWithMetadata(key).get();
    ASSERT_TRUE(entry.has_value());
    int64_t version = entry->metadata.version;

    bool replaced = cache.replaceWithVersion(key, value2, version).get();
    EXPECT_TRUE(replaced);

    auto after = cache.get(key).get();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(value2, after.value());

    cache.disconnect();
}

// Test 2: stale version -> not replaced, original value survives
TEST(ReplaceWithVersionIntegrationTest, StaleVersionDoesNotReplace) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'p', 'v', '2'};
    ByteArray value1 = {'f', 'i', 'r', 's', 't'};
    cache.put(key, value1).get();

    auto entry1 = cache.getWithMetadata(key).get();
    ASSERT_TRUE(entry1.has_value());
    int64_t staleVersion = entry1->metadata.version;

    // Overwrite so the version changes.
    ByteArray value2 = {'s', 'e', 'c', 'o', 'n', 'd'};
    cache.put(key, value2).get();

    // Replace with the stale version must fail.
    ByteArray value3 = {'t', 'h', 'i', 'r', 'd'};
    bool replaced = cache.replaceWithVersion(key, value3, staleVersion).get();
    EXPECT_FALSE(replaced);

    // Value remains the second one, not the attempted third.
    auto after = cache.get(key).get();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(value2, after.value());

    cache.disconnect();
}

// Test 3: missing key -> not replaced
TEST(ReplaceWithVersionIntegrationTest, MissingKeyDoesNotReplace) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'p', 'v', 'n', 'o', 'p', 'e'};
    ByteArray value = {'x'};
    bool replaced = cache.replaceWithVersion(key, value, 123456789LL).get();
    EXPECT_FALSE(replaced);

    auto after = cache.get(key).get();
    EXPECT_FALSE(after.has_value());  // must not have been created

    cache.disconnect();
}

// Test 4: version advances after a successful replace (chained CAS)
TEST(ReplaceWithVersionIntegrationTest, VersionAdvancesAfterReplace) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'p', 'v', '4'};
    cache.put(key, ByteArray{'a'}).get();

    auto e1 = cache.getWithMetadata(key).get();
    ASSERT_TRUE(e1.has_value());
    int64_t v1 = e1->metadata.version;

    ASSERT_TRUE(cache.replaceWithVersion(key, ByteArray{'b'}, v1).get());

    // Old version now stale; a second replace with v1 must fail.
    EXPECT_FALSE(cache.replaceWithVersion(key, ByteArray{'c'}, v1).get());

    // Re-read version and replace succeeds again.
    auto e2 = cache.getWithMetadata(key).get();
    ASSERT_TRUE(e2.has_value());
    EXPECT_NE(v1, e2->metadata.version);
    EXPECT_TRUE(cache.replaceWithVersion(key, ByteArray{'c'}, e2->metadata.version).get());

    cache.disconnect();
}

// Test 5: replaceWithVersion after disconnect should fail
TEST(ReplaceWithVersionIntegrationTest, ReplaceWithVersionAfterDisconnect) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();
    cache.disconnect();

    ByteArray key = {'t', 'e', 's', 't'};
    ByteArray value = {'v'};
    EXPECT_THROW({
        cache.replaceWithVersion(key, value, 1);
    }, std::runtime_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());
    return RUN_ALL_TESTS();
}
