/**
 * Unit tests for server-authenticated TLS on the transport (Connection), issue #18.
 *
 * These exercise the real OpenSSL handshake end-to-end against an in-process TLS
 * server that presents a PEM leaf certificate (SAN=test.local) signed by a test
 * CA. The client (hotrod::Connection) connects over loopback and verifies the
 * server against a PEM trust anchor, mirroring how MultiplexedConnection drives
 * it in production.
 *
 * Coverage:
 *  - Trusted CA + matching host name  -> handshake succeeds; encrypted data
 *    round-trips through SSL_read/SSL_write.
 *  - Untrusted CA (wrong trust anchor) -> handshake throws (chain verification).
 *  - Host-name mismatch with validation on  -> throws.
 *  - Host-name mismatch with validation off -> succeeds (chain still verified).
 *
 * The socket server half uses POSIX sockets, so (like this repo's integration
 * tests) the meaningful body is compiled on non-Windows only. The cross-platform
 * URI-level TLS parsing is covered separately in HotRodURITest.cpp.
 */
#include <gtest/gtest.h>

#ifndef _WIN32

#include "hotrod/Connection.h"
#include "hotrod/TlsConfig.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <string>
#include <thread>

using namespace hotrod;

namespace {

// This guards the in-process echo *server* half (harness code), which does raw
// SSL_write/SSL_shutdown onto client sockets that may already be closed -> SIGPIPE.
// The client (the library under test) suppresses SIGPIPE itself per-connection, so
// it does not rely on this; it is here only for the test's own server thread.
[[maybe_unused]] const bool kSigpipeIgnored = [] {
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}();

std::string fixture(const char* name) {
    return std::string(TLS_FIXTURE_DIR) + "/" + name;
}

// Minimal in-process TLS server: binds an ephemeral loopback port, accepts one
// connection in a background thread, completes the TLS handshake, and echoes
// back whatever it reads until the client closes. Presents the given PEM leaf
// cert + key. Robust to clients that abort mid-handshake (failure-path tests).
class TlsEchoServer {
public:
    TlsEchoServer(const std::string& certFile, const std::string& keyFile) {
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_) throw std::runtime_error("server SSL_CTX_new failed");
        if (SSL_CTX_use_certificate_chain_file(ctx_, certFile.c_str()) != 1)
            throw std::runtime_error("server cert load failed: " + certFile);
        if (SSL_CTX_use_PrivateKey_file(ctx_, keyFile.c_str(), SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("server key load failed: " + keyFile);

        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) throw std::runtime_error("server socket() failed");
        int one = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("server bind() failed");
        if (::listen(listenFd_, 1) != 0)
            throw std::runtime_error("server listen() failed");

        socklen_t len = sizeof(addr);
        if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            throw std::runtime_error("server getsockname() failed");
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this] { run(); });
    }

    ~TlsEchoServer() { stop(); }

    TlsEchoServer(const TlsEchoServer&) = delete;
    TlsEchoServer& operator=(const TlsEchoServer&) = delete;

    uint16_t port() const { return port_; }

    void stop() {
        if (stopped_.exchange(true)) return;
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

private:
    void run() {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) return;

        SSL* ssl = SSL_new(ctx_);
        if (ssl) {
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) == 1) {
                char buf[4096];
                for (;;) {
                    int n = SSL_read(ssl, buf, sizeof(buf));
                    if (n <= 0) break;
                    if (SSL_write(ssl, buf, n) <= 0) break;
                }
            }
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        ::close(fd);
    }

    SSL_CTX* ctx_{nullptr};
    int listenFd_{-1};
    uint16_t port_{0};
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

// A Connection to the loopback server with the given trust/validation settings.
// Uses sni_host to drive the validated DNS name independently of the loopback
// IP we actually dial (a literal IP would skip SNI/host validation by design).
TlsConfig clientTls(const std::string& caFile,
                    const std::string& sniHost,
                    bool hostnameValidation) {
    TlsConfig tls;
    tls.enabled = true;
    tls.ca_file = caFile;
    tls.sni_host = sniHost;
    tls.hostname_validation = hostnameValidation;
    return tls;
}

}  // namespace

TEST(TlsConnectionTest, TrustedCaMatchingHost_HandshakeSucceeds_DataRoundTrips) {
    TlsEchoServer server(fixture("server.pem"), fixture("server.key"));

    Connection conn("127.0.0.1", server.port());
    conn.setTls(clientTls(fixture("ca.pem"), "test.local", true));
    ASSERT_NO_THROW(conn.connect());

    const ByteArray msg{0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    conn.send(msg);
    const ByteArray echoed = conn.receive(msg.size());
    EXPECT_EQ(echoed, msg);

    conn.close();
    server.stop();
}

TEST(TlsConnectionTest, UntrustedCa_HandshakeThrows) {
    TlsEchoServer server(fixture("server.pem"), fixture("server.key"));

    Connection conn("127.0.0.1", server.port());
    // Trust a different, unrelated CA -> chain verification must fail.
    conn.setTls(clientTls(fixture("otherca.pem"), "test.local", true));
    EXPECT_THROW(conn.connect(), std::runtime_error);

    server.stop();
}

TEST(TlsConnectionTest, HostnameMismatch_ValidationOn_Throws) {
    TlsEchoServer server(fixture("server.pem"), fixture("server.key"));

    Connection conn("127.0.0.1", server.port());
    // Cert SAN is test.local; validate against a different name -> reject.
    conn.setTls(clientTls(fixture("ca.pem"), "wrong.local", true));
    EXPECT_THROW(conn.connect(), std::runtime_error);

    server.stop();
}

TEST(TlsConnectionTest, HostnameMismatch_ValidationOff_Succeeds) {
    TlsEchoServer server(fixture("server.pem"), fixture("server.key"));

    Connection conn("127.0.0.1", server.port());
    // Name does not match, but validation is off; the chain is still verified.
    conn.setTls(clientTls(fixture("ca.pem"), "wrong.local", false));
    ASSERT_NO_THROW(conn.connect());

    conn.close();
    server.stop();
}

#endif  // !_WIN32
