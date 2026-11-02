// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_ZERO_IDIOM_HH__
#define __CPU_O3_ZERO_IDIOM_HH__
#include <map>
#include "base/statistics.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "params/ZeroIdiom.hh"
#include "sim/sim_object.hh"

namespace gem5::o3
{
class ZeroIdiom : public SimObject
{
    std::map<InstSeqNum, ThreadID> operations;
    struct Stats : statistics::Group
    {
        statistics::Scalar accepted, completed, retired, cancelled;
        statistics::Vector kinds;
        Stats(statistics::Group *parent);
    } stats;
    void checkDrain();
  public:
    ZeroIdiom(const ZeroIdiomParams &p);
    void admit(const DynInstPtr &inst);
    void retire(ThreadID tid, InstSeqNum through);
    void cancel(InstSeqNum sequence);
    bool empty() const { return operations.empty(); }
    DrainState drain() override;
};
}
#endif
