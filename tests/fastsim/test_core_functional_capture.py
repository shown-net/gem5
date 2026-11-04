"""Literal native source gates; compile only narrow fixtures, never gem5."""
import hashlib
import os
from pathlib import Path
import runpy
import re
import subprocess
import textwrap
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT.parent / 'tmp/p3-core-functional-child-tests'


def method(text, signature):
    begin = text.index(signature)
    brace = text.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


def compile_run(source, name):
    BUILD.mkdir(parents=True, exist_ok=True)
    cc = BUILD / (name + '.cc')
    exe = BUILD / name
    cc.write_text(source)
    subprocess.run(['c++', '-std=c++17', '-O0', '-Wall', '-Wextra',
                    '-Wl,-rpath,/opt/gcc-11/lib64', '-I' + str(ROOT / 'src'),
                    str(cc), '-lz', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


class CoreCaptureTest(unittest.TestCase):
    def test_actual_native_register_headers_match_versioned_codec(self):
        # Use real source definitions and real generated debug declarations;
        # no mocked enum, RegClass, header, or native runtime is involved.
        generated = Path(os.environ.get('GEM5_CORE_FACTS_BUILD', ROOT / 'build/X86'))
        self.assertTrue((generated / 'debug/IntRegs.hh').is_file(),
                        'set GEM5_CORE_FACTS_BUILD to an existing native generated-header directory')
        text = (ROOT / 'src/cpu/o3/probe/tao_trace.cc').read_text()
        begin = text.index('// Core functional register layout source binding')
        end = text.index('// End core functional register layout source binding.', begin)
        assertions = text[begin:end]
        program = '''#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/float.hh"
#include "arch/x86/regs/ccr.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/probe/fst_core_functional_facts.hh"
namespace gem5::o3 {
''' + assertions + '''
static_assert(X86ISA::int_reg::NumRegs == X86ISA::flatIntRegClass.numRegs());
static_assert(X86ISA::int_reg::_ProdlowIdx == X86ISA::int_reg::MicroEnd + 1);
static_assert(X86ISA::int_reg::_DoublebitsIdx + 1 == X86ISA::int_reg::NumRegs);
static_assert(X86ISA::float_reg::NumRegs == X86ISA::flatFloatRegClass.numRegs());
static_assert(X86ISA::misc_reg::NumRegs == X86ISA::miscRegClass.numRegs());
}
'''
        BUILD.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix='actual-native-layout-', dir=BUILD))
        source = work / 'layout.cc'; source.write_text(program)
        subprocess.run(['c++', '-std=c++17', '-fsyntax-only', '-DTRACING_ON=0',
                        '-I' + str(ROOT / 'src'), '-I' + str(generated),
                        '-I' + str(ROOT / 'ext'), str(source)], check=True)
        # Both consumers must validate the same versioned layout and bounds.
        parent = (ROOT.parent / 'include/fastsim/fst_core_functional_facts.hpp').read_text()
        native = (ROOT / 'src/cpu/o3/probe/fst_core_functional_facts.hh').read_text()
        self.assertEqual(parent.replace('"fastsim/retired_frontend_facts.hpp"',
                                       '"retired_frontend_facts.hh"'), native)

    def test_literal_frontend_observation_does_not_enable_native_predicate_fields(self):
        text = (ROOT / 'src/cpu/o3/frontend_record.cc').read_text()
        capture = text[text.index('    if (cpu->markerObservation)'):text.index('#if TRACING_ON')]
        self.assertIn('coreFunctionalContext', capture, 'missing separate frontend observer snapshot')
        start = text.index('    inst->setPredTaken(prediction.taken);')
        end = text.index('    inst->pendingFrontendAcceptance = true;', start)
        transport = text[start:end]
        self.assertIn('coreFunctionalFacts', transport, 'frontend immutable context not transported')
        program = '''#include "cpu/o3/core_functional_facts.hh"
#include <memory>
#include <cassert>
using namespace gem5::o3;
namespace X86ISA {namespace misc_reg {enum {Cr3};}
struct PCState {unsigned n=3;uint64_t context=0x31;
 unsigned size()const{return n;}unsigned functionalByteCount()const{return n;}
 uint64_t functionalDecoderContext()const{return context;}
 template<class T>const T& as()const{return *this;}};}
struct CPU {bool markerObservation=false,coreFunctionalObservation=false,user=true;
 uint64_t asid=0x2000;CPU*tcBase(int){return this;}
 uint64_t readMiscRegNoEffect(int){return asid;}bool inUserMode(int){return user;}};
struct Inst {bool fetchedFromUser=false;uint64_t fetchedAddressSpaceId=0;
 std::unique_ptr<CoreFunctionalSnapshot> coreFunctionalFacts;
 unsigned numSrcRegs()const{return 0;}unsigned numDestRegs()const{return 0;}
 void setPredTaken(bool){}};
struct Record {bool fromUser=false;uint64_t addressSpace=0;
 std::unique_ptr<CoreFunctionalContext> coreFunctionalContext;
 struct {bool taken=false;} prediction;
 void capture(CPU*cpu,int tid,const X86ISA::PCState& at){
''' + capture + '''
 }
 void materialize(Inst*inst){
''' + transport + '''
 }
};
int main(){CPU cpu;X86ISA::PCState pc;
 Record off;off.capture(&cpu,0,pc);Inst offinst;off.materialize(&offinst);
 assert(!off.fromUser && off.addressSpace==0 && !offinst.coreFunctionalFacts);
 cpu.coreFunctionalObservation=true;Record on;on.capture(&cpu,0,pc);
 assert(!cpu.markerObservation && !on.fromUser && on.addressSpace==0);
 cpu.asid=0x9000;cpu.user=false;pc.context=0;Inst inst;on.materialize(&inst);
 assert(!inst.fetchedFromUser && inst.fetchedAddressSpaceId==0);
 auto& c=inst.coreFunctionalFacts->context;
 assert(c.frontendKnown && c.addressSpace==0x2000 && c.fromUser);
 assert(c.decoderKnown && c.decoderContext==0x31);
 cpu.markerObservation=true;Record marked;marked.capture(&cpu,0,pc);
 assert(marked.addressSpace==0x9000 && !marked.fromUser);
 pc.n=0;Record unknown;unknown.capture(&cpu,0,pc);
 assert(!unknown.coreFunctionalContext->decoderKnown);
}
'''
        compile_run(program, 'literal_frontend_context')

    def test_observer_fusion_predicates_match_unchanged_native_opcode_function(self):
        text = (ROOT / 'src/arch/x86/insts/static_inst.hh').read_text()
        native = method(text, 'bool canMacroFuseWith(')
        # The root explicitly requires retaining this original body unchanged.
        expected = 'e100b0020b40d8e59ee94cbabf8cb9d23f20ec8036a5eab48f9855563ed7bf1d'
        self.assertEqual(hashlib.sha256(native.encode()).hexdigest(), expected)
        self.assertIn('coreFusionProducerFamily()', text,
                      'missing observer-only decoded fusion metadata')
        functions = [method(text, sig) for sig in (
            'unsigned coreFusionProducerFamily()', 'unsigned coreFusionAllowedJccMask()',
            'unsigned coreFusionJccCondition()')]
        source = '''#include <cassert>
#include <cstdint>
enum {OneByteOpcode, TwoByteOpcode, OtherOpcode};
struct StaticInst { virtual ~StaticInst()=default;
 virtual bool canMacroFuseWith(const StaticInst&) const{return false;}
 virtual unsigned coreFusionProducerFamily() const{return 0;}
 virtual unsigned coreFusionAllowedJccMask() const{return 0;}
 virtual unsigned coreFusionJccCondition() const{return 255;}
};
struct X86StaticInst : StaticInst {
 struct {struct {int type=0,op=0;} opcode;struct {unsigned reg=0;} modRM;} machInst;
''' + native + '\n' + '\n'.join(functions) + '''
};
int main(){
 X86StaticInst producer,branch;
 for(int type=0;type<3;++type)for(int opcode=0;opcode<256;++opcode)
 for(unsigned group=0;group<8;++group)for(int bt=0;bt<3;++bt)
 for(int bop=0;bop<256;++bop){
  producer.machInst.opcode={type,opcode};producer.machInst.modRM.reg=group;
  branch.machInst.opcode={bt,bop};
  unsigned cond=branch.coreFusionJccCondition();
  bool candidate=cond<16 && (producer.coreFusionAllowedJccMask()&(1u<<cond));
  assert(candidate==producer.canMacroFuseWith(branch));
 }
 StaticInst other;assert(!producer.canMacroFuseWith(other));
}
'''
        compile_run(source, 'fusion_parity')

    def test_actual_rename_call_sites_freeze_architectural_flattening(self):
        text = (ROOT / 'src/cpu/o3/rename.cc').read_text()
        source_begin = text.index('const RegId& src_reg = inst->srcRegIdx(src_idx);')
        source_end = text.index('renamed_reg = map->lookup(flat_reg);', source_begin)
        source = text[source_begin:source_end]
        self.assertIn('observeSource', source, 'rename source snapshot not captured before map lookup')
        dest_begin = text.index('RegId flat_dest_regid = dest_reg.flatten(*isa);')
        dest_end = text.index('if (inst->eliminatedMove)', dest_begin)
        dest = text[dest_begin:dest_end]
        self.assertIn('observeDestination', dest, 'destination snapshot missing before both rename branches')
        flatten_text = (ROOT / 'src/arch/x86/regs/float.cc').read_text()
        flatten = method(flatten_text, 'RegId\nFloatRegClassOps::flatten(')
        # Literal native x87 normalizer; the stubs only supply architectural ISA TOP.
        program = '''#include "cpu/o3/core_functional_facts.hh"
#include <cassert>
#include <memory>
using RegIndex=unsigned;
struct BaseISA {virtual ~BaseISA()=default;};
namespace X86ISA {namespace misc_reg {enum {X87Top};}
 struct ISA:BaseISA {unsigned top=5; unsigned readMiscRegNoEffect(int) const{return top;}};
}
struct RegId {int cls;unsigned idx;bool flat=false;int pins=0;
 operator RegIndex()const{return idx;}
 int classValue() const{return cls;}unsigned index()const{return idx;}
 bool isFlat()const{return flat;}int getNumPinnedWrites()const{return pins;}
 void setNumPinnedWrites(int p){pins=p;}RegId flatten(const BaseISA&)const;
};
namespace X86ISA {namespace float_reg {constexpr unsigned NumRegs=48;
 RegId stack(unsigned index,unsigned top){return {1,(top+index+8)%8,true};}}
constexpr int flatFloatRegClass=1;
struct FloatRegClassOps {RegId flatten(const BaseISA&,const RegId&)const;};
''' + flatten + '''
}
RegId RegId::flatten(const BaseISA& isa)const {
 if(flat)return *this;
 if(cls==1)return X86ISA::FloatRegClassOps{}.flatten(isa,*this);
 return {cls,idx&~64u,true,pins};
}
using PhysRegIdPtr=void*;
struct Inst {std::unique_ptr<gem5::o3::CoreFunctionalSnapshot> coreFunctionalFacts;
 RegId srcRegIdx(int)const{return {1,50};}};
int main(){
 Inst object;auto* inst=&object;X86ISA::ISA state;auto* isa=&state;
 inst->coreFunctionalFacts=std::make_unique<gem5::o3::CoreFunctionalSnapshot>(1,1);
 int src_idx=0,dest_idx=0;RegId dest_reg{0,65,false,3};
''' + source + dest + '''
 (void)renamed_reg;
 inst->coreFunctionalFacts->completeNormalization();
 state.top=0; // later retirement state must never recompute these facts.
 const auto& facts=inst->coreFunctionalFacts->operands();
 assert(facts[0].original_index==50 && facts[0].flattened_index==7);
 assert(facts[1].original_index==65 && facts[1].flattened_index==1);
 assert(facts[1].static_num_pinned_writes==3);
}
'''
        compile_run(program, 'literal_normalization')

    def test_collection_opt_in_and_actual_configuration_export(self):
        module = runpy.run_path(str(ROOT / 'configs/fastsim/collection.py'))
        base = ['--fastsim-root', str(ROOT.parent), '--workload-binary', '/bin/true', '--omp-threads', '1']
        args = module['parse_args'](base)
        self.assertFalse(getattr(args, 'functional_core_facts', None), 'default observer must be disabled')
        self.assertIn('native_core_profile', module, 'missing immutable core parameter export')
        args = module['parse_args'](base + ['--functional-core-facts'])
        self.assertTrue(args.functional_core_facts)
        self.assertTrue(module['trace_options'](args, '/trace', '/profile', {})['functional_core_facts'])
        service = lambda n: SimpleNamespace(instructionsPerBatch=n, opsPerCycle=n+1, batchOps=n+2, latency=n+3)
        dsb = service(7);dsb.numSets=16;dsb.associativity=3;dsb.opsPerEntry=11
        frontend = SimpleNamespace(instructionBytes=SimpleNamespace(bufferBytes=33, bytesPerCycle=17),
                                   idq=SimpleNamespace(capacity=99), mite=service(3),dsb=dsb,ms=service(1),
                                   dsbToMiteDelay=5)
        cpu = SimpleNamespace(numThreads=2, numPhysIntRegs=301, numPhysFloatRegs=277,
                              numPhysVecRegs=4,numPhysVecPredRegs=7,numPhysMatRegs=3,numPhysCCRegs=1700,
                              frontend=frontend)
        out = module['native_core_profile'](cpu)
        self.assertEqual(out['physical_registers']['Int'], 301)
        self.assertEqual(out['physical_registers']['CC'], 1700)
        self.assertEqual(out['frontend']['dsb']['numSets'], 16)
        self.assertEqual(out['frontend']['idq']['capacity'], 99)
        self.assertIsNone(out['architectural_registers'])
        self.assertNotIn('initial_free_registers', out)

    def test_policy_binds_normalization_transport_codec_and_parameter_sources(self):
        module = runpy.run_path(str(ROOT / 'src/cpu/o3/probe/generate_static_uop_policy.py'))
        files = set(module['policy_files'](ROOT))
        required = {'src/cpu/o3/rename.cc', 'src/cpu/o3/core_functional_facts.hh',
                    'src/cpu/o3/probe/fst_core_functional_facts.hh', 'src/cpu/o3/Frontend.py',
                    'src/cpu/o3/BaseO3CPU.py', 'src/arch/x86/x86_traits.hh',
                    'src/arch/x86/isa.hh', 'src/arch/generic/isa.hh',
                    'src/arch/x86/regs/int.hh', 'src/arch/x86/regs/float.hh',
                    'src/arch/x86/regs/ccr.hh', 'src/cpu/reg_class.hh',
                    'src/cpu/o3/frontend_record.cc', 'src/cpu/o3/dyn_inst.hh'}
        self.assertFalse(required-files, 'compiled policy must include all new producer semantics')
        # Build an isolated source-root fixture, alter a normalization source,
        # and exercise the production digest rather than checking text only.
        isolated = BUILD / 'policy-root'
        for name in files:
            path = isolated / name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes((ROOT / name).read_bytes())
        for name in ('src/cpu/o3/rename.cc', 'src/arch/x86/isa.hh', 'src/arch/generic/isa.hh',
                     'src/arch/x86/regs/int.hh', 'src/arch/x86/regs/float.hh',
                     'src/arch/x86/regs/ccr.hh', 'src/cpu/reg_class.hh'):
            before = module['policy_digest'](isolated)
            p = isolated / name;p.write_bytes(p.read_bytes()+b'\n// fixture mutation\n')
            self.assertNotEqual(before, module['policy_digest'](isolated), name)

    def test_actual_static_map_ordering_flags_use_static_uop_and_locked_root(self):
        text = (ROOT / 'src/cpu/o3/probe/tao_trace.cc').read_text()
        self.assertTrue('add_flag(kFstStaticReadBarrier' in text,
                        'native map producer is missing directional barrier facts')
        start = text.index('    add_flag(kFstStaticReadBarrier')
        end = text.index('    if (si->isDirectCtrl())', start)
        block = text[start:end]
        program = '''#include <cassert>
#include <cstdint>
enum FstStaticFlag:uint16_t {kFstStaticReadBarrier=128,kFstStaticWriteBarrier=256,kFstStaticLockedRmw=512};
struct SI {bool read=false,write=true,locked=false;
 bool isReadBarrier()const{return read;}bool isWriteBarrier()const{return write;}
 bool isArchitecturalLockedMemory()const{return locked;}};
struct Inst {SI*macroop;};
int main(){SI uop,macro;macro.locked=true;auto*si=&uop;Inst object{&macro};auto*inst=&object;
 uint16_t flags=0;auto add_flag=[&](FstStaticFlag flag,bool enabled){if(enabled)flags|=flag;};
''' + block + '''
 assert(flags==768); // write barrier + root LOCK; no read bit inferred.
}
'''
        compile_run(program, 'literal_imap_ordering')

    def test_locked_macro_aggregate_satisfies_existing_map_reader_or_is_suppressed(self):
        text = (ROOT / 'src/cpu/o3/probe/tao_trace.cc').read_text()
        start = text.index('    if (!completes_instruction) return;')
        end = text.index('    FstStaticInstruction completed;', start)
        guard = text[start:end]
        self.assertIn('kFstStaticLockedRmw', guard, 'incomplete locked ordering must be suppressed')
        flags_start = text.index('    add_flag(kFstStaticMemory,')
        flags_end = text.index('    if (si->isDirectCtrl())', flags_start)
        flags = text[flags_start:flags_end]
        microcode = runpy.run_path(str(ROOT / 'src/arch/x86/isa/insts/general_purpose/data_transfer/xchg.py'))['microcode']
        sequence = re.search(r'def macroop XCHG_M_R\s*\{(.*?)\};', microcode, re.S).group(1)
        operations = [line.strip().split()[0] for line in sequence.splitlines() if line.strip()]
        spec = (ROOT / 'src/arch/x86/isa/microops/specop.isa').read_text()
        fence_class = textwrap.dedent(spec[spec.index('    class MfenceOp('):spec.index('    microopClasses["mfence"]')])
        env = {'X86Microop': object};exec(fence_class, env)
        fence = env['MfenceOp']().instFlags
        self.assertIn('IsReadBarrier', fence);self.assertIn('IsWriteBarrier', fence)
        rows = ','.join('{'+','.join(str(int(v)) for v in (op=='mfence',op=='mfence',op.startswith('ld'),op.startswith('st'),False))+'}' for op in operations)
        program = '''#include <cassert>
#include <cstdint>
#include <iostream>
enum FstStaticFlag:uint16_t {kFstStaticMemory=64,kFstStaticReadBarrier=128,kFstStaticWriteBarrier=256,kFstStaticLockedRmw=512};
struct SI {bool read,write,load,store,locked;
 bool isReadBarrier()const{return read;}bool isWriteBarrier()const{return write;}
 bool isLoad()const{return load;}bool isStore()const{return store;}bool isAtomic()const{return false;}
 bool isArchitecturalLockedMemory()const{return locked;}};
struct Inst {SI*macroop;};
bool run(bool omit_fences,uint16_t& result){
 struct {uint16_t flags=0;}pending;bool suppressed=false;
 auto suppress_static_pc=[&](int){suppressed=true;};int key=0;bool completes_instruction=true;
 SI operations[]={''' + rows + '''};SI macro{false,false,false,false,true};Inst object{&macro};auto*inst=&object;
 for(auto& item:operations){if(omit_fences&&item.read)continue;auto*si=&item;
 auto add_flag=[&](FstStaticFlag flag,bool enabled){if(enabled)pending.flags|=flag;};
''' + flags + '''
 }
 auto finish=[&](){
''' + guard + '''
 result=pending.flags;
 };finish();return !suppressed;
}
int main(){uint16_t flags=0;assert(run(false,flags));assert(flags==960);
 std::cout<<flags;assert(!run(true,flags));}
'''
        compile_run(program, 'literal_locked_macro')
        emitted = int(subprocess.check_output([str(BUILD / 'literal_locked_macro')]))
        consumer = runpy.run_path(str(ROOT.parent / 'tools/audit_fst_static_instruction_maps.py'))
        consumer['validate_geometry'](4096,4099,0,emitted,3)
        with self.assertRaises(ValueError):
            consumer['validate_geometry'](4096,4099,0,512|64,3)


if __name__ == '__main__':
    unittest.main()
