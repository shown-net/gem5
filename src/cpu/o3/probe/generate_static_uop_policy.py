"""Independent native build identity for static functional descriptor semantics."""
import hashlib
from pathlib import Path

POLICY_FILES = (
    'src/cpu/o3/probe/generate_static_uop_policy.py',
    'src/cpu/o3/probe/fst_static_uops.hh',
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
