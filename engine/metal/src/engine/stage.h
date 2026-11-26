// CPU-testable contract for the Metal split-stage (layer-range cut) layer chain. No GPU or wire
// ownership.
#pragma once
#include <cstring>
#include <string>

namespace strata::metal_engine {
struct StagePlan {
    int begin = 0, end = 0, layers = 0;
    bool first() const { return begin == 0; }
    bool last() const { return end == layers; }
    bool split() const { return !first() || !last(); }
    // Split cuts are not required to align to QSA groups: docs/design/model-split-validation.md
    // (Tectonic repo) ran [0,1), [0,2) and [0,22) on real Metal hardware against an unsplit
    // baseline. [0,2) and [0,22) (misaligned, PLE 0/1 kept together) matched the baseline
    // byte-for-byte; [0,1) (PLE 0/1 split apart) differed on a short completion but, at max_new=128
    // with a checkable prompt, independently derived the same correct answer through a longer,
    // more roundabout chain of reasoning - consistent with PLE's shortcut path not firing rather
    // than a numerical error. There is accordingly no correctness basis for this function to reject
    // any range; any model-specific advisory about where a cut is well- or ill-advised belongs in
    // that model's own adapter (e.g. internal/model/qwen on the Tectonic side), not here - a QSA
    // interval is Qwen/Qwen3-Next-specific, and this engine is not.
    bool configure(int lo, int hi, int nl, bool has_input, std::string& err) {
        if (hi < 0) hi = nl;
        if (lo < 0 || lo >= hi || hi > nl || (lo > 0 && !has_input)) {
            err = "metal stage: invalid range or missing handoff input"; return false;
        }
        begin = lo; end = hi; layers = nl;
        return true;
    }
    // n == 1 is Tier 1.5 (admission only, one resident slot's turn at a time, see window.cpp's
    // run_rows/rows_) - genuinely implemented for a split stage via the same per-slot "Big" buffers
    // (gstBig_/hbBig_/kqBig_/... keyed by rinfo_) a non-split stage already uses for them, and the
    // same stage_import/export below, which already generalizes over row count. n > 1 is Tier 2
    // (multiple different sequences' decode steps combined into one GPU dispatch) - deliberately
    // still rejected: that needs the stage_import/export handoff to carry N independent sequences'
    // residuals in one call, matched correctly to each side's own locally-assigned slot for the same
    // request_id, which has not been built or tested for a split stage.
    bool window(int n, std::string& err) const {
        if (split() && n != 1) {
            err = "metal stage: a split cut has no Tier 2 batched decode yet (one row at a time)"; return false;
        }
        return true;
    }
    int stream_layer(int segment) const { return begin + segment; }
};
// Hand-offs are tensor-major R[T,HC*N], bo[T,N], inj[T,HC], never row-interleaved.
inline void stage_import(const float* hand, float* r, float* bo, float* inj, int t, int n, int hc) {
    std::memcpy(r, hand, (size_t)t * hc * n * sizeof(float));
    std::memcpy(bo, hand + (size_t)t * hc * n, (size_t)t * n * sizeof(float));
    std::memcpy(inj, hand + (size_t)t * (hc + 1) * n, (size_t)t * hc * sizeof(float));
}
inline void stage_export(float* hand, const float* r, const float* bo, const float* inj, int t, int n, int hc) {
    std::memcpy(hand, r, (size_t)t * hc * n * sizeof(float));
    std::memcpy(hand + (size_t)t * hc * n, bo, (size_t)t * n * sizeof(float));
    std::memcpy(hand + (size_t)t * (hc + 1) * n, inj, (size_t)t * hc * sizeof(float));
}
} // namespace strata::metal_engine
