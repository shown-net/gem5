// Production codec/snapshot tests; no native simulation or timing inputs.
#include "cpu/o3/probe/fst_core_functional_facts.hh"
#include "cpu/o3/core_functional_facts.hh"
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>

using namespace fastsim::fst;
template<class F> void rejects(F f) {
    bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; }
    assert(rejected);
}

// RegId-shaped descriptors feed production snapshot methods, which never
// flatten or access an ISA themselves. Literal native normalization is tested
// separately by the Python harness.
struct Reg {
    int cls; unsigned idx; bool flat=false; int pins=0;
    int classValue() const {return cls;}
    unsigned index() const {return idx;}
    bool isFlat() const {return flat;}
    int getNumPinnedWrites() const {return pins;}
};
struct Static {
    Reg sources[2]{{0,65},{1,50}},dests[1]{{6,2,false,3}};
    unsigned numSrcRegs()const{return 2;}unsigned numDestRegs()const{return 1;}
    Reg srcRegIdx(unsigned i)const{return sources[i];}Reg destRegIdx(unsigned i)const{return dests[i];}
    bool isSerializing()const{return true;}bool isSerializeBefore()const{return false;}
    bool isSerializeAfter()const{return true;}bool isReadBarrier()const{return false;}
    bool isWriteBarrier()const{return true;}bool isNonSpeculative()const{return false;}
    bool isStoreConditional()const{return false;}bool isSquashAfter()const{return false;}
    bool isMicrocodeControl()const{return true;}bool isDelayedCommit()const{return true;}
    bool isQuiesce()const{return false;}bool isMicroop()const{return true;}
    bool isFirstMicroop()const{return true;}bool isLastMicroop()const{return false;}
    bool isFullGprMove()const{return false;}bool isFullXmmMove()const{return true;}
    bool isArchitecturalLockedMemory()const{return true;}
    bool isMicroFusedMemory()const{return false;}bool isStackRegAllocation()const{return true;}
    bool coreFusionFactsKnown()const{return true;}
    unsigned coreFusionProducerFamily()const{return 3;}
    unsigned coreFusionAllowedJccMask()const{return 0xf030;}
    unsigned coreFusionJccCondition()const{return 255;}
};

int main(int argc,char** argv) {
    assert(argc==2);
    CoreFunctionalHeader h;
    h.core_id=2; h.policy_sha256[0]=1;
    // X86 ISA installs native placeholder Vec/VecElem/VecPred classes of
    // sizes 1/2/1 even though x86 decoded SIMD operands use FloatRegClass.
    h.architectural_registers={39,48,1,2,1,0,6,512};
    validate_core_header(h,2);
    auto stale=h;stale.architectural_registers[0]=38;
    rejects([&]{validate_core_header(stale,2);});
    auto badh=h; badh.policy_sha256={}; rejects([&]{validate_core_header(badh,2);});
    badh=h; badh.register_layout=2; rejects([&]{validate_core_header(badh,2);});

    gem5::o3::CoreFunctionalSnapshot snap(4,2);
    snap.observeSource(0,Reg{0,65},Reg{0,1,true});
    snap.observeSource(1,Reg{0,65},Reg{0,1,true});
    snap.observeSource(2,Reg{-1,0},Reg{-1,0,true});
    snap.observeSource(3,Reg{1,50},Reg{1,7,true});
    snap.observeDestination(0,Reg{6,4,false,3},Reg{6,4,true,3});
    snap.observeDestination(1,Reg{7,10,true,0},Reg{7,10,true,0});
    snap.completeNormalization();
    assert(snap.status()==CoreNormalizationStatus::Complete);
    auto operands=snap.operands();
    assert(operands.size()==6 && operands[0].original_index==65);
    assert(operands[0].flattened_index==1 && operands[1].flattened_index==1);
    assert(operands[2].original_class==255 && operands[2].flags&kCoreFlatKnown);
    assert(operands[3].original_index==50 && operands[3].flattened_index==7);
    assert(operands[4].static_num_pinned_writes==3);
    assert(operands[5].flags&kCoreOriginalIsFlat);
    snap.observeSource(0,Reg{0,65},Reg{0,1,true});
    assert(snap.status()==CoreNormalizationStatus::Complete);
    snap.observeSource(3,Reg{1,50},Reg{1,2,true});
    assert(snap.status()==CoreNormalizationStatus::Conflicting);
    for(const auto& op:snap.operands())assert(!(op.flags&kCoreFlatKnown));
    gem5::o3::CoreFunctionalSnapshot empty(0,0);
    assert(empty.status()==CoreNormalizationStatus::Unknown);
    empty.completeNormalization(); assert(empty.status()==CoreNormalizationStatus::Complete);
    gem5::o3::CoreFunctionalSnapshot partial(2,0);
    partial.observeSource(0,Reg{0,2},Reg{0,2,true});
    partial.completeNormalization(); assert(partial.status()==CoreNormalizationStatus::Partial);
    rejects([&]{gem5::o3::CoreFunctionalSnapshot too_many(257,0);});

    Static si;
    auto unknown=gem5::o3::collectCoreFunctionalFacts(si,si,nullptr,false);
    assert(unknown.row.static_flags==(1u|4u|16u|256u|512u|2048u|4096u|32768u|65536u));
    assert(unknown.row.root_allocation_flags==2 && unknown.row.fusion_allowed_jcc_mask==0xf030);
    assert(unknown.row.normalization_status==CoreNormalizationStatus::Unknown);
    assert(unknown.operands[0].original_index==65 && !(unknown.operands[0].flags&kCoreFlatKnown));
    validate_core_row(h,unknown.row,unknown.operands);
    gem5::o3::CoreFunctionalSnapshot complete(2,1);
    complete.observeSource(0,si.srcRegIdx(0),Reg{0,1,true});
    complete.observeSource(1,si.srcRegIdx(1),Reg{1,7,true});
    complete.observeDestination(0,si.destRegIdx(0),Reg{6,2,true,3});
    complete.completeNormalization();complete.context.frontendKnown=true;
    complete.context.origin=CoreFrontendOrigin::ObserverRecord;
    complete.context.addressSpace=0x5000;complete.context.fromUser=true;
    complete.context.decoderKnown=true;complete.context.decoderContext=0x31;
    auto collected=gem5::o3::collectCoreFunctionalFacts(si,si,&complete,false);
    assert(collected.row.frontend_asid==0x5000 && collected.row.frontend_from_user==1);
    assert(collected.row.normalization_status==CoreNormalizationStatus::Complete);
    assert(collected.operands[1].flattened_index==7 && collected.operands[2].static_num_pinned_writes==3);
    validate_core_row(h,collected.row,collected.operands);
    auto pseudo=gem5::o3::collectCoreFunctionalFacts(si,si,&complete,true);
    assert(!(pseudo.row.known_flags&(kCoreDecoderKnown|kCoreFusionKnown|kCoreAllocationKnown)));
    assert(!(pseudo.row.static_flags&kCoreArchitecturalLocked));
    validate_core_row(h,pseudo.row,pseudo.operands);

    CoreFunctionalRow row;
    row.known_flags=kCoreOperandsKnown|kCorePinsKnown|kCoreNormalizationKnown;
    row.source_count=4;row.destination_count=2;row.operand_count=6;
    row.normalization_status=CoreNormalizationStatus::Complete;
    validate_core_row(h,row,operands);
    auto bad=row;bad.known_flags|=1u<<31;rejects([&]{validate_core_row(h,bad,operands);});
    auto badops=operands;badops[1].operand_index=0;rejects([&]{validate_core_row(h,row,badops);});
    badops=operands;badops[4].static_num_pinned_writes=-1;rejects([&]{validate_core_row(h,row,badops);});
    badops=operands;badops[3].flattened_index=48;rejects([&]{validate_core_row(h,row,badops);});
    badops=operands;badops[0].flattened_index=2;rejects([&]{validate_core_row(h,row,badops);});
    badops=operands;badops[4].flags|=kCoreOriginalIsFlat;badops[4].flattened_index=3;
    rejects([&]{validate_core_row(h,row,badops);});
    badops=operands;badops[5].flattened_index=11;rejects([&]{validate_core_row(h,row,badops);});
    badops=operands;badops[3].original_index=20;badops[3].flattened_index=20;
    validate_core_row(h,row,badops);badops[3].flattened_index=21;
    rejects([&]{validate_core_row(h,row,badops);});
    for(unsigned top=0;top<8;++top) {
        badops=operands;badops[3].flattened_index=(top+2)%8;
        validate_core_row(h,row,badops); // Captured TOP remains authoritative.
    }
    badops=operands;badops[0].flags&=~kCoreFlatKnown;rejects([&]{validate_core_row(h,row,badops);});
    bad=row;bad.decoder_context=17;rejects([&]{validate_core_row(h,bad,operands);});
    bad=row;bad.frontend_asid=4096;rejects([&]{validate_core_row(h,bad,operands);});
    bad=row;bad.reserved[0]=1;rejects([&]{validate_core_row(h,bad,operands);});
    bad=row;bad.source_count=257;rejects([&]{validate_core_row(h,bad,operands);});
    bad=row;bad.known_flags|=kCoreStaticKnown;bad.static_flags=kCoreSerializeBefore;
    rejects([&]{validate_core_row(h,bad,operands);});
    bad.static_flags|=kCoreSerializing;validate_core_row(h,bad,operands);
    bad.root_allocation_flags=kCoreStackAllocation;rejects([&]{validate_core_row(h,bad,operands);});
    bad.known_flags|=kCoreAllocationKnown;validate_core_row(h,bad,operands);

    std::array<unsigned char,64> hot{};
    hot[0]=0x40;
    row.pc=0x40;row.code_identity=11;row.semantic_prefix_identity=19;
    const std::string path=argv[1];
    CoreFunctionalWriter writer(path,h);
    writer.observe(hot.data(),row,operands);
    rejects([&]{writer.finish(1);}); // Every span needs a binding.
    RetiredFrontendMacro macro;
    macro.begin_record=0;macro.end_record=1;macro.pc=0x40;
    macro.code_identity=11;macro.semantic_identity=19;macro.observed_uops=1;
    writer.append_macro(macro);
    writer.finish(1);
    gzFile input=gzopen(path.c_str(),"rb");assert(input);
    CoreFunctionalHeader disk;assert(gzread(input,&disk,sizeof(disk))==96);
    assert(disk.architectural_registers[0]==39 && disk.core_id==2);
    assert(gzgetc(input)==1);
    CoreFunctionalRow diskrow;assert(gzread(input,&diskrow,sizeof(diskrow))==128);
    assert(diskrow.hot_record_hash==core_record_hash(hot.data()));
    std::vector<CoreFunctionalOperand> diskops(6);
    assert(gzread(input,diskops.data(),144)==144);
    assert(diskops[3].flattened_index==7 && diskops[4].static_num_pinned_writes==3);
    assert(gzgetc(input)==2);
    RetiredFrontendMacro disk_macro;assert(gzread(input,&disk_macro,80)==80);
    assert(disk_macro.semantic_identity==19);
    assert(gzgetc(input)==255);
    CoreFunctionalFooter footer;assert(gzread(input,&footer,sizeof(footer))==40);
    assert(footer.records==1 && footer.macros==1);
    assert(footer.stream_hash==core_stream_hash(kCoreHashSeed,hot.data(),64));
    assert(gzgetc(input)==-1 && gzeof(input));gzclose(input);
    std::cout<<"core functional codec and immutable snapshots passed\n";
}
