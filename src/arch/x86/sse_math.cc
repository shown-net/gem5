// SPDX-License-Identifier: BSD-3-Clause
#include "arch/x86/sse_math.hh"

extern "C" {
#include "softfloat.h"
}
#include "arch/x86/faults.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/exec_context.hh"

namespace gem5::X86ISA
{
namespace
{
struct SoftfloatState
{
    const uint_fast8_t rounding = softfloat_roundingMode;
    const uint_fast8_t flags = softfloat_exceptionFlags;
    const uint_fast8_t tininess = softfloat_detectTininess;
    explicit SoftfloatState(unsigned mode)
    {
        static constexpr uint_fast8_t modes[] = {
            softfloat_round_near_even, softfloat_round_min,
            softfloat_round_max, softfloat_round_minMag};
        softfloat_roundingMode = modes[mode];
        softfloat_exceptionFlags = 0;
        softfloat_detectTininess = softfloat_tininess_afterRounding;
    }
    ~SoftfloatState()
    {
        softfloat_roundingMode = rounding;
        softfloat_exceptionFlags = flags;
        softfloat_detectTininess = tininess;
    }
};
uint8_t exceptionBits(uint_fast8_t flags)
{
    return ((flags & softfloat_flag_invalid) ? 1 : 0) |
        ((flags & softfloat_flag_infinite) ? 4 : 0) |
        ((flags & softfloat_flag_overflow) ? 8 : 0) |
        ((flags & softfloat_flag_underflow) ? 16 : 0) |
        ((flags & softfloat_flag_inexact) ? 32 : 0);
}
}

SSEMath::SSEMath(ExecContext *xc)
    : control(xc->readMiscReg(misc_reg::Mxcsr)) {}

uint64_t
SSEMath::element(uint64_t left, uint64_t right, unsigned bytes,
                 SSEOperation operation)
{
    assert(bytes == 4 || bytes == 8);
    const uint64_t sign = bytes == 4 ? 0x80000000ULL : 0x8000000000000000ULL;
    const uint64_t exponent = bytes == 4 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
    const uint64_t fraction = bytes == 4 ? 0x007fffffULL : 0x000fffffffffffffULL;
    const uint64_t quiet = bytes == 4 ? 0x00400000ULL : 0x0008000000000000ULL;
    if (bytes == 4) { left &= 0xffffffff; right &= 0xffffffff; }
    const auto nan = [&](uint64_t value) {
        return (value & exponent) == exponent && (value & fraction);
    };
    const bool unary = operation == SSEOperation::Sqrt;
    const bool left_nan = nan(left), right_nan = !unary && nan(right);
    if (left_nan || right_nan) {
        if ((left_nan && !(left & quiet)) || (right_nan && !(right & quiet)))
            status |= 1;
        // Intel SDM Table4-8: SSE/AVX prefer the first source NaN, including
        // two NaNs; quiet a signaling payload without replacing its bits.
        return (left_nan ? left : right) | quiet;
    }
    uint8_t lane_status = 0;
    auto operand = [&](uint64_t value) {
        if (!(value & exponent) && (value & fraction)) {
            if (control & (1u << 6)) return value & sign;
            lane_status |= 2;
        }
        return value;
    };
    left = operand(left);
    if (!unary) right = operand(right);
    SoftfloatState state((control >> 13) & 3);
    uint64_t result = 0;
    if (bytes == 4) {
        float32_t a{uint32_t(left)}, b{uint32_t(right)}, out{};
        switch (operation) {
          case SSEOperation::Add: out = f32_add(a,b); break;
          case SSEOperation::Subtract: out = f32_sub(a,b); break;
          case SSEOperation::Multiply: out = f32_mul(a,b); break;
          case SSEOperation::Divide: out = f32_div(a,b); break;
          case SSEOperation::Sqrt: out = f32_sqrt(a); break;
        }
        result = out.v;
    } else {
        float64_t a{left}, b{right}, out{};
        switch (operation) {
          case SSEOperation::Add: out = f64_add(a,b); break;
          case SSEOperation::Subtract: out = f64_sub(a,b); break;
          case SSEOperation::Multiply: out = f64_mul(a,b); break;
          case SSEOperation::Divide: out = f64_div(a,b); break;
          case SSEOperation::Sqrt: out = f64_sqrt(a); break;
        }
        result = out.v;
    }
    const uint8_t arithmetic = exceptionBits(softfloat_exceptionFlags);
    // NaNs, invalid arithmetic and divide by zero have priority over a
    // denormal operand within the same element (SDM Vol.1, section 4.9.2).
    if (arithmetic & 5) lane_status = 0;
    lane_status |= arithmetic;
    if ((softfloat_exceptionFlags & softfloat_flag_invalid) && nan(result))
        result = sign | exponent | quiet; // x86 FP indefinite
    // IEEE SoftFloat reports tiny AND inexact. SSE with UM=0 also reports
    // an exact nonzero tiny result. A tiny value rounded to zero is covered
    // by the inexact condition; exact cancellation to zero is excluded.
    if (!(control & (1u << 11)) && !(result & exponent) &&
        ((result & fraction) || (arithmetic & 32)))
        lane_status |= 16;
    // SPR reports OE alone for an unmasked overflow, while an inexact
    // unmasked underflow retains UE|PE. Suppress only this lane's overflow
    // precision; flags from the other packed lanes still participate in OR.
    if ((lane_status & 8) && !(control & (1u << 10)))
        lane_status &= ~32;
    if (!(result & exponent) && (result & fraction) &&
        (control & (1u << 15)) && (control & (1u << 11))) {
        result &= sign;
        lane_status |= 0x30;
    }
    status |= lane_status;
    return result;
}

uint64_t
SSEMath::packedBinary(uint64_t left, uint64_t right, unsigned bytes,
                      SSEOperation operation)
{
    if (bytes == 8) return binary(left,right,bytes,operation);
    const uint64_t low = binary(uint32_t(left),uint32_t(right),4,operation);
    const uint64_t high = binary(left >> 32,right >> 32,4,operation);
    return low | (high << 32);
}

uint64_t
SSEMath::packedSqrt(uint64_t value, unsigned bytes)
{
    if (bytes == 8) return sqrt(value,8);
    const uint64_t low = sqrt(uint32_t(value),4);
    const uint64_t high = sqrt(value >> 32,4);
    return low | (high << 32);
}

Fault
SSEMath::finish(ExecContext *xc) const
{
    const uint8_t masks = (control >> 7) & 0x3f;
    // Packed operations test all elements for pre-computation exceptions
    // before any post-computation flags become architectural (11.5.3).
    const uint8_t reported = (status & 7 & ~masks) ? status & 7 : status;
    if (reported & ~masks) {
        CR4 cr4 = xc->readMiscReg(misc_reg::Cr4);
        return std::make_shared<SSEArithmeticFault>(reported, !cr4.osxmmexcpt);
    }
    xc->orMiscReg(misc_reg::Mxcsr, reported);
    return NoFault;
}
}
