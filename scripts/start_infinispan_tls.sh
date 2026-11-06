#!/bin/bash
# Start Infinispan server with TLS enabled on the Hot Rod endpoint (issue #18).
#
# Mirrors start_infinispan_auth.sh but mounts a TLS server-identity config plus
# the PKCS12 keystore the server presents. No SASL — this exercises
# server-authenticated TLS in isolation. The client trusts test-configs/tls/ca.pem.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$SCRIPT_DIR/../test-configs"
CONFIG_FILE="$CONFIG_DIR/infinispan-tls.xml"
KEYSTORE_FILE="$CONFIG_DIR/tls/server.p12"
CA_FILE="$CONFIG_DIR/tls/ca.pem"

for f in "$CONFIG_FILE" "$KEYSTORE_FILE" "$CA_FILE"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Config file not found: $f" >&2
        exit 1
    fi
done

CONF=/opt/infinispan/server/conf

# Start container with the TLS config + keystore mounted into the conf dir.
CONTAINER_ID=$(docker run -d \
    -p 0:11222 \
    -v "$CONFIG_FILE:$CONF/infinispan.xml:ro" \
    -v "$KEYSTORE_FILE:$CONF/server.p12:ro" \
    quay.io/infinispan/server:latest)

# Wait for server ready (ISPN080001 = "Infinispan Server started")
echo "Waiting for Infinispan server to start (TLS mode)..." >&2
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
echo "export ISPN_TLS=true"
echo "export ISPN_CA_FILE=$CA_FILE"

echo "Server started: $HOST:$PORT (container: $CONTAINER_ID, TLS mode)" >&2
