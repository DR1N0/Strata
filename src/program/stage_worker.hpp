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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace strata::program {
struct StageWorkerConfig {
    std::string worker_data_endpoint;    // Tectonic: was `agent`; the daemon's data connection, HOST:PORT
    // Tectonic: the signal connection (cancel/shutdown) and the hello handshake on both connections are
    // wired up below. NOT wired up yet: a request_id-multiplexed concurrency scheduler around
    // Verifier::run_slot_rows - this loop still serves one request at a time, so `cancel` below can only
    // mean "the one request currently in flight" (there is nothing else it could mean until concurrency
    // exists). See docs/FORK_LOG.md for the open question blocking that next step: whether `gen`'s
    // tokens[] replaces this loop's per-position BoundaryFrame traffic outright, or only carries the
    // entry stage's prompt while inter-stage handoff still travels as BoundaryFrame underneath it.
    std::string worker_signal_endpoint;
    std::string instance_id;
    int batch = 1;   // --batch's value, self-reported in hello; not yet used to size anything here
    int64_t lo = 0, hi = 0;     // this engine's layer range, and nothing else about its place in the mesh
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

    // Tectonic: the one request in flight can be told to stop; there is no slot/request_id dispatch yet
    // (see the config's field comment), so any cancel on the signal connection applies to it - there is
    // nothing else for it to mean until concurrency exists. shutdown closes the signal connection's own
    // socket reference immediately and closes `link` to unblock the main loop's blocking receive(), which
    // is what actually ends this function; it does not try to flush an in-progress position first.
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> shutdown_requested{false};
    std::thread signal_thread;
    if (have_signal) {
        signal_thread = std::thread([&signal_link, &cancel_requested, &shutdown_requested, &link]() {
            for (;;) {
                core::SignalMessage msg;
                std::string serr;
                if (!core::control_recv_signal(signal_link, msg, serr)) {
                    // The signal connection closing or erroring ends this thread quietly: the main loop's
                    // own data-connection failure (if any) is what stage_worker_run reports, not this one.
                    return;
                }
                if (msg.kind == core::SignalMessage::Kind::kCancel) {
                    cancel_requested.store(true, std::memory_order_relaxed);
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
    int64_t pf_n = -1, pf_elems = 0;
    std::vector<float> pf_rows;
    std::vector<int32_t> pf_tokens;
    for (;;) {
        BoundaryFrame in;
        if (!receive(in)) {
            // A shutdown closes `link` itself to unblock this call, so a shutdown-caused failure is the
            // expected way this function ends, not a transport error worth reporting as one.
            if (shutdown_requested.load(std::memory_order_relaxed)) {
                err.clear();
                return true;
            }
            return false;
        }
        if (prefill && in.kind == kBoundaryRequest) {
            pf_tokens.assign(in.next_token.begin(), in.next_token.end());
            pf_n = in.rows;
            pf_rows.clear();
            pf_elems = 0;
            if (stats) ++stats->requests;
            if (reset && !reset(err)) return false;
            continue;
        }
        if (prefill && pf_n >= 0 && in.kind == kBoundaryPrompt && in.rows > 1) {
            const BoundaryTensor* r = in.find("R");
            if (r == nullptr || r->rows != in.rows || r->elems_per_row <= 0) {
                err = "worker: a prompt chunk frame carries no usable R"; return false;
            }
            if (pf_elems == 0) pf_elems = r->elems_per_row;
            pf_rows.insert(pf_rows.end(), r->host, r->host + r->floats());
            if (stats) { stats->positions += r->rows; stats->prompt_positions += r->rows; }
            if ((int64_t) pf_rows.size() / pf_elems < pf_n) continue;
            const auto t0 = std::chrono::steady_clock::now();
            const StageWorkerEmit no_emit = [](BoundaryFrame&) { return true; };
            const bool ok = prefill(pf_tokens.data(), pf_n, 0, pf_rows.data(), pf_elems, no_emit, err);
            if (stats) {
                stats->prefill_ms +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                stats->prefill_tokens += pf_n;
            }
            pf_n = -1; pf_rows.clear(); pf_elems = 0;
            if (!ok) return false;
            // A stage that emits chunks implies completion by emitting them; a stage that accumulated them has to
            // say so, or the agent has no way to know the prompt is done and the next frame is welcome.
            if (c.lo > 0) {
                BoundaryFrame ack;
                ack.kind = kBoundaryReady;
                ack.rows = 1;
                if (!send(ack)) return false;
            }
            continue;
        }
        // A prompt frame carries the whole prompt, so the chunking is the engine's rather than the wire's.
        if (prefill && in.kind == kBoundaryPrompt && in.rows > 1) {
            if (in.pos0 == 0) {
                if (stats) ++stats->requests;
                if (reset && !reset(err)) return false;
            }
            auto emit = [&](BoundaryFrame& f) {
                f.kind = kBoundaryPrompt;          // the same prompt rows, chunk by chunk
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
            if (stats) ++stats->requests;
            if (reset && !reset(err)) return false;
        }
        // Tectonic: the cancellation checkpoint, at the same granularity as --serve's sl.stop (after every
        // produced token, not once per batch - see docs/FORK_LOG.md). There is only one request in flight
        // here, so a cancel always means this one. What this sends back is a known gap: kBoundaryEnd is the
        // only "this request is over" frame the wire has, so a cancelled request looks the same on the wire
        // as a normally-finished one to whatever is downstream of this engine - the contract's `error` with
        // a cancellation code is a JSON-connection concept this loop does not speak yet (same open question
        // as the handshake-vs-BoundaryFrame one in StageWorkerConfig's comment).
        if (cancel_requested.exchange(false, std::memory_order_relaxed)) {
            BoundaryFrame cancel_end;
            cancel_end.kind = kBoundaryEnd;
            cancel_end.rows = 1;
            cancel_end.pos0 = in.pos0;
            cancel_end.layer_begin = c.lo;
            cancel_end.layer_end = c.hi;
            if (!send(cancel_end)) return false;
            std::fprintf(stderr, "worker: cancelled at position %lld\n", (long long) in.pos0);
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
        // make any prefill accounting wrong, and the difference travels only in the kind.
        out.kind = in.kind == kBoundaryPrompt ? kBoundaryPrompt : kBoundaryRows;
        out.pos0 = in.pos0;
        out.rows = 1;
        out.layer_begin = c.lo;
        out.layer_end = c.hi;
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
