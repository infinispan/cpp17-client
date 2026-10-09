cmake_minimum_required(VERSION 3.15)
project(hotrod-client-cpp VERSION 0.1.0 LANGUAGES CXX)

# Default to an optimized build (with debug symbols) so local builds surface
# the same optimizer/-Werror warnings as CI (which builds Release). Override
# with -DCMAKE_BUILD_TYPE=Debug for an unoptimized, easier-to-step build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# C++17 required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -pedantic)
    # Enable _FORTIFY_SOURCE on optimized builds so glibc's fortified
    # warnings (e.g. warn_unused_result on system()) fire locally, matching
    # CI on distros that don't auto-enable it. It requires optimization, so
    # skip Debug (-O0); -U avoids redefinition warnings on distros that
    # already predefine it.
    add_compile_options($<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
                        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
endif()

# Find dependencies
find_package(OpenSSL REQUIRED)

# Library source files
set(SOURCES
    src/codec/Codec.cpp
    src/codec/HeaderCodec.cpp
    src/auth/SCRAM.cpp
    src/auth/SaslAuthenticator.cpp
    src/topology/TopologyInfo.cpp
    src/hash/MurmurHash3.cpp
    src/hash/ConsistentHash.cpp
    src/transport/Connection.cpp
    src/transport/MultiplexedConnection.cpp
    src/operations/RemoteCache.cpp
)

# Create library
add_library(hotrod-client STATIC ${SOURCES})

target_include_directories(hotrod-client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(hotrod-client
    PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(hotrod-client PUBLIC ws2_32)
endif()

# Testing
enable_testing()
find_package(GTest)

if(GTest_FOUND)
    # Unit tests
    add_executable(unit_tests
        tests/unit/CodecTest.cpp
        tests/unit/HeaderCodecTest.cpp
        tests/unit/SCRAMTest.cpp
        # tests/unit/TopologyInfoTest.cpp  # Removed - uses deprecated parseTopologyUpdate()
        tests/unit/MurmurHash3Test.cpp
        tests/unit/ConsistentHashTest.cpp
        tests/unit/PingTest.cpp
        tests/unit/GetTest.cpp
        tests/unit/GetWithMetadataTest.cpp
        tests/unit/PutTest.cpp
        tests/unit/RemoveTest.cpp
        tests/unit/RemoveWithVersionTest.cpp
        tests/unit/ReplaceWithVersionTest.cpp
        tests/unit/PutIfAbsentTest.cpp
        tests/unit/ReplaceTest.cpp
        tests/unit/ContainsKeyTest.cpp
        tests/unit/HashAwareRoutingTest.cpp
        tests/unit/ErrorHandlingTest.cpp
        tests/unit/ServerSelectionTest.cpp
        tests/unit/AuthCodecTest.cpp
        tests/unit/HotRodURITest.cpp
        tests/unit/SaslAuthenticatorTest.cpp
    )

    target_link_libraries(unit_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            GTest::gtest_main
    )

    add_test(NAME UnitTests COMMAND unit_tests)

    # Copy test scripts and configs to build directory
    file(COPY ${CMAKE_SOURCE_DIR}/scripts
         DESTINATION ${CMAKE_BINARY_DIR}
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                          GROUP_READ GROUP_EXECUTE
                          WORLD_READ WORLD_EXECUTE)

    file(COPY ${CMAKE_SOURCE_DIR}/test-configs
         DESTINATION ${CMAKE_BINARY_DIR})

    # Integration tests: they use POSIX APIs (popen/pclose), bash scripts and
    # Docker to drive a real Infinispan server, so they are only built on
    # non-Windows platforms. They are excluded from CI ctest runs via the
    # "integration" label regardless.
    if(NOT WIN32)
    add_executable(ping_integration_tests
        tests/integration/PingIntegrationTest.cpp
    )

    add_executable(get_integration_tests
        tests/integration/GetIntegrationTest.cpp
    )

    add_executable(getwithmetadata_integration_tests
        tests/integration/GetWithMetadataIntegrationTest.cpp
    )

    add_executable(put_integration_tests
        tests/integration/PutIntegrationTest.cpp
    )

    add_executable(remove_integration_tests
        tests/integration/RemoveIntegrationTest.cpp
    )

    add_executable(removewithversion_integration_tests
        tests/integration/RemoveWithVersionIntegrationTest.cpp
    )

    add_executable(replacewithversion_integration_tests
        tests/integration/ReplaceWithVersionIntegrationTest.cpp
    )

    add_executable(putifabsent_integration_tests
        tests/integration/PutIfAbsentIntegrationTest.cpp
    )

    add_executable(replace_integration_tests
        tests/integration/ReplaceIntegrationTest.cpp
    )

    add_executable(containskey_integration_tests
        tests/integration/ContainsKeyIntegrationTest.cpp
    )

    add_executable(errorhandling_integration_tests
        tests/integration/ErrorHandlingIntegrationTest.cpp
    )

    # Hot Rod URI end-to-end (single server) — fromUri parse->connect->op chain
    add_executable(fromuri_integration_tests
        tests/integration/FromUriIntegrationTest.cpp
    )

    # Topology integration tests (multi-server cluster)
    add_executable(topology_integration_tests
        tests/integration/TopologyChangeTest.cpp
    )

    # Concurrent clients test (single server, BASIC intelligence)
    add_executable(concurrent_clients_tests
        tests/integration/ConcurrentClientsTest.cpp
    )

    target_link_libraries(ping_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(get_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(getwithmetadata_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(put_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(remove_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(removewithversion_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(replacewithversion_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(putifabsent_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(replace_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(containskey_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(errorhandling_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(topology_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(concurrent_clients_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    # Hash-aware routing integration tests
    add_executable(hash_aware_routing_integration_tests
        tests/integration/HashAwareRoutingIntegrationTest.cpp
    )

    target_link_libraries(hash_aware_routing_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    # Concurrent multi-server integration tests
    add_executable(concurrent_multiserver_tests
        tests/integration/ConcurrentMultiServerTest.cpp
    )

    target_link_libraries(concurrent_multiserver_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    # RetryView (Step 11b user-decided retry) integration tests (multi-server)
    add_executable(retryview_integration_tests
        tests/integration/RetryViewIntegrationTest.cpp
    )

    target_link_libraries(retryview_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    # Ping keyless-op retry (Step 11c) integration tests (multi-server)
    add_executable(pingretry_integration_tests
        tests/integration/PingRetryIntegrationTest.cpp
    )

    target_link_libraries(pingretry_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    # SASL/SCRAM authentication integration tests (auth-enabled single server)
    add_executable(auth_integration_tests
        tests/integration/AuthIntegrationTest.cpp
    )

    target_link_libraries(auth_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    target_link_libraries(fromuri_integration_tests
        PRIVATE
            hotrod-client
            GTest::gtest
            # Note: NOT gtest_main - we provide our own main()
    )

    add_test(NAME PingIntegrationTests COMMAND ping_integration_tests)
    add_test(NAME GetIntegrationTests COMMAND get_integration_tests)
    add_test(NAME GetWithMetadataIntegrationTests COMMAND getwithmetadata_integration_tests)
    add_test(NAME PutIntegrationTests COMMAND put_integration_tests)
    add_test(NAME RemoveIntegrationTests COMMAND remove_integration_tests)
    add_test(NAME RemoveWithVersionIntegrationTests COMMAND removewithversion_integration_tests)
    add_test(NAME ReplaceWithVersionIntegrationTests COMMAND replacewithversion_integration_tests)
    add_test(NAME PutIfAbsentIntegrationTests COMMAND putifabsent_integration_tests)
    add_test(NAME ReplaceIntegrationTests COMMAND replace_integration_tests)
    add_test(NAME ContainsKeyIntegrationTests COMMAND containskey_integration_tests)
    add_test(NAME ErrorHandlingIntegrationTests COMMAND errorhandling_integration_tests)
    add_test(NAME FromUriIntegrationTests COMMAND fromuri_integration_tests)
    add_test(NAME TopologyIntegrationTests COMMAND topology_integration_tests)
    add_test(NAME ConcurrentClientsTests COMMAND concurrent_clients_tests)
    add_test(NAME HashAwareRoutingIntegrationTests COMMAND hash_aware_routing_integration_tests)
    add_test(NAME ConcurrentMultiServerTests COMMAND concurrent_multiserver_tests)
    add_test(NAME RetryViewIntegrationTests COMMAND retryview_integration_tests)
    add_test(NAME PingRetryIntegrationTests COMMAND pingretry_integration_tests)
    add_test(NAME AuthIntegrationTests COMMAND auth_integration_tests)

    # Set working directory so scripts can be found
    set_tests_properties(PingIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(GetIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(GetWithMetadataIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(PutIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(RemoveIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(RemoveWithVersionIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(ReplaceWithVersionIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(PutIfAbsentIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(ReplaceIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(ContainsKeyIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(ErrorHandlingIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(FromUriIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker"
    )
    set_tests_properties(TopologyIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker;topology;multi-server"
    )
    set_tests_properties(ConcurrentClientsTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker;concurrent"
    )
    set_tests_properties(HashAwareRoutingIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker;topology;multi-server;hash-aware"
    )
    set_tests_properties(ConcurrentMultiServerTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker;concurrent;multi-server"
    )
    set_tests_properties(RetryViewIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker;topology;multi-server;retry"
    )
    set_tests_properties(PingRetryIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker;topology;multi-server;retry"
    )
    set_tests_properties(AuthIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "integration;docker;auth"
    )
    endif()  # NOT WIN32 (integration tests)

    message(STATUS "Google Test found - unit tests enabled (integration tests: non-Windows only)")
else()
    message(WARNING "Google Test not found - tests disabled")
endif()

# Installation
install(TARGETS hotrod-client
    EXPORT hotrod-client-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)
