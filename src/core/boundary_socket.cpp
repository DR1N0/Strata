// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// boundary_socket.cpp - the transport behind boundary_socket.hpp.
//
// Winsock and POSIX differ in the usual ways (init, close, error codes, the handle type), so both are wrapped
// here and nothing above has to know which one it is on.  The behaviours that matter and are easy to get wrong:
//
//   * a `recv` that returns 0 is a CLOSED PEER, not an empty read;
//   * the handle is pointer-sized on Winsock and must never be squeezed into an int;
//   * `close` without `shutdown` can turn into an RST, which the peer reports as "connection reset" instead of
//     "closed", and which can discard bytes still in flight;
//   * on Windows `SO_REUSEADDR` means "steal a port another socket is bound to", so it is replaced by
//     `SO_EXCLUSIVEADDRUSE`; on POSIX it means "reuse a TIME_WAIT port", which is what we want there.
#include "strata/core/boundary_socket.hpp"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
static constexpr sock_t kBadSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
static constexpr sock_t kBadSock = -1;
#endif

namespace strata::core {
namespace {

#if defined(_WIN32)
struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};
bool ensure_net() {
    static WinsockInit init;
    return init.ok;
}
void close_sock(sock_t s) { closesocket(s); }
void shutdown_sock(sock_t s) { ::shutdown(s, SD_SEND); }
#else
bool ensure_net() { return true; }
void close_sock(sock_t s) { ::close(s); }
void shutdown_sock(sock_t s) { ::shutdown(s, SHUT_WR); }
#endif

bool set_nodelay(sock_t s) {
    int one = 1;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*) &one, sizeof one) == 0;
}

/// Make the socket exclusive on Windows (where SO_REUSEADDR would let another process steal the port) and
/// TIME_WAIT-reusable on POSIX.
bool set_reuse(sock_t s) {
#if defined(_WIN32)
    int one = 1;
    return setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &one, sizeof one) == 0;
#else
    int one = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*) &one, sizeof one) == 0;
#endif
}

bool set_ms_timeout(sock_t s, int opt, int ms) {
#if defined(_WIN32)
    const DWORD v = (DWORD) ms;   // Winsock wants a DWORD of milliseconds
    return setsockopt(s, SOL_SOCKET, opt, (const char*) &v, sizeof v) == 0;
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(s, SOL_SOCKET, opt, (const char*) &tv, sizeof tv) == 0;
#endif
}

}  // namespace

BoundarySocket::~BoundarySocket() { close(); }

BoundarySocket& BoundarySocket::operator=(BoundarySocket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        o.fd_ = kBadHandle;
    }
    return *this;
}

void BoundarySocket::close() {
    if (fd_ != kBadHandle) {
        const sock_t s = (sock_t) fd_;
        // A graceful half-close first: without it `closesocket` on WinSock sends an RST, the peer's `recv`
        // fails with "reset" instead of returning 0, and bytes still queued in the kernel are dropped.
        shutdown_sock(s);
        close_sock(s);
        fd_ = kBadHandle;
    }
}

bool BoundarySocket::listen_on(int port, int& bound_port, std::string& err) {
    if (!ensure_net()) {
        err = "boundary: the socket layer failed to start";
        return false;
    }
    const sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBadSock) {
        err = "boundary: socket() failed";
        return false;
    }
    set_reuse(s);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short) port);
    if (bind(s, (sockaddr*) &a, sizeof a) != 0) {
        err = "boundary: bind to port " + std::to_string(port) + " failed (another stage running?)";
        close_sock(s);
        return false;
    }
    // A HEAD STAGE ACCEPTS TWO CONNECTIONS ON ONE PORT (the driver's prompt and the ring's returning token).
    // A backlog of 1 let the second one sit unaccepted in the kernel queue, which looks exactly like "the tail
    // never connected".
    if (listen(s, SOMAXCONN) != 0) {
        err = "boundary: listen failed";
        close_sock(s);
        return false;
    }
    sockaddr_in got{};
    socklen_t gl = sizeof got;
    if (getsockname(s, (sockaddr*) &got, &gl) == 0) bound_port = ntohs(got.sin_port);
    fd_ = (handle_t) s;
    return true;
}

bool BoundarySocket::accept_one(BoundarySocket& out, std::string& err) {
    if (fd_ == kBadHandle) {
        err = "boundary: accept without listen";
        return false;
    }
    sockaddr_in peer{};
    socklen_t pl = sizeof peer;
    const sock_t c = accept((sock_t) fd_, (sockaddr*) &peer, &pl);
    if (c == kBadSock) {
        err = "boundary: accept failed";
        return false;
    }
    out.close();
    out.fd_ = (handle_t) c;
    set_nodelay(c);
    return true;
}

bool BoundarySocket::accept_n(int n, std::vector<BoundarySocket>& out, std::string& err) {
    if (n <= 0) {
        err = "boundary: accept_n wants at least one connection";
        return false;
    }
    out.clear();
    for (int i = 0; i < n; ++i) {
        BoundarySocket s;
        if (!accept_one(s, err)) {
            out.clear();   // the caller gets nothing half-accepted
            return false;
        }
        out.push_back(std::move(s));
    }
    return true;
}

bool BoundarySocket::connect_to(const std::string& host, int port, int timeout_ms, std::string& err) {
    if (!ensure_net()) {
        err = "boundary: the socket layer failed to start";
        return false;
    }
    const int tried_ms = 200;
    int waited = 0;
    std::string last = "no attempt";
    while (waited <= timeout_ms) {
        const sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kBadSock) {
            err = "boundary: socket() failed";
            return false;
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short) port);
        if (host.empty() || host == "*") a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
                close_sock(s);
                err = "boundary: cannot resolve " + host;
                return false;
            }
            a.sin_addr = ((sockaddr_in*) res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        if (connect(s, (sockaddr*) &a, sizeof a) == 0) {
            close();
            fd_ = (handle_t) s;
            set_nodelay(s);
            return true;
        }
        last = "connect to " + host + ":" + std::to_string(port) + " refused";
        close_sock(s);
#if defined(_WIN32)
        Sleep(tried_ms);
#else
        usleep((useconds_t) tried_ms * 1000);
#endif
        waited += tried_ms;
    }
    err = "boundary: " + last + " (waited " + std::to_string(waited) + " ms)";
    return false;
}

bool BoundarySocket::set_timeouts(int recv_ms, int send_ms, std::string& err) {
    if (fd_ == kBadHandle) {
        err = "boundary: set_timeouts on a socket that is not connected";
        return false;
    }
    const sock_t s = (sock_t) fd_;
    if (recv_ms > 0 && !set_ms_timeout(s, SO_RCVTIMEO, recv_ms)) {
        err = "boundary: SO_RCVTIMEO failed";
        return false;
    }
    if (send_ms > 0 && !set_ms_timeout(s, SO_SNDTIMEO, send_ms)) {
        err = "boundary: SO_SNDTIMEO failed";
        return false;
    }
    return true;
}

bool BoundarySocket::send_bytes(const void* data, size_t n, std::string& err) {
    if (fd_ == kBadHandle) {
        // the old message blamed the peer for a socket that was never connected
        err = "boundary: send on a socket that is not connected";
        return false;
    }
    const auto* p = (const uint8_t*) data;
    size_t sent = 0;
    while (sent < n) {
        const int k = send((sock_t) fd_, (const char*) p + sent, (int) (n - sent), 0);
        if (k <= 0) {
            err = "boundary: send failed (the other stage stopped?)";
            return false;
        }
        sent += (size_t) k;
    }
    return true;
}

bool BoundarySocket::recv_bytes(void* data, size_t n, std::string& err) {
    if (fd_ == kBadHandle) {
        err = "boundary: recv on a socket that is not connected";
        return false;
    }
    auto* p = (uint8_t*) data;
    size_t got = 0;
    while (got < n) {
        const int k = recv((sock_t) fd_, (char*) p + got, (int) (n - got), 0);
        if (k == 0) {
            err = "boundary: the other stage closed the connection";
            return false;
        }
        if (k < 0) {
            err = "boundary: recv failed";
            return false;
        }
        got += (size_t) k;
    }
    return true;
}

bool BoundarySocket::send_frame(const std::vector<uint8_t>& bytes, std::string& err) {
    const uint32_t n = (uint32_t) bytes.size();
    const uint8_t hdr[4] = {(uint8_t) (n & 0xff), (uint8_t) ((n >> 8) & 0xff), (uint8_t) ((n >> 16) & 0xff),
                            (uint8_t) ((n >> 24) & 0xff)};
    if (!send_bytes(hdr, sizeof hdr, err)) return false;
    if (n == 0) return true;
    return send_bytes(bytes.data(), bytes.size(), err);
}

bool BoundarySocket::recv_frame(std::vector<uint8_t>& bytes, std::string& err, size_t max_bytes) {
    uint8_t hdr[4];
    if (!recv_bytes(hdr, sizeof hdr, err)) return false;
    const uint32_t n = (uint32_t) hdr[0] | ((uint32_t) hdr[1] << 8) | ((uint32_t) hdr[2] << 16) |
                       ((uint32_t) hdr[3] << 24);
    if (n == 0) {
        bytes.clear();
        return true;
    }
    if ((size_t) n > max_bytes) {
        err = "boundary: a frame of " + std::to_string(n) + " bytes is past the limit";
        return false;
    }
    bytes.resize(n);
    return recv_bytes(bytes.data(), bytes.size(), err);
}

std::string BoundarySocket::peer() const {
    sockaddr_in a{};
    socklen_t l = sizeof a;
    if (fd_ == kBadHandle || getpeername((sock_t) fd_, (sockaddr*) &a, &l) != 0) return "?";
    char buf[64];
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
}

bool boundary_parse_address(const std::string& text, std::string& host, int& port, std::string& err) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
        err = "boundary: an address is HOST:PORT (or :PORT for any interface)";
        return false;
    }
    host = text.substr(0, colon);
    const std::string p = text.substr(colon + 1);
    if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos) {
        err = "boundary: '" + p + "' is not a port";
        return false;
    }
    port = std::atoi(p.c_str());
    if (port <= 0 || port > 65535) {
        err = "boundary: port out of range";
        return false;
    }
    return true;
}

}  // namespace strata::core
