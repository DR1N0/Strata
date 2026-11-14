// A stage's engine, told nothing about the network.
//
// The engine links to the local agent and does one thing: take a position in, put a position out. It does not
// listen on any port, does not know which node follows it, and does not decide when the request is over - max_new
// and EOS belong to the agent, the only party that sees the whole route. Keeping topology out of the compute unit
// is what lets a route change (A->C becoming A->D) without restarting an engine.
#pragma once
#include "strata/core/boundary_socket.hpp"
#include "strata/core/boundary_wire.hpp"
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace strata::program {
struct StageWorkerConfig {
    std::string agent;          // the local agent's HOST:PORT
    int64_t lo = 0, hi = 0;     // this engine's layer range, and nothing else about its place in the mesh
};
// Per-stage accounting for the whole session. The agent reads request k's own cost from the difference between
// rows k and k-1.
struct StageWorkerStats {
    int64_t positions = 0, prompt_positions = 0, requests = 0;
    double compute_ms = 0, net_send_ms = 0, net_recv_ms = 0;
    int64_t net_out_bytes = 0, net_in_bytes = 0;
    int64_t experts_streamed = 0, file_read_bytes = 0;
};
// `input` is null exactly for a stage that owns the embedding (lo == 0): that stage starts from a token alone.
using StageWorkerCompute = std::function<bool(const core::BoundaryFrame* input, int32_t token, int64_t pos,
                                              core::BoundaryFrame& output, std::string& err)>;
// Called when a position 0 arrives, which is how a new request starts. session_zero covers R, the GDN recurrence,
// the QSA cache and the PLE history; an engine that skipped this would convolve request 2's rows with request 1's.
using StageWorkerReset = std::function<bool(std::string&)>;
// The prompt path. Given the whole prompt it computes every position in chunked batches and hands back one frame
// per chunk, carrying that chunk's `R` rows. This exists because a position-at-a-time prompt is not a design
// choice: a real 100K prompt would be 100K chain round-trips, and the engine's own chunked reader (Prefill) is
// what makes the cost proportional to chunks rather than to tokens.
using StageWorkerPrefill = std::function<bool(const core::BoundaryFrame& in,
                                              std::vector<core::BoundaryFrame>& out, std::string& err)>;
inline bool stage_worker_run(const StageWorkerConfig& c, const StageWorkerCompute& compute,
                             const StageWorkerReset& reset, const StageWorkerPrefill& prefill,
                             StageWorkerStats* stats, std::string& err) {
    using namespace core;
    BoundarySocket link;
    std::string host;
    int port = 0;
    if (!boundary_parse_address(c.agent, host, port, err) || !link.connect_to(host, port, 120000, err)) return false;
    if (!link.set_timeouts(120000, 120000, err)) return false;
    std::fprintf(stderr, "worker: linked to the agent at %s, layers [%lld,%lld)\n", c.agent.c_str(),
                 (long long) c.lo, (long long) c.hi);

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

    for (;;) {
        BoundaryFrame in;
        if (!receive(in)) return false;
        // A prompt frame carries the whole prompt, so the chunking is the engine's rather than the wire's.
        if (prefill && in.kind == kBoundaryPrompt && in.rows > 1) {
            if (in.pos0 == 0) {
                if (stats) ++stats->requests;
                if (reset && !reset(err)) return false;
            }
            std::vector<BoundaryFrame> frames;
            if (!prefill(in, frames, err)) return false;
            for (auto& f : frames) {
                f.kind = kBoundaryPrompt;          // the same prompt rows, chunk by chunk
                f.stage_begin = c.lo;
                f.stage_end = c.hi;
                if (!send(f)) return false;
            }
            if (stats) { stats->positions += in.rows; stats->prompt_positions += in.rows; }
            continue;
        }
        if (in.kind == kBoundaryEnd) {
            // END closes one request, not the engine: a resident stage serves the next request without reloading.
            if (stats) {
                std::fprintf(stderr,
                             "WDONE requests=%lld lo=%lld hi=%lld positions=%lld prompt_positions=%lld "
                             "compute_ms=%.1f net_send_ms=%.1f net_recv_ms=%.1f net_out_bytes=%lld "
                             "net_in_bytes=%lld experts_streamed=%lld file_read_bytes=%lld\n",
                             (long long) stats->requests, (long long) c.lo, (long long) c.hi,
                             (long long) stats->positions, (long long) stats->prompt_positions, stats->compute_ms,
                             stats->net_send_ms, stats->net_recv_ms, (long long) stats->net_out_bytes,
                             (long long) stats->net_in_bytes, (long long) stats->experts_streamed,
                             (long long) stats->file_read_bytes);
                std::fflush(stderr);
            }
            continue;
        }
        if (in.rows != 1) { err = "worker: one position per frame"; return false; }
        if (in.pos0 == 0) {                       // position 0 is where a request begins
            if (stats) ++stats->requests;
            if (reset && !reset(err)) return false;
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
        out.stage_begin = c.lo;
        out.stage_end = c.hi;
        if (!send(out)) return false;
    }
    if (stats) {
        std::fprintf(stderr,
                     "WDONE requests=%lld lo=%lld hi=%lld positions=%lld prompt_positions=%lld compute_ms=%.1f "
                     "net_send_ms=%.1f net_recv_ms=%.1f net_out_bytes=%lld net_in_bytes=%lld "
                     "experts_streamed=%lld file_read_bytes=%lld\n",
                     (long long) stats->requests, (long long) c.lo, (long long) c.hi, (long long) stats->positions,
                     (long long) stats->prompt_positions, stats->compute_ms, stats->net_send_ms, stats->net_recv_ms,
                     (long long) stats->net_out_bytes, (long long) stats->net_in_bytes,
                     (long long) stats->experts_streamed, (long long) stats->file_read_bytes);
        std::fflush(stderr);
    }
    return true;
}
} // namespace strata::program
