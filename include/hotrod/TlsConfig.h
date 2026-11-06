#pragma once

#include <string>

namespace hotrod {

/**
 * TLS transport configuration for a client connection (issue #18, sub-issue of #10).
 *
 * Pure data, applied before connect(). When `enabled` is true the client wraps
 * every socket it opens — the seed connection and every topology-discovered
 * cluster member — in a TLS session (OpenSSL) before the connection is used for
 * operations.
 *
 * This first slice is *server-authenticated* TLS only: the client verifies the
 * server's certificate chain, but does not present a client certificate (mutual
 * TLS is deferred — see issue #10). Certificate material is PEM, loaded directly
 * by OpenSSL; Java keystore formats (JKS/PKCS12) are intentionally not supported
 * for this native/OpenSSL client (see docs/DECISIONS.md).
 *
 * Fields:
 *  - ca_file: path to a PEM file holding the trusted CA certificate(s) used to
 *    verify the server. Empty means "use OpenSSL's default/system trust store"
 *    (SSL_CTX_set_default_verify_paths), which is correct for servers whose
 *    certificates chain to a publicly trusted CA.
 *  - sni_host: the SNI server name sent in the handshake. Empty means "derive
 *    from the connection host" (and skip SNI entirely when the host is a literal
 *    IP address, per RFC 6066 — SNI carries host names, not addresses).
 *  - hostname_validation: when true (the default, matching the Java client) the
 *    server certificate must match the connection host name. When false only the
 *    certificate *chain* is verified, not the host name — chain verification is
 *    always on; this toggle governs the host-name match alone.
 *
 * Reference:
 * - Java: org.infinispan.client.hotrod.configuration.SslConfiguration
 *   (hostnameValidation default true; sniHostName; trustStore*).
 */
struct TlsConfig {
    bool        enabled             = false;
    std::string ca_file;                         // PEM; empty = system trust store
    std::string sni_host;                        // empty = derive from host (skip for IPs)
    bool        hostname_validation = true;      // Java's default
};

}  // namespace hotrod
