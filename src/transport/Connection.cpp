#include "hotrod/Connection.h"
#include <stdexcept>
#include <cstring>
#include <string>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

// Platform-specific includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#endif

// Per-send SIGPIPE suppression (option 2: no process-global signal change).
// Linux has the MSG_NOSIGNAL send flag; the BSDs/macOS instead use the
// SO_NOSIGPIPE socket option (set in connect()), so no flag is needed there;
// Windows has no SIGPIPE at all.
#if defined(_WIN32)
    #define HOTROD_SEND_FLAGS 0
#elif defined(MSG_NOSIGNAL)
    #define HOTROD_SEND_FLAGS MSG_NOSIGNAL
#else
    #define HOTROD_SEND_FLAGS 0
#endif

namespace hotrod {

namespace {

// Drain the OpenSSL error queue into a human-readable string for exceptions.
std::string opensslErrors() {
    std::string out;
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!out.empty()) out += "; ";
        out += buf;
    }
    return out.empty() ? "unknown OpenSSL error" : out;
}

// True when host is a literal IPv4 or IPv6 address. SNI (RFC 6066) carries host
// names only, so for a literal IP we neither send SNI nor use it for the
// certificate host-name match (which would be a DNS-name check).
bool isLiteralIp(const std::string& host) {
    unsigned char buf[16];
    return inet_pton(AF_INET, host.c_str(), buf) == 1 ||
           inet_pton(AF_INET6, host.c_str(), buf) == 1;
}

#ifndef _WIN32
// A custom OpenSSL BIO over a plain fd whose write uses HOTROD_SEND_FLAGS
// (MSG_NOSIGNAL on Linux). OpenSSL's default socket BIO writes with plain
// write()/send(), so a TLS write onto a dropped peer would raise SIGPIPE and
// kill the process. Routing every OpenSSL socket write through this BIO confines
// the suppression to our connections (no process-global SIG_IGN) and covers all
// writes — app data via SSL_write, plus the writes SSL_connect/SSL_read trigger
// internally (handshake, renegotiation, TLS 1.3 post-handshake). The BIO never
// closes the fd; Connection owns the socket lifecycle. Windows has no SIGPIPE,
// so there the default SSL_set_fd BIO is used unchanged.
int bioWrite(BIO* b, const char* buf, int len) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(b)));
    BIO_clear_retry_flags(b);
    const ssize_t n = ::send(fd, buf, static_cast<size_t>(len), HOTROD_SEND_FLAGS);
    if (n <= 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        BIO_set_retry_write(b);
    }
    return static_cast<int>(n);
}

int bioRead(BIO* b, char* buf, int len) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(b)));
    BIO_clear_retry_flags(b);
    const ssize_t n = ::recv(fd, buf, static_cast<size_t>(len), 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        BIO_set_retry_read(b);
    }
    return static_cast<int>(n);
}

long bioCtrl(BIO*, int cmd, long, void*) {
    // Nothing is buffered, so FLUSH is a no-op success; everything else (pending
    // counts, eof, push/pop, close flags) defaults to 0, which is correct here.
    return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

int bioCreate(BIO* b) {
    BIO_set_init(b, 0);
    BIO_set_data(b, nullptr);
    return 1;
}

int bioDestroy(BIO* b) {
    return b != nullptr;  // do not close the fd — Connection owns it
}

// The BIO_METHOD, created once and intentionally leaked for process lifetime.
BIO_METHOD* sigpipeSafeBioMethod() {
    static BIO_METHOD* method = [] {
        BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK,
                                     "hotrod sigpipe-safe socket");
        BIO_meth_set_write(m, bioWrite);
        BIO_meth_set_read(m, bioRead);
        BIO_meth_set_ctrl(m, bioCtrl);
        BIO_meth_set_create(m, bioCreate);
        BIO_meth_set_destroy(m, bioDestroy);
        return m;
    }();
    return method;
}
#endif  // !_WIN32

}  // namespace

// Platform-specific initialization
#ifdef _WIN32
    struct WinsockInitializer {
        WinsockInitializer() {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                throw std::runtime_error("WSAStartup failed");
            }
        }
        ~WinsockInitializer() {
            WSACleanup();
        }
    };
    static WinsockInitializer winsockInit;  // Global initializer
#endif

Connection::Connection(const std::string& host, uint16_t port)
    : host_(host), port_(port), connected_(false), socket_(INVALID_SOCK) {
}

Connection::~Connection() {
    if (connected_) {
        close();
    }
}

void Connection::connect() {
    if (connected_) {
        throw std::runtime_error("Already connected");
    }

    // Resolve hostname
    struct addrinfo hints, *result = nullptr, *rp = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP socket
    hints.ai_protocol = IPPROTO_TCP;

    std::string portStr = std::to_string(port_);
    int status = getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &result);
    if (status != 0) {
#ifdef _WIN32
        throw std::runtime_error("getaddrinfo failed: " + std::to_string(WSAGetLastError()));
#else
        throw std::runtime_error("getaddrinfo failed: " + std::string(gai_strerror(status)));
#endif
    }

    // Try each address until we successfully connect
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        socket_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

#ifdef _WIN32
        if (socket_ == INVALID_SOCKET) {
            continue;
        }
#else
        if (socket_ == -1) {
            continue;
        }
#endif

        // SIGPIPE suppression on platforms with the socket-level option (macOS/
        // BSD). This covers both plain send() and OpenSSL's writes on this fd.
        // Linux lacks it and uses MSG_NOSIGNAL per write instead (HOTROD_SEND_FLAGS
        // + the custom TLS BIO).
#ifdef SO_NOSIGPIPE
        {
            int on = 1;
            setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        }
#endif

        // Set socket to non-blocking mode for connect with timeout
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(socket_, FIONBIO, &mode);
#else
        int flags = fcntl(socket_, F_GETFL, 0);
        fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
#endif

        // Attempt to connect (non-blocking)
        int connectResult = ::connect(socket_, rp->ai_addr, static_cast<socklen_t>(rp->ai_addrlen));

        if (connectResult == 0) {
            // Immediate success (rare for non-blocking)
            connected_ = true;
        } else {
#ifdef _WIN32
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
#else
            if (errno == EINPROGRESS) {
#endif
                // Connection in progress, wait with timeout (5 seconds)
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(socket_, &writefds);

                struct timeval timeout;
                timeout.tv_sec = 5;   // 5 second timeout
                timeout.tv_usec = 0;

                int selectResult = select(static_cast<int>(socket_ + 1), nullptr, &writefds, nullptr, &timeout);

                if (selectResult > 0) {
                    // Check if connection succeeded
                    int so_error;
                    socklen_t len = sizeof(so_error);
                    getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);

                    if (so_error == 0) {
                        connected_ = true;
                    }
                }
            }
        }

        if (connected_) {
            // Set socket back to blocking mode
#ifdef _WIN32
            mode = 0;
            ioctlsocket(socket_, FIONBIO, &mode);
#else
            flags = fcntl(socket_, F_GETFL, 0);
            fcntl(socket_, F_SETFL, flags & ~O_NONBLOCK);
#endif
            break;  // Success
        }

        // Close and try next address
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
        socket_ = INVALID_SOCK;
    }

    freeaddrinfo(result);

    if (!connected_) {
        throw std::runtime_error("Failed to connect to " + host_ + ":" + std::to_string(port_));
    }

    // TLS handshake over the connected socket. On failure the socket is closed so
    // the Connection is left in a clean, unconnected state.
    if (tls_.enabled) {
        try {
            startTls();
        } catch (...) {
            close();
            throw;
        }
    }
}

void Connection::startTls() {
    // Client context. TLS_client_method() negotiates the highest mutually
    // supported protocol; we additionally pin a sane floor of TLS 1.2.
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ctx_) {
        throw std::runtime_error("TLS: SSL_CTX_new failed: " + opensslErrors());
    }
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

    // Trust anchors: an explicit PEM CA file, otherwise the system default store.
    if (!tls_.ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(ctx_, tls_.ca_file.c_str(), nullptr) != 1) {
            throw std::runtime_error("TLS: failed to load CA file '" + tls_.ca_file +
                                     "': " + opensslErrors());
        }
    } else {
        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            throw std::runtime_error("TLS: failed to load system trust store: " +
                                     opensslErrors());
        }
    }

    // Always verify the server's certificate chain. The hostname match below is
    // separately controlled by tls_.hostname_validation.
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);

    ssl_ = SSL_new(ctx_);
    if (!ssl_) {
        throw std::runtime_error("TLS: SSL_new failed: " + opensslErrors());
    }

    // The SNI name / host-name to validate: an explicit override, else the
    // connection host (never a literal IP).
    const std::string serverName =
        !tls_.sni_host.empty() ? tls_.sni_host : host_;
    const bool nameIsIp = isLiteralIp(serverName);

    // SNI (skipped for literal IPs per RFC 6066).
    if (!nameIsIp) {
        // SSL_set_tlsext_host_name takes a non-const char*; the string is copied.
        if (SSL_set_tlsext_host_name(ssl_, serverName.c_str()) != 1) {
            throw std::runtime_error("TLS: failed to set SNI host name: " +
                                     opensslErrors());
        }
    }

    // Host-name validation. SSL_set1_host makes OpenSSL enforce the match as part
    // of chain verification; omitting it leaves only chain verification on. We do
    // not validate a literal IP as a DNS name.
    if (tls_.hostname_validation && !nameIsIp) {
        SSL_set_hostflags(ssl_, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (SSL_set1_host(ssl_, serverName.c_str()) != 1) {
            throw std::runtime_error("TLS: failed to set validation host name: " +
                                     opensslErrors());
        }
    }

#ifdef _WIN32
    // Windows has no SIGPIPE; the default socket BIO is fine.
    if (SSL_set_fd(ssl_, static_cast<int>(socket_)) != 1) {
        throw std::runtime_error("TLS: SSL_set_fd failed: " + opensslErrors());
    }
#else
    // Route all OpenSSL socket I/O through our MSG_NOSIGNAL BIO so a write onto a
    // dropped peer never raises SIGPIPE. SSL takes ownership of the BIO (same BIO
    // for read and write) and frees it in SSL_free; it does not close the fd.
    BIO* bio = BIO_new(sigpipeSafeBioMethod());
    if (!bio) {
        throw std::runtime_error("TLS: BIO_new failed: " + opensslErrors());
    }
    BIO_set_data(bio, reinterpret_cast<void*>(static_cast<intptr_t>(socket_)));
    BIO_set_init(bio, 1);
    SSL_set_bio(ssl_, bio, bio);
#endif

    const int rc = SSL_connect(ssl_);
    if (rc != 1) {
        // A verification failure surfaces a specific, actionable reason.
        const long vr = SSL_get_verify_result(ssl_);
        std::string msg = "TLS handshake with " + host_ + ":" +
                          std::to_string(port_) + " failed";
        if (vr != X509_V_OK) {
            msg += ": certificate verification failed (";
            msg += X509_verify_cert_error_string(vr);
            msg += ")";
        } else {
            msg += ": " + opensslErrors();
        }
        throw std::runtime_error(msg);
    }
}

void Connection::send(const ByteArray& data) {
    if (!connected_) {
        throw std::runtime_error("Not connected");
    }

    size_t totalSent = 0;
    while (totalSent < data.size()) {
        const char* ptr = reinterpret_cast<const char*>(data.data()) + totalSent;
        size_t remaining = data.size() - totalSent;

        if (ssl_) {
            int sent = SSL_write(ssl_, ptr, static_cast<int>(remaining));
            if (sent <= 0) {
                throw std::runtime_error("TLS send failed: " + opensslErrors());
            }
            totalSent += static_cast<size_t>(sent);
            continue;
        }

#ifdef _WIN32
        int sent = ::send(socket_, ptr, static_cast<int>(remaining), 0);
        if (sent == SOCKET_ERROR) {
            throw std::runtime_error("send failed: " + std::to_string(WSAGetLastError()));
        }
#else
        ssize_t sent = ::send(socket_, ptr, remaining, HOTROD_SEND_FLAGS);
        if (sent == -1) {
            throw std::runtime_error("send failed: " + std::string(strerror(errno)));
        }
#endif

        totalSent += sent;
    }
}

ByteArray Connection::receive(size_t length) {
    if (!connected_) {
        throw std::runtime_error("Not connected");
    }

    ByteArray buffer(length);
    size_t totalReceived = 0;

    while (totalReceived < length) {
        char* ptr = reinterpret_cast<char*>(buffer.data()) + totalReceived;
        size_t remaining = length - totalReceived;

        if (ssl_) {
            int received = SSL_read(ssl_, ptr, static_cast<int>(remaining));
            if (received <= 0) {
                const int err = SSL_get_error(ssl_, received);
                if (err == SSL_ERROR_ZERO_RETURN) {
                    throw std::runtime_error("Connection closed by peer");
                }
                throw std::runtime_error("TLS recv failed: " + opensslErrors());
            }
            totalReceived += static_cast<size_t>(received);
            continue;
        }

#ifdef _WIN32
        int received = ::recv(socket_, ptr, static_cast<int>(remaining), 0);
        if (received == SOCKET_ERROR) {
            throw std::runtime_error("recv failed: " + std::to_string(WSAGetLastError()));
        } else if (received == 0) {
            throw std::runtime_error("Connection closed by peer");
        }
#else
        ssize_t received = ::recv(socket_, ptr, remaining, 0);
        if (received == -1) {
            throw std::runtime_error("recv failed: " + std::string(strerror(errno)));
        } else if (received == 0) {
            throw std::runtime_error("Connection closed by peer");
        }
#endif

        totalReceived += received;
    }

    return buffer;
}

void Connection::shutdown() {
    if (!connected_ || socket_ == INVALID_SOCK) {
        return;
    }

    // Shutdown socket to interrupt blocking recv() calls
    // SHUT_RDWR = 2 on both POSIX and Windows
#ifdef _WIN32
    ::shutdown(socket_, SD_BOTH);
#else
    ::shutdown(socket_, SHUT_RDWR);
#endif
}

void Connection::close() {
    // Tear down TLS first, independent of connected_ so a failed handshake in
    // connect() still frees the context it allocated. Quiet shutdown skips the
    // close_notify write: if the peer has already gone, writing it would raise
    // SIGPIPE/EPIPE and kill the process for no benefit during teardown.
    if (ssl_) {
        SSL_set_quiet_shutdown(ssl_, 1);
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }

    if (!connected_) {
        if (socket_ != INVALID_SOCK) {
#ifdef _WIN32
            closesocket(socket_);
#else
            ::close(socket_);
#endif
            socket_ = INVALID_SOCK;
        }
        return;
    }

#ifdef _WIN32
    closesocket(socket_);
#else
    ::close(socket_);
#endif

    socket_ = INVALID_SOCK;
    connected_ = false;
}

// Helper methods for body parsers

VInt Connection::receiveVInt() {
    ByteArray firstByte = receive(1);
    VInt value = firstByte[0] & 0x7F;

    for (int shift = 7; (firstByte[0] & 0x80) != 0; shift += 7) {
        ByteArray nextByte = receive(1);
        value |= static_cast<VInt>(nextByte[0] & 0x7F) << shift;
        firstByte[0] = nextByte[0];  // Update for continuation check
    }

    return value;
}

VLong Connection::receiveVLong() {
    ByteArray firstByte = receive(1);
    VLong value = firstByte[0] & 0x7F;

    for (int shift = 7; (firstByte[0] & 0x80) != 0; shift += 7) {
        ByteArray nextByte = receive(1);
        value |= static_cast<VLong>(nextByte[0] & 0x7F) << shift;
        firstByte[0] = nextByte[0];
    }

    return value;
}

ByteArray Connection::receiveByteArray() {
    // Read vInt length
    VInt length = receiveVInt();

    // Read bytes
    if (length > 0) {
        return receive(length);
    }
    return {};
}

std::string Connection::receiveString() {
    ByteArray bytes = receiveByteArray();
    return std::string(bytes.begin(), bytes.end());
}

EntryMetadata Connection::receiveMetadata() {
    EntryMetadata metadata;

    // Read flag byte
    uint8_t flag = receive(1)[0];

    // Read created + lifespan if not infinite (flag bit 0 == 0)
    if ((flag & 0x01) == 0) {
        // created (s8 - signed 64-bit big-endian)
        ByteArray createdBytes = receive(8);
        int64_t created = 0;
        for (int i = 0; i < 8; i++) {
            created = (created << 8) | createdBytes[i];
        }
        metadata.created = created;

        // lifespan (vint)
        metadata.lifespan = receiveVInt();
    }

    // Read last_used + max_idle if not infinite (flag bit 1 == 0)
    if ((flag & 0x02) == 0) {
        // last_used (s8 - signed 64-bit big-endian)
        ByteArray lastUsedBytes = receive(8);
        int64_t lastUsed = 0;
        for (int i = 0; i < 8; i++) {
            lastUsed = (lastUsed << 8) | lastUsedBytes[i];
        }
        metadata.lastUsed = lastUsed;

        // max_idle (vint)
        metadata.maxIdle = receiveVInt();
    }

    // Read entry_version (s8 - always present)
    ByteArray versionBytes = receive(8);
    int64_t version = 0;
    for (int i = 0; i < 8; i++) {
        version = (version << 8) | versionBytes[i];
    }
    metadata.version = version;

    return metadata;
}

} // namespace hotrod
