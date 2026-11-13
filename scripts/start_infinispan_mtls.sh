#!/bin/bash
# Start Infinispan with mutual TLS on the Hot Rod endpoint (issue #10 integration
# coverage). The endpoint requires a client certificate (require-ssl-client-auth),
# verified against a PEM truststore (test-configs/tls/client-ca.pem).
#
# One script serves two configs, selected by the first argument:
#   infinispan-mtls.xml      (default) — mutual TLS, NO SASL (MutualTlsIntegrationTest)
#   infinispan-external.xml            — mutual TLS + SASL EXTERNAL (ExternalAuthIntegrationTest)
#
# Mounts the server PKCS12 identity (server.p12), the client CA PEM truststore
# (client-ca.pem), and the chosen config. Exports the server trust anchor the
# client needs (ca.pem) plus the client cert/key it must present.

set -e

CONFIG_NAME="${1:-infinispan-mtls.xml}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$SCRIPT_DIR/../test-configs"
CONFIG_FILE="$CONFIG_DIR/$CONFIG_NAME"
KEYSTORE_FILE="$CONFIG_DIR/tls/server.p12"
TRUSTSTORE_FILE="$CONFIG_DIR/tls/client-truststore.pem"
CA_FILE="$CONFIG_DIR/tls/ca.pem"
CLIENT_CERT_FILE="$CONFIG_DIR/tls/client.pem"
CLIENT_KEY_FILE="$CONFIG_DIR/tls/client.key"

for f in "$CONFIG_FILE" "$KEYSTORE_FILE" "$TRUSTSTORE_FILE" "$CA_FILE" \
         "$CLIENT_CERT_FILE" "$CLIENT_KEY_FILE"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Config file not found: $f" >&2
        exit 1
    fi
done

CONF=/opt/infinispan/server/conf

# Start container with the mutual-TLS config + server keystore + client-CA truststore
# mounted into the conf dir.
# --pull always: :latest is unpinned; refresh so a stale local cache can't run an old server.
CONTAINER_ID=$(docker run -d --pull always \
    -p 0:11222 \
    -v "$CONFIG_FILE:$CONF/infinispan.xml:ro" \
    -v "$KEYSTORE_FILE:$CONF/server.p12:ro" \
    -v "$TRUSTSTORE_FILE:$CONF/client-truststore.pem:ro" \
    quay.io/infinispan/server:latest)

# Wait for server ready (ISPN080001 = "Infinispan Server started")
echo "Waiting for Infinispan server to start (mutual TLS, config: $CONFIG_NAME)..." >&2
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
echo "export ISPN_CLIENT_CERT_FILE=$CLIENT_CERT_FILE"
echo "export ISPN_CLIENT_KEY_FILE=$CLIENT_KEY_FILE"

echo "Server started: $HOST:$PORT (container: $CONTAINER_ID, mutual TLS, config: $CONFIG_NAME)" >&2
