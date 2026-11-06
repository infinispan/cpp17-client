#pragma once

#include "Types.h"
#include "TlsConfig.h"
#include <string>
#include <cstdint>

// Forward-declare the OpenSSL types so this header does not drag <openssl/ssl.h>
// into every translation unit that merely talks to a Connection.
struct ssl_st;
struct ssl_ctx_st;

namespace hotrod {

// Portable socket handle type. On Windows a SOCKET is an unsigned
// pointer-sized handle (UINT_PTR); on POSIX it is an int file descriptor.
// Using the right width avoids truncating the handle (MSVC C4244).
#ifdef _WIN32
using socket_t = uintptr_t;
#else
using socket_t = int;
#endif

// Portable invalid-socket sentinel: equals INVALID_SOCKET on Windows and
// -1 on POSIX, without a signed/unsigned conversion warning at each use.
constexpr socket_t INVALID_SOCK = static_cast<socket_t>(-1);

/**
 * TCP connection to Infinispan server.
 * Cross-platform: POSIX sockets (Linux/Unix) and Winsock2 (Windows)
 */
class Connection {
public:
    Connection(const std::string& host, uint16_t port);
    ~Connection();

    // Configure TLS. Must be called before connect(). When tls.enabled is true,
    // connect() performs a TLS handshake over the TCP socket and all subsequent
    // send()/receive() traffic is encrypted.
    void setTls(const TlsConfig& tls) { tls_ = tls; }

    // Connect to server
    void connect();

    // Send data
    void send(const ByteArray& data);

    // Receive data
    ByteArray receive(size_t length);

    // Shutdown connection (interrupts blocking recv)
    void shutdown();

    // Close connection
    void close();

    bool isConnected() const { return connected_; }

    // Helper methods for body parsers (used by MultiplexedConnection)
    VInt receiveVInt();
    VLong receiveVLong();
    ByteArray receiveByteArray();  // lp_bytes (vInt length + bytes)
    std::string receiveString();   // lp_string (vInt length + UTF-8)
    EntryMetadata receiveMetadata(); // entry_metadata (Protocol 4.0+)

private:
    // Perform the TLS handshake on the already-connected socket_ (server-auth,
    // OpenSSL). Throws std::runtime_error with a descriptive message on failure.
    void startTls();

    std::string host_;
    uint16_t port_;
    bool connected_;
    socket_t socket_;  // Socket descriptor (platform-specific)

    // TLS state. tls_.enabled gates the handshake in connect(); when a session is
    // live, ssl_ is non-null and send()/receive() route through it. ctx_ owns the
    // per-connection OpenSSL context. Both are freed in close()/dtor.
    TlsConfig  tls_;
    ssl_st*    ssl_{nullptr};
    ssl_ctx_st* ctx_{nullptr};
};

} // namespace hotrod
