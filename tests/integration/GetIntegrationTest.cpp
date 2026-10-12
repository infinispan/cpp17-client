#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"
#include <cstdlib>
#include <sstream>
#include <iostream>

using namespace hotrod;
using namespace hotrod::test;

/**
 * GET Integration Tests - No Authentication
 *
 * Tests run against Infinispan server managed by InfinispanTestEnvironment.
 * Uses Hot Rod PUT to store data, then Hot Rod GET to retrieve it.
 *
 * Reference:
 * - ROADMAP Step 7: Cross-client validation
 * - Pure Hot Rod protocol testing (PUT→GET round-trip)
 */

namespace {

// Helper: Create cache via CLI (same approach as Go client)
void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

// Helper: PUT via Hot Rod protocol
void putViaHotRod(const std::string& cacheName, const std::string& key, const std::string& value) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                     InfinispanTestEnvironment::port,
                     cacheName);
    cache.connect();

    ByteArray keyBytes(key.begin(), key.end());
    ByteArray valueBytes(value.begin(), value.end());
    cache.put(keyBytes, valueBytes).get();  // Async API: call .get() to block

    cache.disconnect();
}

} // anonymous namespace

// Test 1: GET existing key (PUT via REST)
TEST(GetIntegrationTest, GetExistingKey) {
    // Setup: Create cache and PUT key
    createCacheViaCLI("testcache");
    putViaHotRod("testcache", "key1", "value1");

    // Test: GET key via Hot Rod
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'k', 'e', 'y', '1'};
    auto value = cache.get(key).get();

    ASSERT_TRUE(value.has_value());
    ASSERT_EQ(6, value->size());

    std::string valueStr(value->begin(), value->end());
    EXPECT_EQ("value1", valueStr);

    cache.disconnect();

    // Note: Cleanup skipped - let cache TTL handle it
}

// Test 2: GET non-existent key
TEST(GetIntegrationTest, GetNonExistentKey) {
    // Setup: Create cache (no need for dummy key)
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'n', 'o', 't', 'f', 'o', 'u', 'n', 'd'};
    auto value = cache.get(key).get();

    EXPECT_FALSE(value.has_value());

    cache.disconnect();

    // Note: Cleanup skipped - let cache TTL handle it
}

// Test 3: GET with different cache
TEST(GetIntegrationTest, GetFromDifferentCache) {
    // Create both caches
    createCacheViaCLI("cache1");
    createCacheViaCLI("cache2");

    // PUT to cache1
    putViaHotRod("cache1", "sharedkey", "fromcache1");

    // PUT to cache2 with same key
    putViaHotRod("cache2", "sharedkey", "fromcache2");

    // GET from cache1
    RemoteCache cache1(InfinispanTestEnvironment::host,
                       InfinispanTestEnvironment::port,
                       "cache1");
    cache1.connect();

    ByteArray key = {'s', 'h', 'a', 'r', 'e', 'd', 'k', 'e', 'y'};
    auto value1 = cache1.get(key).get();

    ASSERT_TRUE(value1.has_value());
    std::string str1(value1->begin(), value1->end());
    EXPECT_EQ("fromcache1", str1);

    cache1.disconnect();

    // GET from cache2
    RemoteCache cache2(InfinispanTestEnvironment::host,
                       InfinispanTestEnvironment::port,
                       "cache2");
    cache2.connect();

    auto value2 = cache2.get(key).get();

    ASSERT_TRUE(value2.has_value());
    std::string str2(value2->begin(), value2->end());
    EXPECT_EQ("fromcache2", str2);

    cache2.disconnect();

    // Note: Cleanup skipped - let cache TTL handle it
}

// Test 4: Multiple GETs on same connection
TEST(GetIntegrationTest, MultipleGetsOnSameConnection) {
    // Setup: Create cache and PUT multiple keys
    createCacheViaCLI("testcache");
    putViaHotRod("testcache", "multi1", "val1");
    putViaHotRod("testcache", "multi2", "val2");
    putViaHotRod("testcache", "multi3", "val3");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    // GET key1
    ByteArray key1 = {'m', 'u', 'l', 't', 'i', '1'};
    auto value1 = cache.get(key1).get();
    ASSERT_TRUE(value1.has_value());
    EXPECT_EQ("val1", std::string(value1->begin(), value1->end()));

    // GET key2
    ByteArray key2 = {'m', 'u', 'l', 't', 'i', '2'};
    auto value2 = cache.get(key2).get();
    ASSERT_TRUE(value2.has_value());
    EXPECT_EQ("val2", std::string(value2->begin(), value2->end()));

    // GET key3
    ByteArray key3 = {'m', 'u', 'l', 't', 'i', '3'};
    auto value3 = cache.get(key3).get();
    ASSERT_TRUE(value3.has_value());
    EXPECT_EQ("val3", std::string(value3->begin(), value3->end()));

    cache.disconnect();

    // Note: Cleanup skipped - let cache TTL handle it
}

// Test 5: GET with large key
TEST(GetIntegrationTest, GetWithLargeKey) {
    // Create a large key (>127 bytes for multi-byte vInt)
    std::string largeKeyStr(200, 'X');
    ByteArray largeKey(largeKeyStr.begin(), largeKeyStr.end());

    // Setup: Create cache and PUT
    createCacheViaCLI("testcache");
    putViaHotRod("testcache", largeKeyStr, "largeKeyValue");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    auto value = cache.get(largeKey).get();

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ("largeKeyValue", std::string(value->begin(), value->end()));

    cache.disconnect();

    // Note: Cleanup skipped - let cache TTL handle it
}

// Test 6: GET with large value
TEST(GetIntegrationTest, GetWithLargeValue) {
    // Create a large value (1000 bytes)
    std::string largeValueStr(1000, 'Y');

    createCacheViaCLI("testcache");
    putViaHotRod("testcache", "bigval", largeValueStr);

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'b', 'i', 'g', 'v', 'a', 'l'};
    auto value = cache.get(key).get();

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(1000, value->size());
    EXPECT_EQ('Y', (*value)[0]);
    EXPECT_EQ('Y', (*value)[999]);

    cache.disconnect();

    // Note: Cleanup skipped - let cache TTL handle it
}

// Test 7: GET with empty value
TEST(GetIntegrationTest, GetWithEmptyValue) {
    // Create cache and PUT empty value
    createCacheViaCLI("testcache");
    putViaHotRod("testcache", "emptyval", "");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'e', 'm', 'p', 't', 'y', 'v', 'a', 'l'};
    auto value = cache.get(key).get();

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(0, value->size());

    cache.disconnect();

    // Note: Cleanup skipped - let cache TTL handle it
}

// Test 8: GET after disconnect should fail
TEST(GetIntegrationTest, GetAfterDisconnect) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();
    cache.disconnect();

    ByteArray key = {'t', 'e', 's', 't'};

    EXPECT_THROW({
        cache.get(key).get();
    }, std::runtime_error);
}

// Main function - registers the global environment
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    // Add global environment (starts/stops server once for all tests)
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());

    return RUN_ALL_TESTS();
}
