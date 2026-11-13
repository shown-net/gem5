/* SPR calibration proxy for selecting a logical LLC bank and set. */
#ifndef __MEM_RUBY_COMMON_SPR_BANK_HASH_HH__
#define __MEM_RUBY_COMMON_SPR_BANK_HASH_HH__

#include "base/types.hh"

namespace gem5
{
namespace ruby
{

// This is an engineering approximation, not the hardware's disclosed hash.
// The mapping must be used for both bank selection and indexing inside that
// bank; requests, coherence tags, and writebacks retain their original address.
inline Addr
sprL3IndexLine(Addr line, unsigned banks)
{
    return banks == 52 ? line ^ (line >> 11) ^ (line >> 23) : line;
}

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_COMMON_SPR_BANK_HASH_HH__
