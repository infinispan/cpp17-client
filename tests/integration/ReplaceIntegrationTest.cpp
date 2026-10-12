#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"

using namespace hotrod;
using namespace hotrod::test;

/**
 * REPLACE Integration Tests - No Auth
 *
 * Replaces only when the key is present. Reference:
 * - ROADMAP Step 10: conditional operations
 * - Java: org.infinispan.client.hotrod.impl.operations.ReplaceOperation
 */

namespace {

void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

} // anonymous namespace

// Test 1: key present -> value is replaced
TEST(ReplaceIntegrationTest, ReplacesWhenPresent) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'p', '1'};
    ByteArray value1 = {'o', 'l', 'd'};
    ByteArray value2 = {'n', 'e', 'w'};
    cache.put(key, value1).get();

    cache.replace(key, value2).get();

    auto after = cache.get(key).get();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(value2, after.value());

    cache.disconnect();
}

// Test 2: key absent -> nothing stored
TEST(ReplaceIntegrationTest, DoesNotStoreWhenAbsent) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'p', 'n', 'o', 'p', 'e'};
    ByteArray value = {'x'};
    cache.remove(key).get();  // ensure absent

    auto prev = cache.replace(key, value).get();
    EXPECT_FALSE(prev.has_value());

    auto after = cache.get(key).get();
    EXPECT_FALSE(after.has_value());  // must not have been created

    cache.disconnect();
}

// Test 3: key present + previousValue -> returns the replaced entry
TEST(ReplaceIntegrationTest, ReturnsPreviousWhenReplaced) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'r', 'p', '3'};
    ByteArray value1 = {'o', 'l', 'd'};
    ByteArray value2 = {'n', 'e', 'w'};
    cache.put(key, value1).get();

    auto prev = cache.replace(key, value2, 0, 0, true).get();
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(value1, prev->value);

    auto after = cache.get(key).get();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(value2, after.value());

    cache.disconnect();
}

// Test 4: replace after disconnect should throw
TEST(ReplaceIntegrationTest, ReplaceAfterDisconnect) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();
    cache.disconnect();

    ByteArray key = {'t', 'e', 's', 't'};
    ByteArray value = {'v'};
    EXPECT_THROW({
        cache.replace(key, value);
    }, std::runtime_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());
    return RUN_ALL_TESTS();
}
