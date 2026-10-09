#pragma once

#include "HeaderCodec.h"            // ClientIntelligence, Protocol::VERSION_*
#include "HotRodClientException.h"  // HotRodClientException, ServerAddress, FailurePhase
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <cctype>
#include <algorithm>

namespace hotrod {

/**
 * Parsed, validated Hot Rod connection URI (issue #9).
 *
 * Mirrors the Java client's org.infinispan.client.hotrod.impl.HotRodURI for the
 * subset this client supports today. The grammar is:
 *
 *   hotrod://[user:password@]host1[:port1][,host2[:port2]...][?k=v&...]
 *
 * Authoritative behaviour is the Java parser (AGENTS.md source order); the .NET
 * client docs linked from the issue paraphrase some values (notably the protocol
 * version form), so Java wins where they disagree.
 *
 * This is a *pure* value — no I/O, no RemoteCache dependency — so parsing and
 * validation are unit-testable in isolation. RemoteCache::fromUri() consumes it
 * and applies the flat setters.
 *
 * v1 scope (the rest is deferred to issue #10):
 *  - scheme: only "hotrod://". "hotrods://" (TLS) throws, since the transport has
 *    no TLS yet.
 *  - multiple comma-separated servers are parsed into `servers`, but the caller
 *    uses only the first as the seed (topology discovery finds the rest).
 *  - query parameters are a typed whitelist; an unknown key or a bad value for a
 *    known key throws. Deferred params (timeouts, TLS, use_ssl, non-SCRAM SASL)
 *    are therefore rejected rather than silently ignored.
 */
struct HotRodURI {
    std::vector<ServerAddress>       servers;             // all parsed, in order (>=1)
    std::optional<std::string>       username;
    std::optional<std::string>       password;
    std::optional<std::string>       saslMechanism;       // e.g. "SCRAM-SHA-256"
    std::optional<ClientIntelligence> clientIntelligence;
    std::optional<uint8_t>           protocolVersion;     // Protocol::VERSION_40 / _41

    /** The first server in the list — the seed used by RemoteCache in v1. */
    const ServerAddress& seed() const { return servers.front(); }

    /**
     * Parse and validate a Hot Rod URI string.
     * @throws HotRodClientException (FailurePhase::BeforeSend) on any malformed or
     *         unsupported element.
     */
    static HotRodURI parse(const std::string& uri);

private:
    [[noreturn]] static void fail(const std::string& message) {
        throw HotRodClientException(message, FailurePhase::BeforeSend);
    }

    static std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    /**
     * Percent-decode a single URI component (RFC 3986): "%XX" → the byte with
     * hex value XX, every other character verbatim. Mirrors the decoding the Java
     * client gets for free from java.net.URI.getUserInfo()/getQuery(), so a
     * credential or value carrying a URI-reserved character (e.g. an '@' or '/'
     * in a password, written "%40"/"%2F") reaches the client as its real bytes.
     *
     * A '+' is left as-is (java.net.URI does NOT treat it as a space — that is
     * form-encoding, not URI decoding). A malformed escape ('%' not followed by
     * two hex digits) throws, matching java.net.URI's parse-time rejection and
     * this parser's fail-fast stance.
     *
     * @param what  component name, used only in the error message.
     */
    static std::string percentDecode(const std::string& s, const char* what) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '%') {
                out.push_back(s[i]);
                continue;
            }
            if (i + 2 >= s.size() ||
                !std::isxdigit(static_cast<unsigned char>(s[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                fail(std::string("invalid percent-encoding in ") + what + ": " + s);
            }
            const int hi = hexValue(s[i + 1]);
            const int lo = hexValue(s[i + 2]);
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        }
        return out;
    }

    static int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;  // caller validated with std::isxdigit
    }
};

inline HotRodURI HotRodURI::parse(const std::string& uri) {
    // --- scheme ---
    const auto schemeEnd = uri.find("://");
    if (schemeEnd == std::string::npos) {
        fail("not a Hot Rod URI (missing scheme): " + uri);
    }
    const std::string scheme = toLower(uri.substr(0, schemeEnd));
    if (scheme == "hotrods") {
        fail("hotrods:// (TLS) is not supported");
    }
    if (scheme != "hotrod") {
        fail("not a Hot Rod URI (unsupported scheme '" + scheme + "')");
    }

    std::string rest = uri.substr(schemeEnd + 3);  // after "://"

    // --- split off query (?...) ---
    std::string query;
    if (const auto q = rest.find('?'); q != std::string::npos) {
        query = rest.substr(q + 1);
        rest = rest.substr(0, q);
    }

    // --- strip any path (/...) — Java ignores it; no cache name is carried here ---
    if (const auto slash = rest.find('/'); slash != std::string::npos) {
        rest = rest.substr(0, slash);
    }

    // --- authority: [userinfo@]hosts ---
    std::string authority = rest;
    HotRodURI result;

    if (const auto at = authority.find('@'); at != std::string::npos) {
        const std::string userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
        // Split userinfo on the *raw* string so an encoded '@'/'%40' or ':'/'%3A'
        // in the password is not mistaken for a delimiter, then percent-decode
        // each half (the Java client works on the decoded java.net.URI.getUserInfo()).
        // Decoding after the split — rather than Java's decode-then-split(':') —
        // additionally preserves an encoded ':' inside the password instead of
        // truncating the tail (a known Java limitation we intentionally improve on).
        const auto colon = userinfo.find(':');
        if (colon == std::string::npos) {
            result.username = percentDecode(userinfo, "username");
        } else {
            result.username = percentDecode(userinfo.substr(0, colon), "username");
            result.password = percentDecode(userinfo.substr(colon + 1), "password");
        }
    }

    if (authority.empty()) {
        fail("Hot Rod URI has no server host: " + uri);
    }

    // --- hosts: host[:port][,host[:port]...] ---
    size_t pos = 0;
    while (pos <= authority.size()) {
        const auto comma = authority.find(',', pos);
        const std::string hostPort =
            authority.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (hostPort.empty()) {
            fail("Hot Rod URI has an empty server entry: " + uri);
        }
        ServerAddress addr;
        // lastIndexOf(':') mirrors Java; no IPv6-bracket handling (parity).
        if (const auto c = hostPort.rfind(':'); c != std::string::npos) {
            addr.host = hostPort.substr(0, c);
            const std::string portStr = hostPort.substr(c + 1);
            try {
                size_t consumed = 0;
                const int p = std::stoi(portStr, &consumed);
                // Reject trailing garbage ("8080x") and surrounding whitespace that
                // std::stoi tolerates but Integer.parseInt (the Java parser) does not.
                if (consumed != portStr.size() || p < 1 || p > 65535) {
                    fail("Hot Rod URI has an invalid port '" + portStr + "'");
                }
                addr.port = static_cast<uint16_t>(p);
            } catch (const HotRodClientException&) {
                throw;
            } catch (const std::exception&) {
                fail("Hot Rod URI has an invalid port '" + portStr + "'");
            }
        } else {
            addr.host = hostPort;
            addr.port = 11222;  // ConfigurationProperties.DEFAULT_HOTROD_PORT
        }
        if (addr.host.empty()) {
            fail("Hot Rod URI has an empty host: " + uri);
        }
        result.servers.push_back(std::move(addr));

        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    // --- query parameters (typed whitelist; throw on unknown key / bad value) ---
    if (!query.empty()) {
        size_t qpos = 0;
        while (qpos <= query.size()) {
            const auto amp = query.find('&', qpos);
            const std::string part =
                query.substr(qpos, amp == std::string::npos ? std::string::npos : amp - qpos);
            if (!part.empty()) {
                const auto eq = part.find('=');
                if (eq == std::string::npos) {
                    fail("invalid Hot Rod URI parameter (expected key=value): " + part);
                }
                const std::string key = part.substr(0, eq);
                // Decode the value (Java reads the decoded java.net.URI.getQuery()).
                // The key stays raw: it is matched against the fixed whitelist names
                // below, none of which contain escapable characters.
                const std::string value = percentDecode(part.substr(eq + 1),
                                                         "query parameter value");

                if (key == "sasl_mechanism") {
                    // SCRAM-only is enforced later at connect(); pass through here.
                    result.saslMechanism = value;
                } else if (key == "client_intelligence") {
                    const std::string v = toLower(value);
                    if (v == "basic") {
                        result.clientIntelligence = ClientIntelligence::BASIC;
                    } else if (v == "topology_aware") {
                        result.clientIntelligence = ClientIntelligence::TOPOLOGY_AWARE;
                    } else if (v == "hash_distribution_aware") {
                        result.clientIntelligence = ClientIntelligence::HASH_DISTRIBUTION_AWARE;
                    } else {
                        fail("invalid client_intelligence value '" + value +
                             "' (expected basic, topology_aware, or hash_distribution_aware)");
                    }
                } else if (key == "protocol_version") {
                    // Java form "major.minor"; only 4.0/4.1 are supported here.
                    if (value == "4.0") {
                        result.protocolVersion = Protocol::VERSION_40;
                    } else if (value == "4.1") {
                        result.protocolVersion = Protocol::VERSION_41;
                    } else {
                        fail("unsupported protocol_version '" + value +
                             "' (supported: 4.0, 4.1)");
                    }
                } else {
                    fail("unsupported Hot Rod URI parameter: " + key);
                }
            }
            if (amp == std::string::npos) break;
            qpos = amp + 1;
        }
    }

    return result;
}

}  // namespace hotrod
