#pragma once

#include <string>

namespace hotrod {

/**
 * SASL authentication configuration for a client connection.
 *
 * Pure data, applied before connect(). When `enabled` is true the client runs
 * the SASL handshake (AUTH_MECH_LIST + AUTH) on every connection it opens — the
 * seed connection and every topology-discovered cluster member — before the
 * connection is used for operations.
 *
 * Supported mechanisms:
 *  - SCRAM family: SCRAM-SHA-1 / SCRAM-SHA-256 / SCRAM-SHA-512 — uses
 *    `username` + `password`.
 *  - PLAIN — uses `username` + `password` (send over TLS).
 *  - EXTERNAL — identity comes from the mutual-TLS client certificate; no
 *    credentials needed (`username`, if set, is sent as the authorization id).
 *  - OAUTHBEARER — uses `token` (a bearer token); `username`, if set, is sent as
 *    the authorization id.
 * Any other configured mechanism throws a HotRodClientException at connect time.
 * `realm` and `serverName` are carried for parity with the Java client but are
 * not used on the wire by these mechanisms.
 *
 * Reference:
 * - Java: org.infinispan.client.hotrod.configuration.AuthenticationConfiguration
 *   (defaults: mechanism "SCRAM-SHA-256", realm "default", serverName "infinispan")
 */
struct Authentication {
    bool        enabled     = false;
    std::string username;
    std::string password;
    std::string token;                          // bearer token for OAUTHBEARER
    std::string realm       = "default";
    std::string serverName  = "infinispan";
    std::string mechanism   = "SCRAM-SHA-256";
};

} // namespace hotrod
