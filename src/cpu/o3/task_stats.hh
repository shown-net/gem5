#ifndef __CPU_O3_TASK_STATS_HH__
#define __CPU_O3_TASK_STATS_HH__
#include <deque>
#include <functional>
#include <memory>
#include "base/statistics.hh"
#include "cpu/pc_event.hh"
namespace gem5 { class ThreadContext;
namespace o3 {
class CPU;
// Event updates use original storage. Only boundaries account TASK intervals.
class TaskStatsObserver
{
    struct ScalarEntry {
        statistics::ScalarInfo *info;
        statistics::Counter previous = 0, total = 0;
        const statistics::Counter *source = nullptr;
        statistics::Counter raw() const
        { return source ? *source : info->rawValue(); }
    };
    struct VectorEntry {
        std::function<const statistics::VCounter &()> raw;
        statistics::VCounter previous, total;
    };
    CPU &cpu;
    bool enabled = false, nextEnabled = false;
    uint32_t target = 0;
    unsigned pidOffset = 0;
    std::unique_ptr<PCEvent> switchEvent;
    std::deque<ScalarEntry> scalars;
    std::deque<VectorEntry> vectors;
    void bind(statistics::Group &group, const char *name,
              const statistics::Counter *source = nullptr);
  public:
    explicit TaskStatsObserver(CPU &owner) : cpu(owner) {}
    void observe(gem5::ThreadContext *tc, Addr switchPc, Addr currentOffset,
                 unsigned pid_offset);
    void switched(gem5::ThreadContext *tc);
    void settle();
    void endCycle();
};
} }
#endif
