// CPU-only three-stage topology and READY/request/stream ordering test. No model or GPU.
#include "stage_ring.hpp"
#include <array>
#include <thread>
#include <cstdio>
int main() {
    using namespace strata::core;
    std::array<std::string, 3> errors;
    std::array<bool, 3> success{};
    BoundarySocket listener;
    std::string err; int port = 0;
    if (!listener.listen_on(19054, port, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::array<std::thread, 3> workers;
    for (int i = 0; i < 3; ++i) workers[i] = std::thread([&, i] {
        strata::program::StageRingConfig c;
        c.listen = std::to_string(19051 + i);
        c.downstream = "127.0.0.1:" + std::to_string(19051 + (i + 1) % 3);
        if (i == 0) c.orch = "127.0.0.1:19054";
        c.lo = i; c.hi = i + 1; c.layers = 3; c.max_context = 128; c.fingerprint = 42;
        auto fake = [i](const BoundaryFrame* input, int32_t token, int64_t,
                        BoundaryFrame& output, std::string&) {
            if ((i == 0) != (input == nullptr)) return false;
            if (i < 2) {
                if (!boundary_frame_alloc(output, 1, {"R"}, {1})) return false;
                output.storage[0] = float(token);
            } else output.next_token[0] = token + 1;
            return true;
        };
        success[i] = strata::program::stage_ring_run(c, fake, errors[i]);
    });
    BoundarySocket result, request;
    bool ok = listener.accept_one(result, err) && result.set_timeouts(5000, 5000, err);
    auto recv = [&](BoundaryFrame& f) {
        std::vector<uint8_t> b;
        return result.recv_frame(b, err, 1 << 20) && boundary_deserialize(b.data(), b.size(), f, err);
    };
    auto send = [&](const BoundaryFrame& f) {
        std::vector<uint8_t> b;
        return boundary_serialize(f, b, err) && request.send_frame(b, err);
    };
    BoundaryFrame ready;
    ok = ok && recv(ready) && ready.kind == kBoundaryReady && ready.model_fingerprint == 42;
    // Submit only after full-ring READY. This connection shares the head's SINGLE listen port.
    ok = ok && request.connect_to("127.0.0.1", 19051, 5000, err);
    BoundaryFrame q; q.kind = kBoundaryRequest; q.model_fingerprint = 42;
    q.rows = 3; q.pos0 = 4; q.next_token = {1, 2, 3};
    ok = ok && send(q);
    for (int i = 0; i < 4 && ok; ++i) {
        BoundaryFrame out;
        ok = recv(out) && out.kind == kBoundaryToken && out.pos0 == 2 + i && out.next_token[0] == 4 + i;
    }
    BoundaryFrame end;
    ok = ok && recv(end) && end.kind == kBoundaryEnd;
    result.close(); request.close();
    for (auto& worker : workers) worker.join();
    for (int i = 0; i < 3; ++i) if (!success[i]) {
        ok = false; std::fprintf(stderr, "stage%d: %s\n", i, errors[i].c_str());
    }
    std::fprintf(stderr, "three-stage ring READY + prompt + streamed 4,5,6,7 + END: %s (%s)\n",
                 ok ? "PASS" : "FAIL", err.c_str());
    return ok ? 0 : 1;
}
