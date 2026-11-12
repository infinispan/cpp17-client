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
 * The client always verifies the server's certificate chain. It additionally
 * presents a client certificate (mutual TLS) when `cert_file`/`key_file` are set.
 * All certificate material is PEM, loaded directly by OpenSSL; Java keystore
 * formats (JKS/PKCS12) are intentionally not supported for this native/OpenSSL
 * client (see docs/DECISIONS.md).
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
 *  - cert_file: path to a PEM file holding the client certificate chain presented
 *    to the server for mutual TLS. Empty means "no client certificate" (the
 *    default; server-authenticated TLS only). Set together with key_file.
 *  - key_file: path to a PEM file holding the client private key matching
 *    cert_file. Empty means none. cert_file and key_file must be set together; a
 *    cert without a key (or vice versa) is a configuration error caught at handshake.
 *  - key_password: passphrase for an encrypted key_file. Empty means the key is
 *    unencrypted. Supplied to OpenSSL so an encrypted key never blocks on a
 *    terminal passphrase prompt.
 *
 * Reference:
 * - Java: org.infinispan.client.hotrod.configuration.SslConfiguration
 *   (hostnameValidation default true; sniHostName; trustStore and keyStore). Java's
 *   trust/key stores are keystore containers; the PEM equivalents here are
 *   ca_file (trust) and cert_file/key_file (client identity).
 */
struct TlsConfig {
    bool        enabled             = false;
    std::string ca_file;                         // PEM; empty = system trust store
    std::string sni_host;                        // empty = derive from host (skip for IPs)
    bool        hostname_validation = true;      // Java's default
    std::string cert_file;                       // PEM client cert chain (mTLS); empty = none
    std::string key_file;                        // PEM client private key; empty = none
    std::string key_password;                    // passphrase for an encrypted key_file
};

}  // namespace hotrod
