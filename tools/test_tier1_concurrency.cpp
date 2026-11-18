// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// test_tier1_concurrency.cpp - a real two-process-shaped, CPU-only runtime check of wire v4's per-request
// isolation (stage_worker.hpp) and the entry bridge (entry_bridge.hpp, control_wire.hpp's gen/delta/done).
// No GPU, no model: stage_worker_run runs in its own thread with fake compute/reset/prefill callbacks, and
// this file plays the daemon on real TCP sockets, exactly as it would across a real process boundary - the
// two are not in the same call stack, only the same address space, which is why this is a meaningful check
// of the socket-level protocol and not just a function call.
//
// Build (POSIX):
//   clang++ -std=c++20 -O1 -I include -I third_party/strata-llama/vendor -I src/program -pthread \
//       tools/test_tier1_concurrency.cpp src/core/boundary_wire.cpp src/core/boundary_socket.cpp \
//       src/core/control_wire.cpp src/core/entry_bridge.cpp -o test_tier1
//
// Section A: two request_ids interleaved on one connection to a non-bridged (interior-style) stage -
// proves wire v4's per-request map (stage_worker.hpp) does not let them corrupt each other, and that
// cancelling one does not affect the other.
// Section B: the entry bridge end-to-end over real sockets - a multi-token gen, its done, a second gen
// queued during the first and admitted after it frees up, a one-token gen, and its cancellation.
// Section C: EntryBridge::admit's capacity limit, directly - section B's integration never drives this
// branch (see its comment on why), so this is what actually exercises "a request over capacity is
// refused", with no socket in the way to obscure whether the check itself is correct.
#include "stage_worker.hpp"
#include "strata/core/control_wire.hpp"
#include "strata/core/entry_bridge.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

using namespace strata::core;
using namespace strata::program;
using json = nlohmann::json;

namespace {

int g_checks = 0, g_failed = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::fprintf(stderr, "  FAIL  %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok    %s\n", what.c_str());
    }
}

// Server-side hello handshake, played from the daemon's side. Reuses control_wire.hpp's generic
// (direction-agnostic) hello functions. Split from reading `ready` on purpose: the engine (stage_worker_run)
// completes BOTH connections' hello exchanges before it ever sends `ready` (on the data connection only,
// at the very end) - a daemon that tried to read `ready` right after the data hello, before also accepting
// and handshaking the signal connection, would deadlock against the engine waiting on exactly that.
bool daemon_hello(BoundarySocket& sock, const std::string& want_instance, int64_t lo, int64_t hi) {
    HelloFields hello;
    std::string type, err;
    if (!control_recv_hello(sock, hello, type, err)) {
        std::fprintf(stderr, "daemon: recv hello: %s\n", err.c_str());
        return false;
    }
    if (hello.instance_id != want_instance || hello.layer_begin != lo || hello.layer_end != hi) {
        std::fprintf(stderr, "daemon: hello mismatch\n");
        return false;
    }
    return control_send_hello_ok(sock, want_instance, err);
}

bool daemon_recv_ready(BoundarySocket& sock) {
    std::vector<uint8_t> bytes;
    std::string err;
    if (!sock.recv_frame(bytes, err, 1 << 20)) {
        std::fprintf(stderr, "daemon: recv ready: %s\n", err.c_str());
        return false;
    }
    json j = json::parse(bytes.begin(), bytes.end());
    return j.value("type", std::string()) == "ready";
}

// ---- Section A: wire-level concurrency on a non-bridged (interior-style) stage --------------------------

void section_a() {
    std::fprintf(stderr, "[section A: wire v4 per-request isolation, no entry bridge]\n");

    BoundarySocket data_listener, signal_listener;
    int data_port = 0, signal_port = 0;
    std::string err;
    check(data_listener.listen_on(0, data_port, err), "A: data listener bound");
    check(signal_listener.listen_on(0, signal_port, err), "A: signal listener bound");

    StageWorkerConfig cfg;
    cfg.worker_data_endpoint = "127.0.0.1:" + std::to_string(data_port);
    cfg.worker_signal_endpoint = "127.0.0.1:" + std::to_string(signal_port);
    cfg.instance_id = "test-a";
    cfg.lo = 0;
    cfg.hi = 1;   // irrelevant to the fake compute below, just has to be consistent

    // Fake compute: deterministic, no tensors (behaves like an exit-owning stage so the generic per-position
    // path is exercised without needing prefill's chunk machinery). output token = input token + 1.
    std::atomic<int> reset_calls{0};
    StageWorkerCompute compute = [](const BoundaryFrame*, int32_t token, int64_t, BoundaryFrame& out,
                                    std::string&) {
        out.next_token.assign(1, token + 1);
        out.n_tensors = 0;
        return true;
    };
    StageWorkerReset reset = [&reset_calls](std::string&) { ++reset_calls; return true; };
    StageWorkerPrefill prefill = nullptr;   // not exercised in section A; section B exercises it

    std::string worker_err;
    StageWorkerStats stats;
    std::thread worker([&] {
        stage_worker_run(cfg, compute, reset, prefill, &stats, worker_err);
    });

    BoundarySocket data_conn, signal_conn;
    check(data_listener.accept_one(data_conn, err), "A: data connection accepted");
    check(daemon_hello(data_conn, "test-a", 0, 1), "A: data hello");
    check(signal_listener.accept_one(signal_conn, err), "A: signal connection accepted");
    check(daemon_hello(signal_conn, "test-a", 0, 1), "A: signal hello");
    check(daemon_recv_ready(data_conn), "A: ready");

    auto send_frame = [&](BoundaryFrame f) {
        std::vector<uint8_t> bytes;
        std::string serr;
        if (!boundary_serialize(f, bytes, serr)) { check(false, "A: serialize: " + serr); return; }
        if (!data_conn.send_frame(bytes, serr)) check(false, "A: send: " + serr);
    };
    auto recv_frame = [&]() -> BoundaryFrame {
        std::vector<uint8_t> bytes;
        std::string rerr;
        BoundaryFrame f;
        if (!data_conn.recv_frame(bytes, rerr, 1 << 20)) { check(false, "A: recv: " + rerr); return f; }
        if (!boundary_deserialize(bytes.data(), bytes.size(), f, rerr)) check(false, "A: deserialize: " + rerr);
        return f;
    };

    // Two requests' first positions, interleaved: 100 then 200, replying to whichever arrives. A single
    // shared-state implementation (pre wire-v4) would have request 200's position silently take over
    // request 100's bookkeeping; request_id-keyed state must not.
    BoundaryFrame req100;
    req100.kind = kBoundaryRows; req100.request_id = 100; req100.pos0 = 0; req100.rows = 1;
    req100.next_token = {10};
    send_frame(req100);
    BoundaryFrame out100a = recv_frame();
    check(out100a.request_id == 100 && out100a.next_token[0] == 11, "A: request 100 position 0 -> 11, tagged 100");

    BoundaryFrame req200;
    req200.kind = kBoundaryRows; req200.request_id = 200; req200.pos0 = 0; req200.rows = 1;
    req200.next_token = {50};
    send_frame(req200);
    BoundaryFrame out200a = recv_frame();
    check(out200a.request_id == 200 && out200a.next_token[0] == 51, "A: request 200 position 0 -> 51, tagged 200");

    // Advance 100 again - its own position 1, unrelated to 200's state.
    BoundaryFrame req100b;
    req100b.kind = kBoundaryRows; req100b.request_id = 100; req100b.pos0 = 1; req100b.rows = 1;
    req100b.next_token = {11};
    send_frame(req100b);
    BoundaryFrame out100b = recv_frame();
    check(out100b.request_id == 100 && out100b.next_token[0] == 12, "A: request 100 position 1 -> 12, tagged 100");

    check(reset_calls.load() == 2, "A: reset() called exactly once per request (100 and 200), not per frame");

    // Cancel request 100 via the signal connection; request 200 must be unaffected.
    {
        json cancel;
        cancel["type"] = "cancel";
        cancel["request_id"] = "doesnt-matter-the-wire-id-is-what-matters";
        // The signal connection's cancel targets "whichever request_id the main loop is currently on" -
        // see stage_worker.hpp's comment on current_request_id. The last position processed was request
        // 100's (pos0=1), so a cancel landing now must hit 100, not 200 - this is exactly the real
        // constraint documented there, and this test exercises it rather than asserting around it.
        const std::string text = cancel.dump();
        std::vector<uint8_t> bytes(text.begin(), text.end());
        std::string serr;
        check(signal_conn.send_frame(bytes, serr), "A: cancel sent on signal connection");
    }
    // Give the signal thread a moment to land the flag before the next position arrives.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BoundaryFrame req100c;
    req100c.kind = kBoundaryRows; req100c.request_id = 100; req100c.pos0 = 2; req100c.rows = 1;
    req100c.next_token = {12};
    send_frame(req100c);
    BoundaryFrame cancelled = recv_frame();
    check(cancelled.kind == kBoundaryEnd && cancelled.request_id == 100, "A: request 100 cancelled (kBoundaryEnd, tagged 100)");

    // Request 200 keeps going, unaffected by 100's cancellation.
    BoundaryFrame req200b;
    req200b.kind = kBoundaryRows; req200b.request_id = 200; req200b.pos0 = 1; req200b.rows = 1;
    req200b.next_token = {51};
    send_frame(req200b);
    BoundaryFrame out200b = recv_frame();
    check(out200b.request_id == 200 && out200b.next_token[0] == 52,
         "A: request 200 position 1 -> 52 AFTER 100 was cancelled - unaffected");

    // Clean shutdown.
    {
        json sd; sd["type"] = "shutdown"; sd["deadline_ms"] = 2000;
        const std::string text = sd.dump();
        std::vector<uint8_t> bytes(text.begin(), text.end());
        std::string serr;
        signal_conn.send_frame(bytes, serr);
    }
    worker.join();
    check(worker_err.empty(), "A: stage_worker_run returned cleanly: " + worker_err);
}

// ---- Section B: the entry bridge end to end --------------------------------------------------------

void section_b() {
    std::fprintf(stderr, "[section B: entry bridge gen/delta/done/error]\n");

    BoundarySocket data_listener, signal_listener;
    int data_port = 0, signal_port = 0;
    std::string err;
    check(data_listener.listen_on(0, data_port, err), "B: data listener bound");
    check(signal_listener.listen_on(0, signal_port, err), "B: signal listener bound");

    EntryBridge bridge;
    StageWorkerConfig cfg;
    cfg.worker_data_endpoint = "127.0.0.1:" + std::to_string(data_port);
    cfg.worker_signal_endpoint = "127.0.0.1:" + std::to_string(signal_port);
    cfg.instance_id = "test-b";
    cfg.lo = 0;
    cfg.hi = 1;   // single-stage: owns the whole (fake) model
    cfg.entry_bridge = &bridge;

    std::vector<std::string> prefill_calls;   // records what prefill() was handed, for the split-point check
    // The entry bridge self-feeds a sampled token straight back as the next position's input (see
    // stage_worker.hpp) with no real network or compute delay in between, so a cancellation test needs
    // SOME per-position cost or the decode loop finishes before a signal sent from another thread has any
    // chance to land. A real engine has real compute time here; this fake needs to simulate having it.
    StageWorkerCompute compute = [](const BoundaryFrame*, int32_t token, int64_t, BoundaryFrame& out,
                                    std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        out.next_token.assign(1, token + 1);   // same deterministic transform as section A
        out.n_tensors = 0;
        return true;
    };
    StageWorkerReset reset = [](std::string&) { return true; };
    StageWorkerPrefill prefill = [&](const int32_t* tokens, int64_t n, int64_t, const float*, int64_t,
                                     const StageWorkerEmit&, std::string&) {
        std::string s;
        for (int64_t i = 0; i < n; ++i) s += std::to_string(tokens[i]) + ",";
        prefill_calls.push_back(s);
        return true;
    };

    std::string worker_err;
    StageWorkerStats stats;
    std::thread worker([&] { stage_worker_run(cfg, compute, reset, prefill, &stats, worker_err); });

    BoundarySocket data_conn, signal_conn;
    check(data_listener.accept_one(data_conn, err), "B: data connection accepted");
    check(daemon_hello(data_conn, "test-b", 0, 1), "B: data hello");
    check(signal_listener.accept_one(signal_conn, err), "B: signal connection accepted");
    check(daemon_hello(signal_conn, "test-b", 0, 1), "B: signal hello");
    check(daemon_recv_ready(data_conn), "B: ready");

    auto send_json = [&](BoundarySocket& sock, const json& j) {
        const std::string text = j.dump();
        std::vector<uint8_t> bytes(text.begin(), text.end());
        std::string serr;
        if (!sock.send_frame(bytes, serr)) check(false, "B: send json: " + serr);
    };
    auto recv_json = [&]() -> json {
        std::vector<uint8_t> bytes;
        std::string rerr;
        if (!data_conn.recv_frame(bytes, rerr, 1 << 20)) { check(false, "B: recv json: " + rerr); return json(); }
        return json::parse(bytes.begin(), bytes.end());
    };

    // A 4-token prompt (tokens 1,2,3,4) with max_new=2: prefill must get exactly the first 3 tokens
    // (n - 1), and the first delta must be token 4 + 1 = 5 (the last prompt position's prediction), per
    // the split documented in stage_worker.hpp's gen-handling.
    json gen1;
    gen1["type"] = "gen"; gen1["request_id"] = "req-A"; gen1["tokens"] = {1, 2, 3, 4}; gen1["max_new"] = 2;
    send_json(data_conn, gen1);

    json delta1 = recv_json();
    check(delta1.value("type", "") == "delta" && delta1.value("request_id", "") == "req-A" &&
         delta1.value("token", -1) == 5,
         "B: first delta is token 5 (last prompt token 4, + 1)");
    check(prefill_calls.size() == 1 && prefill_calls[0] == "1,2,3,", "B: prefill() got exactly the first n-1 tokens");

    // Sent now, while req-A is still mid-flight (one more position still to go before max_new=2).
    json gen2;
    gen2["type"] = "gen"; gen2["request_id"] = "req-B"; gen2["tokens"] = {9}; gen2["max_new"] = 1;
    send_json(data_conn, gen2);

    // DISCOVERED DURING THIS TEST, REPORTED RATHER THAN PAPERED OVER: a `gen` sent while another is still
    // in flight does NOT race a live `batch_full` rejection through the socket. The self-feeding decode
    // loop (stage_worker.hpp: a sampled token not at max_new is pushed onto `pending` and consumed before
    // the socket is read again) means the loop never polls the socket again until the in-flight request's
    // `pending` queue is fully drained - which only happens once that request reaches max_new or is
    // cancelled. A second `gen` sent now simply waits in the TCP receive buffer and is read (and freshly
    // admitted) only after req-A has already finished and released its slot - it is never seen while
    // EntryBridge::admit's capacity count is actually at its limit. So this is sequential queuing, not
    // "rejected while busy": `admit()`'s capacity check is real (see the standalone check of it below,
    // with no socket in the way) but this integration never drives it to the rejecting branch. A caller
    // that wants a live `batch_full` on a busy connection needs the loop to poll the socket between
    // positions too, not just drain `pending` - out of scope for this round, flagged here instead of
    // silently claimed as working.
    const auto done_tokens_check = [&](const json& done, const char* expect_id, std::vector<int> expect) {
        check(done.value("type", "") == "done" && done.value("request_id", "") == expect_id,
             std::string("B: done for ") + expect_id);
        const auto got = done.value("tokens", std::vector<int>{});
        check(got == expect, std::string("B: ") + expect_id + " done.tokens matches");
    };

    // The first request's second (and final, max_new=2) token, then done.
    json delta2 = recv_json();
    check(delta2.value("type", "") == "delta" && delta2.value("token", -1) == 6, "B: second delta is token 6");
    json done1 = recv_json();
    done_tokens_check(done1, "req-A", {5, 6});

    // Sent while req-A was still mid-flight (see the comment above), req-B's `gen` was sitting in the TCP
    // buffer the whole time and is only read now, AFTER req-A's slot was released - so it is freshly
    // admitted, not rejected. Draining it (and its one delta) before req-C is what the comment above means
    // by "sequential queuing, not rejection".
    json delta_b = recv_json();
    check(delta_b.value("type", "") == "delta" && delta_b.value("request_id", "") == "req-B" &&
         delta_b.value("token", -1) == 10,
         "B: req-B (queued during req-A, admitted after it freed up) -> delta 10");
    json done_b = recv_json();
    done_tokens_check(done_b, "req-B", {10});

    // Now that req-A and req-B are both finished, a new request is admitted normally.
    json gen3;
    // max_new is generous on purpose: at 20ms/position (see compute()'s comment), this gives the
    // cancellation below a wide, non-racy window to land well before the request would finish naturally.
    gen3["type"] = "gen"; gen3["request_id"] = "req-C"; gen3["tokens"] = {7}; gen3["max_new"] = 40;
    send_json(data_conn, gen3);
    json delta3 = recv_json();
    check(delta3.value("type", "") == "delta" && delta3.value("request_id", "") == "req-C" &&
         delta3.value("token", -1) == 8,
         "B: one-token prompt req-C -> first delta is token 8");

    // Cancel req-C mid-flight via the signal connection. The main loop only checks cancellation between
    // positions (see stage_worker.hpp's comment), so this races the self-feeding decode loop - compute()'s
    // 20ms/position cost and req-C's generous max_new (see gen3's comment) are what make this race safe to
    // call non-flaky rather than a true guarantee.
    json cancel;
    cancel["type"] = "cancel"; cancel["request_id"] = "req-C";
    send_json(signal_conn, cancel);
    // The cancel races whatever position is already in flight, so zero or more further `delta`s for req-C
    // may still arrive before it lands - that is expected, not a bug, and this drains them rather than
    // asserting the very next message is the cancellation.
    json msg;
    int drained = 0;
    for (;;) {
        msg = recv_json();
        if (msg.value("type", "") != "delta") break;
        ++drained;
        if (drained > 40) { check(false, "B: cancel never landed within max_new positions"); break; }
    }
    check(msg.value("type", "") == "error" && msg.value("code", "") == "cancelled" &&
         msg.value("request_id", "") == "req-C",
         "B: req-C cancelled -> error{code:cancelled, request_id:req-C} (after draining " +
             std::to_string(drained) + " in-flight delta(s))");

    json sd; sd["type"] = "shutdown"; sd["deadline_ms"] = 2000;
    send_json(signal_conn, sd);
    worker.join();
    check(worker_err.empty(), "B: stage_worker_run returned cleanly: " + worker_err);
}

// ---- Section C: EntryBridge::admit's capacity limit, in isolation -----------------------------------

void section_c() {
    std::fprintf(stderr, "[section C: EntryBridge admission capacity, no socket]\n");
    EntryBridge bridge;
    uint64_t id1 = 0, id2 = 0;
    check(bridge.admit("req-X", 10, /*max_in_flight=*/1, id1), "C: first admit succeeds (capacity 1)");
    check(bridge.in_flight() == 1, "C: in_flight() == 1 after one admit");
    uint64_t id2_attempt = 0;
    check(!bridge.admit("req-Y", 10, /*max_in_flight=*/1, id2_attempt),
         "C: second admit at capacity 1 is refused");
    check(bridge.in_flight() == 1, "C: in_flight() still 1 after the refused admit");
    bridge.release(id1);
    check(bridge.in_flight() == 0, "C: in_flight() == 0 after release");
    check(bridge.admit("req-Y", 10, /*max_in_flight=*/1, id2), "C: admit succeeds again after release");
    check(id2 != id1, "C: a released wire id is not reused for a different request");
    std::string resolved;
    check(bridge.resolve(id2, resolved) && resolved == "req-Y", "C: resolve() finds the newly admitted request");
    check(!bridge.resolve(id1, resolved), "C: resolve() correctly fails for a released id");

    // Capacity above 1 (not reachable through stage_worker.hpp today - see StageWorkerConfig::batch's
    // comment - but the class itself does not hard-code 1, so this proves the limit is the CALLER's
    // choice, not baked into EntryBridge).
    EntryBridge bridge2;
    uint64_t a = 0, b = 0, c3 = 0;
    check(bridge2.admit("p", 5, /*max_in_flight=*/2, a), "C: capacity 2, first admit ok");
    check(bridge2.admit("q", 5, /*max_in_flight=*/2, b), "C: capacity 2, second admit ok");
    check(!bridge2.admit("r", 5, /*max_in_flight=*/2, c3), "C: capacity 2, third admit refused");
}

}  // namespace

int main() {
    section_a();
    section_b();
    section_c();
    std::fprintf(stderr, "\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
