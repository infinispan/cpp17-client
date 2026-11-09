#!/bin/bash
# Start Infinispan server in anonymous mode (no authentication)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="$SCRIPT_DIR/../test-configs/infinispan-noauth.xml"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Config file not found: $CONFIG_FILE" >&2
    exit 1
fi

# Start container with custom XML mounted (anonymous mode).
# --pull always: :latest is unpinned; refresh so a stale local cache can't run an old server.
CONTAINER_ID=$(docker run -d --pull always \
    -p 0:11222 \
    -v "$CONFIG_FILE:/opt/infinispan/server/conf/infinispan.xml:ro" \
    quay.io/infinispan/server:latest)

# Wait for server ready (ISPN080001 = "Infinispan Server started")
echo "Waiting for Infinispan server to start (anonymous mode)..." >&2
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
echo "export ISPN_AUTH=false"

echo "Server started: $HOST:$PORT (container: $CONTAINER_ID, anonymous mode)" >&2
