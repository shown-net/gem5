/*
 * Copyright (c) 2007-2008 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "arch/x86/linux/fs_workload.hh"

#include <fstream>
#include <vector>

#include "arch/x86/regs/int.hh"
#include "base/trace.hh"
#include "cpu/pc_event.hh"
#include "cpu/thread_context.hh"
#include "kern/linux/events.hh"
#include "mem/port_proxy.hh"
#include "params/X86FsLinux.hh"
#include "sim/byteswap.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

namespace X86ISA
{

FsLinux::FsLinux(const Params &p) :
    X86ISA::FsWorkload(p), e820Table(p.e820_table)
{}

void
FsLinux::startup()
{
    KernelWorkload::startup();

    addExitOnKernelOopsEvent();
    addExitOnKernelPanicEvent();
}


void
FsLinux::addExitOnKernelPanicEvent()
{
    const std::string dmesg_output = name() + ".dmesg";

    if (params().exit_on_kernel_panic) {
        // This was taken from the RISCV implementation. Some kernels may not
        // have kernel symbols, causing `kernelSymtab` to be empty.
        // In that case, addKernelFuncEvent tries to access the "panic" symbol
        // in the symbol table but can't, so the event won't be added and the
        // simulation will hang upon kernel panic.
        kernelPanicPcEvent = addKernelFuncEvent<linux::PanicOrOopsEvent>(
            "panic", "Kernel panic in simulated system.",
            dmesg_output, params().on_panic
        );
    }
}

void
FsLinux::addExitOnKernelOopsEvent()
{
    const std::string dmesg_output = name() + ".dmesg";
    if (params().exit_on_kernel_oops) {
        // This was taken from the RISCV implementation. Some kernels may not
        // have kernel symbols, causing `kernelSymtab` to be empty.
        // In that case, addKernelFuncEvent tries to access the "panic" symbol
        // in the symbol table but can't, so the event won't be added and the
        // simulation will continue upon kernel oops.
        kernelOopsPcEvent = addKernelFuncEvent<linux::PanicOrOopsEvent>(
            "oops_exit", "Kernel oops in simulated system.",
            dmesg_output, params().on_oops
        );
    }
}

void
FsLinux::initState()
{
    X86ISA::FsWorkload::initState();

    auto phys_proxy = system->physProxy;

    // The location of the real mode data structure.
    const Addr realModeData = 0x90200;

    /*
     * Deal with the command line stuff.
     */

    // A buffer to store the command line.
    const Addr commandLineBuff = 0x90000;
    // A pointer to the commandLineBuff stored in the real mode data.
    const Addr commandLinePointer = realModeData + 0x228;

    panic_if(commandLine.length() + 1 > realModeData - commandLineBuff,
             "Command line \"%s\" is longer than %d characters.",
                commandLine, realModeData - commandLineBuff - 1);
    phys_proxy.writeString(commandLineBuff, commandLine.c_str());

    // Generate a pointer of the right size and endianness to put into
    // commandLinePointer.
    uint32_t guestCommandLineBuff = htole((uint32_t)commandLineBuff);
    phys_proxy.writeBlob(commandLinePointer, &guestCommandLineBuff,
                         sizeof(guestCommandLineBuff));

    if (!params().initrd.empty()) {
        std::ifstream input(params().initrd, std::ios::binary | std::ios::ate);
        fatal_if(!input, "Cannot open initrd %s", params().initrd);
        const auto file_size = input.tellg();
        const Addr addr = params().initrd_addr;
        // The 32-bit boot protocol uses inclusive 0x37ffffff as the legacy
        // maximum, which also fits within the low RAM of the x86 board.
        fatal_if(file_size <= 0 || file_size > 0x38000000 ||
                 addr > 0x37ffffff ||
                 uint64_t(file_size) > 0x38000000 - addr,
                 "Initrd does not fit in the Linux x86 boot protocol range");
        const Addr size = uint64_t(file_size);
        fatal_if(!system->isMemAddr(addr) || !system->isMemAddr(addr + size - 1),
                 "Initrd is outside guest RAM");
        fatal_if(addr <= image.maxAddr() && addr + size > image.minAddr(),
                 "Initrd overlaps the loaded kernel image");
        std::vector<char> contents(size);
        input.seekg(0);
        input.read(contents.data(), size);
        fatal_if(!input, "Cannot read initrd %s", params().initrd);
        phys_proxy.writeBlob(addr, contents.data(), size);

        // Linux boot protocol offsets are relative to boot_params, not the
        // setup header. An unassigned loader identifies itself as 0xff.
        const uint8_t loader = 0xff;
        const uint32_t guestAddr = htole(static_cast<uint32_t>(addr));
        const uint32_t guestSize = htole(static_cast<uint32_t>(size));
        phys_proxy.writeBlob(realModeData + 0x210, &loader, sizeof(loader));
        phys_proxy.writeBlob(realModeData + 0x218, &guestAddr, sizeof(guestAddr));
        phys_proxy.writeBlob(realModeData + 0x21c, &guestSize, sizeof(guestSize));
    }

    /*
     * Screen Info.
     */

    // We'll skip on this for now because it's only needed for framebuffers,
    // something we don't support at the moment.

    /*
     * EDID info
     */

    // Skipping for now.

    /*
     * Saved video mode
     */

    // Skipping for now.

    /*
     * Loader type.
     */

    // Skipping for now.

    /*
     * E820 memory map
     */

    // A pointer to the number of E820 entries there are.
    const Addr e820MapNrPointer = realModeData + 0x1e8;

    // A pointer to the buffer for E820 entries.
    const Addr e820MapPointer = realModeData + 0x2d0;

    e820Table->writeTo(phys_proxy, e820MapNrPointer, e820MapPointer);

    /*
     * Pass the location of the real mode data structure to the kernel
     * using register %esi. We'll use %rsi which should be equivalent.
     */
    system->threads[0]->setReg(int_reg::Rsi, realModeData);
}

} // namespace X86ISA
} // namespace gem5
