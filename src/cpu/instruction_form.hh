/* ISA form identity, independent of a CPU's timing configuration. */
#ifndef __CPU_INSTRUCTION_FORM_HH__
#define __CPU_INSTRUCTION_FORM_HH__
namespace gem5
{
enum class InstructionForm : unsigned {
    None, Add64Reg, Imul64Reg, DivsdReg, Count
};
}
#endif
