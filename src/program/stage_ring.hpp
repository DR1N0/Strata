// Experimental process-separated layer ring. Transport does not schedule speculative windows.
#pragma once
#include "strata/core/boundary_socket.hpp"
#include "strata/core/boundary_wire.hpp"
#include <chrono>
#include <cstdio>
#include <functional>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <string>
#include <vector>

namespace strata::program {
struct StageRingConfig {
    std::string listen, downstream, orch;
    int64_t lo = 0, hi = 0, layers = 0, max_context = 0;
    uint64_t fingerprint = 0;
    std::vector<int64_t> eos_ids;
    bool stop_eos = true;
};
// Per-stage accounting for ONE request. Prompt positions and decode positions are counted apart because a stage's
// cost per position is what a caller compares against another split; the transport's byte and time totals are what
// tells bandwidth from latency. `experts_streamed` and `file_read_bytes` are filled by the compute callback, which
// owns the engine's counters - this header cannot see them.
struct StageRingStats {
    int64_t positions = 0, prompt_positions = 0;
    double compute_ms = 0, net_send_ms = 0, net_recv_ms = 0;
    int64_t net_out_bytes = 0, net_in_bytes = 0;
    int64_t experts_streamed = 0, file_read_bytes = 0;
};
// Compute one real model position, including prompt positions. Nonzero stages MUST consume input.
using StageRingCompute = std::function<bool(const core::BoundaryFrame*, int32_t, int64_t,
                                           core::BoundaryFrame&, std::string&)>;
// `stats` may be null. When it is not, the run prints one RDONE line at exit, including on failure, so a caller can
// tell a slow stage from a stage that never finished.
inline bool stage_ring_run(const StageRingConfig& c, const StageRingCompute& compute, StageRingStats* stats,
                           std::string& err) {
    using namespace core;
    const bool head = c.lo == 0, tail = c.hi == c.layers;
    if (c.listen.empty() || c.downstream.empty() || (head && c.orch.empty())) {
        err = "ring: require --listen, --downstream, and --orch on the head"; return false;
    }
    BoundarySocket listener, upstream, downstream, orch;
    int port = 0, bound = 0; std::string host;
    if (!boundary_parse_address(":" + c.listen, host, port, err) ||
        !listener.listen_on(port, bound, err)) return false;
    std::fprintf(stderr, "ring: bound ingress %d, layers [%lld,%lld)\n", bound, (long long)c.lo, (long long)c.hi);
    if (!boundary_parse_address(c.downstream, host, port, err) ||
        !downstream.connect_to(host, port, 120000, err)) return false;
    if (head && (!boundary_parse_address(c.orch, host, port, err) ||
                 !orch.connect_to(host, port, 120000, err))) return false;
    if (!listener.accept_one(upstream, err)) return false;
    if (!upstream.set_timeouts(120000, 120000, err) || !downstream.set_timeouts(120000, 120000, err) ||
        (head && !orch.set_timeouts(120000, 120000, err))) return false;
    auto send = [&](BoundarySocket& s, const BoundaryFrame& f) {
        std::vector<uint8_t> bytes;
        if (!boundary_serialize(f, bytes, err)) return false;
        if (f.n_tensors > 0) if (const char* trace = std::getenv("STRATA_RING_TRACE_DIRECTORY")) {
            auto path = std::filesystem::path(trace) / ("boundary-" + std::to_string(f.stage_begin) + "-" +
                std::to_string(f.stage_end) + "-p" + std::to_string(f.pos0) + ".bin");
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            if (!out) { err = "ring: boundary trace write failed"; return false; }
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (!s.send_frame(bytes, err)) return false;
        if (stats) {
            stats->net_send_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            stats->net_out_bytes += (int64_t) bytes.size();
        }
        return true;
    };
    auto receive = [&](BoundarySocket& s, BoundaryFrame& f) {
        std::vector<uint8_t> bytes;
        const auto t0 = std::chrono::steady_clock::now();
        if (!s.recv_frame(bytes, err, size_t(256) << 20)) return false;
        if (stats) {
            stats->net_recv_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            stats->net_in_bytes += (int64_t) bytes.size();
        }
        if (!boundary_deserialize(bytes.data(), bytes.size(), f, err)) return false;
        if (f.model_fingerprint != c.fingerprint) { err = "ring: model identity mismatch"; return false; }
        return true;
    };
    // Printed from the destructor so every exit - including every early error - leaves one machine-readable line.
    struct StatsPrinter {
        const StageRingStats* s; int64_t lo, hi; bool completed = false;
        ~StatsPrinter() {
            if (!s) return;
            std::fprintf(stderr,
                         "RDONE lo=%lld hi=%lld status=%s positions=%lld prompt_positions=%lld compute_ms=%.1f "
                         "net_send_ms=%.1f net_recv_ms=%.1f net_out_bytes=%lld net_in_bytes=%lld "
                         "experts_streamed=%lld file_read_bytes=%lld\n",
                         (long long) lo, (long long) hi, completed ? "ok" : "aborted",
                         (long long) s->positions, (long long) s->prompt_positions, s->compute_ms,
                         s->net_send_ms, s->net_recv_ms, (long long) s->net_out_bytes,
                         (long long) s->net_in_bytes, (long long) s->experts_streamed,
                         (long long) s->file_read_bytes);
            std::fflush(stderr);
        }
    } printer{stats, c.lo, c.hi};
    auto control = [&](uint16_t kind) {
        BoundaryFrame f; f.kind = kind; f.rows = 1; f.model_fingerprint = c.fingerprint;
        f.stage_begin = c.lo; f.stage_end = c.hi; return f;
    };
    // READY makes a full trip only after every process initialized its compute callback and its links.
    if (head) { auto ready = control(kBoundaryReady); if (!send(downstream, ready)) return false; }
    BoundaryFrame ready;
    if (!receive(upstream, ready) || ready.kind != kBoundaryReady) { err = "ring: expected READY"; return false; }
    if (!head && !send(downstream, ready)) return false;
    if (head && !send(orch, ready)) return false;
    std::fprintf(stderr, "ring: ready\n");
    BoundaryFrame request;
    if (head) {
        BoundarySocket client;
        if (!listener.accept_one(client, err)) return false;
        std::vector<uint8_t> bytes;
        if (!client.recv_frame(bytes, err, size_t(4) << 20) ||
            !boundary_deserialize(bytes.data(), bytes.size(), request, err)) return false;
        if (request.kind != kBoundaryRequest || request.rows < 1 || request.pos0 < 1 ||
            request.rows + request.pos0 > c.max_context) {
            err = "ring: invalid request or context capacity exceeded"; return false;
        }
        request.kind = kBoundaryStart; request.model_fingerprint = c.fingerprint;
        if (!send(downstream, request)) return false;
        BoundaryFrame echoed;
        if (!receive(upstream, echoed) || echoed.kind != kBoundaryStart) return false;
    } else {
        if (!receive(upstream, request) || request.kind != kBoundaryStart || !send(downstream, request)) return false;
    }
    const int64_t n_prompt = request.rows, max_new = request.pos0;
    if (n_prompt < 1 || max_new < 1 || n_prompt + max_new > c.max_context) {
        err = "ring: invalid START capacity"; return false;
    }
    int64_t pos = 0, emitted = 0;
    int32_t token = request.next_token[0];
    for (;;) {
        BoundaryFrame input, output;
        if (!head) {
            if (!receive(upstream, input)) return false;
            if (input.kind == kBoundaryEnd) {
                if (!send(downstream, input)) return false;
                printer.completed = true;
                return true;
            }
            if ((input.kind != kBoundaryRows && input.kind != kBoundaryPrompt) ||
                input.rows != 1 || input.pos0 != pos || input.stage_end != c.lo) {
                err = "ring: wrong message kind, position, row count, or adjacent layer range"; return false;
            }
            token = input.next_token[0];
        }
        const auto compute_t0 = std::chrono::steady_clock::now();
        if (!compute(head ? nullptr : &input, token, pos, output, err)) return false;
        if (stats) {
            stats->compute_ms +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compute_t0).count();
            ++stats->positions;
            if (pos < n_prompt - 1) ++stats->prompt_positions;
        }
        output.model_fingerprint = c.fingerprint; output.pos0 = pos; output.rows = 1;
        output.stage_begin = c.lo; output.stage_end = c.hi;
        output.kind = tail ? kBoundaryToken : (pos < n_prompt - 1 ? kBoundaryPrompt : kBoundaryRows);
        if (tail && pos >= n_prompt - 1) {
            bool finished = pos - (n_prompt - 1) + 1 >= max_new;
            for (auto eos : c.eos_ids) if (c.stop_eos && output.next_token[0] == eos) finished = true;
            if (finished) output.kind = kBoundaryEnd; // Tail owns sampling and reports its completion decision.
        }
        if (!tail) output.next_token[0] = token; // INPUT token, never an uninitialized prediction.
        if (!send(downstream, output)) return false;
        if (head) {
            BoundaryFrame result;
            if (!receive(upstream, result) || (result.kind != kBoundaryToken && result.kind != kBoundaryEnd) ||
                result.pos0 != pos || result.rows != 1) {
                err = "ring: expected token for current position"; return false;
            }
            if (pos >= n_prompt - 1) {
                ++emitted;
                const bool stop = result.kind == kBoundaryEnd;
                result.kind = kBoundaryToken; // Stream the final sampled token before the separate completion event.
                if (!send(orch, result)) return false;
                std::fprintf(stderr, "ring: token %d at position %lld\n", result.next_token[0], (long long)pos);
                token = result.next_token[0];
                if (stop) {
                    auto end = control(kBoundaryEnd); end.pos0 = pos + 1;
                    if (!send(downstream, end)) return false;
                    BoundaryFrame done;
                    if (!receive(upstream, done) || done.kind != kBoundaryEnd || !send(orch, end)) return false;
                    std::fprintf(stderr, "ring: complete, %lld tokens\n", (long long)emitted);
                    printer.completed = true;
                    return true;
                }
            } else token = request.next_token[(size_t)pos + 1];
        }
        ++pos;
    }
}
} // namespace strata::program
