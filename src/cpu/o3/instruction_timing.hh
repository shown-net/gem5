#ifndef __CPU_O3_INSTRUCTION_TIMING_HH__
#define __CPU_O3_INSTRUCTION_TIMING_HH__

#include <cmath>
#include "base/logging.hh"
#include "cpu/instruction_form.hh"
#include "params/InstructionTiming.hh"
#include "sim/sim_object.hh"

namespace gem5::o3
{
class InstructionTiming : public SimObject
{
  public:
    const InstructionForm form;
    const Cycles latency;
    const double throughput; // Reference cycles/instruction, not a delay.
    const std::vector<std::string> ports;
    const std::string unit;
    const Cycles issueInterval;

    static InstructionForm parseForm(const std::string &name)
    {
        if (name == "add.r64.r64") return InstructionForm::Add64Reg;
        if (name == "imul.r64.r64") return InstructionForm::Imul64Reg;
        if (name == "divsd.xmm.xmm") return InstructionForm::DivsdReg;
        fatal("Unsupported instruction timing form: %s", name);
    }
    explicit InstructionTiming(const InstructionTimingParams &p)
        : SimObject(p), form(parseForm(p.form)), latency(p.latency),
          throughput(p.throughput), ports(p.ports), unit(p.unit),
          issueInterval(p.issue_interval)
    {
        fatal_if(latency < Cycles(1) || !std::isfinite(throughput) ||
                 throughput <= 0 || ports.empty(),
                 "Instruction timing needs positive latency/throughput and ports");
        fatal_if(unit.empty() != (issueInterval == Cycles(0)),
                 "unit and positive issue_interval must be supplied together");
    }
};
}
#endif
