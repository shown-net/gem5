// Standalone native writer contract; same codec is consumed by FastSim.
#include "fst_instruction_translations.hh"
#include <cassert>
#include <cstdio>
int main(int argc,char** argv) {
    assert(argc==2);
    using namespace fastsim::fst;
    std::array<std::uint8_t,32> policy{};policy[0]=1;
    InstructionTranslationWriter writer(argv[1],0,policy);
    RetiredFrontendMacro macro;
    macro.pc=0x1000;macro.end_record=1;macro.observed_uops=1;
    macro.known_flags=kFrontendRequiredKnown;
    macro.flags=kFrontendComplete|kFrontendStartsMacro;
    macro.instruction_size=1;macro.bytes[0]=0x90;macro.code_identity=1;macro.semantic_identity=2;
    InstructionFetchFacts unknown;unknown.pc=macro.pc;unknown.bytes=1;
    unsigned char hot[64]{};
    writer.observe_record(hot);writer.append(macro,unknown);writer.finish(1);
    bool invalid=false;
    try {auto broken=unknown;broken.status=InstructionFetchStatus::Mapped;validate_instruction_binding(macro,broken);}
    catch(const std::runtime_error&){invalid=true;}
    assert(invalid);
}
