#pragma once

#include "Types.h"
#include <string>

namespace hotrod {

/**
 * Pure, I/O-free builders for the SASL *initial responses* of the single-message
 * (non-SCRAM) mechanisms this client supports. Kept header-only and side-effect
 * free so the exact wire bytes are unit-testable in isolation (same precedent as
 * AuthCodec.h / ServerSelection.h). SaslAuthenticator wraps these in the
 * AUTH_MECH_LIST + AUTH exchange.
 *
 * These mechanisms complete in a single client message (the server answers with
 * AUTH_RESPONSE completed=true on success, or an ERROR frame on failure), unlike
 * the multi-turn SCRAM family. OAUTHBEARER has one extra client turn only on the
 * failure path (RFC 7628), which SaslAuthenticator handles.
 *
 * References:
 *  - PLAIN:       RFC 4616  — message = authzid NUL authcid NUL passwd
 *  - EXTERNAL:    RFC 4422 §A — message = authorization identity (often empty)
 *  - OAUTHBEARER: RFC 7628  — gs2-header + ^Aauth=Bearer <token>^A^A  (^A = 0x01)
 */

/**
 * PLAIN (RFC 4616): authzid NUL authcid NUL passwd. The response carries NUL
 * bytes, so it is built as raw bytes (never a C string). An empty authzid means
 * "authorize as the authentication id", which is the usual case.
 */
inline ByteArray saslPlainInitialResponse(const std::string& authzid,
                                          const std::string& authcid,
                                          const std::string& password) {
    ByteArray out;
    out.reserve(authzid.size() + authcid.size() + password.size() + 2);
    out.insert(out.end(), authzid.begin(), authzid.end());
    out.push_back(0x00);
    out.insert(out.end(), authcid.begin(), authcid.end());
    out.push_back(0x00);
    out.insert(out.end(), password.begin(), password.end());
    return out;
}

/**
 * EXTERNAL (RFC 4422): the initial response is the authorization identity, which
 * is typically empty — the identity is taken from the mutual-TLS client
 * certificate. A non-empty authzid requests authorization as that identity.
 */
inline ByteArray saslExternalInitialResponse(const std::string& authzid) {
    return ByteArray(authzid.begin(), authzid.end());
}

/**
 * OAUTHBEARER (RFC 7628) client-first:
 *   gs2-header  kvsep  "auth=Bearer " token  kvsep kvsep
 * where gs2-header = "n," [ "a=" authzid ] "," and kvsep = 0x01. With an empty
 * authzid the header is "n,,".
 */
inline ByteArray saslOAuthBearerInitialResponse(const std::string& authzid,
                                                const std::string& token) {
    constexpr char kvsep = 0x01;
    std::string msg = "n,";
    if (!authzid.empty()) {
        msg += "a=";
        msg += authzid;
    }
    msg += ",";
    msg += kvsep;
    msg += "auth=Bearer ";
    msg += token;
    msg += kvsep;
    msg += kvsep;
    return ByteArray(msg.begin(), msg.end());
}

}  // namespace hotrod
