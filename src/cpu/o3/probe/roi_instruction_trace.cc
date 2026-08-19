#include "cpu/o3/probe/roi_instruction_trace.hh"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <zstd.h>

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/static_inst.hh"

namespace gem5
{
namespace
{
constexpr std::array<uint8_t, 8> Magic{'R','O','I','P','C','T','R',0};
template <class T> void le(std::vector<uint8_t> &o, T v) { for(size_t i=0;i<sizeof(T);++i)o.push_back(static_cast<uint8_t>(v>>(8*i))); }
void bytes(std::vector<uint8_t>&o,const uint8_t*p,size_t n){o.insert(o.end(),p,p+n);}
void write(std::ofstream &out,const std::vector<uint8_t>&v){out.write(reinterpret_cast<const char*>(v.data()),v.size());if(!out)panic("ROI trace write failed");}
std::vector<uint8_t> pack(const std::vector<uint8_t>&raw,int level){ZSTD_CCtx*ctx=ZSTD_createCCtx();if(!ctx)panic("ROI trace zstd context failed");std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));size_t n=ZSTD_CCtx_setParameter(ctx,ZSTD_c_compressionLevel,level);if(!ZSTD_isError(n))n=ZSTD_CCtx_setParameter(ctx,ZSTD_c_checksumFlag,1);if(!ZSTD_isError(n))n=ZSTD_compress2(ctx,out.data(),out.size(),raw.data(),raw.size());ZSTD_freeCCtx(ctx);if(ZSTD_isError(n))panic("ROI trace zstd failed: %s",ZSTD_getErrorName(n));out.resize(n);return out;}
std::array<uint8_t,32> sha(const std::string&s){if(s.size()!=64)panic("ROI trace ELF SHA-256 is invalid");std::array<uint8_t,32>a{};for(size_t i=0;i<a.size();++i){try{a[i]=static_cast<uint8_t>(std::stoul(s.substr(i*2,2),nullptr,16));}catch(...){panic("ROI trace ELF SHA-256 is invalid");}}return a;}
}

RoiInstructionTrace::RoiInstructionTrace(const RoiInstructionTraceParams &p)
    : ProbeListenerObject(p), output(new std::ofstream(simout.resolve(p.output_file),std::ios::binary|std::ios::trunc)), chunkRecords(p.chunk_records), zstdLevel(p.zstd_level), loadBias(p.load_bias), endPc(p.end_pc)
{
    fatal_if(!output->good() || !chunkRecords || zstdLevel < -5 || zstdLevel > 22,
             "cannot initialize ROI binary trace");
    std::vector<uint8_t> header(Magic.begin(),Magic.end()); header.push_back(1); header.push_back(1); le<uint32_t>(header,chunkRecords); const auto identity=sha(p.elf_sha256); bytes(header,identity.data(),identity.size()); write(*output,header);
    pcs.reserve(chunkRecords); flags.reserve(chunkRecords); sizes.reserve(chunkRecords); opcodes.reserve(chunkRecords); disassemblies.reserve(chunkRecords);
}
RoiInstructionTrace::~RoiInstructionTrace(){finalize();}
void RoiInstructionTrace::regProbeListeners(){connectListener<RetireListener>(this,"ArchitecturalRetire",&RoiInstructionTrace::retire);}
void RoiInstructionTrace::startTracing(){fatal_if(finalized,"ROI instruction trace cannot restart after finalize");fatal_if(tracing,"ROI instruction trace is already active");tracing=true;}
void RoiInstructionTrace::stopTracing(){if(!tracing)return;tracing=false;finalize();}
uint64_t RoiInstructionTrace::recordCount() const{return records;}
void RoiInstructionTrace::retire(const std::pair<StaticInstPtr, Addr> &retired){if(!tracing||retired.second==endPc)return;fatal_if(retired.second<loadBias,"ROI trace PC is below load bias");const auto pc=retired.second-loadBias;pcs.push_back(pc);flags.push_back(retired.first->flagsValue());sizes.push_back(retired.first->size());if(describedPcs.insert(pc).second){opcodes.push_back(retired.first->getName());disassemblies.push_back(retired.first->disassemble(retired.second));}else{opcodes.emplace_back();disassemblies.emplace_back();}++records;if(pcs.size()==chunkRecords)flushChunk();}
void RoiInstructionTrace::flushChunk(){if(pcs.empty())return;fatal_if(pcs.size()!=flags.size()||pcs.size()!=sizes.size()||pcs.size()!=opcodes.size()||pcs.size()!=disassemblies.size(),"ROI trace columns lost alignment");struct S{uint8_t id;std::vector<uint8_t> raw,compressed;};std::vector<S>s;auto add=[&](uint8_t id)->S&{s.push_back({id,{},{}});return s.back();};auto&pc=add(1);for(auto v:pcs)le<uint64_t>(pc.raw,v);auto&native=add(2);for(auto v:flags)le<uint64_t>(native.raw,v);auto&size=add(3);for(auto v:sizes){fatal_if(v==0||v>15,"invalid ROI instruction size");size.raw.push_back(v);}uint32_t text_count=0;for(auto const&x:opcodes)if(!x.empty())++text_count;if(text_count){auto&text=add(7);le<uint32_t>(text.raw,text_count);for(uint32_t i=0;i<opcodes.size();++i)if(!opcodes[i].empty()){le<uint32_t>(text.raw,i);le<uint16_t>(text.raw,opcodes[i].size());le<uint16_t>(text.raw,disassemblies[i].size());bytes(text.raw,reinterpret_cast<const uint8_t*>(opcodes[i].data()),opcodes[i].size());bytes(text.raw,reinterpret_cast<const uint8_t*>(disassemblies[i].data()),disassemblies[i].size());}}for(auto&x:s)x.compressed=pack(x.raw,zstdLevel);std::vector<uint8_t>h{'B','L','K',0};le<uint64_t>(h,records-pcs.size());le<uint32_t>(h,pcs.size());h.push_back(s.size());for(auto const&x:s){h.push_back(x.id);le<uint32_t>(h,x.raw.size());le<uint32_t>(h,x.compressed.size());}write(*output,h);for(auto const&x:s)write(*output,x.compressed);++chunks;pcs.clear();flags.clear();sizes.clear();opcodes.clear();disassemblies.clear();}
void RoiInstructionTrace::finalize(){if(finalized||!output)return;fatal_if(tracing,"ROI instruction trace finalized while active");flushChunk();std::vector<uint8_t>footer{'E','N','D',0};le<uint64_t>(footer,records);le<uint64_t>(footer,chunks);write(*output,footer);output->close();delete output;output=nullptr;finalized=true;}
} // namespace gem5
