/* Ordinary-load research service model. Not a recovered SPR bank topology. */
#ifndef __CPU_O3_ORDINARY_LOAD_SERVICE_HH__
#define __CPU_O3_ORDINARY_LOAD_SERVICE_HH__

#include <algorithm>
#include <vector>
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
    unsigned readPorts = 2;

    void geometry(unsigned line_bytes)
    {
        occupied.resize(line_bytes / ReadWindowBytes, 0);
    }

    Decision accept(Addr vaddr, unsigned size, Tick edge)
    {
        if (edge != epoch) {
            epoch = edge;
            accepted = 0;
            std::fill(occupied.begin(), occupied.end(), 0);
        }
        if (admission && accepted == AdmissionWidth)
            return Decision::AdmissionRetry;
        // Probe the entire access before claiming resources. Each real load
        // consumes a read slot, including repeated reads of the same bytes.
        std::vector<unsigned> touched;
        for (unsigned offset = 0; offset < size;) {
            Addr addr = vaddr + offset;
            unsigned window = (addr / ReadWindowBytes) % occupied.size();
            if (arbitration && occupied[window] >= readPorts)
                return Decision::ArbitrationRetry;
            touched.push_back(window);
            offset += ReadWindowBytes - (addr % ReadWindowBytes);
        }
        for (auto window : touched)
            ++occupied[window];
        ++accepted;
        return Decision::Accepted;
    }

  private:
    // Width: Intel SPR maximum scalar-load admission. Eight-byte read-window
    // overlap models the measured layout/alignment response. Its granularity
    // and two-read limit do not establish the physical SPR bank topology.
    static constexpr unsigned AdmissionWidth = 3;
    static constexpr unsigned ReadWindowBytes = 8;
    Tick epoch = Tick(-1);
    unsigned accepted = 0;
    std::vector<unsigned> occupied;
};
}
#endif
