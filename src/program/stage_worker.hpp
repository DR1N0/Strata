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
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace strata::program {
struct StageWorkerConfig {
    std::string worker_data_endpoint;    // Tectonic: was `agent`; the daemon's data connection, HOST:PORT
    // Tectonic: the signal connection (cancel/shutdown) and the hello handshake on both connections are
    // wired up below, and (wire v4, ADR 0005) this loop's own request bookkeeping below is now keyed by
    // BoundaryFrame::request_id instead of one shared set of variables - Tier 1 concurrency, meaning more
    // than one request's frames can be in flight on this connection without corrupting each other. NOT
    // wired up yet: `Verifier::run_slot_rows` (Tier 2 - real concurrent *compute*, batching several
    // requests' decode steps into one kernel call). Tier 1 keeps this stage's compute state single-slot
    // (one session's R/GDN/QSA-cache/PLE-history resident at a time, same as before); the per-request map
    // below is dispatch/bookkeeping correctness, not a promise that two requests are ever computed at
    // once. See docs/FORK_LOG.md for the still-open question that blocks Tier 2: whether `gen`'s tokens[]
    // replaces this loop's per-position BoundaryFrame traffic outright, or only carries the entry stage's
    // prompt while inter-stage handoff still travels as BoundaryFrame underneath it.
    std::string worker_signal_endpoint;
    std::string instance_id;
    // --batch's value, self-reported in hello. Today (Tier 1) this is a protocol-level admission number -
    // how many requests the caller may let be in flight on the data connection at once without getting
    // `batch_full` - NOT a promise that `batch` independent sessions' compute state coexist: this stage
    // still runs one session's state at a time (see the comment above). A caller that actually wants N
    // independently-stateful concurrent sessions needs Tier 2, not a bigger `batch` number by itself.
    int batch = 1;
    int64_t lo = 0, hi = 0;     // this engine's layer range, and nothing else about its place in the mesh
    // Non-null only for the entry stage (lo == 0) of a single-stage (non-split) deployment - see
    // entry_bridge.hpp's header comment for exactly what this does and does not drive yet. Null (the
    // default) for every non-entry stage, and for an entry stage that has not been told to speak
    // gen/delta/done (today's real engine's current callers never set this).
    core::EntryBridge* entry_bridge = nullptr;
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
// `input` is null exactly for a stage that owns the embedding (lo == 0): that stage starts from a token alone.
using StageWorkerCompute = std::function<bool(const core::BoundaryFrame* input, int32_t token, int64_t pos,
                                              core::BoundaryFrame& output, std::string& err)>;
// Called when a position 0 arrives, which is how a new request starts. session_zero covers R, the GDN recurrence,
// the QSA cache and the PLE history; an engine that skipped this would convolve request 2's rows with request 1's.
using StageWorkerReset = std::function<bool(std::string&)>;
// The prompt path. Given the whole prompt it computes every position in chunked batches and hands each chunk over
// AS IT FINISHES, through `emit`. Emitting per chunk is not an optimisation: a 20K prompt's rows are about a
// gigabyte, so collecting them into one frame would exceed both the wire's payload bound and any sane receive
// buffer. This exists because a position-at-a-time prompt is not a design choice either - a real prompt would cost
// one chain round-trip per token, while chunks make it proportional to chunks.
using StageWorkerEmit = std::function<bool(core::BoundaryFrame&)>;
// `hand_rows` is null for the entry stage, which owns the embedding; every other stage is handed the prompt's whole
// row tensor, one row per token, in the same order.
using StageWorkerPrefill = std::function<bool(const int32_t* tokens, int64_t n, int64_t pos0,
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

    // Tectonic (wire v4, ADR 0005): per-request state, replacing the single shared set of variables this
    // loop used back when a connection could only ever carry one request's frames (wire v3 had no
    // request_id - see boundary_wire.hpp). The prefill accumulation buffers below are touched only by
    // this, the main, thread - the signal thread never reaches into them, so they need no synchronization
    // of their own. `cancelled` is an atomic because the signal thread DOES write it; `requests_mu` guards
    // only the map's own structure (insert/find/erase), which both threads touch. A `PerRequestState&` or
    // `*` obtained under the lock stays valid without holding it further: `unordered_map` never relocates
    // an existing element's storage on inserting another or erasing a different key, and this is the only
    // thread that ever erases.
    struct PerRequestState {
        int64_t pf_n = -1, pf_elems = 0;
        std::vector<float> pf_rows;
        std::vector<int32_t> pf_tokens;
        std::atomic<bool> cancelled{false};
    };
    std::mutex requests_mu;
    std::unordered_map<uint64_t, PerRequestState> requests;
    // Which request_id the main loop most recently started a position for. Call only while holding
    // requests_mu. A `cancel` on the signal connection names a Tectonic-level STRING request_id
    // (SignalMessage::request_id) that this loop has no way to resolve to a wire request_id - only the
    // entry stage's gen/delta/done bridge keeps that mapping (entry_bridge.hpp), and teaching every
    // interior stage the same mapping is mesh orchestration work this round does not do (docs/FORK_LOG.md).
    // So a cancel here can only mean "whichever request I am computing right now" - which is exactly
    // Tier 1's real constraint anyway (one session's compute state resident at a time, see
    // StageWorkerConfig::batch's comment), not a regression from the single-shared-bool version this
    // replaces.
    uint64_t current_request_id = 0;
    bool have_current = false;
    // Resets (or creates) one request's bookkeeping to "nothing accumulated, not cancelled". Call only
    // while holding requests_mu. Field-by-field rather than `requests[id] = PerRequestState{}`: the
    // atomic `cancelled` member has no copy/move assignment to call.
    auto reset_request_state = [&](uint64_t rid) -> PerRequestState& {
        PerRequestState& st = requests[rid];
        st.pf_n = -1;
        st.pf_elems = 0;
        st.pf_rows.clear();
        st.pf_tokens.clear();
        st.cancelled.store(false, std::memory_order_relaxed);
        return st;
    };

    // Tectonic: shutdown still ends the whole process regardless of how many requests are in flight -
    // there is no per-request "stop accepting new work but drain the rest" in this round.
    std::atomic<bool> shutdown_requested{false};
    std::thread signal_thread;
    if (have_signal) {
        signal_thread = std::thread([&signal_link, &shutdown_requested, &link, &requests_mu, &requests,
                                     &current_request_id, &have_current]() {
            for (;;) {
                core::SignalMessage msg;
                std::string serr;
                if (!core::control_recv_signal(signal_link, msg, serr)) {
                    // The signal connection closing or erroring ends this thread quietly: the main loop's
                    // own data-connection failure (if any) is what stage_worker_run reports, not this one.
                    return;
                }
                if (msg.kind == core::SignalMessage::Kind::kCancel) {
                    std::lock_guard<std::mutex> lock(requests_mu);
                    if (have_current) {
                        auto it = requests.find(current_request_id);
                        if (it != requests.end()) it->second.cancelled.store(true, std::memory_order_relaxed);
                    }
                } else if (msg.kind == core::SignalMessage::Kind::kShutdown) {
                    shutdown_requested.store(true, std::memory_order_relaxed);
                    link.close();   // unblocks the main loop's receive()
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

    auto send = [&](const BoundaryFrame& f) {
        std::vector<uint8_t> bytes;
        if (!boundary_serialize(f, bytes, err)) return false;
        const auto t0 = std::chrono::steady_clock::now();
        if (!link.send_frame(bytes, err)) return false;
        if (stats) {
            stats->net_send_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            stats->net_out_bytes += (int64_t) bytes.size();
        }
        return true;
    };
    auto receive = [&](BoundaryFrame& f) {
        std::vector<uint8_t> bytes;
        const auto t0 = std::chrono::steady_clock::now();
        if (!link.recv_frame(bytes, err, size_t(256) << 20)) return false;
        if (stats) {
            stats->net_recv_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            stats->net_in_bytes += (int64_t) bytes.size();
        }
        return boundary_deserialize(bytes.data(), bytes.size(), f, err);
    };

    // A stage with an upstream cannot act on one chunk frame: its prompt path needs EVERY row before it runs,
    // because those rows are one tensor in token order. So the upstream stage first sends the prompt itself (a
    // header frame carrying all its tokens), then the chunk frames; this side accumulates until it has them all.
    // An entry stage does the opposite: it receives the prompt and EMITS chunk frames as its prompt path runs.
    // (Tectonic, wire v4: this accumulation state now lives in `requests[in.request_id]`, not in variables
    // shared by every request on the connection - see the per-request state block above.)
    //
    // Tectonic (entry bridge, Tier 1, ADR 0005): frames this loop synthesizes for itself, consumed before
    // the socket is asked for more - the entry bridge's `gen` turns into a kBoundaryPrompt-shaped `in`
    // below, and a sampled token not yet at max_new turns into the next position's `in` (see the
    // per-position branch's tail). A plain std::vector, not a queue class: this never holds more than one
    // entry under Tier 1's single-resident-session constraint.
    std::vector<BoundaryFrame> pending;
    for (;;) {
        BoundaryFrame in;
        if (!pending.empty()) {
            in = std::move(pending.front());
            pending.erase(pending.begin());
        } else if (c.entry_bridge != nullptr) {
            // The entry stage's connection carries two shapes of frame now: BoundaryFrame bytes (to and
            // from this stage's own neighbours, exactly as before) and JSON gen/delta/done/error (to and
            // from this stage's own local daemon) - see control_wire.hpp's "RESOLVED" comment. Both share
            // the same u32-length-prefixed framing, so reading the raw bytes once and peeking the shape is
            // enough to tell them apart without a second framing primitive.
            std::vector<uint8_t> bytes;
            const auto t0 = std::chrono::steady_clock::now();
            if (!link.recv_frame(bytes, err, size_t(256) << 20)) {
                if (shutdown_requested.load(std::memory_order_relaxed)) {
                    err.clear();
                    return true;
                }
                return false;
            }
            if (stats) {
                stats->net_recv_ms +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                stats->net_in_bytes += (int64_t) bytes.size();
            }
            if (looks_like_boundary_frame(bytes)) {
                if (!boundary_deserialize(bytes.data(), bytes.size(), in, err)) return false;
            } else {
                GenFields gf;
                std::string type;
                if (!control_parse_gen(bytes, gf, type, err)) return false;
                if (gf.tokens.empty()) {
                    if (!control_send_error(link, gf.request_id, "invalid_request", "gen with no tokens", err))
                        return false;
                    continue;
                }
                uint64_t wire_id = 0;
                // Tier 1: effectively one request's compute state resident at a time (see
                // StageWorkerConfig::batch's comment) - 1 is deliberate, not a stand-in for "whatever
                // --batch reports", until Tier 2 gives independent per-request compute state.
                if (!c.entry_bridge->admit(gf.request_id, gf.max_new, /*max_in_flight=*/1, wire_id)) {
                    if (!control_send_error(link, gf.request_id, "batch_full",
                                            "this engine is already serving a request", err)) return false;
                    continue;
                }
                // Tectonic: "the token loop starts at the last prompt position, whose prediction is the
                // first generated token" (generate.cpp's own plan v0.3 P5 comment) - the entry stage's
                // non-bridged path already relies on this split, so the bridge has to reproduce it rather
                // than hand every prompt token to prefill() as one span. Not kBoundaryRequest for the
                // prefill-shaped part: that kind means "an upstream is about to hand me tensor chunks", a
                // non-entry stage's protocol - the entry stage owns the embedding and calls prefill()
                // directly from token ids, the same shape a real multi-row prompt frame already uses.
                //
                // The split is pushed onto `pending` and NOT also assigned to `in` here: every synthesized
                // frame goes through the exact same top-of-loop path a frame read off the wire would,
                // which is what keeps this one `pending` mechanism correct for both "gen produced a frame"
                // and "a decode step produced the next one" (see the per-position branch's tail) without
                // two copies of the same bookkeeping.
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
                    // Too short for the chunked prefill path (it requires rows > 1): run position 0 through
                    // the generic per-position path instead, same as a real one-token prompt would, just
                    // not reported as a delta (kBoundaryPrompt, not kBoundaryRows - see the per-position
                    // branch's delta-emission check).
                    BoundaryFrame prompt_part;
                    prompt_part.kind = kBoundaryPrompt;
                    prompt_part.request_id = wire_id;
                    prompt_part.pos0 = 0;
                    prompt_part.rows = 1;
                    prompt_part.next_token.assign(1, gf.tokens[0]);
                    pending.push_back(std::move(prompt_part));
                }
                // The last prompt token is tagged kBoundaryRows, not kBoundaryPrompt: positionally it is
                // still part of the prompt, but its OUTPUT is the first token the client sees, so it must
                // take the decode (delta-worthy) path, not the silent-prefill one. (n == 1: this is the
                // only frame, at pos0 0, which is exactly a one-token prompt's existing behaviour.)
                BoundaryFrame first_decode;
                first_decode.kind = kBoundaryRows;
                first_decode.request_id = wire_id;
                first_decode.pos0 = (int64_t) (n - 1);
                first_decode.rows = 1;
                first_decode.next_token.assign(1, (int32_t) last_token);
                pending.push_back(std::move(first_decode));
                // `in` is untouched on purpose (see the comment above): go round once more so the
                // top-of-loop `pending` check is what fills it in, instead of a second, divergent copy of
                // that logic here.
                continue;
            }
        } else if (!receive(in)) {
            // A shutdown closes `link` itself to unblock this call, so a shutdown-caused failure is the
            // expected way this function ends, not a transport error worth reporting as one.
            if (shutdown_requested.load(std::memory_order_relaxed)) {
                err.clear();
                return true;
            }
            return false;
        }
        if (prefill && in.kind == kBoundaryRequest) {
            {
                std::lock_guard<std::mutex> lock(requests_mu);
                PerRequestState& st = reset_request_state(in.request_id);
                st.pf_tokens.assign(in.next_token.begin(), in.next_token.end());
                st.pf_n = in.rows;
                current_request_id = in.request_id;
                have_current = true;
            }
            if (stats) ++stats->requests;
            if (reset && !reset(err)) return false;
            continue;
        }
        if (prefill && in.kind == kBoundaryPrompt && in.rows > 1) {
            // Only this (single) thread ever inserts or erases by key, so taking the lock just for the
            // find and releasing it before prefill() - which can run for a while - is correct: nothing
            // else can erase this entry while it is in use below.
            PerRequestState* st = nullptr;
            {
                std::lock_guard<std::mutex> lock(requests_mu);
                auto it = requests.find(in.request_id);
                if (it != requests.end() && it->second.pf_n >= 0) st = &it->second;
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
                const auto t0 = std::chrono::steady_clock::now();
                const StageWorkerEmit no_emit = [](BoundaryFrame&) { return true; };
                const bool ok = prefill(st->pf_tokens.data(), st->pf_n, 0, st->pf_rows.data(), st->pf_elems, no_emit, err);
                if (stats) {
                    stats->prefill_ms +=
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    stats->prefill_tokens += st->pf_n;
                }
                {
                    // Accumulation for this request is done either way - but the entry (and its cancel
                    // flag) stays alive: the decode positions that follow under the same request_id still
                    // need it, which is why this clears the fields rather than erasing the key.
                    std::lock_guard<std::mutex> lock(requests_mu);
                    st->pf_n = -1; st->pf_rows.clear(); st->pf_elems = 0;
                }
                if (!ok) return false;
                // A stage that emits chunks implies completion by emitting them; a stage that accumulated them has to
                // say so, or the agent has no way to know the prompt is done and the next frame is welcome.
                if (c.lo > 0) {
                    BoundaryFrame ack;
                    ack.kind = kBoundaryReady;
                    ack.rows = 1;
                    ack.request_id = in.request_id;
                    if (!send(ack)) return false;
                }
                continue;
            }
        }
        // A prompt frame carries the whole prompt, so the chunking is the engine's rather than the wire's.
        if (prefill && in.kind == kBoundaryPrompt && in.rows > 1) {
            if (in.pos0 == 0) {
                {
                    std::lock_guard<std::mutex> lock(requests_mu);
                    reset_request_state(in.request_id);
                    current_request_id = in.request_id;
                    have_current = true;
                }
                if (stats) ++stats->requests;
                if (reset && !reset(err)) return false;
            }
            auto emit = [&](BoundaryFrame& f) {
                f.kind = kBoundaryPrompt;          // the same prompt rows, chunk by chunk
                f.request_id = in.request_id;
                f.layer_begin = c.lo;
                f.layer_end = c.hi;
                return send(f);
            };
            const auto t_prefill = std::chrono::steady_clock::now();
            if (!prefill(in.next_token.data(), in.rows, in.pos0, nullptr, 0, emit, err)) return false;
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
            {
                std::lock_guard<std::mutex> lock(requests_mu);
                requests.erase(in.request_id);
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
        if (in.pos0 == 0) {                       // position 0 is where a request begins
            {
                std::lock_guard<std::mutex> lock(requests_mu);
                reset_request_state(in.request_id);
                current_request_id = in.request_id;
                have_current = true;
            }
            if (stats) ++stats->requests;
            if (reset && !reset(err)) return false;
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
            std::lock_guard<std::mutex> lock(requests_mu);
            current_request_id = in.request_id;
            have_current = true;
            auto it = requests.find(in.request_id);
            if (it != requests.end()) was_cancelled = it->second.cancelled.exchange(false, std::memory_order_relaxed);
        }
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
            std::lock_guard<std::mutex> lock(requests_mu);
            requests.erase(in.request_id);
            continue;
        }
        const int32_t token = in.next_token.empty() ? -1 : in.next_token[0];
        const auto t0 = std::chrono::steady_clock::now();
        BoundaryFrame out;
        if (!compute(c.lo > 0 ? &in : nullptr, token, in.pos0, out, err)) return false;
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
            std::vector<int32_t> tokens_so_far;
            const bool more = c.entry_bridge->record_token(in.request_id, sampled, tokens_so_far);
            if (!control_send_delta(link, tectonic_id, sampled, err)) return false;
            if (!more) {
                if (!control_send_done(link, tectonic_id, tokens_so_far, err)) return false;
                c.entry_bridge->release(in.request_id);
                std::lock_guard<std::mutex> lock(requests_mu);
                requests.erase(in.request_id);
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
