"""CPU contract regression; production Window boundary methods with shared-memory stubs.

Does not assert Metal arithmetic, graph execution or KV/GDN equivalence. Those
require the real-model stage comparison, separately bounded and logged.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "stage.h"
#include <cassert>
#include <vector>
using namespace strata::metal_engine;
constexpr int N=4, HC=2;
struct Buffer { float* ptr; template<class T> T* as() const { return (T*)ptr; } };
struct Window {
    StagePlan stage_;
    bool initialized=true, boundary_ready_=false, boundary_applied_=false;
    int64_t boundary_pos_=-1;
    float r[8]={}, b[4]={}, i[2]={}, j[2]={};
    Buffer R_{r}, bo_{b}, inj_[2]={{i},{j}};
    bool ready() const { return initialized; }
    bool boundary_capture(float*,float*,float*,int,int64_t,std::string&) const;
    bool boundary_apply(const float*,const float*,const float*,int,std::string&);
};
__METHODS__
int main() {
    std::string err;
    StagePlan full, head, middle, tail;
    assert(full.configure(0,-1,48,4,false,err));
    assert(full.first() && full.last() && !full.split() && full.window(8,true,err));
    assert(head.configure(0,24,48,4,false,err));
    assert(tail.configure(24,48,48,4,true,err));
    assert(middle.configure(8,24,48,4,true,err));
    assert(head.first() && !head.last() && !tail.first() && tail.last());
    assert(!middle.first() && !middle.last());
    assert(head.window(1,false,err) && !head.window(2,false,err) && !head.window(1,true,err));
    StagePlan bad;
    assert(!bad.configure(-1,24,48,4,false,err));
    assert(!bad.configure(24,24,48,4,true,err));
    assert(!bad.configure(0,49,48,4,false,err));
    assert(!bad.configure(24,48,48,4,false,err));
    assert(!bad.configure(1,24,48,4,true,err));
    assert(!bad.configure(0,23,48,4,false,err));
    std::vector<int> visits(48);
    for (auto s: {head,tail}) for(int l=s.begin;l<s.end;++l) {
        visits[l]++; assert(s.stream_layer(l-s.begin)==l);
    }
    for(int v: visits) assert(v==1);
    assert(middle.stream_layer(0)==8 && middle.stream_layer(15)==23);
    // Distinct nonzero tensor data catches R/bo/inj aliases and zero-fill bugs.
    float hand[28], copy[28], r[16], b[8], i[4];
    for(int k=0;k<28;k++) hand[k]=float(k+1);
    stage_import(hand,r,b,i,2,N,HC);
    assert(r[15]==16 && b[0]==17 && b[7]==24 && i[0]==25);
    stage_export(copy,r,b,i,2,N,HC);
    for(int k=0;k<28;k++) assert(copy[k]==hand[k]);
    Window w; w.stage_=tail;
    assert(w.boundary_apply(r,b,i,1,err));
    assert(w.boundary_applied_ && !w.boundary_ready_);
    assert(w.r[7]==8 && w.b[0]==17 && w.i[0]==25);
    assert(!w.boundary_apply(r,b,i,2,err));
    w.stage_=head;
    assert(!w.boundary_apply(r,b,i,1,err));
    float ro[8],bo[4],io[2];
    assert(!w.boundary_capture(ro,bo,io,1,7,err));
    w.boundary_ready_=true; w.boundary_pos_=7;
    assert(!w.boundary_capture(ro,bo,io,2,7,err));
    assert(!w.boundary_capture(ro,bo,io,1,8,err));
    assert(w.boundary_capture(ro,bo,io,1,7,err));
    assert(ro[7]==8 && bo[0]==17 && io[0]==25);
    // Keep raw injection logits, not sigmoid weights or fabricated zeroes.
    for(int c=0;c<HC;c++) assert(io[c]==w.i[c]);
    w.initialized=false; w.stage_=tail;
    assert(!w.boundary_apply(r,b,i,1,err));
}
'''


class MetalStage(unittest.TestCase):
    def test_production_boundary_and_stage_contract(self):
        source = (ROOT/'metal/src/engine/window.cpp').read_text()
        methods = source.split('bool Window::boundary_capture(', 1)[1].split('bool Window::run(', 1)[0]
        code = HARNESS.replace('__METHODS__', 'bool Window::boundary_capture(' + methods)
        with tempfile.TemporaryDirectory(prefix='strata-metal-stage-') as td:
            cpp, exe = Path(td)/'stage.cpp', Path(td)/'stage'
            cpp.write_text(code)
            result = subprocess.run(['clang++', '-std=c++20', '-O1', '-fsanitize=undefined',
                                     '-I', str(ROOT/'metal/src/engine'), str(cpp), '-o', str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
