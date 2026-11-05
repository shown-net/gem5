// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_INST_CODE_HH__
#define __MEM_INST_CODE_HH__
#include "base/extensible.hh"
#include "mem/request.hh"

namespace gem5
{
/** Native code-validity fact; it is independent of optional trace observers. */
class InstructionCodeValidity : public Extension<Request, InstructionCodeValidity>
{
  public:
    bool stale = false;
    std::unique_ptr<ExtensionBase> clone() const override
    { return std::make_unique<InstructionCodeValidity>(*this); }
};
}
#endif
