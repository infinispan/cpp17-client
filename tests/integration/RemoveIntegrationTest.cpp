#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"

using namespace hotrod;
using namespace hotrod::test;

/**
 * REMOVE Integration Tests - No Authentication
 *
 * Tests run against Infinispan server managed by InfinispanTestEnvironment.
 * Uses Hot Rod PUT, REMOVE, and GET for complete validation.
 *
 * Reference:
 * - ROADMAP Step 9: REMOVE operation
 */

namespace {

// Helper: Create cache via CLI (same approach as Go client)
void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

} // anonymous namespace

// Test 1: PUT→REMOVE→GET (key should not exist after remove)
TEST(RemoveIntegrationTest, PutRemoveGet) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    // PUT
    ByteArray key = {'r', 'e', 'm', 'k', 'e', 'y', '1'};
    ByteArray value = {'r', 'e', 'm', 'v', 'a', 'l', '1'};
    auto putHadPrevious = cache.put(key, value).get();
    EXPECT_FALSE(putHadPrevious.has_value());

    // REMOVE
    auto prevValue = cache.remove(key,true).get();
    EXPECT_TRUE(prevValue.has_value());  // Key existed
    // Note: Previous value may or may not be returned depending on server config
    // If server returns it (status 0x03), verify it matches
    // If not (status 0x00), previousValue will be empty

    // GET (should not exist)
    auto retrievedValue = cache.get(key).get();
    EXPECT_FALSE(retrievedValue.has_value());  // Key should not exist

    cache.disconnect();
}

// Test 2: REMOVE non-existent key
TEST(RemoveIntegrationTest, RemoveNonExistent) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'n', 'o', 'e', 'x', 'i', 's', 't'};
    auto prevValue = cache.remove(key).get();

    EXPECT_FALSE(prevValue.has_value());  // Key didn't exist

    cache.disconnect();
}

// Test 3: REMOVE with large value
TEST(RemoveIntegrationTest, RemoveLargeValue) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'b', 'i', 'g', 'r', 'e', 'm'};
    ByteArray value(1000, 'R');  // 1000 'R' characters

    // PUT
    cache.put(key, value);

    // REMOVE
    auto prevValue = cache.remove(key, true).get();

    EXPECT_TRUE(prevValue.has_value());  // Key existed
    // Note: Server may or may not return previous value (depends on config)
    // Just verify removal succeeded
    // If previousValue is returned, verify it
    if (prevValue.has_value() && prevValue->value.size() > 0) {
        EXPECT_EQ(1000, prevValue->value.size());
        EXPECT_EQ('R', prevValue->value[0]);
        EXPECT_EQ('R', prevValue->value[999]);
    }

    cache.disconnect();
}

// Test 4: REMOVE empty value
TEST(RemoveIntegrationTest, RemoveEmptyValue) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'e', 'm', 'p', 't', 'y', 'r', 'e', 'm'};
    ByteArray value;  // Empty

    // PUT
    cache.put(key, value);

    // REMOVE
    auto prevValue = cache.remove(key, true).get();

    EXPECT_TRUE(prevValue.has_value());
    // Empty value (whether returned or not, size should be 0)

    cache.disconnect();
}

// Test 5: Multiple REMOVEs on same connection
TEST(RemoveIntegrationTest, MultipleRemoves) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    // PUT 10 entries
    for (int i = 0; i < 10; i++) {
        std::string keyStr = "rmulti" + std::to_string(i);
        std::string valueStr = "rval" + std::to_string(i);

        ByteArray key(keyStr.begin(), keyStr.end());
        ByteArray value(valueStr.begin(), valueStr.end());

        cache.put(key, value);
    }

    // REMOVE all 10 entries
    for (int i = 0; i < 10; i++) {
        std::string keyStr = "rmulti" + std::to_string(i);
        ByteArray key(keyStr.begin(), keyStr.end());

        auto prevValue = cache.remove(key, true).get();

        EXPECT_TRUE(prevValue.has_value()) << "REMOVE #" << i << " failed";

        // Verify previous value if server returned it
        if (prevValue.has_value() && prevValue->value.size() > 0) {
            std::string expectedValue = "rval" + std::to_string(i);
            std::string actualValue(prevValue->value.begin(), prevValue->value.end());
            EXPECT_EQ(expectedValue, actualValue) << "Value mismatch for #" << i;
        }
    }

    cache.disconnect();
}

// Test 6: REMOVE without retrieving previous value
TEST(RemoveIntegrationTest, RemoveWithoutPreviousValue) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'n', 'o', 'p', 'r', 'e', 'v'};
    ByteArray value = {'v', 'a', 'l', 'u', 'e'};

    // PUT
    cache.put(key, value);

    // REMOVE without previous value parameter
    auto prevValue = cache.remove(key, true).get();

    EXPECT_TRUE(prevValue.has_value());

    // Verify key doesn't exist
    auto retrieved = cache.get(key).get();
    EXPECT_FALSE(retrieved.has_value());

    cache.disconnect();
}

// Test 7: REMOVE from different caches
TEST(RemoveIntegrationTest, RemoveFromDifferentCaches) {
    createCacheViaCLI("cache1");
    createCacheViaCLI("cache2");

    RemoteCache cache1(InfinispanTestEnvironment::host,
                       InfinispanTestEnvironment::port,
                       "cache1");
    cache1.connect();

    RemoteCache cache2(InfinispanTestEnvironment::host,
                       InfinispanTestEnvironment::port,
                       "cache2");
    cache2.connect();

    ByteArray key = {'s', 'h', 'a', 'r', 'e', 'd', 'r', 'e', 'm'};
    ByteArray value1 = {'c', '1'};
    ByteArray value2 = {'c', '2'};

    // PUT to both caches
    cache1.put(key, value1);
    cache2.put(key, value2);

    // REMOVE from cache1
    auto removed1 = cache1.remove(key, true).get();
    EXPECT_TRUE(removed1.has_value());  // Key existed in cache1

    // Verify cache1 doesn't have it
    auto found1 = cache1.get(key).get();
    EXPECT_FALSE(found1.has_value());

    // Verify cache2 still has it
    auto found2 = cache2.get(key).get();
    EXPECT_TRUE(found2.has_value());
    EXPECT_EQ(value2, found2.value());

    cache1.disconnect();   
    cache2.disconnect();
}

// Test 8: REMOVE after disconnect should fail
TEST(RemoveIntegrationTest, RemoveAfterDisconnect) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();
    cache.disconnect();

    ByteArray key = {'t', 'e', 's', 't'};

    EXPECT_THROW({
        cache.remove(key);
    }, std::runtime_error);
}

// Main function - registers the global environment
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    // Add global environment (starts/stops server once for all tests)
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());

    return RUN_ALL_TESTS();
}
