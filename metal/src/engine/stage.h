// CPU-testable contract for the Metal L0 T=1 layer chain. No GPU or wire ownership.
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
        // L0 deliberately supports whole QSA groups only; not a claim that the
        // generic host protocol requires aligned cuts. This also keeps PLE 0/1 together.
        if ((lo != 0 || hi != nl) && (qi <= 0 || lo % qi || hi % qi)) {
            err = "metal stage: L0 cuts must align to QSA groups"; return false;
        }
        begin = lo; end = hi; layers = nl;
        return true;
    }
    bool window(int n, bool rows, std::string& err) const {
        if (split() && (n != 1 || rows)) {
            err = "metal stage: L0 supports T=1, single sequence only"; return false;
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
