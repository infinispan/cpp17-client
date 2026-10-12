#include <tuple>
#include <gtest/gtest.h>
#include "hotrod/RemoteCache.h"
#include "hotrod/HotRodClientException.h"
#include "InfinispanTestEnvironment.h"
#include <cstdlib>
#include <string>

using namespace hotrod;
using namespace hotrod::test;

namespace {
// Create a cache via the server CLI (same approach as the other integration tests).
void createCacheViaCLI(const std::string& cacheName) {
    std::string cmd = "docker exec " + InfinispanTestEnvironment::containerID +
                     " bash -c \"echo 'create cache --template=org.infinispan.DIST_SYNC " + cacheName +
                     "' | /opt/infinispan/bin/cli.sh -c http://admin:password@localhost:11222 -f -\" >/dev/null 2>&1";
    std::ignore = system(cmd.c_str());
}
} // namespace

/**
 * Step 11a — Error handling integration tests.
 *
 * Exercises the real server ERROR (0x50) path end-to-end: an operation against a
 * cache that does not exist makes the server reply with an ERROR response, which
 * the client must surface as a typed HotRodClientException carrying the server
 * status and message — not desync the connection.
 *
 * Reference: docs/ERROR_HANDLING_DESIGN.md §3.1.
 */

// A cache name that is never created → server returns an ERROR response.
static const char* kMissingCache = "no_such_cache_11a";

// Operating on an undefined cache surfaces a typed HotRodClientException.
TEST(ErrorHandlingIntegrationTest, ServerErrorOnMissingCache) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      kMissingCache);
    cache.connect();

    ByteArray key = {'k', 'e', 'y'};

    try {
        cache.get(key).get();
        FAIL() << "expected a HotRodClientException for a missing cache";
    } catch (const HotRodClientException& e) {
        EXPECT_EQ(FailurePhase::ServerError, e.phase);
        EXPECT_TRUE(e.serverStatus.has_value());
        EXPECT_STRNE("", e.what());  // server-provided message propagated
    }

    cache.disconnect();
}

// The typed exception is still catchable as std::runtime_error (back-compat).
TEST(ErrorHandlingIntegrationTest, ServerErrorCatchableAsRuntimeError) {
    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      kMissingCache);
    cache.connect();

    ByteArray key = {'k'};
    EXPECT_THROW({ cache.get(key).get(); }, std::runtime_error);

    cache.disconnect();
}

// After a server ERROR the connection is still usable — the ERROR body was fully
// drained, so the stream stayed in sync. A real round-trip on the same connection
// against a valid cache must succeed (a desync would hang or return garbage).
TEST(ErrorHandlingIntegrationTest, ConnectionUsableAfterServerError) {
    createCacheViaCLI("err_ok_cache");

    RemoteCache cache(InfinispanTestEnvironment::host,
                      InfinispanTestEnvironment::port,
                      kMissingCache);
    cache.connect();

    ByteArray key = {'k'};
    EXPECT_THROW({ cache.get(key).get(); }, HotRodClientException);

    // Same connection, now target the valid cache: PUT then GET must round-trip.
    cache.setCacheName("err_ok_cache");
    ByteArray value = {'v'};
    ASSERT_NO_THROW({ cache.put(key, value).get(); });
    auto got = cache.get(key).get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(value, *got);

    cache.disconnect();
}

// Main function - registers the global environment
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new InfinispanTestEnvironment());
    return RUN_ALL_TESTS();
}
