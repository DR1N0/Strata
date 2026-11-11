"""CPU regression: compile the real Metal run_slot_rows branch with GPU stubs.

No model, GPU call or watchdog timeout is used. The stubs exercise success and
failure returns from the production branch; this is not a Metal kernel test.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include "strata/core/progress.hpp"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace strata::kernels {
constexpr int kVerifyMaxT = 8;
struct SamplerParams {};
}
static bool fail_window = false, fail_sampling = false;
static int sampler_result = 0;
static void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
namespace strata::metal_engine {
bool window_run_slots(void*, const int*, int S, const int32_t*, const int64_t*, int32_t* out, std::string&) {
    // A real blocking window must not have emitted a completion heartbeat yet.
    for (int i = 0; i < S; ++i) out[i] = 42;
    return !fail_window;
}
bool window_sample_slot_row(void*, int, const strata::kernels::SamplerParams&, const int32_t*, int, int32_t* out, std::string&) {
    *out = 43;
    return !fail_sampling;
}
}
namespace strata::core {
using PoolMultiFn = void (*)(void);
struct Verifier {
    void* metal_ = nullptr;
    int last_t_ = 0, last_rows_[8] = {};
    int32_t last_tokens_[8] = {};
    int64_t last_pos_b_[8] = {};
    bool last_batch_ = false;
    int slot_row_draw(int, int, strata::kernels::SamplerParams&, const int32_t*&, int&, std::vector<int32_t>&, std::string&) {
        return sampler_result;
    }
    bool run_slot_rows(const int* rows, int S, const int32_t* tokens, const int64_t* pos, PoolMultiFn pool,
                       void* user, int32_t* out, std::string& err) {
        __PRODUCTION_BRANCH__
    }
};
}
int main() {
    using namespace strata::core;
    Verifier v;
    int rows[4] = {0,1,2,3}; int32_t tokens[4] = {1,2,3,4}, out[4] = {};
    int64_t pos[4] = {16000,16000,16000,16000}; std::string err;
    auto& p = progress(); p.busy.store(true);
    // Long-lived requests must advance the completion heartbeat every window,
    // even if the watchdog never samples the short gaps between busy windows.
    for (int i = 0; i < 1024; ++i) {
        const auto before = p.beats.load();
        require(v.run_slot_rows(rows, 4, tokens, pos, nullptr, nullptr, out, err), "greedy window success");
        require(p.beats.load() == before + 1, "successful Metal window did not beat");
        require(std::strcmp(p.where.load(), "decode") == 0, "successful stage not decode");
        require(v.last_t_ == 4 && v.last_batch_ && out[3] == 42, "greedy state/output changed");
    }
    auto before = p.beats.load();
    require(!v.run_slot_rows(rows, 0, tokens, pos, nullptr, nullptr, out, err), "zero-width accepted");
    require(!v.run_slot_rows(rows, 9, tokens, pos, nullptr, nullptr, out, err), "oversized window accepted");
    require(p.beats.load() == before, "invalid window beat");
    fail_window = true;
    require(!v.run_slot_rows(rows, 4, tokens, pos, nullptr, nullptr, out, err), "window failure hidden");
    require(p.beats.load() == before, "failed window beat");
    require(std::strcmp(p.where.load(), "verify batch: waiting for Metal window") == 0 && p.detail.load() == 4,
            "failed window lost waiting-stage diagnostics");
    fail_window = false; sampler_result = -1;
    require(!v.run_slot_rows(rows, 4, tokens, pos, nullptr, nullptr, out, err), "sampler preparation failure hidden");
    require(p.beats.load() == before, "sampler preparation failure beat");
    sampler_result = 1; fail_sampling = true;
    require(!v.run_slot_rows(rows, 4, tokens, pos, nullptr, nullptr, out, err), "sampling failure hidden");
    require(p.beats.load() == before, "failed sampling beat");
    fail_sampling = false;
    require(v.run_slot_rows(rows, 4, tokens, pos, nullptr, nullptr, out, err), "sampled window success");
    require(p.beats.load() == before + 1 && out[3] == 43, "sampled window heartbeat/output changed");
    require(p.busy.load(), "verifier changed caller busy state");
    std::puts("PASS: 6 Metal-branch heartbeat scenarios (GPU stubs)");
}
'''


def metal_branch(source):
    return source.split('bool Verifier::run_slot_rows(', 1)[1].split(
        '#if defined(STRATA_GPU_METAL)', 1)[1].split('#endif', 1)[0]


class MetalBatchProgress(unittest.TestCase):
    def compile_and_run(self, source):
        with tempfile.TemporaryDirectory(prefix='strata-metal-progress-') as td:
            cpp, exe = Path(td)/'progress.cpp', Path(td)/'progress'
            cpp.write_text(HARNESS.replace('__PRODUCTION_BRANCH__', metal_branch(source)))
            build = subprocess.run(['clang++', '-std=c++20', '-O2', '-I', str(ROOT/'include'),
                                    str(cpp), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True)

    def test_current_branch_success_beats_and_failures_do_not(self):
        result = self.compile_and_run((ROOT/'src/core/verify.cpp').read_text())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PASS: 6', result.stdout)

    def test_missing_completion_heartbeat_is_detected(self):
        source = (ROOT/'src/core/verify.cpp').read_text()
        branch = metal_branch(source)
        self.assertIn('progress_beat();', branch)
        broken = source.replace(branch, branch.replace('progress_beat();', ''), 1)
        result = self.compile_and_run(broken)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('successful Metal window did not beat', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
