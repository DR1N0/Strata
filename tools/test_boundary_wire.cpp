// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// test_boundary_wire.cpp - a CPU-only check of the boundary frame and its transport.
//
// Build (Windows, from the repository root):
//   g++ -std=c++20 -O1 -I include tools/test_boundary_wire.cpp \
//       src/core/boundary_wire.cpp src/core/boundary_socket.cpp -lws2_32 -o test_boundary
// Build (POSIX):
//   g++ -std=c++20 -O1 -I include tools/test_boundary_wire.cpp \
//       src/core/boundary_wire.cpp src/core/boundary_socket.cpp -o test_boundary
//
// It touches no engine, no GPU and no model: only the serialization and the socket pair.
#include "strata/core/boundary_socket.hpp"
#include "strata/core/boundary_wire.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace strata::core;

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const std::string& what, const std::string& detail = "") {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    }
}

void section(const char* name) { std::printf("[%s]\n", name); }

/// A frame with the three decode tensors, filled with a recognizable pattern.
BoundaryFrame make_rows_frame(int64_t rows, int64_t hc, int64_t n) {
    BoundaryFrame f;
    f.kind = kBoundaryRows;
    f.model_fingerprint = 0x1234567890abcdefull;
    f.pos0 = 7;
    f.stage_begin = 0;
    f.stage_end = 24;
    const bool ok = boundary_frame_alloc(f, rows, {"R", "bo", "inj"}, {hc * n, n, hc});
    check(ok, "frame_alloc with tensors");
    for (size_t i = 0; i < f.storage.size(); ++i) f.storage[i] = (float) (i % 97) + 0.5f;
    for (int64_t r = 0; r < rows && r < kBoundaryMaxRows; ++r) f.next_token[(size_t) r] = (int32_t) (100 + r);
    return f;
}

/// Round-trip a frame through the wire and compare everything a consumer depends on.
void round_trip(const BoundaryFrame& in, const char* what) {
    std::vector<uint8_t> bytes;
    std::string err;
    const bool ser = boundary_serialize(in, bytes, err);
    check(ser, std::string(what) + ": serialize", err);
    if (!ser) return;

    BoundaryFrame out;
    const bool de = boundary_deserialize(bytes.data(), bytes.size(), out, err);
    check(de, std::string(what) + ": deserialize", err);
    if (!de) return;

    check(out.magic == in.magic, std::string(what) + ": magic");
    check(out.version == in.version, std::string(what) + ": version");
    check(out.kind == in.kind, std::string(what) + ": kind");
    check(out.model_fingerprint == in.model_fingerprint, std::string(what) + ": fingerprint");
    check(out.pos0 == in.pos0, std::string(what) + ": pos0");
    check(out.rows == in.rows, std::string(what) + ": rows");
    check(out.stage_begin == in.stage_begin && out.stage_end == in.stage_end, std::string(what) + ": stage range");
    check(out.n_tensors == in.n_tensors, std::string(what) + ": n_tensors");
    check(out.next_token == in.next_token, std::string(what) + ": next_token");
    check(out.storage == in.storage, std::string(what) + ": storage");
    for (int i = 0; i < in.n_tensors; ++i) {
        const BoundaryTensor* a = in.find(in.tensors[i].name);
        const BoundaryTensor* b = out.find(in.tensors[i].name);
        check(a != nullptr && b != nullptr, std::string(what) + ": tensor present");
        if (a != nullptr && b != nullptr) {
            check(b->rows == a->rows && b->elems_per_row == a->elems_per_row,
                  std::string(what) + ": tensor shape " + a->name);
            check(b->host != nullptr && b->host >= out.storage.data() &&
                      b->host + b->floats() <= out.storage.data() + out.storage.size(),
                  std::string(what) + ": tensor host points into the frame's own storage " + a->name);
            check(b->host - out.storage.data() == a->host - in.storage.data(),
                  std::string(what) + ": exact named tensor offset " + a->name);
            check(std::memcmp(a->host, b->host, (size_t)a->floats() * sizeof(float)) == 0,
                  std::string(what) + ": named tensor payload " + a->name);
            BoundaryFrame copied = out;
            BoundaryFrame moved = std::move(copied);
            const auto* after_move = moved.find(a->name);
            check(after_move && std::memcmp(a->host, after_move->host, (size_t)a->floats() * sizeof(float)) == 0,
                  std::string(what) + ": decoded tensor survives copy and move " + a->name);
        }
    }
}

}  // namespace

int main() {
    section("a decode window: three tensors, 16 rows");
    // 16 rows is the window bound; rows > that is a prompt chunk and is covered below.
    for (int64_t rows : {1, 2, 16}) {
        BoundaryFrame f = make_rows_frame(rows, 4, 8);
        round_trip(f, ("rows=" + std::to_string(rows)).c_str());
    }

    section("a prompt chunk: more rows than a window");
    {
        // The bug this covers: a chunk of 256 rows could not be framed at all, so a prefill could not cross a
        // stage.  256 is what the prompt path reports by default.
        BoundaryFrame f = make_rows_frame(256, 4, 8);
        check(f.rows == 256, "chunk rows survive alloc");
        round_trip(f, "chunk 256");
    }

    section("a token frame: no tensors at all");
    {
        // The other bug: a token frame serialized but could not be deserialized ("implausible shape").
        BoundaryFrame f;
        f.kind = kBoundaryToken;
        f.rows = 1;
        f.pos0 = 42;
        f.next_token = {777};
        std::vector<uint8_t> bytes;
        std::string err;
        check(boundary_serialize(f, bytes, err), "token frame: serialize", err);
        BoundaryFrame out;
        check(boundary_deserialize(bytes.data(), bytes.size(), out, err), "token frame: deserialize", err);
        check(out.n_tensors == 0, "token frame: no tensors");
        check(out.next_token.size() == 1 && out.next_token[0] == 777, "token frame: the token survives");
        check(out.kind == kBoundaryToken, "token frame: kind");
    }

    section("a Request frame: a whole prompt in next_token");
    {
        // Request carries the prompt: rows = its length, pos0 = max_new, no tensors.  This is why `ntok` is
        // explicit on the wire - the count no longer derives from the window bound.
        BoundaryFrame f;
        f.kind = kBoundaryRequest;
        f.pos0 = 64;   // max_new
        f.next_token.resize(200);
        for (size_t i = 0; i < f.next_token.size(); ++i) f.next_token[i] = (int32_t) (1000 + i);
        f.rows = (int64_t) f.next_token.size();
        std::vector<uint8_t> bytes;
        std::string err;
        check(boundary_serialize(f, bytes, err), "Request: serialize", err);
        BoundaryFrame out;
        check(boundary_deserialize(bytes.data(), bytes.size(), out, err), "Request: deserialize", err);
        check(out.kind == kBoundaryRequest, "Request: kind");
        check(out.rows == 200, "Request: rows is the prompt length");
        check(out.next_token.size() == 200, "Request: all 200 prompt tokens survive");
        for (size_t i = 0; i < out.next_token.size(); ++i) {
            if (out.next_token[i] != (int32_t) (1000 + i)) {
                check(false, "Request: token " + std::to_string(i) + " differs");
                break;
            }
        }
        check(out.pos0 == 64, "Request: pos0 is max_new");
    }

    section("a decode window still has 16 token slots by default");
    {
        // Existing callers use `frame.next_token[r]` for r < 16 without sizing anything.
        BoundaryFrame f;
        check(f.next_token.size() == (size_t) kBoundaryMaxRows, "default next_token is 16");
        f.next_token[15] = 5;
        check(f.next_token[15] == 5, "index 15 is writable");
    }

    section("copy and move keep the tensors pointing at the new frame");
    {
        BoundaryFrame f = make_rows_frame(4, 4, 8);
        const BoundaryTensor* before = f.find("R");
        check(before != nullptr && before->host == f.storage.data(), "source R starts at storage.data()");

        BoundaryFrame copy = f;   // copy constructor
        const BoundaryTensor* c = copy.find("R");
        check(c != nullptr, "copy has R");
        check(c->host == copy.storage.data(), "copy R points at the COPY's storage (not the source's)");
        check(copy.storage == f.storage, "copy payload equal");

        BoundaryFrame moved = std::move(f);   // move constructor
        const BoundaryTensor* m = moved.find("R");
        check(m != nullptr, "moved has R");
        check(m->host == moved.storage.data(), "moved R points at the MOVED-TO storage");
        check(f.n_tensors == 0 && f.storage.empty(), "the moved-from frame is emptied");

        // and a container, which is what a multi-frame stream uses
        std::vector<BoundaryFrame> v;
        v.push_back(make_rows_frame(2, 4, 8));   // may reallocate
        v.push_back(make_rows_frame(2, 4, 8));
        v.push_back(make_rows_frame(2, 4, 8));
        for (size_t i = 0; i < v.size(); ++i) {
            const BoundaryTensor* t = v[i].find("R");
            check(t != nullptr && t->host == v[i].storage.data(),
                  "vector element " + std::to_string(i) + " keeps its own storage");
        }
    }

    section("a stream of several frames");
    {
        // `consumed` walks one frame at a time; the prompt path writes one per chunk and decode one per window.
        BoundaryFrame a = make_rows_frame(3, 4, 8);
        BoundaryFrame b;
        b.kind = kBoundaryToken;
        b.rows = 2;
        b.next_token = {11, 12};
        BoundaryFrame c2 = make_rows_frame(256, 4, 8);

        std::vector<uint8_t> stream;
        std::string err;
        for (const BoundaryFrame* f : {&a, &b, &c2}) {
            std::vector<uint8_t> one;
            check(boundary_serialize(*f, one, err), "stream: serialize one", err);
            stream.insert(stream.end(), one.begin(), one.end());
        }
        size_t at = 0;
        int seen = 0;
        while (at < stream.size()) {
            BoundaryFrame out;
            size_t used = 0;
            if (!boundary_deserialize(stream.data() + at, stream.size() - at, out, err, &used)) {
                check(false, "stream: deserialize", err);
                break;
            }
            if (used == 0) {
                check(false, "stream: no progress");
                break;
            }
            at += used;
            ++seen;
            if (seen == 2) check(out.kind == kBoundaryToken && out.next_token[1] == 12, "stream: middle frame");
            if (seen == 3) check(out.rows == 256 && out.n_tensors == 3, "stream: last frame");
        }
        check(seen == 3, "stream: three frames read");
        check(at == stream.size(), "stream: consumed exactly the stream");
    }

    section("refusals");
    {
        BoundaryFrame f = make_rows_frame(2, 4, 8);
        std::vector<uint8_t> bytes;
        std::string err;
        check(boundary_serialize(f, bytes, err), "refusal: baseline serialize");

        {   // wrong magic
            auto bad = bytes;
            bad[0] ^= 0xff;
            BoundaryFrame out;
            check(!boundary_deserialize(bad.data(), bad.size(), out, err), "refuses a wrong magic");
        }
        {   // wrong version
            auto bad = bytes;
            bad[4] = (uint8_t) (kBoundaryVersion + 1);
            BoundaryFrame out;
            check(!boundary_deserialize(bad.data(), bad.size(), out, err), "refuses an unknown version");
        }
        {   // truncated
            BoundaryFrame out;
            check(!boundary_deserialize(bytes.data(), bytes.size() / 2, out, err), "refuses a truncated frame");
        }
        {   // trailing bytes without `consumed`
            auto bad = bytes;
            bad.push_back(0x7f);
            BoundaryFrame out;
            check(!boundary_deserialize(bad.data(), bad.size(), out, err), "refuses trailing bytes");
        }
        {   // a frame with tensors but no storage is refused instead of reading dangling pointers
            BoundaryFrame broken;
            broken.n_tensors = 1;
            broken.tensors[0].name = "R";
            broken.tensors[0].rows = 1;
            broken.tensors[0].elems_per_row = 1;
            broken.tensors[0].host = nullptr;
            std::vector<uint8_t> out;
            check(!boundary_serialize(broken, out, err), "refuses tensors without storage");
        }
        {   // rows past the frame bound
            BoundaryFrame big;
            check(!boundary_frame_alloc(big, kBoundaryMaxFrameRows + 1, {"R"}, {8}),
                  "refuses rows past kBoundaryMaxFrameRows");
        }
    }

    section("the transport: two sockets carry a frame and a token back");
    {
        BoundarySocket server;
        int bound = 0;
        std::string err;
        if (!server.listen_on(0, bound, err)) {
            check(false, "listen", err);
        } else {
            check(bound > 0, "listen bound a port");

            // TWO connections on one port, which is what a head stage needs (driver + ring closure).
            BoundarySocket client_a, client_b;
            std::thread client([&] {
                std::string cerr;
                client_a.connect_to("127.0.0.1", bound, 5000, cerr);
                client_b.connect_to("127.0.0.1", bound, 5000, cerr);
            });
            std::vector<BoundarySocket> accepted;
            const bool got = server.accept_n(2, accepted, err);
            client.join();
            check(got, "accept_n(2)", err);
            check(accepted.size() == 2, "two connections accepted");
            check(client_a.valid() && client_b.valid(), "both clients connected");

            if (got && accepted.size() == 2) {
                // a frame down, a token back up
                BoundaryFrame down = make_rows_frame(3, 4, 8);
                std::vector<uint8_t> ob;
                check(boundary_serialize(down, ob, err), "transport: serialize rows", err);
                check(accepted[0].send_frame(ob, err), "transport: send rows", err);

                std::vector<uint8_t> ib;
                check(client_a.recv_frame(ib, err, (size_t) 64 << 20), "transport: recv rows", err);
                BoundaryFrame got_f;
                check(boundary_deserialize(ib.data(), ib.size(), got_f, err), "transport: deserialize rows", err);
                check(got_f.rows == 3 && got_f.n_tensors == 3, "transport: the frame arrived intact");
                check(got_f.storage == down.storage, "transport: the payload is byte-identical");

                BoundaryFrame tok;
                tok.kind = kBoundaryToken;
                tok.rows = 1;
                tok.next_token = {4242};
                std::vector<uint8_t> tb;
                check(boundary_serialize(tok, tb, err), "transport: serialize token", err);
                check(client_a.send_frame(tb, err), "transport: send token", err);

                std::vector<uint8_t> rb;
                check(accepted[0].recv_frame(rb, err, (size_t) 64 << 20), "transport: recv token", err);
                BoundaryFrame got_t;
                check(boundary_deserialize(rb.data(), rb.size(), got_t, err), "transport: deserialize token", err);
                check(got_t.kind == kBoundaryToken && got_t.next_token[0] == 4242, "transport: the token returned");
            }

            // a timeout is reported, not waited out forever
            if (client_b.valid()) {
                check(client_b.set_timeouts(150, 150, err), "set_timeouts", err);
                std::vector<uint8_t> never;
                const bool r = client_b.recv_frame(never, err, (size_t) 64 << 20);
                check(!r, "recv times out instead of blocking forever", err);
            }
        }
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
