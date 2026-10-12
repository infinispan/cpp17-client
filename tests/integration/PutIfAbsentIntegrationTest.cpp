#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "InfinispanTestEnvironment.h"

using namespace hotrod;
using namespace hotrod::test;

/**
 * PUT_IF_ABSENT Integration Tests - No Auth
 *
 * Stores only when the key is absent. Reference:
 * - ROADMAP Step 10: conditional operations
 * - Java: org.infinispan.client.hotrod.impl.operations.PutIfAbsentOperation
 */

namespace {

void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}

} // anonymous namespace

// Test 1: key absent -> value is stored
TEST(PutIfAbsentIntegrationTest, StoresWhenAbsent) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'p', 'i', 'a', '1'};
    ByteArray value = {'v', '1'};
    cache.remove(key).get();  // ensure absent

    auto prev = cache.putIfAbsent(key, value).get();
    EXPECT_FALSE(prev.has_value());  // stored, no previous

    auto after = cache.get(key).get();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(value, after.value());

    cache.disconnect();
}

// Test 2: key present -> not overwritten
TEST(PutIfAbsentIntegrationTest, DoesNotOverwriteWhenPresent) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'p', 'i', 'a', '2'};
    ByteArray first = {'f', 'i', 'r', 's', 't'};
    ByteArray second = {'s', 'e', 'c', 'o', 'n', 'd'};
    cache.put(key, first).get();

    cache.putIfAbsent(key, second).get();  // must not overwrite

    auto after = cache.get(key).get();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(first, after.value());  // original survives

    cache.disconnect();
}

// Test 3: key present + previousValue -> returns the existing entry
TEST(PutIfAbsentIntegrationTest, ReturnsPreviousWhenPresent) {
    createCacheViaCLI("testcache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();

    ByteArray key = {'p', 'i', 'a', '3'};
    ByteArray existing = {'e', 'x'};
    ByteArray attempt = {'n', 'o'};
    cache.put(key, existing).get();

    auto prev = cache.putIfAbsent(key, attempt, 0, 0, true).get();
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(existing, prev->value);

    cache.disconnect();
}

// Test 4: putIfAbsent after disconnect should throw
TEST(PutIfAbsentIntegrationTest, PutIfAbsentAfterDisconnect) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      "testcache");
    cache.connect();
    cache.disconnect();

    ByteArray key = {'t', 'e', 's', 't'};
    ByteArray value = {'v'};
    EXPECT_THROW({
        cache.putIfAbsent(key, value);
    }, std::runtime_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());
    return RUN_ALL_TESTS();
}
