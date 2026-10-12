#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"

using namespace hotrod;
using namespace hotrod::test;

/**
 * CONTAINS_KEY Integration Tests - No Auth
 *
 * Reference:
 * - ROADMAP Step 10: conditional operations
 * - Java: org.infinispan.client.hotrod.impl.operations.ContainsKeyOperation
 */

namespace {

void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

} // anonymous namespace

// Test 1: existing key -> true
TEST(ContainsKeyIntegrationTest, ReturnsTrueWhenPresent) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'c', 'k', '1'};
    ByteArray value = {'v'};
    cache.put(key, value).get();

    EXPECT_TRUE(cache.containsKey(key).get());

    cache.disconnect();
}

// Test 2: absent key -> false
TEST(ContainsKeyIntegrationTest, ReturnsFalseWhenAbsent) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'c', 'k', 'n', 'o', 'p', 'e'};
    cache.remove(key).get();  // ensure absent

    EXPECT_FALSE(cache.containsKey(key).get());

    cache.disconnect();
}

// Test 3: reflects removal
TEST(ContainsKeyIntegrationTest, ReflectsRemoval) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'c', 'k', '3'};
    ByteArray value = {'v'};
    cache.put(key, value).get();
    EXPECT_TRUE(cache.containsKey(key).get());

    cache.remove(key).get();
    EXPECT_FALSE(cache.containsKey(key).get());

    cache.disconnect();
}

// Test 4: containsKey after disconnect should throw
TEST(ContainsKeyIntegrationTest, ContainsKeyAfterDisconnect) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();
    cache.disconnect();

    ByteArray key = {'t', 'e', 's', 't'};
    EXPECT_THROW({
        cache.containsKey(key);
    }, std::runtime_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());
    return RUN_ALL_TESTS();
}
