#!/bin/bash
# Regenerate the test TLS certificate fixtures used by the #18 TLS suites.
#
# The fixtures are committed to git and normally need no regeneration (they carry
# ~100-year validity, so they do not expire). This script exists to *document* how
# they were built and to reproduce them when something must change — e.g. a new SAN,
# a different key type, or mutual-TLS client certs (issue #10).
#
# It produces certificates with the SAME PROPERTIES as the committed fixtures
# (subjects, SANs, RSA-2048, SHA-256, long validity), NOT byte-identical files:
# keys, serials and timestamps are freshly random each run. After running you must
# `git add` the regenerated files if you intend to replace the committed set.
#
# Two independent sets are produced:
#   tests/fixtures/tls/   — unit test in-process TlsEchoServer (TlsConnectionTest)
#   test-configs/tls/     — Docker integration server identity   (TlsIntegrationTest)
#
# CA private keys are intentionally NOT written to the tree (they are not part of
# the committed fixtures); each run regenerates the whole chain together.
#
# Usage:
#   scripts/gen-test-certs.sh
# Override output locations (used by the project's own self-test):
#   UNIT_DIR=/tmp/u INT_DIR=/tmp/i scripts/gen-test-certs.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

UNIT_DIR="${UNIT_DIR:-$REPO/tests/fixtures/tls}"
INT_DIR="${INT_DIR:-$REPO/test-configs/tls}"
DAYS="${DAYS:-36500}"          # ~100 years, matches the committed fixtures
P12_PASS="${P12_PASS:-password}"  # server.p12 password expected by the integration test

mkdir -p "$UNIT_DIR" "$INT_DIR"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# make_ca <out-pem> <out-key> <CN>
# Self-signed root. Stock `req -x509` adds critical basicConstraints CA:TRUE plus
# Subject/Authority Key Identifier (the default v3_ca extensions) — matching the
# committed CAs.
make_ca() {
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$DAYS" \
        -keyout "$2" -out "$1" -subj "/CN=$3" 2>/dev/null
}

# make_leaf <ca-pem> <ca-key> <out-key> <out-pem> <CN> <SAN>
# Leaf signed by the given CA, carrying only a subjectAltName (as the fixtures do).
make_leaf() {
    openssl req -newkey rsa:2048 -nodes -sha256 \
        -keyout "$3" -out "$TMP/leaf.csr" -subj "/CN=$5" 2>/dev/null
    openssl x509 -req -in "$TMP/leaf.csr" -CA "$1" -CAkey "$2" \
        -CAserial "$TMP/$(basename "$1").srl" -CAcreateserial \
        -sha256 -days "$DAYS" -out "$4" \
        -extfile <(printf 'subjectAltName=%s\n' "$6") 2>/dev/null
}

echo ">> unit fixtures -> $UNIT_DIR"
make_ca   "$UNIT_DIR/ca.pem"      "$TMP/ca.key"      "Test Root CA"
make_leaf "$UNIT_DIR/ca.pem"      "$TMP/ca.key" \
          "$UNIT_DIR/server.key"  "$UNIT_DIR/server.pem" \
          "test.local" "DNS:test.local"
chmod 600 "$UNIT_DIR/server.key"
# Second, unrelated root for the untrusted-anchor rejection test (key discarded).
make_ca   "$UNIT_DIR/otherca.pem" "$TMP/otherca.key" "Other Root CA"
# Client identity for the mutual-TLS tests: a client CA (committed, so the test
# server can trust it) and a client leaf signed by it (client.pem + client.key).
# The client CA *private* key is discarded, like the other CAs.
make_ca   "$UNIT_DIR/clientca.pem" "$TMP/clientca.key" "Test Client CA"
make_leaf "$UNIT_DIR/clientca.pem" "$TMP/clientca.key" \
          "$UNIT_DIR/client.key"  "$UNIT_DIR/client.pem" \
          "test-client" "DNS:test-client"
chmod 600 "$UNIT_DIR/client.key"

echo ">> integration certs -> $INT_DIR"
make_ca   "$INT_DIR/ca.pem"       "$TMP/int-ca.key"  "Integration Test CA"
make_leaf "$INT_DIR/ca.pem"       "$TMP/int-ca.key" \
          "$TMP/int-server.key"   "$TMP/int-server.pem" \
          "localhost" "DNS:localhost,IP:127.0.0.1"
# PKCS12 identity the Infinispan server presents: alias 'server', password 'password'.
openssl pkcs12 -export -out "$INT_DIR/server.p12" \
    -inkey "$TMP/int-server.key" -in "$TMP/int-server.pem" \
    -name server -passout "pass:$P12_PASS" 2>/dev/null

echo ">> done."
echo "   Regenerated fixtures are NOT byte-identical to the committed ones (fresh"
echo "   random keys/serials). 'git add' them only if you mean to replace the set."
