// A stage's engine, told nothing about the network.
//
// The engine links to the daemon (Tectonic: was "the local agent" - see StageWorkerConfig's field comments
// for why that word is retired) and does one thing: take a position in, put a position out. It does not
// listen on any port, does not know which node follows it, and does not decide when the request is over -
// max_new and EOS belong to the daemon, the only party that sees the whole route. Keeping topology out of
// the compute unit is what lets a route change (A->C becoming A->D) without restarting an engine.
#pragma once
#include "strata/core/boundary_socket.hpp"
#include "strata/core/boundary_wire.hpp"
#include "strata/core/control_wire.hpp"
#include "strata/core/entry_bridge.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace strata::program {
// True if a sampled token ends generation on its own - model knowledge this file otherwise has none
// of (see the header comment above). See StageWorkerConfig::is_eos.
using StageWorkerIsEOS = std::function<bool(int32_t token)>;

struct StageWorkerConfig {
    std::string worker_data_endpoint;    // Tectonic: was `agent`; the daemon's data connection, HOST:PORT
    // Tectonic: the signal connection (cancel/shutdown) and the hello handshake on both connections are
    // wired up below, and (wire v4, ADR 0005; extended by the ADR 0005 addendum below) this loop's own
    // request bookkeeping is keyed by BoundaryFrame::request_id, with `batch` independently resident slots
    // (R/GDN/QSA-cache/PLE-history each): more than one request can be ACTUALLY COMPUTING, interleaved -
    // one decode step at a time per slot, cycling - not just dispatched without corrupting each other.
    // This is still not `Verifier::run_slot_rows`' kernel-level BATCHING (Tier 2: several slots' decode
    // steps combined into one kernel call, S > 1): this loop always calls it (or the equivalent compute
    // callback) with exactly one active slot per invocation. See docs/FORK_LOG.md's 2026-11-19 section for
    // why that split is real in the underlying Verifier/BSlot implementation, not assumed.
    std::string worker_signal_endpoint;
    std::string instance_id;
    // --batch's value, self-reported in hello, AND the number of resident slots this loop actually
    // maintains (slot_state below is sized to it). A caller that wants true kernel-level batched
    // throughput (Tier 2) still needs separate work - this only interleaves single-slot compute steps,
    // which is a dispatch/scheduling change, not a GPU kernel-composition one.
    int batch = 1;
    int64_t lo = 0, hi = 0;     // this engine's layer range, and nothing else about its place in the mesh
    // Non-null only for the entry stage (lo == 0) of a single-stage (non-split) deployment - see
    // entry_bridge.hpp's header comment for exactly what this does and does not drive yet. Null (the
    // default) for every non-entry stage, and for an entry stage that has not been told to speak
    // gen/delta/done (today's real engine's current callers never set this).
    core::EntryBridge* entry_bridge = nullptr;
    // is_eos: true if a sampled token ends generation on its own, model knowledge this loop otherwise
    // has none of (see this struct's own "model-agnostic by design" framing above). Null (the default)
    // means no EOS checking - entry_bridge.hpp's ring-closing paths then stop only at max_new or
    // cancellation, same as before this existed. generate.cpp wires its own --eos-ids/--stop-eos
    // options into this without stage_worker.hpp ever learning what a token means.
    StageWorkerIsEOS is_eos;
};
// Per-stage accounting for the whole session. The agent reads request k's own cost from the difference between
// rows k and k-1.
struct StageWorkerStats {
    int64_t positions = 0, prompt_positions = 0, requests = 0;
    double compute_ms = 0, net_send_ms = 0, net_recv_ms = 0;
    int64_t net_out_bytes = 0, net_in_bytes = 0;
    int64_t experts_streamed = 0, file_read_bytes = 0;
    // The prompt path's own numbers, kept apart from the decode counters: a prompt is where the wall clock went in
    // the 20K run, and without these the only evidence was a total that nobody could attribute.
    double prefill_ms = 0;
    int64_t prefill_tokens = 0, prefill_chunks = 0, prefill_experts = 0, prefill_resident = 0;
    double prefill_host_ms = 0;
};
// `slot` (ADR 0005 addendum): which of `batch` resident sessions this call reads and writes - the caller
// (generate.cpp) binds it to that slot's own SessionState (Verifier::init_slots / run_slot_rows with S=1,
// rows={slot}) before running. Two different `slot` values must never alias the same state; this loop's
// job is exactly to guarantee that, not the callback's. `input` is null exactly for a stage that owns the
// embedding (lo == 0): that stage starts from a token alone.
using StageWorkerCompute = std::function<bool(int slot, const core::BoundaryFrame* input, int32_t token,
                                              int64_t pos, core::BoundaryFrame& output, std::string& err)>;
// Called when a position 0 arrives for `slot`, which is how a new request starts there. session_zero covers
// R, the GDN recurrence, the QSA cache and the PLE history, for THAT slot's SessionState only; an engine
// that skipped this, or reset the wrong slot, would convolve one request's rows with another's.
using StageWorkerReset = std::function<bool(int slot, std::string&)>;
// The prompt path, run against `slot`'s own SessionState/Prefill binding. Given the whole prompt it computes
// every position in chunked batches and hands each chunk over AS IT FINISHES, through `emit`. Emitting per
// chunk is not an optimisation: a 20K prompt's rows are about a gigabyte, so collecting them into one frame
// would exceed both the wire's payload bound and any sane receive buffer. This exists because a
// position-at-a-time prompt is not a design choice either - a real prompt would cost one chain round-trip
// per token, while chunks make it proportional to chunks. Prefill calls across different slots are never
// concurrent in this loop - see docs/FORK_LOG.md's 2026-11-19 section on why that is deliberate, not a gap.
using StageWorkerEmit = std::function<bool(core::BoundaryFrame&)>;
// `hand_rows` is null for the entry stage, which owns the embedding; every other stage is handed the prompt's whole
// row tensor, one row per token, in the same order.
using StageWorkerPrefill = std::function<bool(int slot, const int32_t* tokens, int64_t n, int64_t pos0,
                                              const float* hand_rows, int64_t hand_elems,
                                              const StageWorkerEmit& emit, std::string& err)>;
inline bool stage_worker_run(const StageWorkerConfig& c, const StageWorkerCompute& compute,
                             const StageWorkerReset& reset, const StageWorkerPrefill& prefill,
                             StageWorkerStats* stats, std::string& err) {
    using namespace core;
    BoundarySocket link;
    std::string host;
    int port = 0;
    // The socket timeout is the idle window, not the request's duration: a stage that has nothing to do yet must
    // sit still until the entry stage's prompt arrives. 120 s was measured to be too short on a 4K prompt (the
    // head's prefill alone is ~45 s and the tail had already given up), and a longer prompt is worse. The default
    // is now 10 minutes; STRATA_STAGE_IDLE_TIMEOUT_MS overrides it for a caller that wants a tighter leash.
    int idle_ms = 600000;
    if (const char* e = std::getenv("STRATA_STAGE_IDLE_TIMEOUT_MS")) {
        const int v = std::atoi(e);
        if (v > 0) idle_ms = v;
    }
    if (!boundary_parse_address(c.worker_data_endpoint, host, port, err) ||
        !link.connect_to(host, port, idle_ms, err)) return false;
    if (!link.set_timeouts(idle_ms, idle_ms, err)) return false;
    std::fprintf(stderr, "worker: linked to the daemon's data endpoint at %s, layers [%lld,%lld)\n",
                 c.worker_data_endpoint.c_str(), (long long) c.lo, (long long) c.hi);

    // Tectonic: the handshake, on both connections. Order between the two connections is not required
    // (docs/design/engine.md: "两条连接谁先连谁后连不强制顺序") - this implementation happens to connect
    // and shake hands on the data connection first, purely because it is the one already open above.
    core::HelloFields hello;
    hello.instance_id = c.instance_id;
    hello.layer_begin = c.lo;
    hello.layer_end = c.hi;
    hello.batch = c.batch;
    hello.cap_cancel = true;    // the signal connection below is what makes this true, not a promise
    hello.build_name = "strata";
    if (!core::control_send_hello(link, hello, err)) return false;
    if (!core::control_recv_hello_reply(link, err)) return false;

    core::BoundarySocket signal_link;
    bool have_signal = false;
    if (!c.worker_signal_endpoint.empty()) {
        std::string shost; int sport = 0;
        if (!boundary_parse_address(c.worker_signal_endpoint, shost, sport, err) ||
            !signal_link.connect_to(shost, sport, idle_ms, err)) return false;
        if (!signal_link.set_timeouts(idle_ms, idle_ms, err)) return false;
        if (!core::control_send_hello(signal_link, hello, err)) return false;
        if (!core::control_recv_hello_reply(signal_link, err)) return false;
        have_signal = true;
        std::fprintf(stderr, "worker: linked to the daemon's signal endpoint at %s\n",
                     c.worker_signal_endpoint.c_str());
    }
    if (!core::control_send_ready(link, err)) return false;

    // Tectonic (ADR 0005 addendum, "Tier 1.5"): `batch` independently resident slots, replacing wire v4's
    // single implicit session. Each slot owns its own prefill accumulation buffers (touched only by this,
    // the main, thread - the signal thread never reaches into them) and its own cancellation flag (an
    // atomic, since the signal thread DOES write it). `state_mu` guards only the two maps' own structure
    // (insert/find/erase) and the `occupied`/assignment bookkeeping, not the buffers themselves. A
    // `SlotState&`/`*` obtained under the lock stays valid without holding it further: neither container
    // relocates an existing element's storage on an unrelated insert or erase, and this is the only thread
    // that ever erases from `request_slot` or flips `occupied` off.
    struct SlotState {
        bool occupied = false;
        int64_t pf_n = -1, pf_elems = 0;
        std::vector<float> pf_rows;
        std::vector<int32_t> pf_tokens;
        std::atomic<bool> cancelled{false};
    };
    std::mutex state_mu;
    std::vector<SlotState> slot_state((size_t) std::max(1, c.batch));
    std::unordered_map<uint64_t, int> request_slot;   // request_id -> index into slot_state
    // Which request_id the main loop most recently started a position for. Call only while holding
    // state_mu. A `cancel` on the signal connection names a Tectonic-level STRING request_id
    // (SignalMessage::request_id) that this loop has no way to resolve to a wire request_id - only the
    // entry stage's gen/delta/done bridge keeps that mapping (entry_bridge.hpp), and teaching every
    // interior stage the same mapping is mesh orchestration work this round does not do (docs/FORK_LOG.md).
    // So a cancel here can only mean "whichever request I am computing right now" - true regardless of how
    // many slots are resident, since this is about which ONE position the signal race lands on.
    uint64_t current_request_id = 0;
    bool have_current = false;
    // Looks up request_id's slot, assigning a free one and resetting it ("nothing accumulated, not
    // cancelled") if this is the first time this request_id has been seen. -1 if new and no slot is free -
    // a protocol violation by whatever routed this request here (the entry bridge's own admission control,
    // or - for a non-entry stage - the mesh layer, neither of which this round allows to overrun `batch`
    // slots; see docs/FORK_LOG.md). Call only while holding state_mu. Field-by-field reset, not
    // `SlotState{}` assignment: the atomic `cancelled` member has no copy/move assignment to call.
    auto ensure_slot = [&](uint64_t rid) -> int {
        auto it = request_slot.find(rid);
        int idx;
        if (it != request_slot.end()) {
            idx = it->second;
        } else {
            idx = -1;
            for (size_t i = 0; i < slot_state.size(); ++i) {
                if (!slot_state[i].occupied) { idx = (int) i; break; }
            }
            if (idx < 0) return -1;
            slot_state[(size_t) idx].occupied = true;
            request_slot[rid] = idx;
        }
        SlotState& st = slot_state[(size_t) idx];
        st.pf_n = -1;
        st.pf_elems = 0;
        st.pf_rows.clear();
        st.pf_tokens.clear();
        st.cancelled.store(false, std::memory_order_relaxed);
        return idx;
    };
    // Looks up request_id's slot without touching its state - for a frame that continues a request already
    // in flight (not pos0 == 0, not kBoundaryRequest). -1 if not found: a position for a request this stage
    // never saw start, a protocol error. Call only while holding state_mu.
    auto find_slot = [&](uint64_t rid) -> int {
        auto it = request_slot.find(rid);
        return it == request_slot.end() ? -1 : it->second;
    };
    // Frees request_id's slot for reuse. A no-op if not found (a release racing another, or a request that
    // was never admitted). Call only while holding state_mu.
    auto release_slot = [&](uint64_t rid) {
        auto it = request_slot.find(rid);
        if (it == request_slot.end()) return;
        slot_state[(size_t) it->second].occupied = false;
        request_slot.erase(it);
    };

    // Tectonic: shutdown still ends the whole process regardless of how many requests are in flight -
    // there is no per-request "stop accepting new work but drain the rest" in this round.
    std::atomic<bool> shutdown_requested{false};
    std::thread signal_thread;
    if (have_signal) {
        signal_thread = std::thread([&signal_link, &shutdown_requested, &link, &state_mu, &request_slot,
                                     &slot_state, &current_request_id, &have_current]() {
            for (;;) {
                core::SignalMessage msg;
                std::string serr;
                if (!core::control_recv_signal(signal_link, msg, serr)) {
                    // The signal connection closing or erroring ends this thread quietly: the main loop's
                    // own data-connection failure (if any) is what stage_worker_run reports, not this one.
                    return;
                }
                if (msg.kind == core::SignalMessage::Kind::kCancel) {
                    std::lock_guard<std::mutex> lock(state_mu);
                    if (have_current) {
                        auto it = request_slot.find(current_request_id);
                        if (it != request_slot.end())
                            slot_state[(size_t) it->second].cancelled.store(true, std::memory_order_relaxed);
                    }
                } else if (msg.kind == core::SignalMessage::Kind::kShutdown) {
                    shutdown_requested.store(true, std::memory_order_relaxed);
                    link.close();   // unblocks the reader thread's blocking recv_frame
                    return;
                }
            }
        });
    }
    // Joins on every return path below, not just the bottom of the function: a std::thread that is still
    // joinable at destruction calls std::terminate, and this function returns from inside the loop on
    // most error paths.
    struct SignalJoinGuard {
        std::thread* t;
        ~SignalJoinGuard() { if (t && t->joinable()) t->join(); }
    } signal_join_guard{&signal_thread};

    const bool debug_wire = std::getenv("STRATA_DEBUG_STAGE_WIRE") != nullptr;
    auto send = [&](const BoundaryFrame& f) {
        std::vector<uint8_t> bytes;
        if (!boundary_serialize(f, bytes, err)) return false;
        if (debug_wire) {
            std::fprintf(stderr, "[wire-debug lo=%lld hi=%lld] SEND kind=%d rows=%lld pos0=%lld request_id=%llu n_tensors=%d\n",
                         (long long) c.lo, (long long) c.hi, (int) f.kind, (long long) f.rows, (long long) f.pos0,
                         (unsigned long long) f.request_id, f.n_tensors);
            std::fflush(stderr);
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (!link.send_frame(bytes, err)) return false;
        if (stats) {
            stats->net_send_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            stats->net_out_bytes += (int64_t) bytes.size();
        }
        return true;
    };

    // Tectonic (real cross-machine evidence, 2026-11-21): the per-position was_cancelled check below
    // only ever runs when a frame for THAT SAME request_id arrives - fine for a cancel racing a still-
    // flowing decode, but an explicit cancel (as opposed to a disconnect this loop's upstream notices on
    // its own) makes the upstream stop sending entirely, so that frame never comes and the slot stays
    // held forever (observed: a cancelled request's tail-side slot wedged until a full worker restart,
    // confirmed via real cross-machine mesh debug logging - see docs/FORK_LOG.md). Called from the main
    // loop's own idle-wait timeout (not the signal thread - sending stays on this one thread, same as
    // every other `send`/`control_send_*` call here), it sweeps for slots the signal thread marked
    // cancelled that a frame will never arrive to close out, and closes them exactly like the
    // was_cancelled branch does.
    auto drain_cancelled_slots = [&]() -> bool {
        std::vector<uint64_t> to_close;
        {
            std::lock_guard<std::mutex> lock(state_mu);
            for (auto& kv : request_slot) {
                if (slot_state[(size_t) kv.second].cancelled.exchange(false, std::memory_order_relaxed)) {
                    to_close.push_back(kv.first);
                }
            }
        }
        for (uint64_t rid : to_close) {
            std::string tectonic_id;
            if (c.entry_bridge != nullptr && c.entry_bridge->resolve(rid, tectonic_id)) {
                if (!control_send_error(link, tectonic_id, "cancelled", "request cancelled", err)) return false;
                c.entry_bridge->release(rid);
                std::fprintf(stderr, "worker: cancelled request %llu (%s) while idle (no further frame was coming)\n",
                             (unsigned long long) rid, tectonic_id.c_str());
            } else {
                BoundaryFrame cancel_end;
                cancel_end.kind = kBoundaryEnd;
                cancel_end.request_id = rid;
                cancel_end.rows = 1;
                cancel_end.pos0 = 0;
                cancel_end.layer_begin = c.lo;
                cancel_end.layer_end = c.hi;
                if (!send(cancel_end)) return false;
                std::fprintf(stderr, "worker: cancelled request %llu while idle (no further frame was coming)\n",
                             (unsigned long long) rid);
            }
            std::lock_guard<std::mutex> lock(state_mu);
            release_slot(rid);
        }
        return true;
    };

    // Tectonic (ADR 0005 addendum): `link` is now read by its OWN thread, decoupled from the main loop's
    // compute/dispatch. Without this, admitting a new request while another is already resident would be
    // impossible: the old design only ever read the socket when it had nothing else to do, so a slot's own
    // decode-continuation frames (self-fed or arriving from a neighbour) always starved out a waiting `gen`
    // until that slot's request fully finished (see docs/FORK_LOG.md's 2026-11-19 section for how this was
    // found - the same way the Tier 1 round found the current_request_id bug, by writing the test first).
    // `BoundarySocket::send`/`recv` touch no shared mutable state beyond the OS file descriptor itself (the
    // same reasoning the signal connection's own independent thread already relies on), so a reader thread
    // here and `send` above on the main thread are safe together.
    std::mutex incoming_mu;
    std::condition_variable incoming_cv;
    std::deque<std::vector<uint8_t>> incoming;
    bool reader_done = false;
    std::string reader_err;
    std::thread reader_thread([&] {
        for (;;) {
            std::vector<uint8_t> bytes;
            const auto t0 = std::chrono::steady_clock::now();
            if (!link.recv_frame(bytes, reader_err, size_t(256) << 20)) {
                if (debug_wire) {
                    std::fprintf(stderr, "[wire-debug lo=%lld hi=%lld] RECV FAILED: %s\n",
                                 (long long) c.lo, (long long) c.hi, reader_err.c_str());
                    std::fflush(stderr);
                }
                std::lock_guard<std::mutex> lock(incoming_mu);
                reader_done = true;
                incoming_cv.notify_all();
                return;
            }
            if (debug_wire) {
                std::fprintf(stderr, "[wire-debug lo=%lld hi=%lld] RECV bytes=%zu\n",
                             (long long) c.lo, (long long) c.hi, bytes.size());
                std::fflush(stderr);
            }
            if (stats) {
                stats->net_recv_ms +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                stats->net_in_bytes += (int64_t) bytes.size();
            }
            {
                std::lock_guard<std::mutex> lock(incoming_mu);
                incoming.push_back(std::move(bytes));
            }
            incoming_cv.notify_one();
        }
    });
    struct ReaderJoinGuard {
        std::thread* t;
        ~ReaderJoinGuard() { if (t && t->joinable()) t->join(); }
    } reader_join_guard{&reader_thread};
    // Declared last so it destructs first: closes both sockets before the join guards above run theirs.
    // Without this, any return other than the signal thread's own kShutdown handling leaves the reader
    // (and signal thread) blocked in recv_frame forever, and the joins above never complete.
    struct CloseLinksGuard {
        BoundarySocket* link;
        BoundarySocket* signal_link;
        ~CloseLinksGuard() {
            if (link) link->close();
            if (signal_link) signal_link->close();
        }
    } close_links_guard{&link, have_signal ? &signal_link : nullptr};

    // A stage with an upstream cannot act on one chunk frame: its prompt path needs EVERY row before it runs,
    // because those rows are one tensor in token order. So the upstream stage first sends the prompt itself (a
    // header frame carrying all its tokens), then the chunk frames; this side accumulates until it has them all.
    // An entry stage does the opposite: it receives the prompt and EMITS chunk frames as its prompt path runs.
    // (Tectonic, wire v4: this accumulation state lives per-slot in `slot_state`, not in variables shared by
    // every request on the connection.)
    //
    // Tectonic (entry bridge + ADR 0005 addendum, "Tier 1.5"): frames this loop synthesizes for itself - the
    // entry bridge's `gen` turns into one or two kBoundaryPrompt/kBoundaryRows-shaped `in`s, and a sampled
    // token not yet at max_new turns into the next position's `in` (see the per-position branch's tail) -
    // are queued in `pending` and consumed before falling back to the socket's own queue (`incoming`,
    // filled by the reader thread above). Checking `incoming` FIRST, every iteration, rather than draining
    // `pending` to empty first, is what makes admitting a new request possible while another slot is mid
    // self-feed: the old (Tier 1) order starved new admissions behind an already-resident slot's decode
    // loop for as long as that slot stayed busy - found by writing this round's test, not predicted
    // (docs/FORK_LOG.md's 2026-11-19 section).
    std::vector<BoundaryFrame> pending;
    for (;;) {
        BoundaryFrame in;
        bool have_in = false;

        {
            std::unique_lock<std::mutex> lock(incoming_mu);
            if (!incoming.empty()) {
                std::vector<uint8_t> bytes = std::move(incoming.front());
                incoming.pop_front();
                lock.unlock();
                if (looks_like_boundary_frame(bytes)) {
                    if (!boundary_deserialize(bytes.data(), bytes.size(), in, err)) return false;
                    have_in = true;
                } else if (c.entry_bridge == nullptr) {
                    err = "worker: non-bridged stage received a non-BoundaryFrame message";
                    return false;
                } else {
                    GenFields gf;
                    std::string type;
                    if (!control_parse_gen(bytes, gf, type, err)) return false;
                    if (gf.tokens.empty()) {
                        if (!control_send_error(link, gf.request_id, "invalid_request", "gen with no tokens", err))
                            return false;
                    } else {
                        uint64_t wire_id = 0;
                        if (!c.entry_bridge->admit(gf.request_id, gf.max_new, (size_t) std::max(1, c.batch),
                                                   wire_id)) {
                            if (!control_send_error(link, gf.request_id, "batch_full", "no free slot for a new request",
                                                    err)) return false;
                        } else {
                            // Tectonic: "the token loop starts at the last prompt position, whose prediction
                            // is the first generated token" (generate.cpp's own plan v0.3 P5 comment) - the
                            // entry stage's non-bridged path already relies on this split, so the bridge has
                            // to reproduce it rather than hand every prompt token to prefill() as one span.
                            // Not kBoundaryRequest for the prefill-shaped part: that kind means "an upstream
                            // is about to hand me tensor chunks", a non-entry stage's protocol - the entry
                            // stage owns the embedding and calls prefill() directly from token ids, the same
                            // shape a real multi-row prompt frame already uses.
                            const size_t n = gf.tokens.size();
                            const int64_t last_token = gf.tokens.back();
                            if (n > 2) {
                                BoundaryFrame prompt_part;
                                prompt_part.kind = kBoundaryPrompt;
                                prompt_part.request_id = wire_id;
                                prompt_part.pos0 = 0;
                                prompt_part.rows = (int64_t) (n - 1);
                                prompt_part.next_token.assign(gf.tokens.begin(), gf.tokens.end() - 1);
                                pending.push_back(std::move(prompt_part));
                            } else if (n == 2) {
                                // Too short for the chunked prefill path (it requires rows > 1): run position
                                // 0 through the generic per-position path instead, same as a real one-token
                                // prompt would, just not reported as a delta (kBoundaryPrompt, not
                                // kBoundaryRows - see the per-position branch's delta-emission check).
                                BoundaryFrame prompt_part;
                                prompt_part.kind = kBoundaryPrompt;
                                prompt_part.request_id = wire_id;
                                prompt_part.pos0 = 0;
                                prompt_part.rows = 1;
                                prompt_part.next_token.assign(1, gf.tokens[0]);
                                pending.push_back(std::move(prompt_part));
                            }
                            // The last prompt token is tagged kBoundaryRows, not kBoundaryPrompt: positionally
                            // it is still part of the prompt, but its OUTPUT is the first token the client
                            // sees, so it must take the decode (delta-worthy) path. (n == 1: this is the only
                            // frame, at pos0 0, exactly a one-token prompt's existing behaviour.)
                            BoundaryFrame first_decode;
                            first_decode.kind = kBoundaryRows;
                            first_decode.request_id = wire_id;
                            first_decode.pos0 = (int64_t) (n - 1);
                            first_decode.rows = 1;
                            first_decode.next_token.assign(1, (int32_t) last_token);
                            pending.push_back(std::move(first_decode));
                        }
                    }
                }
            }
        }

        if (!have_in && !pending.empty()) {
            in = std::move(pending.front());
            pending.erase(pending.begin());
            have_in = true;
        }

        if (!have_in) {
            std::unique_lock<std::mutex> lock(incoming_mu);
            // Tectonic: bounded, not indefinite - an explicit cancel can mean no further frame for this
            // slot is ever coming (see drain_cancelled_slots's own comment), and only a periodic wake-up
            // gives that case a chance to be noticed and closed out instead of holding its slot forever.
            const bool woke_for_frame =
                incoming_cv.wait_for(lock, std::chrono::milliseconds(50),
                                      [&] { return !incoming.empty() || reader_done; });
            if (incoming.empty() && reader_done) {
                // A shutdown closes `link` itself to unblock the reader thread's recv_frame, so a
                // shutdown-caused close is the expected way this function ends, not a transport error.
                lock.unlock();
                if (shutdown_requested.load(std::memory_order_relaxed)) {
                    err.clear();
                    return true;
                }
                err = reader_err.empty() ? "worker: data connection closed" : reader_err;
                return false;
            }
            lock.unlock();
            if (!woke_for_frame && !drain_cancelled_slots()) return false;
            continue;   // something is in `incoming` now - the opportunistic check above picks it up
        }
        if (prefill && in.kind == kBoundaryRequest) {
            int slot = -1;
            {
                std::lock_guard<std::mutex> lock(state_mu);
                slot = ensure_slot(in.request_id);
                if (slot >= 0) {
                    SlotState& st = slot_state[(size_t) slot];
                    st.pf_tokens.assign(in.next_token.begin(), in.next_token.end());
                    st.pf_n = in.rows;
                    current_request_id = in.request_id;
                    have_current = true;
                }
            }
            if (slot < 0) { err = "worker: no free slot for a new request (kBoundaryRequest)"; return false; }
            if (stats) ++stats->requests;
            if (reset && !reset(slot, err)) return false;
            continue;
        }
        if (prefill && in.kind == kBoundaryPrompt && in.rows > 1) {
            // Only this (single) thread ever inserts or erases by key, so taking the lock just for the
            // find and releasing it before prefill() - which can run for a while - is correct: nothing
            // else can erase this entry while it is in use below.
            SlotState* st = nullptr;
            {
                std::lock_guard<std::mutex> lock(state_mu);
                const int slot = find_slot(in.request_id);
                if (slot >= 0 && slot_state[(size_t) slot].pf_n >= 0) st = &slot_state[(size_t) slot];
            }
            if (st == nullptr && debug_wire) {
                std::fprintf(stderr, "[wire-debug lo=%lld hi=%lld] DROPPED kBoundaryPrompt chunk for request_id=%llu "
                             "- no kBoundaryRequest ever initialized this slot's pf_n\n",
                             (long long) c.lo, (long long) c.hi, (unsigned long long) in.request_id);
                std::fflush(stderr);
            }
            if (st != nullptr) {
                const BoundaryTensor* r = in.find("R");
                if (r == nullptr || r->rows != in.rows || r->elems_per_row <= 0) {
                    err = "worker: a prompt chunk frame carries no usable R"; return false;
                }
                if (st->pf_elems == 0) st->pf_elems = r->elems_per_row;
                st->pf_rows.insert(st->pf_rows.end(), r->host, r->host + r->floats());
                if (stats) { stats->positions += r->rows; stats->prompt_positions += r->rows; }
                if ((int64_t) st->pf_rows.size() / st->pf_elems < st->pf_n) continue;
                int slot = -1;
                {
                    std::lock_guard<std::mutex> lock(state_mu);
                    slot = find_slot(in.request_id);
                }
                if (slot < 0) { err = "worker: slot vanished mid-accumulation"; return false; }
                const auto t0 = std::chrono::steady_clock::now();
                const StageWorkerEmit no_emit = [](BoundaryFrame&) { return true; };
                const bool ok =
                    prefill(slot, st->pf_tokens.data(), st->pf_n, 0, st->pf_rows.data(), st->pf_elems, no_emit, err);
                if (stats) {
                    stats->prefill_ms +=
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    stats->prefill_tokens += st->pf_n;
                }
                {
                    // Accumulation for this request is done either way - but the slot (and its cancel flag)
                    // stays resident: the decode positions that follow under the same request_id still need
                    // it, which is why this clears the fields rather than releasing the slot.
                    std::lock_guard<std::mutex> lock(state_mu);
                    st->pf_n = -1; st->pf_rows.clear(); st->pf_elems = 0;
                }
                if (!ok) return false;
                // No ack here: nothing downstream waits for one (entry sends the first decode frame right after
                // its own prefill, not after hearing from this stage), and the mesh relay forwards every
                // boundary-shaped frame it sees - a stray kBoundaryReady would reach entry's own dispatch and be
                // mistaken for a new position-0 request (found by running the real two-node case, not review).
                continue;
            }
        }
        // A prompt frame carries the whole prompt, so the chunking is the engine's rather than the wire's.
        if (prefill && in.kind == kBoundaryPrompt && in.rows > 1) {
            int slot = -1;
            if (in.pos0 == 0) {
                {
                    std::lock_guard<std::mutex> lock(state_mu);
                    slot = ensure_slot(in.request_id);
                    if (slot >= 0) {
                        current_request_id = in.request_id;
                        have_current = true;
                    }
                }
                if (slot < 0) { err = "worker: no free slot for a new request (whole-prompt)"; return false; }
                if (stats) ++stats->requests;
                if (reset && !reset(slot, err)) return false;
            } else {
                std::lock_guard<std::mutex> lock(state_mu);
                slot = find_slot(in.request_id);
            }
            if (slot < 0) { err = "worker: a prompt frame continues a request this stage never started"; return false; }
            auto emit = [&](BoundaryFrame& f) {
                // kBoundaryRequest (the accumulation header a split prefill sends once, before any rows) is the
                // one kind this prompt path emits itself rather than defaulting - everything else is the same
                // prompt rows, chunk by chunk.
                if (f.kind != kBoundaryRequest) f.kind = kBoundaryPrompt;
                f.request_id = in.request_id;
                f.layer_begin = c.lo;
                f.layer_end = c.hi;
                return send(f);
            };
            const auto t_prefill = std::chrono::steady_clock::now();
            const bool prefill_ok = prefill(slot, in.next_token.data(), in.rows, in.pos0, nullptr, 0, emit, err);
            if (debug_wire) {
                std::fprintf(stderr, "[wire-debug lo=%lld hi=%lld] prefill() call site returned ok=%d err=%s\n",
                             (long long) c.lo, (long long) c.hi, (int) prefill_ok, err.c_str());
                std::fflush(stderr);
            }
            if (!prefill_ok) return false;
            if (stats) {
                stats->positions += in.rows;
                stats->prompt_positions += in.rows;
                stats->prefill_ms +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_prefill).count();
                stats->prefill_tokens += in.rows;
            }
            continue;
        }
        if (in.kind == kBoundaryEnd) {
            // END closes one request, not the engine: a resident stage serves the next request without reloading.
            // Arriving here at entry is always a downstream stage's own cancellation (a normal finish is entry's
            // own call - see the ring-closing branch above, which sends kBoundaryEnd onward but never receives it
            // back): the client is still on an open connection waiting for something, so tell it.
            if (c.entry_bridge != nullptr && c.lo == 0) {
                std::string tectonic_id;
                if (c.entry_bridge->resolve(in.request_id, tectonic_id)) {
                    if (!control_send_error(link, tectonic_id, "cancelled", "request cancelled", err)) return false;
                    c.entry_bridge->release(in.request_id);
                }
            }
            {
                std::lock_guard<std::mutex> lock(state_mu);
                release_slot(in.request_id);
            }
            if (stats) {
                std::fprintf(stderr,
                             "WDONE requests=%lld lo=%lld hi=%lld positions=%lld prompt_positions=%lld "
                             "compute_ms=%.1f net_send_ms=%.1f net_recv_ms=%.1f net_out_bytes=%lld "
                             "net_in_bytes=%lld experts_streamed=%lld file_read_bytes=%lld prefill_ms=%.1f prefill_tokens=%lld prefill_chunks=%lld prefill_experts=%lld prefill_resident=%lld prefill_host_ms=%.1f\n",
                             (long long) stats->requests, (long long) c.lo, (long long) c.hi,
                             (long long) stats->positions, (long long) stats->prompt_positions, stats->compute_ms,
                             stats->net_send_ms, stats->net_recv_ms, (long long) stats->net_out_bytes,
                             (long long) stats->net_in_bytes, (long long) stats->experts_streamed,
                             (long long) stats->file_read_bytes,
                             stats->prefill_ms, (long long) stats->prefill_tokens, (long long) stats->prefill_chunks, (long long) stats->prefill_experts,
                             (long long) stats->prefill_resident, stats->prefill_host_ms);
                std::fflush(stderr);
            }
            continue;
        }
        if (in.rows != 1) { err = "worker: one position per frame"; return false; }
        int slot = -1;
        if (in.pos0 == 0) {                       // position 0 is where a request begins
            {
                std::lock_guard<std::mutex> lock(state_mu);
                slot = ensure_slot(in.request_id);
                if (slot >= 0) {
                    current_request_id = in.request_id;
                    have_current = true;
                }
            }
            if (slot < 0) { err = "worker: no free slot for a new request (position 0)"; return false; }
            if (stats) ++stats->requests;
            if (reset && !reset(slot, err)) return false;
        }
        // Tectonic: the cancellation checkpoint, at the same granularity as --serve's sl.stop (after every
        // produced token, not once per batch - see docs/FORK_LOG.md). Per-request_id now (wire v4), but see
        // the per-request state block's comment on `current_request_id`: a cancel from the signal connection
        // can still only target "whatever I am computing right now", because that connection names a string
        // id this loop cannot resolve on its own. `current_request_id` has to move to THIS position on every
        // iteration, not just at pos0 == 0 - "whatever I am computing right now" means the position about to
        // be computed, and with more than one request_id now able to appear on the same connection (wire
        // v4), a later request's non-zero positions must not leave an earlier request looking "current".
        bool was_cancelled = false;
        {
            std::lock_guard<std::mutex> lock(state_mu);
            current_request_id = in.request_id;
            have_current = true;
            if (slot < 0) slot = find_slot(in.request_id);
            if (slot >= 0) was_cancelled = slot_state[(size_t) slot].cancelled.exchange(false, std::memory_order_relaxed);
        }
        if (slot < 0) { err = "worker: a position continues a request this stage never started"; return false; }
        if (was_cancelled) {
            // The entry bridge gets the real answer here (JSON `error{code:"cancelled"}`, the contract's
            // actual shape) instead of the known wire-level gap below: it knows which request_ids are its
            // own, so it does not need a new BoundaryKind to tell a cancellation apart from a normal finish.
            std::string tectonic_id;
            if (c.entry_bridge != nullptr && c.entry_bridge->resolve(in.request_id, tectonic_id)) {
                if (!control_send_error(link, tectonic_id, "cancelled", "request cancelled", err)) return false;
                c.entry_bridge->release(in.request_id);
                std::fprintf(stderr, "worker: cancelled request %llu (%s) at position %lld\n",
                             (unsigned long long) in.request_id, tectonic_id.c_str(), (long long) in.pos0);
            } else {
                // No bridge, or a request_id it does not recognize (a non-entry stage, or a legacy
                // unbridged caller): kBoundaryEnd is the only "this request is over" frame the wire has, so
                // a cancelled request looks the same on the wire as a normally-finished one to whatever is
                // downstream of this engine - the contract's `error` with a cancellation code is a
                // JSON-connection concept this path does not reach (same open question as the
                // handshake-vs-BoundaryFrame one in StageWorkerConfig's comment, for a non-entry stage).
                BoundaryFrame cancel_end;
                cancel_end.kind = kBoundaryEnd;
                cancel_end.request_id = in.request_id;
                cancel_end.rows = 1;
                cancel_end.pos0 = in.pos0;
                cancel_end.layer_begin = c.lo;
                cancel_end.layer_end = c.hi;
                if (!send(cancel_end)) return false;
                std::fprintf(stderr, "worker: cancelled request %llu at position %lld\n",
                             (unsigned long long) in.request_id, (long long) in.pos0);
            }
            std::lock_guard<std::mutex> lock(state_mu);
            release_slot(in.request_id);
            continue;
        }
        if (c.entry_bridge != nullptr && c.lo == 0 && in.layer_begin > 0) {
            // A split entry's own locally-synthesized frames (prompt_part, first_decode) never set
            // layer_begin, so layer_begin > 0 here can only mean this came back off the wire: the ring
            // closed, and in.next_token[0] is the real stage's real sampled token, not something to embed
            // and forward again as-is. Report it, then (if more are wanted) re-embed it at the next
            // position - this is the split counterpart of the n_tensors == 0 check below, which only a
            // single-stage deployment's self-loop ever hits.
            const int32_t sampled = in.next_token.empty() ? -1 : in.next_token[0];
            std::string tectonic_id;
            if (!c.entry_bridge->resolve(in.request_id, tectonic_id)) {
                err = "worker: entry bridge lost request_id " + std::to_string(in.request_id);
                return false;
            }
            // An EOS token is a control signal, not content: it is never reported as a delta or kept in
            // the completion (same as a real model-serving API) - peek the completion instead of
            // recording this token into it, and stop the same way max_new's own "!more" already does.
            const bool eos = c.is_eos && c.is_eos(sampled);
            std::vector<int32_t> tokens_so_far;
            bool more;
            if (eos) {
                c.entry_bridge->tokens_so_far(in.request_id, tokens_so_far);
                more = false;
            } else {
                more = c.entry_bridge->record_token(in.request_id, sampled, tokens_so_far);
                if (!control_send_delta(link, tectonic_id, sampled, err)) return false;
            }
            if (!more) {
                if (!control_send_done(link, tectonic_id, tokens_so_far, err)) return false;
                c.entry_bridge->release(in.request_id);
                // Tell the downstream too: it has no entry bridge and no other way to learn this request is
                // over, and with a resident slot per request (not reallocated per call) its slot would stay
                // held forever otherwise - found by running a second request right after the first, which
                // the held slot then refused outright.
                BoundaryFrame end_frame;
                end_frame.kind = kBoundaryEnd;
                end_frame.request_id = in.request_id;
                end_frame.rows = 1;
                end_frame.layer_begin = c.lo;
                end_frame.layer_end = c.hi;
                if (!send(end_frame)) return false;
                std::lock_guard<std::mutex> lock(state_mu);
                release_slot(in.request_id);
                continue;
            }
            in.pos0 += 1;
            in.next_token.assign(1, sampled);
            in.layer_begin = 0;
        }
        const int32_t token = in.next_token.empty() ? -1 : in.next_token[0];
        const auto t0 = std::chrono::steady_clock::now();
        BoundaryFrame out;
        if (!compute(slot, c.lo > 0 ? &in : nullptr, token, in.pos0, out, err)) return false;
        if (stats) {
            stats->compute_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++stats->positions;
            if (in.kind == kBoundaryPrompt) ++stats->prompt_positions;
        }
        // Carry the prompt/decode distinction downstream: a stage that reported every position as decode would
        // make any prefill accounting wrong, and the difference travels only in the kind. request_id travels
        // the same way: every stage passes it through unchanged (wire v4), never renumbers it.
        out.kind = in.kind == kBoundaryPrompt ? kBoundaryPrompt : kBoundaryRows;
        out.request_id = in.request_id;
        out.pos0 = in.pos0;
        out.rows = 1;
        out.layer_begin = c.lo;
        out.layer_end = c.hi;
        if (c.entry_bridge != nullptr && c.lo == 0 && out.n_tensors == 0 && out.kind == kBoundaryPrompt) {
            // A silent prompt position the entry bridge synthesized to seed state (see its gen-handling
            // comment on the n == 2 case): not a real generated token, so nothing to report and nowhere
            // real to forward it to in a single-stage deployment - just move on to whatever comes next
            // (already queued in `pending`, or the wire).
            continue;
        }
        if (c.entry_bridge != nullptr && c.lo == 0 && out.n_tensors == 0) {
            // A tensor-free kBoundaryRows output at the entry stage is a genuinely sampled token:
            // compute_stage only skips boundary_capture when this stage owns the exit (see generate.cpp).
            // See entry_bridge.hpp's header comment for why this check is only correct for a single-stage
            // (non-split) deployment - a real split's ring-closing token is not handled here.
            const int32_t sampled = out.next_token.empty() ? -1 : out.next_token[0];
            std::string tectonic_id;
            if (!c.entry_bridge->resolve(in.request_id, tectonic_id)) {
                err = "worker: entry bridge lost request_id " + std::to_string(in.request_id);
                return false;
            }
            // See the split-stage ring-closing branch above for why EOS skips record_token/delta.
            const bool eos = c.is_eos && c.is_eos(sampled);
            std::vector<int32_t> tokens_so_far;
            bool more;
            if (eos) {
                c.entry_bridge->tokens_so_far(in.request_id, tokens_so_far);
                more = false;
            } else {
                more = c.entry_bridge->record_token(in.request_id, sampled, tokens_so_far);
                if (!control_send_delta(link, tectonic_id, sampled, err)) return false;
            }
            if (!more) {
                if (!control_send_done(link, tectonic_id, tokens_so_far, err)) return false;
                c.entry_bridge->release(in.request_id);
                std::lock_guard<std::mutex> lock(state_mu);
                release_slot(in.request_id);
                continue;
            }
            // Feed the sampled token back as the next position's input - the old Python daemon prototype's
            // echo loop used to do exactly this from outside the process; the Go daemon does not speak
            // BoundaryFrame to do it, so the engine does it here instead. See entry_bridge.hpp's EOS scope
            // cut for what this does not check before continuing (only max_new, no EOS token detection).
            BoundaryFrame next_in;
            next_in.kind = kBoundaryRows;
            next_in.request_id = in.request_id;
            next_in.pos0 = in.pos0 + 1;
            next_in.rows = 1;
            next_in.next_token.assign(1, sampled);
            pending.push_back(std::move(next_in));
            continue;
        }
        if (!send(out)) return false;
    }
    if (stats) {
        std::fprintf(stderr,
                     "WDONE requests=%lld lo=%lld hi=%lld positions=%lld prompt_positions=%lld compute_ms=%.1f "
                     "net_send_ms=%.1f net_recv_ms=%.1f net_out_bytes=%lld net_in_bytes=%lld "
                     "experts_streamed=%lld file_read_bytes=%lld prefill_ms=%.1f prefill_tokens=%lld prefill_chunks=%lld prefill_experts=%lld prefill_resident=%lld prefill_host_ms=%.1f\n",
                     (long long) stats->requests, (long long) c.lo, (long long) c.hi, (long long) stats->positions,
                     (long long) stats->prompt_positions, stats->compute_ms, stats->net_send_ms, stats->net_recv_ms,
                     (long long) stats->net_out_bytes, (long long) stats->net_in_bytes,
                     (long long) stats->experts_streamed, (long long) stats->file_read_bytes,
                     stats->prefill_ms, (long long) stats->prefill_tokens, (long long) stats->prefill_chunks,
                     (long long) stats->prefill_experts, (long long) stats->prefill_resident, stats->prefill_host_ms);
        std::fflush(stderr);
    }
    return true;
}
} // namespace strata::program
