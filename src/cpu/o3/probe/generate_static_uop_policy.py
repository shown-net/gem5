"""Independent native build identity for static functional descriptor semantics."""
import hashlib
from pathlib import Path

LOGICAL_MMU_CONTROLS_VERSION = 1
CORE_FUNCTIONAL_FACTS_VERSION = 1
STATIC_ORDERING_MAP_VERSION = 1
ARCHITECTURAL_BOUNDARY_VERSION = 1
ARCHITECTURAL_CORE_FACTS_VERSION = 2
ARCHITECTURAL_MMU_CONTROLS_VERSION = 2

POLICY_FILES = (
    'taogen/shared/fst_architectural_boundaries.hh',
    'taogen/shared/fst_mmu_controls_v2.hh',
    'src/mem/functional_architectural_boundaries.hh',
    'src/sim/faults.hh',
    'taogen/shared/fst_mmu_controls.hh',
    'src/mem/functional_mmu_controls.hh',
    'src/arch/generic/mmu.cc',
    'src/arch/generic/mmu.hh',
    'src/arch/generic/tlb.hh',
    'src/arch/x86/faults.cc',
    'src/arch/x86/faults.hh',
    'src/arch/x86/kvm/x86_cpu.cc',
    'src/arch/x86/kvm/x86_cpu.hh',
    'src/cpu/base.cc',
    'src/cpu/base.hh',
    'src/cpu/BaseCPU.py',
    'src/cpu/thread_context.cc',
    'src/cpu/thread_context.hh',
    'src/cpu/exec_context.hh',
    'src/cpu/reg_class.cc',
    'src/cpu/o3/cpu.cc',
    'src/cpu/o3/commit.cc',
    'src/cpu/o3/commit.hh',
    'src/cpu/o3/thread_context.cc',
    'src/cpu/o3/thread_context.hh',
    'src/cpu/o3/thread_state.cc',
    'src/cpu/o3/thread_state.hh',
    'src/cpu/kvm/base.cc',
    'src/cpu/kvm/base.hh',
    'src/sim/mem_state.cc',
    'configs/fastsim/fs_capture.py',
    'src/arch/generic/BaseMMU.py',
    'src/arch/generic/BaseTLB.py',
    'src/arch/x86/X86MMU.py',
    'src/arch/x86/X86TLB.py',

    'src/cpu/o3/probe/generate_static_uop_policy.py',
    'src/cpu/o3/probe/functional_store_pair.hh',
    'src/cpu/o3/probe/functional_syscall_isolation.hh',
    'src/cpu/o3/probe/fst_static_uops.hh',
    'src/cpu/o3/probe/fst_core_functional_facts.hh',
    'src/cpu/o3/core_functional_facts.hh',
    'src/cpu/o3/rename.cc',
    'src/cpu/o3/Frontend.py', 'src/cpu/o3/BaseO3CPU.py',
    'src/arch/x86/x86_traits.hh',
    'src/cpu/o3/probe/retired_frontend_facts.hh',
    'src/cpu/o3/probe/TaoTrace.py',
    'src/cpu/o3/probe/SConscript',
    'taogen/shared/fst_instruction_fetch.hh',
    'taogen/shared/fst_instruction_translations.hh',
    'src/arch/x86/tlb.cc', 'src/arch/x86/tlb.hh',
    'src/arch/x86/pagetable.cc', 'src/arch/x86/pagetable.hh',
    'src/arch/x86/pagetable_walker.cc', 'src/arch/x86/pagetable_walker.hh',
    'src/arch/x86/pcstate.hh',
    'src/arch/generic/pcstate.hh', 'src/arch/generic/decoder.hh',
    'src/cpu/o3/dyn_inst.cc', 'src/base/extensible.hh',
    'src/arch/x86/isa.cc', 'src/arch/x86/isa.hh', 'src/arch/generic/isa.hh', 'src/arch/x86/mmu.hh',
    'src/arch/x86/decoder.cc', 'src/arch/x86/decoder.hh',
    'src/cpu/o3/fetch.cc', 'src/cpu/o3/fetch.hh',
    'src/cpu/o3/frontend.cc', 'src/cpu/o3/frontend.hh',
    'src/cpu/o3/frontend_fetch.cc',
    'src/cpu/o3/frontend_record.cc', 'src/cpu/o3/frontend_record.hh',
    'src/cpu/o3/cpu.hh', 'configs/fastsim/collection.py',
    'src/cpu/o3/probe/fst_store_requests.hh',
    'taogen/shared/fst_translation_facts.hh',
    'taogen/shared/fst_dynamic_translation_writer.hh',
    'src/cpu/o3/probe/tao_trace.cc',
    'src/cpu/o3/probe/tao_trace.hh',
    'src/cpu/static_inst.hh',
    'src/cpu/static_inst.cc',
    'src/cpu/StaticInstFlags.py',
    'src/cpu/reg_class.hh',
    'src/cpu/int_execution_info.hh',
    'src/cpu/zero_idiom_info.hh',
    'src/cpu/o3/dyn_inst.hh',
    'src/cpu/o3/lsq_unit.hh',
    'src/cpu/o3/lsq.hh',
    'src/cpu/o3/lsq.cc',
    'src/mem/request.hh',
    'src/mem/functional_translation.hh',
    'src/cpu/o3/lsq_unit.cc',
)
POLICY_TREES = ('src/arch/x86/isa', 'src/arch/x86/insts', 'src/arch/x86/regs')

def policy_files(root):
    root=Path(root)
    files=set(POLICY_FILES)
    for tree in POLICY_TREES:
        directory=root/tree
        if not directory.is_dir():
            raise ValueError('missing static decoder source tree: '+str(directory))
        for path in directory.rglob('*'):
            if path.is_file() and path.suffix in ('.isa','.py','.hh','.cc'):
                files.add(path.relative_to(root).as_posix())
    return tuple(sorted(files))

def policy_digest(root):
    root = Path(root)
    digest = hashlib.sha256(b'fst-static-uop-policy-v1\0')
    for relative in policy_files(root):
        blob = (root / relative).read_bytes()
        digest.update(relative.encode() + b'\0')
        digest.update(len(blob).to_bytes(8, 'little'))
        digest.update(blob)
    return digest.digest()

def write_header(root, target):
    literal = ','.join('0x%02x' % byte for byte in policy_digest(root))
    Path(target).write_text('#pragma once\n#include <array>\n#include <cstdint>\n'
        'namespace fastsim::fst { inline constexpr std::array<std::uint8_t,32> '
        'kStaticUopPolicySha256{' + literal + '}; }\n')
