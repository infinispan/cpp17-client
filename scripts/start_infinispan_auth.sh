#!/bin/bash
# Start Infinispan server with SASL/SCRAM authentication enabled.
#
# Mirrors start_infinispan_noauth.sh but mounts a security-realm config plus the
# plain-text user/group property files, and exports ISPN_AUTH=true along with the
# credentials the auth integration test uses.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$SCRIPT_DIR/../test-configs"
CONFIG_FILE="$CONFIG_DIR/infinispan-auth.xml"
USERS_FILE="$CONFIG_DIR/auth-users.properties"
GROUPS_FILE="$CONFIG_DIR/auth-groups.properties"

# Credentials baked into auth-users.properties.
AUTH_USER=testuser
AUTH_PASS=testpassword

for f in "$CONFIG_FILE" "$USERS_FILE" "$GROUPS_FILE"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Config file not found: $f" >&2
        exit 1
    fi
done

CONF=/opt/infinispan/server/conf

# Start container with the auth config + property files mounted into the conf dir.
# --pull always: :latest is unpinned; refresh so a stale local cache can't run an old server.
CONTAINER_ID=$(docker run -d --pull always \
    -p 0:11222 \
    -v "$CONFIG_FILE:$CONF/infinispan.xml:ro" \
    -v "$USERS_FILE:$CONF/test-users.properties:ro" \
    -v "$GROUPS_FILE:$CONF/test-groups.properties:ro" \
    quay.io/infinispan/server:latest)

# Wait for server ready (ISPN080001 = "Infinispan Server started")
echo "Waiting for Infinispan server to start (auth mode)..." >&2
timeout 120 bash -c "
while ! docker logs $CONTAINER_ID 2>&1 | grep -q 'ISPN080001'; do
    sleep 1
done
" || {
    echo "ERROR: Server failed to start within 120 seconds" >&2
    docker logs $CONTAINER_ID >&2
    docker rm -f $CONTAINER_ID >&2
    exit 1
}

# Get mapped port
HOST=localhost
PORT=$(docker port $CONTAINER_ID 11222 | cut -d: -f2)

if [ -z "$PORT" ]; then
    echo "ERROR: Failed to get mapped port" >&2
    docker rm -f $CONTAINER_ID >&2
    exit 1
fi

# Export for C++ tests (stdout only - for parsing)
echo "export ISPN_CONTAINER_ID=$CONTAINER_ID"
echo "export ISPN_HOST=$HOST"
echo "export ISPN_PORT=$PORT"
echo "export ISPN_AUTH=true"
echo "export ISPN_USER=$AUTH_USER"
echo "export ISPN_PASS=$AUTH_PASS"

echo "Server started: $HOST:$PORT (container: $CONTAINER_ID, auth mode, user: $AUTH_USER)" >&2
