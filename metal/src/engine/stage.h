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
    bool configure(int lo, int hi, int nl, int qi, bool has_input, std::string& err) {
        if (hi < 0) hi = nl;
        if (lo < 0 || lo >= hi || hi > nl || (lo > 0 && !has_input)) {
            err = "metal stage: invalid range or missing handoff input"; return false;
        }
        // A split stage deliberately supports whole QSA groups only; not a claim that the generic
        // host protocol requires aligned cuts. This also keeps PLE 0/1 together. Kept as-is even
        // after Tier 1.5 batch-slot support landed below: every configuration Tectonic currently
        // compiles cuts on a QSA boundary already (qsa_interval's value is compile-time fixed, see
        // geometry_checks.hpp's static_asserts), so this has never actually constrained anything in
        // practice - relaxing it would need verifying a cut through a QSA group's internal state,
        // which nothing has exercised, for no configuration that needs it today.
        if ((lo != 0 || hi != nl) && (qi <= 0 || lo % qi || hi % qi)) {
            err = "metal stage: split cuts must align to QSA groups"; return false;
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
