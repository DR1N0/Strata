// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// boundary_socket.hpp - carrying a BoundaryFrame between two processes.
//
// The frame's serialization is in boundary_wire.hpp; this is only the transport.  A stage listens on its own
// ingress port (`--listen`) and connects out to `--downstream` (and, from a head stage, `--orch`).  The wire
// is a length-prefixed frame, one per message, in order:
//
//   u32 bytes_follow | bytes_follow bytes of BoundaryFrame
//
// No framing beyond that: TCP already delivers the two in order on one stream.  A closed peer is an error, not
// an empty frame - a stage that loses its neighbour must stop rather than compute on a half prompt.
//
// THE HANDLE IS NOT AN `int`.  Winsock's SOCKET is an unsigned pointer-sized value and truncating it to 32 bits
// is undefined; `intptr_t` is the common type that holds either.  `kBadHandle` is the matching "no socket" value.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace strata::core {

/// A listening or connected socket.  Move-only; `close` is idempotent.
class BoundarySocket {
public:
    using handle_t = std::intptr_t;
    static constexpr handle_t kBadHandle = -1;

    BoundarySocket() = default;
    ~BoundarySocket();
    BoundarySocket(const BoundarySocket&) = delete;
    BoundarySocket& operator=(const BoundarySocket&) = delete;
    BoundarySocket(BoundarySocket&& o) noexcept : fd_(o.fd_) { o.fd_ = kBadHandle; }
    BoundarySocket& operator=(BoundarySocket&& o) noexcept;

    /// Bind and listen on `port` (0: any free port, reported back through `bound_port`).
    ///
    /// The backlog is `SOMAXCONN`: a head stage accepts TWO connections on one port (the driver's prompt and the
    /// ring's returning token), and a backlog of 1 left the second one waiting in the kernel's queue forever.
    bool listen_on(int port, int& bound_port, std::string& err);
    /// Accept one connection.  Blocks until one arrives (or fails).
    bool accept_one(BoundarySocket& out, std::string& err);
    /// Accept `n` connections in order.  The caller decides how many: 1 for a middle/tail stage's ingress, 2 for
    /// a head's (driver + ring closure).
    bool accept_n(int n, std::vector<BoundarySocket>& out, std::string& err);
    /// Connect to a peer.  Retries for `timeout_ms` (the listener may still be starting).
    bool connect_to(const std::string& host, int port, int timeout_ms, std::string& err);

    /// Receive/send timeouts in milliseconds (0 = no timeout, the default).  Set before the exchange that must
    /// not hang forever; a timeout makes `recv_bytes`/`send_bytes` fail with an error naming it.
    bool set_timeouts(int recv_ms, int send_ms, std::string& err);

    /// One message.  Both return false with `err` set on a closed or broken peer, and refuse an invalid handle
    /// instead of calling into the socket layer with it.
    bool send_bytes(const void* data, size_t n, std::string& err);
    bool recv_bytes(void* data, size_t n, std::string& err);

    bool valid() const { return fd_ != kBadHandle; }
    void close();
    /// The peer's address as "host:port", for a log line.
    std::string peer() const;

    /// A frame's payload as one message: `u32 size` then the bytes.
    bool send_frame(const std::vector<uint8_t>& bytes, std::string& err);
    bool recv_frame(std::vector<uint8_t>& bytes, std::string& err, size_t max_bytes);

private:
    handle_t fd_ = kBadHandle;
};

/// Parse "HOST:PORT" into its parts (the host may be omitted: ":5000" is all interfaces).
bool boundary_parse_address(const std::string& text, std::string& host, int& port, std::string& err);

}  // namespace strata::core
