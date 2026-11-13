// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "cpu/o3/probe/fst_core_functional_facts.hh"

namespace gem5::o3 {
// Independent pure observer context; never used by source/fusion/admission.
struct CoreFunctionalContext {
    std::uint64_t decoderContext=0,addressSpace=0;
    bool fromUser=false,decoderKnown=false,frontendKnown=false;
    fastsim::fst::CoreFrontendOrigin origin=fastsim::fst::CoreFrontendOrigin::Unknown;
    template<class PC> void observeDecoder(const PC& pc) {
        decoderKnown=pc.size()>0 && pc.size()<=15 && pc.functionalByteCount()==pc.size();
        decoderContext=decoderKnown?pc.functionalDecoderContext():0;
    }
};

// Only architectural descriptors cross this boundary. It neither flattens
// registers nor accepts physical registers, readiness, pin state or ticks.
class CoreFunctionalSnapshot {
    std::uint16_t sources_,destinations_;
    std::vector<fastsim::fst::CoreFunctionalOperand> operands_;
    fastsim::fst::CoreNormalizationStatus status_=fastsim::fst::CoreNormalizationStatus::Unknown;
    void conflict() {
        status_=fastsim::fst::CoreNormalizationStatus::Conflicting;
        for(auto& op:operands_) {
            op.flags&=~fastsim::fst::kCoreFlatKnown;
            op.flattened_class=255;op.flattened_index=0;
            op.normalization_origin=fastsim::fst::CoreNormalizationOrigin::Unknown;
        }
    }
    template<class Reg> void observe(unsigned index,bool dest,const Reg& original,const Reg& flat) {
        using namespace fastsim::fst;
        if(index>=(dest?destinations_:sources_))throw std::out_of_range("core normalization operand index");
        if(status_==CoreNormalizationStatus::Conflicting)return;
        CoreFunctionalOperand value;value.operand_index=index;
        value.original_class=static_cast<std::uint8_t>(original.classValue());value.original_index=original.index();
        value.flattened_class=static_cast<std::uint8_t>(flat.classValue());value.flattened_index=flat.index();
        value.flags=kCoreOriginalKnown|kCoreFlatKnown|(original.isFlat()?kCoreOriginalIsFlat:0);
        value.normalization_origin=CoreNormalizationOrigin::RenameOperand;
        if(dest){value.flags|=kCorePinKnown;value.static_num_pinned_writes=original.getNumPinnedWrites();}
        auto& previous=operands_[dest?sources_+index:index];
        if(previous.flags&kCoreFlatKnown) {
            if(std::memcmp(&previous,&value,sizeof(value)))conflict();
            return;
        }
        previous=value;status_=CoreNormalizationStatus::Partial;
    }
public:
    CoreFunctionalContext context;
    CoreFunctionalSnapshot(unsigned sources,unsigned destinations):sources_(sources),destinations_(destinations) {
        if(sources>fastsim::fst::kCoreMaxOperands || destinations>fastsim::fst::kCoreMaxOperands)
            throw std::runtime_error("core normalization operand count exceeds bound");
        operands_.resize(sources+destinations);
        for(unsigned i=0;i<sources+destinations;++i)operands_[i].operand_index=i<sources?i:i-sources;
    }
    template<class Reg> void observeSource(unsigned i,const Reg& original,const Reg& flat){observe(i,false,original,flat);}
    template<class Reg> void observeDestination(unsigned i,const Reg& original,const Reg& flat){observe(i,true,original,flat);}
    void completeNormalization() {
        using namespace fastsim::fst;
        if(status_==CoreNormalizationStatus::Conflicting)return;
        for(const auto& op:operands_)if(!(op.flags&kCoreFlatKnown)){status_=CoreNormalizationStatus::Partial;return;}
        status_=CoreNormalizationStatus::Complete;
    }
    auto status() const {return status_;}
    const auto& operands() const {return operands_;}
};

struct CollectedCoreFunctionalFacts {
    fastsim::fst::CoreFunctionalRow row;
    std::vector<fastsim::fst::CoreFunctionalOperand> operands;
};

// Static metadata may be read at retirement; context-dependent normalization
// is accepted only from the immutable snapshot at the original rename point.
template<class Static>
CollectedCoreFunctionalFacts collectCoreFunctionalFacts(
        const Static& si,const Static& root,const CoreFunctionalSnapshot* snapshot,bool pseudo) {
    using namespace fastsim::fst;
    CollectedCoreFunctionalFacts result;auto& row=result.row;
    const auto sources=si.numSrcRegs(),destinations=si.numDestRegs();
    if(sources>kCoreMaxOperands || destinations>kCoreMaxOperands)
        throw std::runtime_error("core functional static operand count exceeds bound");
    row.source_count=sources;row.destination_count=destinations;row.operand_count=sources+destinations;
    row.known_flags=kCoreStaticKnown|kCoreOperandsKnown|kCorePinsKnown;
    auto flag=[&](bool value,std::uint32_t bit){if(value)row.static_flags|=bit;};
    flag(si.isSerializing(),kCoreSerializing);flag(si.isSerializeBefore(),kCoreSerializeBefore);
    flag(si.isSerializeAfter(),kCoreSerializeAfter);flag(si.isReadBarrier(),kCoreReadBarrier);
    flag(si.isWriteBarrier(),kCoreWriteBarrier);flag(si.isNonSpeculative(),kCoreNonSpeculative);
    flag(si.isStoreConditional(),kCoreStoreConditional);flag(si.isSquashAfter(),kCoreSquashAfter);
    flag(si.isMicrocodeControl(),kCoreMicrocodeControl);flag(si.isDelayedCommit(),kCoreDelayedCommit);
    flag(si.isQuiesce(),kCoreQuiesce);flag(si.isMicroop(),kCoreMicroop);
    flag(si.isFirstMicroop(),kCoreFirstMicroop);flag(si.isLastMicroop(),kCoreLastMicroop);
    flag(si.isFullGprMove(),kCoreFullGprMove);flag(si.isFullXmmMove(),kCoreFullXmmMove);
    if(!pseudo) {
        row.known_flags|=kCoreAllocationKnown;
        flag(root.isArchitecturalLockedMemory(),kCoreArchitecturalLocked);
        if(root.isMicroFusedMemory())row.root_allocation_flags|=kCoreMicrofusedMemory;
        if(root.isStackRegAllocation())row.root_allocation_flags|=kCoreStackAllocation;
        if(root.coreFusionFactsKnown()) {
            row.known_flags|=kCoreFusionKnown;
            row.fusion_producer_family=root.coreFusionProducerFamily();
            row.fusion_allowed_jcc_mask=root.coreFusionAllowedJccMask();
            row.jcc_condition=root.coreFusionJccCondition();
        }
    }
    result.operands.reserve(row.operand_count);
    bool conflict=snapshot && snapshot->operands().size()!=row.operand_count;
    for(unsigned i=0;i<row.operand_count;++i) {
        const bool dest=i>=sources;const auto index=dest?i-sources:i;
        const auto original=dest?si.destRegIdx(index):si.srcRegIdx(index);
        CoreFunctionalOperand op;op.operand_index=index;
        op.original_class=static_cast<std::uint8_t>(original.classValue());op.original_index=original.index();
        op.flags=kCoreOriginalKnown|(original.isFlat()?kCoreOriginalIsFlat:0);
        if(dest){op.flags|=kCorePinKnown;op.static_num_pinned_writes=original.getNumPinnedWrites();}
        if(snapshot && !conflict) {
            const auto& frozen=snapshot->operands()[i];
            if(frozen.flags&kCoreOriginalKnown) {
                if(frozen.original_class!=op.original_class || frozen.original_index!=op.original_index ||
                   (frozen.flags&kCoreOriginalIsFlat)!=(op.flags&kCoreOriginalIsFlat) ||
                   frozen.static_num_pinned_writes!=op.static_num_pinned_writes)conflict=true;
                else if(frozen.flags&kCoreFlatKnown) {
                    op.flags|=kCoreFlatKnown;op.flattened_class=frozen.flattened_class;
                    op.flattened_index=frozen.flattened_index;op.normalization_origin=frozen.normalization_origin;
                }
            }
        }
        result.operands.push_back(op);
    }
    if(snapshot) {
        row.normalization_status=conflict?CoreNormalizationStatus::Conflicting:snapshot->status();
        const auto& context=snapshot->context;
        if(context.frontendKnown) {
            row.known_flags|=kCoreFrontendKnown;row.frontend_asid=context.addressSpace;
            row.frontend_from_user=context.fromUser;row.frontend_origin=context.origin;
        }
        if(!pseudo && context.decoderKnown) {
            row.known_flags|=kCoreDecoderKnown;row.decoder_context=context.decoderContext;
            row.decoder_origin=CoreDecoderOrigin::ExactByteWitness;
        }
    }
    if(row.normalization_status==CoreNormalizationStatus::Conflicting) {
        for(auto& op:result.operands) {
            op.flags&=~kCoreFlatKnown;op.flattened_class=255;op.flattened_index=0;
            op.normalization_origin=CoreNormalizationOrigin::Unknown;
        }
    }
    if(row.normalization_status==CoreNormalizationStatus::Complete)row.known_flags|=kCoreNormalizationKnown;
    return result;
}
} // namespace gem5::o3
