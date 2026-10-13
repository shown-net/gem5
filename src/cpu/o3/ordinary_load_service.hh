/* Ordinary-load research service model. Not a recovered SPR bank topology. */
#ifndef __CPU_O3_ORDINARY_LOAD_SERVICE_HH__
#define __CPU_O3_ORDINARY_LOAD_SERVICE_HH__

#include <algorithm>
#include <set>
#include <vector>
#include "base/intmath.hh"
#include "base/types.hh"

namespace gem5::o3
{

class OrdinaryLoadService
{
  public:
    enum class Decision { Accepted, AdmissionRetry, ArbitrationRetry };
    bool enabled = false;
    bool admission = true;
    bool arbitration = true;
    bool propagation = true;

    void geometry(unsigned line_bytes, unsigned sets)
    {
        lineBits = floorLog2(line_bytes);
        setMask = sets - 1;
        occupied.resize(line_bytes / BankBytes, false);
    }

    Decision accept(Addr vaddr, unsigned size, Tick edge)
    {
        if (edge != epoch) {
            epoch = edge;
            accepted = 0;
            std::fill(occupied.begin(), occupied.end(), false);
            recent.clear();
        }
        if (admission && accepted == AdmissionWidth)
            return Decision::AdmissionRetry;
        // xs-gem5 LSQ::loadBankConflictedCheck: probe the entire access before
        // claiming resources; repeated set/bank keys reuse this cycle's read.
        std::vector<std::pair<unsigned, Addr>> touched;
        for (unsigned offset = 0; offset < size;) {
            Addr addr = vaddr + offset;
            unsigned bank = (addr / BankBytes) % occupied.size();
            Addr key = (((addr >> lineBits) & setMask) * occupied.size()) + bank;
            if (arbitration && occupied[bank] && !recent.count(key))
                return Decision::ArbitrationRetry;
            touched.emplace_back(bank, key);
            offset += BankBytes - (addr % BankBytes);
        }
        for (auto [bank, key] : touched) {
            occupied[bank] = true;
            recent.insert(key);
        }
        ++accepted;
        return Decision::Accepted;
    }

  private:
    // Width: Intel SPR maximum scalar-load admission. BankBytes: xs reference
    // default, held fixed as an alien-architecture hypothesis, never fitted.
    static constexpr unsigned AdmissionWidth = 3;
    static constexpr unsigned BankBytes = 2;
    unsigned lineBits = 0;
    Addr setMask = 0;
    Tick epoch = Tick(-1);
    unsigned accepted = 0;
    std::vector<bool> occupied;
    std::set<Addr> recent;
};
}
#endif
