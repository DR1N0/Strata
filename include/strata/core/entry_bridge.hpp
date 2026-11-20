// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// THE ENTRY BRIDGE: id-mapping and admission bookkeeping for the entry stage's gen/delta/done <->
// BoundaryFrame translation (ADR 0005, extended by the ADR 0005 addendum - "Tier 1.5"). See control_wire.hpp
// for the JSON message shapes this bridge reads and writes, and stage_worker.hpp for where this plugs into
// the main loop - only an engine started with layer_begin == 0 (the entry stage) ever constructs one.
//
// `max_in_flight` passed to admit() is now `batch` resident slots, not a hardcoded 1: stage_worker.hpp
// assigns each admitted request its own slot (its own R/GDN/QSA-cache/PLE-history), and its main loop
// interleaves single-slot decode steps across whichever requests have input ready, rather than fully
// draining one request's self-feed chain before reading the socket for another. See docs/FORK_LOG.md's
// 2026-11-19 section for the real-code evidence that this is separable from Verifier::run_slot_rows'
// kernel-level batching (S > 1 in one call), which stays out of scope (ADR 0005's Tier 2).
//
// EOS detection (the scope cut this comment used to flag) is fixed: both ring-closing paths now check
// stage_worker.hpp's is_eos callback before record_token, so a real model stops where it actually would,
// not just at max_new or cancellation - see stage_worker.hpp and generate.cpp's wc.is_eos for the
// model's own --eos-ids/--stop-eos options reaching this layer without stage_worker.hpp itself having to
// know what a token means.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace strata::core {

class EntryBridge {
public:
    /// Admits a new request, minting a wire id unique within this process. False (wire_id untouched) only
    /// when `max_in_flight` is already reached - the caller must then reply with `error{code:"batch_full"}`
    /// rather than silently queuing (docs/design/engine.md's "并发" section).
    bool admit(const std::string& tectonic_request_id, int max_new, size_t max_in_flight, uint64_t& wire_id);

    /// The Tectonic-level string id for a wire id this bridge minted. False if not found: already
    /// released, or never admitted - a protocol error upstream, not something to guess about here.
    bool resolve(uint64_t wire_id, std::string& tectonic_request_id) const;

    /// Records one more produced token for `wire_id`. `tokens_so_far` is set to the full completion so
    /// far, for a `done` message's `tokens[]`. Returns false once `max_new` has been reached by this call
    /// (the caller must then send `done` and call release() - see the EOS scope cut above for what this
    /// does NOT detect).
    bool record_token(uint64_t wire_id, int32_t token, std::vector<int32_t>& tokens_so_far);

    /// Peeks the completion so far without recording a new token - for a sampled token that must never
    /// appear in the reported completion itself (EOS - see stage_worker.hpp's is_eos). False if not
    /// found, `out` cleared.
    bool tokens_so_far(uint64_t wire_id, std::vector<int32_t>& out) const;

    /// Forgets `wire_id`. A no-op if it is not present (release() racing a second release, or a cancel
    /// that already released it, must not be an error).
    void release(uint64_t wire_id);

    size_t in_flight() const;

private:
    mutable std::mutex mu_;
    struct Entry {
        std::string tectonic_request_id;
        int max_new = 0;
        std::vector<int32_t> tokens;
    };
    std::unordered_map<uint64_t, Entry> entries_;
    // Starts at 1, not 0: every caller outside this bridge (today's un-bridged BoundaryFrame traffic -
    // the old Python daemon prototype, stage_probe.py) leaves request_id at its default of 0. That is a
    // valid id, not a protocol sentinel (see boundary_wire.hpp's field comment) - but keeping this
    // bridge's own minted ids disjoint from it is a debugging convenience, not a correctness requirement.
    uint64_t next_wire_id_ = 1;
};

/// True if `bytes` is shaped like a serialized BoundaryFrame (starts with the little-endian kBoundaryMagic)
/// rather than a JSON control-wire message (starts, after any leading whitespace, with `{`). The entry
/// stage's receive loop uses this to tell its two kinds of incoming traffic apart before deciding which
/// deserializer to call - see stage_worker.hpp.
bool looks_like_boundary_frame(const std::vector<uint8_t>& bytes);

}  // namespace strata::core
