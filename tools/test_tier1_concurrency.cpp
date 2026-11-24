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
// Section D: a REAL two-stage chain (two independent stage_worker_run instances, two real request_ids,
// each with its own resident slot at each stage), with an artificial "network transit" delay standing in
// for a real inter-machine hop. Proves ACTUAL temporal overlap - stage A computing request 2's position
// while request 1's frame is sitting in the (simulated) transit delay elsewhere - with timestamps, not
// just eventual correctness (which sections A-C already cover).
#include "stage_worker.hpp"
#include "strata/core/control_wire.hpp"
#include "strata/core/entry_bridge.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_map>
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
    // 2 resident slots: this section runs requests 100 and 200 concurrently, each needing its own slot
    // (ADR 0005 addendum) - leaving this at the default of 1 would make ensure_slot() refuse the second
    // request outright (no free slot), which is exactly what happened the first time this test ran.
    cfg.batch = 2;

    // Fake compute: deterministic, no tensors (behaves like an exit-owning stage so the generic per-position
    // path is exercised without needing prefill's chunk machinery). output token = input token + 1.
    std::atomic<int> reset_calls{0};
    StageWorkerCompute compute = [](int, const BoundaryFrame*, int32_t token, int64_t, BoundaryFrame& out,
                                    std::string&) {
        out.next_token.assign(1, token + 1);
        out.n_tensors = 0;
        return true;
    };
    StageWorkerReset reset = [&reset_calls](int, std::string&) { ++reset_calls; return true; };
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

    // Cancel request 100 via the signal connection; request 200 must be unaffected. No further frame for
    // 100 is ever sent - a real explicit cancel means the upstream has already stopped sending once it
    // cancelled on its own side (docs/FORK_LOG.md, 2026-11-21: a tail-side slot found wedged forever by
    // real cross-machine testing because the old per-position was_cancelled check only ever ran when a
    // frame for THAT SAME request_id arrived). The idle-wait's own periodic sweep (not a subsequent frame)
    // must be what closes this out; recv_frame() blocks until it does.
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
    BoundaryFrame cancelled = recv_frame();
    check(cancelled.kind == kBoundaryEnd && cancelled.request_id == 100,
          "A: request 100 cancelled with no further frame for it ever sent (kBoundaryEnd, tagged 100)");

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
    StageWorkerCompute compute = [](int, const BoundaryFrame*, int32_t token, int64_t, BoundaryFrame& out,
                                    std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        out.next_token.assign(1, token + 1);   // same deterministic transform as section A
        out.n_tensors = 0;
        return true;
    };
    StageWorkerReset reset = [](int, std::string&) { return true; };
    StageWorkerPrefill prefill = [&](int, const int32_t* tokens, int64_t n, int64_t, const float*, int64_t,
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
    // max_new is 5, not 2: the busy-rejection check below needs a real wall-clock window (several more
    // 20ms positions) between req-A's first delta and its done for gen2's bytes to reliably reach
    // `incoming` before req-A's slot frees up - with too few positions this is a genuine, found-not-assumed
    // race (see that check's comment).
    json gen1;
    gen1["type"] = "gen"; gen1["request_id"] = "req-A"; gen1["tokens"] = {1, 2, 3, 4}; gen1["max_new"] = 5;
    send_json(data_conn, gen1);

    json delta1 = recv_json();
    check(delta1.value("type", "") == "delta" && delta1.value("request_id", "") == "req-A" &&
         delta1.value("token", -1) == 5,
         "B: first delta is token 5 (last prompt token 4, + 1)");
    check(prefill_calls.size() == 1 && prefill_calls[0] == "1,2,3,", "B: prefill() got exactly the first n-1 tokens");

    // Sent now, while req-A is still mid-flight (one more position still to go before max_new=2), with
    // `cfg.batch` left at its default of 1 - so there is no free slot for req-B yet. Earlier (Tier 1) this
    // would have sat unread in the TCP buffer until req-A finished, because the loop only read the socket
    // once its self-feed chain (`pending`) was empty - a `gen` sent while busy was queued, never actually
    // rejected. The ADR 0005 addendum's opportunistic incoming-check (every iteration, before draining
    // `pending`) fixes exactly this: a `gen` over capacity now gets a LIVE `error{code:batch_full}` before
    // req-A's slot is freed - but EXACTLY where that error lands relative to req-A's own remaining delta is
    // a genuine race (whether the reader thread has drained gen2's bytes into `incoming` by the time the
    // main loop checks it, vs. req-A's already-queued `pending` continuation) - so this collects messages
    // until req-A's `done` instead of asserting one fixed order.
    json gen2;
    gen2["type"] = "gen"; gen2["request_id"] = "req-B"; gen2["tokens"] = {9}; gen2["max_new"] = 1;
    send_json(data_conn, gen2);

    const auto done_tokens_check = [&](const json& done, const char* expect_id, std::vector<int> expect) {
        check(done.value("type", "") == "done" && done.value("request_id", "") == expect_id,
             std::string("B: done for ") + expect_id);
        const auto got = done.value("tokens", std::vector<int>{});
        check(got == expect, std::string("B: ") + expect_id + " done.tokens matches");
    };

    int req_a_deltas = 0;
    bool saw_busy = false;
    json done1;
    for (;;) {
        json msg = recv_json();
        const std::string type = msg.value("type", "");
        if (type == "delta" && msg.value("request_id", "") == "req-A") { ++req_a_deltas; continue; }
        if (type == "error" && msg.value("code", "") == "batch_full") { saw_busy = true; continue; }
        if (type == "done") { done1 = msg; break; }
        check(false, "B: unexpected message while waiting for req-A's done: " + msg.dump());
        break;
    }
    check(req_a_deltas == 4, "B: saw req-A's remaining 4 deltas (tokens 6,7,8,9) before its done");
    check(saw_busy, "B: saw req-B's LIVE error{code:batch_full} before req-A's slot freed - not queued");
    done_tokens_check(done1, "req-A", {5, 6, 7, 8, 9});

    // Now that req-A is finished and its slot is free, a RETRY of the same request is admitted normally.
    json gen2_retry;
    gen2_retry["type"] = "gen"; gen2_retry["request_id"] = "req-B"; gen2_retry["tokens"] = {9};
    gen2_retry["max_new"] = 1;
    send_json(data_conn, gen2_retry);
    json delta_b = recv_json();
    check(delta_b.value("type", "") == "delta" && delta_b.value("request_id", "") == "req-B" &&
         delta_b.value("token", -1) == 10,
         "B: req-B retried after req-A freed the slot -> admitted, delta 10");
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
    check(bridge.admit("req-X", 10, /*max_in_flight=*/1, /*session_id=*/0, id1), "C: first admit succeeds (capacity 1)");
    check(bridge.in_flight() == 1, "C: in_flight() == 1 after one admit");
    uint64_t id2_attempt = 0;
    check(!bridge.admit("req-Y", 10, /*max_in_flight=*/1, /*session_id=*/0, id2_attempt),
         "C: second admit at capacity 1 is refused");
    check(bridge.in_flight() == 1, "C: in_flight() still 1 after the refused admit");
    bridge.release(id1);
    check(bridge.in_flight() == 0, "C: in_flight() == 0 after release");
    check(bridge.admit("req-Y", 10, /*max_in_flight=*/1, /*session_id=*/0, id2), "C: admit succeeds again after release");
    check(id2 != id1, "C: a released wire id is not reused for a different request");
    std::string resolved;
    check(bridge.resolve(id2, resolved) && resolved == "req-Y", "C: resolve() finds the newly admitted request");
    check(!bridge.resolve(id1, resolved), "C: resolve() correctly fails for a released id");

    // Capacity above 1 - section B leaves cfg.batch at its default of 1; a bridge is always given
    // whatever `batch` the caller configures (see stage_worker.hpp's admit() call site), so this proves
    // the limit is the CALLER's choice, not baked into EntryBridge.
    EntryBridge bridge2;
    uint64_t a = 0, b = 0, c3 = 0;
    check(bridge2.admit("p", 5, /*max_in_flight=*/2, /*session_id=*/0, a), "C: capacity 2, first admit ok");
    check(bridge2.admit("q", 5, /*max_in_flight=*/2, /*session_id=*/0, b), "C: capacity 2, second admit ok");
    check(!bridge2.admit("r", 5, /*max_in_flight=*/2, /*session_id=*/0, c3), "C: capacity 2, third admit refused");
}

// ---- Section D: a real two-stage chain, timing-based proof of actual overlap ------------------------
//
// Two real stage_worker_run instances (stage A, layers [0,1); stage B, layers [1,2)), each with 2
// resident slots. Two driver threads (one per request) each run their own request through BOTH stages,
// round-trip after round-trip, inserting an artificial "transit" sleep between stages to stand in for a
// real inter-machine hop. Because TWO driver threads write to and read from the SAME two connections
// concurrently, and stage_worker_run does not promise to reply in send order once more than one request_id
// is in flight (that is the whole point of wire v4), a connection needs its own demultiplexer on this
// (test/daemon) side too - the same problem stage_worker.hpp solves for a real daemon, solved here in
// miniature so the test itself does not introduce a race that looks like a production bug.

using Clock = std::chrono::steady_clock;

// Reads frames off one connection in its own thread and routes them (by request_id) to whichever caller
// is blocked in recv_for() waiting for that request's next frame. Without this, two driver threads
// reading the same connection directly could each accidentally consume the OTHER's reply.
class FrameDemux {
public:
    explicit FrameDemux(BoundarySocket& conn) : conn_(conn) {
        reader_ = std::thread([this] { run(); });
    }
    ~FrameDemux() { if (reader_.joinable()) reader_.join(); }

    bool send(const BoundaryFrame& f) {
        std::lock_guard<std::mutex> lock(send_mu_);
        std::vector<uint8_t> bytes;
        std::string err;
        if (!boundary_serialize(f, bytes, err)) return false;
        return conn_.send_frame(bytes, err);
    }

    BoundaryFrame recv_for(uint64_t request_id) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return !queues_[request_id].empty() || done_; });
        BoundaryFrame f = std::move(queues_[request_id].front());
        queues_[request_id].pop_front();
        return f;
    }

private:
    void run() {
        for (;;) {
            std::vector<uint8_t> bytes;
            std::string err;
            if (!conn_.recv_frame(bytes, err, 1 << 20)) {
                std::lock_guard<std::mutex> lock(mu_);
                done_ = true;
                cv_.notify_all();
                return;
            }
            BoundaryFrame f;
            if (!boundary_deserialize(bytes.data(), bytes.size(), f, err)) {
                std::lock_guard<std::mutex> lock(mu_);
                done_ = true;
                cv_.notify_all();
                return;
            }
            std::lock_guard<std::mutex> lock(mu_);
            queues_[f.request_id].push_back(std::move(f));
            cv_.notify_all();
        }
    }

    BoundarySocket& conn_;
    std::mutex mu_, send_mu_;
    std::condition_variable cv_;
    std::unordered_map<uint64_t, std::deque<BoundaryFrame>> queues_;
    bool done_ = false;
    std::thread reader_;
};

void section_d() {
    std::fprintf(stderr, "[section D: real two-stage chain, timing-based overlap proof]\n");

    BoundarySocket a_data_l, a_signal_l, b_data_l, b_signal_l;
    int a_data_p = 0, a_signal_p = 0, b_data_p = 0, b_signal_p = 0;
    std::string err;
    check(a_data_l.listen_on(0, a_data_p, err), "D: stage A data listener bound");
    check(a_signal_l.listen_on(0, a_signal_p, err), "D: stage A signal listener bound");
    check(b_data_l.listen_on(0, b_data_p, err), "D: stage B data listener bound");
    check(b_signal_l.listen_on(0, b_signal_p, err), "D: stage B signal listener bound");

    StageWorkerConfig cfg_a, cfg_b;
    cfg_a.worker_data_endpoint = "127.0.0.1:" + std::to_string(a_data_p);
    cfg_a.worker_signal_endpoint = "127.0.0.1:" + std::to_string(a_signal_p);
    cfg_a.instance_id = "stage-a"; cfg_a.lo = 0; cfg_a.hi = 1; cfg_a.batch = 2;
    cfg_b.worker_data_endpoint = "127.0.0.1:" + std::to_string(b_data_p);
    cfg_b.worker_signal_endpoint = "127.0.0.1:" + std::to_string(b_signal_p);
    cfg_b.instance_id = "stage-b"; cfg_b.lo = 1; cfg_b.hi = 2; cfg_b.batch = 2;

    struct Interval { std::string label; Clock::time_point start, end; };
    std::mutex log_mu;
    std::vector<Interval> compute_log;   // tagged by request label, not which stage - see make_compute

    // Identifies which logical request a position belongs to by the TOKEN VALUE's range, not request_id:
    // stage A's compute callback gets `input == nullptr` for lo == 0 (StageWorkerCompute's own comment),
    // so there is nothing to read a request_id off of inside compute() itself. Request 1's tokens stay in
    // [1000, 2000); request 2's in [2000, 3000) - both stages just do token + 1, so the range survives
    // every hop, which is enough to label a compute event without threading request_id through the
    // callback signature just for this test.
    auto label_for = [](int32_t token) { return token < 2000 ? std::string("R1") : std::string("R2"); };
    auto make_compute = [&](int compute_ms) {
        return [&log_mu, &compute_log, compute_ms, label_for](int, const BoundaryFrame*, int32_t token, int64_t,
                                                               BoundaryFrame& out, std::string&) {
            const auto t0 = Clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(compute_ms));
            const auto t1 = Clock::now();
            out.next_token.assign(1, token + 1);
            out.n_tensors = 0;
            std::lock_guard<std::mutex> lock(log_mu);
            compute_log.push_back({label_for(token), t0, t1});
            return true;
        };
    };
    StageWorkerCompute compute_a = make_compute(5);
    StageWorkerCompute compute_b = make_compute(5);
    StageWorkerReset reset = [](int, std::string&) { return true; };
    StageWorkerPrefill prefill = nullptr;

    std::string err_a, err_b;
    StageWorkerStats stats_a, stats_b;
    std::thread worker_a([&] { stage_worker_run(cfg_a, compute_a, reset, prefill, &stats_a, err_a); });
    std::thread worker_b([&] { stage_worker_run(cfg_b, compute_b, reset, prefill, &stats_b, err_b); });

    BoundarySocket a_data, a_signal, b_data, b_signal;
    check(a_data_l.accept_one(a_data, err), "D: stage A data accepted");
    check(daemon_hello(a_data, "stage-a", 0, 1), "D: stage A data hello");
    check(a_signal_l.accept_one(a_signal, err), "D: stage A signal accepted");
    check(daemon_hello(a_signal, "stage-a", 0, 1), "D: stage A signal hello");
    check(daemon_recv_ready(a_data), "D: stage A ready");
    check(b_data_l.accept_one(b_data, err), "D: stage B data accepted");
    check(daemon_hello(b_data, "stage-b", 1, 2), "D: stage B data hello");
    check(b_signal_l.accept_one(b_signal, err), "D: stage B signal accepted");
    check(daemon_hello(b_signal, "stage-b", 1, 2), "D: stage B signal hello");
    check(daemon_recv_ready(b_data), "D: stage B ready");

    FrameDemux a_demux(a_data), b_demux(b_data);

    struct TransitLog { std::string label; Clock::time_point start, end; };
    std::mutex transit_mu;
    std::vector<TransitLog> transit_log;

    const int kPositions = 4;
    const int kTransitMs = 40;   // >> compute_ms (5): the window the overlap proof below needs to be real

    // One full round trip per position: stage A, an artificial A->B transit delay, stage B, an artificial
    // B->A transit delay (closing the ring back to the head) - see boundary_wire.hpp's header comment on
    // why a single request's own path is a cycle back to its entry stage.
    auto drive = [&](const std::string& label, uint64_t request_id, int32_t start_token) {
        int32_t token = start_token;
        for (int i = 0; i < kPositions; ++i) {
            BoundaryFrame in_a;
            in_a.kind = kBoundaryRows;
            in_a.request_id = request_id;
            in_a.pos0 = (int64_t) i;
            in_a.rows = 1;
            in_a.next_token = {token};
            a_demux.send(in_a);
            BoundaryFrame out_a = a_demux.recv_for(request_id);

            const auto t0 = Clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(kTransitMs));
            const auto t1 = Clock::now();
            { std::lock_guard<std::mutex> lock(transit_mu); transit_log.push_back({label, t0, t1}); }

            BoundaryFrame in_b = out_a;
            in_b.request_id = request_id;
            b_demux.send(in_b);
            BoundaryFrame out_b = b_demux.recv_for(request_id);

            const auto t2 = Clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(kTransitMs));
            const auto t3 = Clock::now();
            { std::lock_guard<std::mutex> lock(transit_mu); transit_log.push_back({label, t2, t3}); }

            token = out_b.next_token.empty() ? -1 : out_b.next_token[0];
        }
    };

    std::thread d1([&] { drive("R1", 1, 1000); });
    std::thread d2([&] { drive("R2", 2, 2000); });
    d1.join();
    d2.join();

    // THE PROOF: a transit interval belonging to one request overlapping, in REAL wall-clock time, with a
    // compute interval belonging to the OTHER request - not "both eventually finished", but a stage
    // genuinely computing request Y's position during a window where request Z's frame was away in
    // (simulated) transit, not at that stage at all.
    auto overlaps = [](Clock::time_point a0, Clock::time_point a1, Clock::time_point b0, Clock::time_point b1) {
        return a0 < b1 && b0 < a1;
    };
    int overlap_count = 0;
    for (const auto& t : transit_log) {
        for (const auto& c : compute_log) {
            if (c.label == t.label) continue;   // must be the OTHER request
            if (overlaps(t.start, t.end, c.start, c.end)) ++overlap_count;
        }
    }
    std::fprintf(stderr,
                "  (section D: %d compute/transit overlaps found across %zu transit windows and %zu "
                "compute events)\n",
                overlap_count, transit_log.size(), compute_log.size());
    check(overlap_count > 0,
         "D: found a real wall-clock overlap - a stage computed one request's position WHILE the other "
         "request's frame was in (simulated) transit elsewhere, not merely interleaved after the fact");

    // Correctness, not just timing: both requests were admitted exactly once per stage despite running
    // concurrently through two real driver threads, not one thread sending frames back-to-back.
    check(stats_a.requests == 2 && stats_b.requests == 2, "D: both stages saw both requests exactly once");

    auto send_end = [&](uint64_t request_id) {
        BoundaryFrame end_f;
        end_f.kind = kBoundaryEnd;
        end_f.request_id = request_id;
        end_f.rows = 1;
        a_demux.send(end_f);
        b_demux.send(end_f);
    };
    send_end(1);
    send_end(2);

    auto send_shutdown = [&](BoundarySocket& signal_conn) {
        json sd; sd["type"] = "shutdown"; sd["deadline_ms"] = 2000;
        const std::string text = sd.dump();
        std::vector<uint8_t> bytes(text.begin(), text.end());
        std::string serr;
        signal_conn.send_frame(bytes, serr);
    };
    send_shutdown(a_signal);
    send_shutdown(b_signal);
    worker_a.join();
    worker_b.join();
    check(err_a.empty(), "D: stage A stage_worker_run returned cleanly: " + err_a);
    check(err_b.empty(), "D: stage B stage_worker_run returned cleanly: " + err_b);
}

// ---- Section E: GenFields.session_id round-trips over the real wire (Tectonic ADR 0007 §4) ----------
//
// Sections A-D predate session_id and never set it, so none of them would notice a wrong JSON key
// (e.g. a typo like "sesion_id") silently zeroing every session's routing key on the wire - this is the
// one new thing control_wire.cpp actually serializes for this change, as opposed to stage_worker.hpp's
// edits, which are one-line copies next to an already-proven request_id line each. Real sockets, not a
// struct-to-struct comparison, so a framing-level mistake (wrong byte order, wrong field width) would
// show here too, not just a JSON round-trip through the same process.
void section_e() {
    std::fprintf(stderr, "[section E: GenFields.session_id round-trips over the wire]\n");

    BoundarySocket listener;
    int port = 0;
    std::string err;
    check(listener.listen_on(0, port, err), "E: listener bound");

    BoundarySocket server_side;
    std::thread acceptor([&] { check(listener.accept_one(server_side, err), "E: server accepted"); });
    BoundarySocket client_side;
    check(client_side.connect_to("127.0.0.1", port, 5000, err), "E: client connected");
    acceptor.join();

    GenFields sent;
    sent.request_id = "sess-req-1";
    sent.tokens = {1, 2, 3};
    sent.max_new = 4;
    sent.session_id = 0xDEADBEEFCAFEULL;
    check(control_send_gen(client_side, sent, err), "E: control_send_gen: " + err);

    GenFields got;
    std::string type;
    check(control_recv_gen(server_side, got, type, err), "E: control_recv_gen: " + err);
    check(type == "gen", "E: type is gen");
    check(got.request_id == sent.request_id, "E: request_id round-trips");
    check(got.session_id == sent.session_id,
         "E: session_id round-trips (" + std::to_string(got.session_id) + " vs " +
             std::to_string(sent.session_id) + ")");

    // The legal-zero case (ADR 0007: "0 仍然合法，不是协议级必须非零") must not be confused with "absent".
    GenFields zero_sent;
    zero_sent.request_id = "sess-req-2";
    zero_sent.tokens = {1};
    zero_sent.max_new = 1;
    zero_sent.session_id = 0;
    check(control_send_gen(client_side, zero_sent, err), "E: control_send_gen (session_id 0): " + err);
    GenFields zero_got;
    check(control_recv_gen(server_side, zero_got, type, err), "E: control_recv_gen (session_id 0): " + err);
    check(zero_got.session_id == 0, "E: session_id 0 round-trips as 0, not some other default");
}

}  // namespace

int main() {
    section_a();
    section_b();
    section_c();
    section_d();
    section_e();
    std::fprintf(stderr, "\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
