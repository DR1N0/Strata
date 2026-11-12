// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// boundary_wire.cpp - the frame's serialization.  See boundary_wire.hpp for why it exists.
//
// The wire layout (little-endian, packed, no padding), version 4:
//
//   u32 magic | u16 version | u16 kind | u16 n_tensors | u64 model_fingerprint
//   i64 pos0  | i64 rows    | i64 stage_begin | i64 stage_end
//   n_tensors x { u16 name_len | name bytes | i64 rows | i64 elems_per_row }
//   rows x i32 next_token
//   payload: for each tensor in order, rows * elems_per_row f32
//
// THE TOKEN COUNT IS `rows`: one token per row, always.  A decode window has <= 16 rows, a prompt chunk has
// hundreds, a Request's rows ARE the prompt.  The count is implicit so that a reader with no tensor table (an
// orchestrator that only sends prompts and prints tokens) can parse the header with one fixed struct
// (`<IHHHQqqqq`, 50 bytes) and then read `rows` int32s.
//
// Everything a reader needs to reject a frame it cannot interpret is in front of the payload, and
// every length is checked against the bytes actually present before it is used.
#include "strata/core/boundary_wire.hpp"

#include <algorithm>
#include <cstdio>

namespace strata::core {
namespace {

void put_u16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back((uint8_t) (v & 0xff));
    o.push_back((uint8_t) (v >> 8));
}
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t) ((v >> (8 * i)) & 0xff));
}
void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back((uint8_t) ((v >> (8 * i)) & 0xff));
}
void put_f32(std::vector<uint8_t>& o, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    put_u32(o, u);
}

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    /// `k > n - at` rather than `at + k > n`: the latter overflows for a hostile length.
    bool need(size_t k) {
        if (!ok || k > n - at) {
            ok = false;
            return false;
        }
        return true;
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = (uint16_t) (p[at] | ((uint16_t) p[at + 1] << 8));
        at += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (uint32_t) p[at + (size_t) i] << (8 * i);
        at += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (uint64_t) p[at + (size_t) i] << (8 * i);
        at += 8;
        return v;
    }
    int64_t i64() { return (int64_t) u64(); }
};

}  // namespace

// ---- ownership ---------------------------------------------------------------------------------------
//
// `BoundaryTensor::host` points into `storage` and `name` points at a copy in `name_storage`.  The
// compiler's copy/move would duplicate the vectors but leave those pointers aimed at the source, so both
// go through `rebind()`, which recomputes them from the recorded offsets.

void BoundaryFrame::add(const char* name, int64_t rows_, int64_t per_row, const float* host) {
    if (n_tensors >= kBoundaryMaxTensors || name == nullptr) return;
    const int i = n_tensors;
    tensors[i].rows = rows_;
    tensors[i].elems_per_row = per_row;
    tensors[i].host = host;
    tensor_offsets_[i] = (host != nullptr && !storage.empty()) ? (int64_t) (host - storage.data()) : 0;
    const size_t len = std::strlen(name);
    const size_t keep = std::min<size_t>(len, sizeof name_storage[i] - 1);
    std::memcpy(name_storage[i], name, keep);
    name_storage[i][keep] = '\0';
    name_owned_[i] = true;
    tensors[i].name = name_storage[i];
    ++n_tensors;
}

bool BoundaryFrame::bind_tensor_offset(int index, int64_t offset) {
    if (index < 0 || index >= n_tensors || offset < 0 || (uint64_t)offset > storage.size()) return false;
    const auto& t = tensors[index];
    if (t.rows <= 0 || t.elems_per_row <= 0 ||
        (uint64_t)t.rows > (storage.size() - (size_t)offset) / (uint64_t)t.elems_per_row) return false;
    tensor_offsets_[index] = offset;
    tensors[index].host = storage.data() + offset;
    return true;
}

void BoundaryFrame::rebind() {
    for (int i = 0; i < n_tensors; ++i) {
        const int64_t off = tensor_offsets_[i];
        if (off < 0 || (size_t) off > storage.size() || storage.empty()) {
            tensors[i].host = nullptr;   // no storage (or a corrupt offset): point nowhere rather than somewhere wrong
            continue;
        }
        tensors[i].host = storage.data() + off;
        if (name_owned_[i]) tensors[i].name = name_storage[i];
    }
}

BoundaryFrame& BoundaryFrame::operator=(const BoundaryFrame& o) {
    if (this == &o) return *this;
    magic = o.magic;
    version = o.version;
    kind = o.kind;
    model_fingerprint = o.model_fingerprint;
    pos0 = o.pos0;
    rows = o.rows;
    stage_begin = o.stage_begin;
    stage_end = o.stage_end;
    next_token = o.next_token;
    n_tensors = o.n_tensors;
    storage = o.storage;
    std::memcpy(name_storage, o.name_storage, sizeof name_storage);
    std::memcpy(tensor_offsets_, o.tensor_offsets_, sizeof tensor_offsets_);
    std::memcpy(name_owned_, o.name_owned_, sizeof name_owned_);
    std::memcpy(tensors, o.tensors, sizeof tensors);
    rebind();   // the pointers above are the source's; aim them at this frame's buffers
    return *this;
}

BoundaryFrame& BoundaryFrame::operator=(BoundaryFrame&& o) noexcept {
    if (this == &o) return *this;
    magic = o.magic;
    version = o.version;
    kind = o.kind;
    model_fingerprint = o.model_fingerprint;
    pos0 = o.pos0;
    rows = o.rows;
    stage_begin = o.stage_begin;
    stage_end = o.stage_end;
    next_token = std::move(o.next_token);
    n_tensors = o.n_tensors;
    storage = std::move(o.storage);
    std::memcpy(name_storage, o.name_storage, sizeof name_storage);
    std::memcpy(tensor_offsets_, o.tensor_offsets_, sizeof tensor_offsets_);
    std::memcpy(name_owned_, o.name_owned_, sizeof name_owned_);
    std::memcpy(tensors, o.tensors, sizeof tensors);
    rebind();
    // the moved-from frame keeps its bookkeeping but no payload: it must not look usable
    o.n_tensors = 0;
    o.storage.clear();
    o.rows = 0;
    o.next_token.assign(kBoundaryMaxRows, -1);
    return *this;
}

// ---- allocation --------------------------------------------------------------------------------------

bool boundary_frame_alloc(BoundaryFrame& f, int64_t rows, const std::vector<const char*>& names,
                          const std::vector<int64_t>& per_row) {
    if (rows <= 0 || rows > kBoundaryMaxFrameRows || names.size() != per_row.size() ||
        names.size() > (size_t) kBoundaryMaxTensors)
        return false;
    int64_t total = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (per_row[i] <= 0) return false;
        // rows <= 2^20 and per_row comes from the geometry (hc*n_embd is 10,240 for Qwen3.8), so the product
        // cannot overflow int64; the payload bound below is what refuses an over-large frame.
        total += rows * per_row[i];
        if (total > kBoundaryMaxPayloadFloats) return false;
    }
    f.storage.assign((size_t) total, 0.0f);
    f.rows = rows;
    // ONE TOKEN PER ROW: the prompt path fills `next_token[r]` for every row it reports, so the vector has to
    // be as long as the frame.  It never shrinks below the default window below either.
    if ((int64_t) f.next_token.size() != rows) f.next_token.assign((size_t) rows, -1);
    f.n_tensors = 0;
    int64_t off = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        f.add(names[i], rows, per_row[i], total > 0 ? f.storage.data() + off : nullptr);
        off += rows * per_row[i];
    }
    return true;
}

// ---- the wire ----------------------------------------------------------------------------------------

int64_t boundary_wire_token_count(const BoundaryFrame& f) {
    // exactly the rows: a short vector is padded with -1 on the wire, never truncated
    return std::min<int64_t>(f.rows, kBoundaryMaxTokens);
}

size_t boundary_serialize_size(const BoundaryFrame& f) {
    size_t n = 4 + 2 + 2 + 2 + 8 + 8 + 8 + 8 + 8;   // magic, version, kind, n_tensors, five i64/u64
    for (int i = 0; i < f.n_tensors; ++i) n += 2 + std::strlen(f.tensors[i].name) + 8 + 8;
    n += (size_t) boundary_wire_token_count(f) * 4;
    for (int i = 0; i < f.n_tensors; ++i) n += (size_t) f.tensors[i].floats() * 4;
    return n;
}

bool boundary_serialize(const BoundaryFrame& f, std::vector<uint8_t>& out, std::string& err) {
    if (f.magic != kBoundaryMagic) {
        err = "boundary: not a frame";
        return false;
    }
    if (f.n_tensors > 0 && f.storage.empty()) {
        // a moved-from or half-built frame: its tensors' hosts point nowhere.  Checking this before the
        // in-bounds test matters, because that test is skipped when storage is empty.
        err = "boundary: tensors without storage";
        return false;
    }
    const int64_t have = (int64_t) f.storage.size();
    for (int i = 0; i < f.n_tensors; ++i) {
        const BoundaryTensor& t = f.tensors[i];
        if (t.name == nullptr || t.rows <= 0 || t.elems_per_row <= 0 || t.host == nullptr) {
            err = "boundary: a tensor is unset";
            return false;
        }
        const int64_t off = (int64_t) (t.host - f.storage.data());
        if (off < 0 || off + t.floats() > have) {
            err = std::string("boundary: tensor ") + t.name + " leaves the frame storage";
            return false;
        }
    }
    out.clear();
    out.reserve(boundary_serialize_size(f));
    put_u32(out, f.magic);
    put_u16(out, f.version);
    put_u16(out, f.kind);
    put_u16(out, (uint16_t) f.n_tensors);
    put_u64(out, f.model_fingerprint);
    put_u64(out, (uint64_t) f.pos0);
    put_u64(out, (uint64_t) f.rows);
    put_u64(out, (uint64_t) f.stage_begin);
    put_u64(out, (uint64_t) f.stage_end);
    for (int i = 0; i < f.n_tensors; ++i) {
        const BoundaryTensor& t = f.tensors[i];
        const size_t len = std::strlen(t.name);
        put_u16(out, (uint16_t) len);
        out.insert(out.end(), t.name, t.name + len);
        put_u64(out, (uint64_t) t.rows);
        put_u64(out, (uint64_t) t.elems_per_row);
    }
    // ONE i32 PER ROW: a caller that sized `next_token` short (a hand-built Request) gets -1 for the rest, so
    // the fixed-offset reader on the other side always finds `rows` tokens.
    for (int64_t r = 0; r < f.rows; ++r) {
        const int32_t tok = r < (int64_t) f.next_token.size() ? f.next_token[(size_t) r] : -1;
        put_u32(out, (uint32_t) tok);
    }
    for (int i = 0; i < f.n_tensors; ++i) {
        const BoundaryTensor& t = f.tensors[i];
        for (int64_t k = 0; k < t.floats(); ++k) put_f32(out, t.host[k]);
    }
    return true;
}

bool boundary_deserialize(const uint8_t* bytes, size_t n, BoundaryFrame& f, std::string& err,
                          size_t* consumed) {
    Reader r{bytes, n};
    f.magic = r.u32();
    f.version = r.u16();
    f.kind = r.u16();
    const uint16_t nt = r.u16();
    f.model_fingerprint = r.u64();
    f.pos0 = r.i64();
    f.rows = r.i64();
    f.stage_begin = r.i64();
    f.stage_end = r.i64();
    if (!r.ok) {
        err = "boundary: truncated header";
        return false;
    }
    if (f.magic != kBoundaryMagic) {
        err = "boundary: wrong magic";
        return false;
    }
    if (f.version != kBoundaryVersion) {
        err = "boundary: version " + std::to_string(f.version) + " is not " + std::to_string(kBoundaryVersion);
        return false;
    }
    if (f.kind > (uint16_t) kBoundaryStart) {
        err = "boundary: unknown kind " + std::to_string(f.kind);
        return false;
    }
    // `nt == 0` is a TOKEN or REQUEST frame: no tensors, only tokens.  It used to be refused here after being
    // written happily, which made a sampled token (and a prompt) impossible to receive.
    if (nt > (uint16_t) kBoundaryMaxTensors || f.rows <= 0 || f.rows > kBoundaryMaxFrameRows) {
        err = "boundary: implausible shape";
        return false;
    }
    f.n_tensors = 0;
    int64_t total = 0;
    for (uint16_t i = 0; i < nt; ++i) {
        const uint16_t len = r.u16();
        if (!r.ok || len == 0 || len > 15 || !r.need(len)) {
            err = "boundary: bad tensor name";
            return false;
        }
        char name_buf[16] = {};
        std::memcpy(name_buf, r.p + r.at, len);
        name_buf[len] = '\0';
        r.at += len;
        const int64_t rows = r.i64(), per = r.i64();
        if (!r.ok || rows != f.rows || per <= 0 || per > (int64_t) 1 << 20) {
            err = "boundary: bad tensor shape";
            return false;
        }
        total += rows * per;
        if (total > kBoundaryMaxPayloadFloats) {
            err = "boundary: payload too large";
            return false;
        }
        f.add(name_buf, rows, per, nullptr);
    }
    f.next_token.assign((size_t) f.rows, -1);
    for (int64_t i = 0; i < f.rows; ++i) {
        f.next_token[(size_t) i] = (int32_t) r.u32();
    }
    if (!r.ok) {
        err = "boundary: truncated tensor table or tokens";
        return false;
    }
    f.storage.assign((size_t) total, 0.0f);
    int64_t off = 0;
    for (int i = 0; i < f.n_tensors; ++i) {
        BoundaryTensor& t = f.tensors[i];
        if (!f.bind_tensor_offset(i, off)) {
            err = "boundary: invalid tensor payload offset";
            return false;
        }
        for (int64_t k = 0; k < t.floats(); ++k) {
            const uint32_t u = r.u32();
            std::memcpy(const_cast<float*>(&t.host[k]), &u, 4);
        }
        off += t.floats();
    }
    if (!r.ok) {
        err = "boundary: truncated payload";
        return false;
    }
    f.rebind();   // the offsets recorded while parsing now aim at this frame's storage
    // A STREAM MAY HOLD SEVERAL FRAMES: the prompt path writes one per chunk and the decode path one per window,
    // so the reader is told how much this frame took and the caller walks the rest.  A single-frame file still
    // has `consumed == n`.
    if (consumed != nullptr) *consumed = r.at;
    else if (r.at != n) {
        err = "boundary: trailing bytes";
        return false;
    }
    return true;
}

uint64_t boundary_model_fingerprint(int64_t n_layers, int64_t n_embd, int64_t hc, int64_t n_expert,
                                    int64_t experts_per_token, const char* pack_identity) {
    // FNV-1a over the shape and the pack's identity: a stage that runs another checkpoint, another
    // quantization or another geometry must fail the frame's check rather than compute on it.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t k) {
        const auto* b = (const uint8_t*) p;
        for (size_t i = 0; i < k; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(&n_layers, sizeof n_layers);
    mix(&n_embd, sizeof n_embd);
    mix(&hc, sizeof hc);
    mix(&n_expert, sizeof n_expert);
    mix(&experts_per_token, sizeof experts_per_token);
    if (pack_identity != nullptr) mix(pack_identity, std::strlen(pack_identity));
    return h;
}

}  // namespace strata::core
