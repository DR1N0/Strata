// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// THE CONTROL WIRE: the JSON handshake and lifecycle messages a Tectonic engine exchanges with its
// daemon, as opposed to the binary BoundaryFrame tensor traffic in boundary_wire.hpp. These are two
// different things carried over two different connections (data, signal) even though the framing is
// identical - see Tectonic's docs/design/engine.md ("帧编码", "握手") for the authoritative contract
// this file implements.
//
// FRAMING: reuses BoundarySocket::send_frame/recv_frame exactly as-is (u32 little-endian length +
// payload bytes) - the payload here is UTF-8 JSON text instead of a serialized BoundaryFrame. There is
// no reason to invent a second framing primitive for the same shape.
//
// RESOLVED (Tier 1, ADR 0005, wire v4): `gen`'s tokens[] is a shell around the entry stage's own request
// lifecycle, not a replacement for BoundaryFrame traffic. Inter-stage handoff (residual tensors, the ring
// closing back to the entry with a sampled token) keeps travelling as BoundaryFrame, unchanged, on every
// stage including the entry's own outbound/inbound BoundaryFrame traffic to its neighbours. Only the ENTRY
// stage (layer_begin == 0) also speaks `gen`/`delta`/`done`/`error` below, on the SAME data connection -
// see entry_bridge.hpp for where the two meet. A non-entry stage never sees these four message types.
#pragma once

#include "strata/core/boundary_socket.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace strata::core {

// Tectonic's contract version integer (docs/design/engine.md "版本"): bumps only on an incompatible
// change to frame encoding, semantics, or a removed field. Adding a field never bumps this - unknown
// fields must be ignored by both sides.
constexpr int kControlVersion = 1;

constexpr size_t kControlMaxFrameBytes = size_t(8) << 20;   // 8 MiB, per the contract's frame cap

/// What this engine reports about itself in `hello`. Fields the daemon only records (model,
/// capabilities, build) are plain strings/bools here rather than a nested type - nothing downstream
/// needs to interpret them structurally yet, and the contract explicitly treats them as opaque reports.
struct HelloFields {
    std::string instance_id;         // echoed back by the daemon's hello_ok; identifies this start
    int64_t layer_begin = 0, layer_end = 0;   // this engine's layer range (was "stage", see --layers)
    // The concurrency this instance actually started with (--batch). Tier 1 (ADR 0005, wire v4): this is
    // how many requests may be in flight on the data connection at once, not how many sessions' compute
    // state coexist - see StageWorkerConfig::batch in stage_worker.hpp for the full distinction.
    int batch = 1;
    std::string model_fingerprint;   // reported, not validated (engine.md: daemon already chose this)
    int64_t model_layers = 0;
    bool cap_compute = true, cap_kv = true, cap_cancel = false, cap_prefix_reuse = false;
    std::string build_name = "strata", build_version, build_platform;
};

/// Sends `hello` on `sock` built from `f`. One shape, used on both the data and the signal connection
/// (docs/design/engine.md: "信号连接自己的握手...复用同一个 hello/hello_ok/hello_reject 形状").
bool control_send_hello(BoundarySocket& sock, const HelloFields& f, std::string& err);

/// Reads one message and, if it is `hello`, fills `out`. `type_out` always gets the message's `type`
/// field so a caller can tell a non-hello message (there should not be one here, but silently
/// misinterpreting one as a successful hello would be worse than naming what actually arrived).
bool control_recv_hello(BoundarySocket& sock, HelloFields& out, std::string& type_out, std::string& err);

/// The three rejection reasons the contract names (docs/design/engine.md "握手"): a version this
/// daemon does not speak, an instance_id that is not the one it just started, or a layer_begin/
/// layer_end pair that does not match the assignment it gave this process on its command line.
enum class HelloRejectReason { kUnsupportedVersion, kInstanceMismatch, kLayersMismatch };
bool control_send_hello_ok(BoundarySocket& sock, const std::string& instance_id, std::string& err);
bool control_send_hello_reject(BoundarySocket& sock, HelloRejectReason reason, const std::string& message,
                               std::string& err);

/// True once `hello_ok` (not `hello_reject`) arrives. On false, `err` explains what arrived instead
/// (hello_reject's message, or a plain transport/parse error).
bool control_recv_hello_reply(BoundarySocket& sock, std::string& err);

/// `ready`: sent once, on the data connection only, after its hello exchange - the signal connection
/// has no ready (docs/design/engine.md: readiness is a data-connection concept).
bool control_send_ready(BoundarySocket& sock, std::string& err);

/// Signal-connection messages. `cancel` names one request; `shutdown` carries the grace period the
/// daemon is giving the process before it forces termination. Both are read from the signal connection
/// by whatever owns it (see stage_worker.hpp's signal_loop) and handed to the data loop as plain state,
/// not acted on here - this file only knows the wire shape.
struct SignalMessage {
    enum class Kind { kUnknown, kCancel, kShutdown } kind = Kind::kUnknown;
    std::string request_id;    // kCancel
    int64_t deadline_ms = 0;   // kShutdown
};
bool control_recv_signal(BoundarySocket& sock, SignalMessage& out, std::string& err);

// ---- entry-stage request lifecycle (Tier 1, ADR 0005): gen / delta / done / error --------------------
//
// Only the entry stage (layer_begin == 0) ever sends or receives these, on the data connection, mixed in
// with that same connection's BoundaryFrame traffic to this stage's own neighbours - see entry_bridge.hpp
// for how a reader tells the two apart (a BoundaryFrame starts with kBoundaryMagic; these do not). The
// shapes mirror Tectonic's Go-side internal/engine/protocol.go exactly: same field names, same meaning.

/// `gen`: one request. `max_new` doubles as this stage's own stop condition (see entry_bridge.hpp) since
/// nothing downstream of the bridge tracks it - the old Python daemon prototype's echo loop used to decide
/// this externally; here the engine does, because the Go daemon does not speak BoundaryFrame to do it.
struct GenFields {
    std::string request_id;
    std::vector<int32_t> tokens;
    int max_new = 0;
    // The caller's session key (Tectonic ADR 0007 §4): entry normalizes this to a non-zero value for
    // every request it admits, but 0 stays a legal value at the protocol level - a non-tectonic
    // (`--serve`) caller of this same wire shape has no entry imposing that guarantee, and the field
    // simply travels unread by this stage's own compute either way (see stage_worker.hpp for where it
    // is written into the boundary frame instead of acted on here).
    uint64_t session_id = 0;
};
bool control_send_gen(BoundarySocket& sock, const GenFields& f, std::string& err);
/// `type_out` lets a caller peek what arrived without guessing; a non-"gen" message here is a protocol
/// error for this round (the entry bridge only ever expects `gen` from the daemon on this path).
bool control_recv_gen(BoundarySocket& sock, GenFields& out, std::string& type_out, std::string& err);
/// Same parse as control_recv_gen, from bytes already read off the wire. For a reader (the entry bridge's
/// receive loop, stage_worker.hpp) that must peek a frame's shape - BoundaryFrame vs JSON - before deciding
/// which deserializer to call, and so cannot let this function do its own `recv_frame`.
bool control_parse_gen(const std::vector<uint8_t>& bytes, GenFields& out, std::string& type_out, std::string& err);

bool control_send_delta(BoundarySocket& sock, const std::string& request_id, int32_t token, std::string& err);

/// `tokens` is the whole completion, matching engine.md's `done` shape; `stats` is intentionally omitted
/// here - the entry bridge does not yet have anything worth reporting beyond what BoundaryFrame's own
/// StageWorkerStats already logs server-side, and a half-filled stats object would be worse than none.
bool control_send_done(BoundarySocket& sock, const std::string& request_id, const std::vector<int32_t>& tokens,
                       std::string& err);

bool control_send_error(BoundarySocket& sock, const std::string& request_id, const std::string& code,
                        const std::string& message, std::string& err);

}  // namespace strata::core
