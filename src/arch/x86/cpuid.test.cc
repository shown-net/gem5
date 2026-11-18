/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "arch/x86/cpuid.hh"

namespace gem5
{
namespace X86ISA
{
namespace
{

void
expectZero(const CpuidResult &result)
{
    EXPECT_EQ(result.rax, 0);
    EXPECT_EQ(result.rbx, 0);
    EXPECT_EQ(result.rcx, 0);
    EXPECT_EQ(result.rdx, 0);
}

TEST(X86CpuidTest, DefaultLeaf7DoesNotRepeatSubleafZero)
{
    X86CPUID cpuid("GenuineIntel", "test");
    cpuid.addStandardFunc(ExtendedFeatures, {0, 0x01880601, 0, 0});
    CpuidResult result;
    ASSERT_TRUE(cpuid.hasSignificantIndex(ExtendedFeatures));
    ASSERT_TRUE(cpuid.doCpuid(nullptr, ExtendedFeatures, 0, result));
    EXPECT_EQ(result.rbx, 0x01880601);
    ASSERT_TRUE(cpuid.doCpuid(nullptr, ExtendedFeatures, 1, result));
    expectZero(result);
    ASSERT_TRUE(cpuid.doCpuid(nullptr, ExtendedFeatures,
                             std::numeric_limits<uint32_t>::max(), result));
    expectZero(result);
}

TEST(X86CpuidTest, Leaf7ReturnsConfiguredSubleafOne)
{
    X86CPUID cpuid("GenuineIntel", "test");
    // Vector register order is EAX, EBX, EDX, ECX.
    cpuid.addStandardFunc(ExtendedFeatures,
                          {1, 0x200, 0, 0, 0x1c00, 2, 3, 4});
    CpuidResult result;
    ASSERT_TRUE(cpuid.doCpuid(nullptr, ExtendedFeatures, 1, result));
    EXPECT_EQ(result.rax, 0x1c00);
    EXPECT_EQ(result.rbx, 2);
    EXPECT_EQ(result.rdx, 3);
    EXPECT_EQ(result.rcx, 4);
    ASSERT_TRUE(cpuid.doCpuid(nullptr, ExtendedFeatures, 2, result));
    expectZero(result);
}

TEST(X86CpuidTest, EmptyIntermediateLeaf7DoesNotHideLaterSubleaf)
{
    X86CPUID cpuid("GenuineIntel", "test");
    cpuid.addStandardFunc(ExtendedFeatures,
                          {2, 0x200, 0, 0, 0, 0, 0, 0, 0, 0, 0x17, 0});
    CpuidResult result;
    ASSERT_TRUE(cpuid.doCpuid(nullptr, ExtendedFeatures, 1, result));
    expectZero(result);
    ASSERT_TRUE(cpuid.doCpuid(nullptr, ExtendedFeatures, 2, result));
    EXPECT_EQ(result.rdx, 0x17);
}

TEST(X86CpuidTest, RejectsIncompleteLeaf7Configuration)
{
    X86CPUID cpuid("GenuineIntel", "test");
    ASSERT_DEATH(cpuid.addStandardFunc(ExtendedFeatures, {}), "leaf 7");
    ASSERT_DEATH(cpuid.addStandardFunc(ExtendedFeatures,
                 (std::vector<uint32_t>{1, 0, 0, 0})), "leaf 7");
    ASSERT_DEATH(cpuid.addStandardFunc(ExtendedFeatures,
                 (std::vector<uint32_t>{0, 0, 0, 0, 0})), "leaf 7");
}

} // anonymous namespace
} // namespace X86ISA
} // namespace gem5
