// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// THE BOUNDARY FRAME: what one stage of a split hands to the next, as one named object instead of
// several ad-hoc buffers.
//
// A stage boundary used to be a scatter of implicit conventions: `--dump-final-r` wrote the residual
// only, `Prefill::hand_in_` wanted a packed `hc * n_embd` row, `Verifier::hand_in_` wanted
// `[R][bo][inj]` at `handoff_floats` stride.  Every path that produced or consumed a boundary had its
// own layout, and they disagreed the moment a prompt chunk and a decode window shared a file.  This
// header replaces that with one description of one thing.
//
// LAYERING:
//   * the engine fills and reads a frame through `boundary_capture` / `boundary_apply` - device
//     pointers in, host bytes out (and back).  It never learns whether the bytes go to a file, a
//     pipe or a socket.
//   * Tectonic serializes and deserializes (below).  The wire format is self-describing, so a stage
//     cannot silently read another version's rows.
//
// The format is little-endian, packed, and checked: a reader that does not recognize the magic, the
// version, the model fingerprint or a tensor's shape refuses the frame instead of reinterpreting its
// bytes as floats.
//
// OWNERSHIP: `BoundaryTensor::host` points into `BoundaryFrame::storage`, and `BoundaryTensor::name`
// points into `BoundaryFrame::name_storage`.  A frame therefore must not be copied or moved by the
// compiler's rules - the copies would keep pointing at the source's storage.  `BoundaryFrame` handles
// that with explicit copy/move that call `rebind()` (which recomputes every host offset and name
// pointer against the new object's own buffers).  Never store a raw `BoundaryTensor*` across such a
// copy.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace strata::core {

inline constexpr uint32_t kBoundaryMagic = 0x31544354u;   // "TCT1"
/// 3: the header is a fixed 50 bytes (`<IHHHQqqqq`) and the token count IS `rows`.  A decode window's rows
/// are <= 16, a prompt chunk's are hundreds, a Request's are the whole prompt - and in every one of them
/// there is exactly one token per row.  Keeping the count implicit is what lets a reader with no tensor table
/// (an orchestrator that only sends prompts and prints tokens) parse a frame with a fixed-offset struct.
inline constexpr uint16_t kBoundaryVersion = 3;

/// Rows a decode window can hold: the default length of `next_token`.
inline constexpr int kBoundaryMaxRows = 16;
/// Rows one frame can carry.  A PROMPT CHUNK OR A WHOLE PROMPT: the prompt path reports a chunk through
/// `on_chunk` (256 rows by default), and a Request frame reports the prompt itself, so this has to hold a
/// long conversation.  The payload bound below, not this number, is what keeps a frame finite.
inline constexpr int64_t kBoundaryMaxFrameRows = 1 << 20;
/// Floats one frame can carry (1 GiB of boundary rows; a token-only frame's tokens are counted separately).
inline constexpr int64_t kBoundaryMaxPayloadFloats = 1 << 28;
inline constexpr int kBoundaryMaxTensors = 4;
/// Tokens one frame can carry.  Equal to the row bound: one token per row, and a Request's prompt is its rows.
inline constexpr int64_t kBoundaryMaxTokens = kBoundaryMaxFrameRows;

/// WHAT A MESSAGE IS, in a ring topology where one stage's socket carries more than one transaction.
///
/// A split is a ring, not a pipeline: the user endpoint sits on the head stage, so the tail's downstream is
/// the head again.  That means the head's socket sees two kinds of traffic - a prompt from the user side, and
/// a sampled token from the tail - and it must tell them apart without a control plane.
///
/// 0..3 keep their wire numbers from version 2.  4..6 are the handshake a driver needs before it can send a
/// request: READY travels the ring once every stage is up, REQUEST carries the prompt (in `next_token`, with
/// `rows` = its length and `pos0` = max_new), and START echoes the same frame so every stage knows the prompt
/// on the same terms.
enum BoundaryKind : uint16_t {
    kBoundaryPrompt = 0,    ///< a prompt chunk's rows, produced by the first stage for the next one
    kBoundaryRows = 1,      ///< a decode window's rows, produced by a stage for its downstream
    kBoundaryToken = 2,     ///< the sampled token(s) a stage sends back: its `next_token` field is meaningful
    kBoundaryEnd = 3,       ///< the same, with "generation finished" set (an EOS was sampled)
    kBoundaryReady = 4,     ///< every stage is up; travels the ring once
    kBoundaryRequest = 5,   ///< the prompt itself: `next_token` holds it, `rows` is its length, `pos0` is max_new
    kBoundaryStart = 6,     ///< the same Request, echoed around the ring so each stage starts on the same prompt
};

/// One tensor of a boundary: a name its consumer matches on, a shape, and the floats.
///
/// `host` points into the owning frame's `storage`; `name` into the frame's `name_storage`.  Both are
/// re-pointed by `BoundaryFrame::rebind`.
struct BoundaryTensor {
    const char* name = nullptr;   ///< "R" (hc streams), "bo" (the second half's input), "inj" (the FFN half's injection)
    int64_t rows = 0;             ///< T
    int64_t elems_per_row = 0;
    const float* host = nullptr;  ///< rows * elems_per_row floats, row-major

    int64_t floats() const { return rows * elems_per_row; }
};

/// A whole hand-off: identity, position, the tensors, and the tokens.
struct BoundaryFrame {
    uint32_t magic = kBoundaryMagic;
    uint16_t version = kBoundaryVersion;
    uint16_t kind = kBoundaryPrompt;   ///< one of BoundaryKind: what this message is for
    uint64_t model_fingerprint = 0;   ///< checkpoint + quantization + geometry; a mismatch is refused, not corrected
    int64_t pos0 = 0;                 ///< the absolute position of row 0 (a Request: max_new)
    int64_t rows = 0;                 ///< T: a decode window, a prompt chunk, or a Request's prompt length
    int64_t stage_begin = 0;          ///< the producing stage's [begin, end)
    int64_t stage_end = 0;
    /// The tokens this message carries: ONE PER ROW, so `next_token.size()` grows with `rows`.  It starts at
    /// `kBoundaryMaxRows` (16) so that `frame.next_token[r]` keeps working for a decode window without sizing
    /// anything; `boundary_frame_alloc` and the deserializer resize it to `rows`, which is what makes a
    /// Request's whole prompt (or a prompt chunk's rows) fit.
    std::vector<int32_t> next_token = std::vector<int32_t>(kBoundaryMaxRows, -1);
    int n_tensors = 0;
    BoundaryTensor tensors[kBoundaryMaxTensors];
    /// The frame's own copy of every tensor's floats, filled by `boundary_frame_alloc` and by the deserializer.
    std::vector<float> storage;
    /// The frame's own copy of every tensor name, so a deserialized frame does not point into the bytes
    /// it was parsed from (those are the caller's buffer and may go away).
    char name_storage[kBoundaryMaxTensors][16] = {};

    BoundaryFrame() = default;
    BoundaryFrame(const BoundaryFrame& other) { *this = other; }
    BoundaryFrame& operator=(const BoundaryFrame& other);
    BoundaryFrame(BoundaryFrame&& other) noexcept { *this = std::move(other); }
    BoundaryFrame& operator=(BoundaryFrame&& other) noexcept;

    /// Re-point every tensor's `host` into this frame's `storage` (by its recorded offset) and every name into
    /// this frame's `name_storage`.  Must run after any copy or move; the operators above do it for you.
    void rebind();
    /// Bind one tensor to an offset in owned storage; records the offset for later copies/moves.
    bool bind_tensor_offset(int index, int64_t offset);

    const BoundaryTensor* find(const char* want) const {
        for (int i = 0; i < n_tensors; ++i)
            if (tensors[i].name != nullptr && std::strcmp(tensors[i].name, want) == 0) return &tensors[i];
        return nullptr;
    }
    /// `name` is copied into `name_storage`, so a caller's buffer need not outlive this call.
    void add(const char* name, int64_t rows_, int64_t per_row, const float* host);

private:
    /// The byte offset of a tensor's host inside `storage`, kept across copies (host - storage.data()).
    int64_t tensor_offsets_[kBoundaryMaxTensors] = {};
    /// Whether a tensor's name lives in `name_storage` (always true after `add`).
    bool name_owned_[kBoundaryMaxTensors] = {};
};

/// Allocate `storage` for the given shapes and point the tensors into it.  `names` and `per_row` are
/// parallel.  Returns false on a bad shape (zero rows, past `kBoundaryMaxFrameRows`, past the payload bound).
bool boundary_frame_alloc(BoundaryFrame& f, int64_t rows, const std::vector<const char*>& names,
                          const std::vector<int64_t>& per_row);

/// The serialized size of a frame (header + tensor table + tokens + payload).
size_t boundary_serialize_size(const BoundaryFrame& f);

/// The tokens a frame puts on the wire: exactly `rows` (one per row), padded with -1 if `next_token` is short.
int64_t boundary_wire_token_count(const BoundaryFrame& f);

/// Serialize into `out` (resized).  Refuses a frame whose tensors point outside its storage, or that has
/// tensors but no storage at all (a moved-from frame).
bool boundary_serialize(const BoundaryFrame& f, std::vector<uint8_t>& out, std::string& err);

/// Deserialize and validate.  On success `f.storage` holds the payload and the tensors point into it.
/// `n_tensors == 0` is legal and is what a token or Request frame is: it carries only tokens.
bool boundary_deserialize(const uint8_t* bytes, size_t n, BoundaryFrame& f, std::string& err,
                          size_t* consumed = nullptr);

/// A stable 64-bit identity of the model a stage runs, for the frame's `model_fingerprint`.  Built from
/// the geometry; two stages of one plan must produce the same value.
uint64_t boundary_model_fingerprint(int64_t n_layers, int64_t n_embd, int64_t hc, int64_t n_expert,
                                    int64_t experts_per_token, const char* pack_identity);

}  // namespace strata::core
