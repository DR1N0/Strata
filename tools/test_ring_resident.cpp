// CPU-only resident-ring test: TWO requests through one process must not share sequence state.
//
// This is the correctness guard for --max-requests. Every stage keeps a counter that only its reset callback
// clears, so a stage that silently kept the previous document would answer request 2 from where request 1
// stopped - the failure mode a resident worker introduces and a single-shot run cannot show. No model or GPU.
#include "stage_ring.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <cstdio>

int main() {
    using namespace strata::core;
    constexpr int64_t kRequests = 2;
    constexpr int64_t kPrompt = 3, kMaxNew = 4;
    // T=1 walks prompt positions too: positions 0 .. (kPrompt + kMaxNew - 2) = 0..5.
    constexpr int64_t kPositionsPerRequest = kPrompt + kMaxNew - 1;

    std::array<std::string, 3> errors;
    std::array<bool, 3> success{};
    std::array<std::atomic<int64_t>, 3> seq{};
    std::array<std::atomic<int64_t>, 3> resets{};

    BoundarySocket orch_listener;
    std::string err; int port = 0;
    if (!orch_listener.listen_on(19074, port, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

    std::array<std::thread, 3> workers;
    for (int i = 0; i < 3; ++i) workers[i] = std::thread([&, i] {
        strata::program::StageRingConfig c;
        c.listen = std::to_string(19071 + i);
        c.downstream = "127.0.0.1:" + std::to_string(19071 + (i + 1) % 3);
        if (i == 0) c.orch = "127.0.0.1:19074";
        c.lo = i; c.hi = i + 1; c.layers = 3; c.max_context = 128; c.fingerprint = 42;
        c.max_requests = kRequests;
        auto fake = [&, i](const BoundaryFrame* input, int32_t token, int64_t,
                           BoundaryFrame& output, std::string&) {
            if ((i == 0) != (input == nullptr)) return false;
            const int64_t step = ++seq[i];   // one per position; a stage that forgot request 1 would start at 7
            if (i < 2) {
                if (!boundary_frame_alloc(output, 1, {"R"}, {1})) return false;
                output.storage[0] = float(step);
            } else output.next_token[0] = (int32_t) step;
            return true;
        };
        auto reset = [&, i](std::string&) { seq[i] = 0; ++resets[i]; return true; };
        strata::program::StageRingStats stats;
        success[i] = strata::program::stage_ring_run(c, fake, reset, &stats, errors[i]);
        if (success[i] && (stats.positions != kRequests * kPositionsPerRequest || resets[i] != kRequests)) {
            success[i] = false;
            errors[i] = "stage " + std::to_string(i) + ": served " + std::to_string(stats.positions) + " positions in " +
                        std::to_string(resets[i].load()) + " resets; expected " +
                        std::to_string(kRequests * kPositionsPerRequest) + " in " + std::to_string(kRequests);
        }
    });

    BoundarySocket tokens;
    bool ok = orch_listener.accept_one(tokens, err) && tokens.set_timeouts(5000, 5000, err);
    auto recv_tokens = [&](BoundaryFrame& f) {
        std::vector<uint8_t> b;
        return tokens.recv_frame(b, err, 1 << 20) && boundary_deserialize(b.data(), b.size(), f, err);
    };
    BoundaryFrame ready;
    ok = ok && recv_tokens(ready) && ready.kind == kBoundaryReady && ready.model_fingerprint == 42;

    // Each request is its own connection to the head's single listen port; the token stream stays open across both.
    for (int64_t r = 0; r < kRequests && ok; ++r) {
        BoundarySocket request;
        ok = request.connect_to("127.0.0.1", 19071, 5000, err);
        BoundaryFrame q; q.kind = kBoundaryRequest; q.model_fingerprint = 42;
        q.rows = kPrompt; q.pos0 = kMaxNew; q.next_token = {1, 2, 3};
        std::vector<uint8_t> b;
        ok = ok && boundary_serialize(q, b, err) && request.send_frame(b, err);
        request.close();
        // The decode positions are the last kMaxNew of the walk: step 3,4,5,6 - and identically on request 2.
        for (int64_t i = 0; i < kMaxNew && ok; ++i) {
            BoundaryFrame out;
            ok = recv_tokens(out) && out.kind == kBoundaryToken && out.pos0 == (kPrompt - 1 + i) &&
                 out.next_token[0] == (int32_t)(kPrompt + i);
        }
        BoundaryFrame end;
        ok = ok && recv_tokens(end) && end.kind == kBoundaryEnd;
    }

    tokens.close(); orch_listener.close();
    for (auto& worker : workers) worker.join();
    for (int i = 0; i < 3; ++i) if (!success[i]) {
        ok = false; std::fprintf(stderr, "stage%d: %s\n", i, errors[i].c_str());
    }
    std::fprintf(stderr, "resident ring served %lld requests without carrying sequence state: %s (%s)\n",
                 (long long) kRequests, ok ? "PASS" : "FAIL", err.c_str());
    return ok ? 0 : 1;
}
