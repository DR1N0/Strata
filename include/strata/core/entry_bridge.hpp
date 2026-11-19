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
// ONE SCOPE CUT REMAINS, FLAGGED HERE RATHER THAN SILENTLY ASSUMED:
//
// This bridge drives decode continuation, counted by `max_new`, for both a single-stage deployment (its own
// self-loop, stage_worker.hpp's `out.n_tensors == 0 && c.lo == 0` check) and a split one (a ring-closing
// frame back from the real last stage, told apart by `in.layer_begin > 0` - a locally-synthesized admission
// frame never sets it). Neither path detects EOS: the old Python daemon prototype's echo loop used to decide
// "stop sampling" by comparing the sampled token against a model's EOS id, which this bridge has no way to
// know (stage_worker.hpp is model-agnostic by design - see its own header comment). Until an EOS id (or an
// EOS flag on BoundaryFrame/the compute callback's output) is threaded down to this layer, a request only
// ever ends by reaching `max_new` or being cancelled. Real generation will run past the point a real model
// would have stopped itself; this is wrong for production text quality, not for the wire mechanics this
// round is about.
//
// This is independent of the request_id plumbing (wire v4) and the slot-indexed scheduling (the ADR 0005
// addendum) themselves, which are unconditionally correct and usable by a future, more complete bridge
// without further changes to boundary_wire.hpp or stage_worker.hpp's per-slot state.
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
