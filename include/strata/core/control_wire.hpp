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
// WHAT THIS FILE DOES NOT DECIDE: whether `gen`'s tokens[] replaces the per-position BoundaryFrame
// traffic for every stage, or only carries the entry stage's prompt while inter-stage handoff still
// travels as BoundaryFrame - see docs/FORK_LOG.md's open question. This file only implements the
// handshake (hello/hello_ok/hello_reject) and the two signal-connection messages (cancel/shutdown),
// none of which depend on that answer.
#pragma once

#include "strata/core/boundary_socket.hpp"
#include <cstdint>
#include <string>

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
    int batch = 1;                   // the concurrency this instance actually started with (--batch)
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

}  // namespace strata::core
