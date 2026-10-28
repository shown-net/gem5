// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_INT_EXECUTION_HH__
#define __CPU_O3_INT_EXECUTION_HH__

#include <array>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>

#include "base/statistics.hh"
#include "cpu/int_execution_info.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/reg_class.hh"
#include "params/ExecutionPorts.hh"
#include "params/ExecutionPort.hh"
#include "params/IntExecutionResource.hh"
#include "params/IntegerALU.hh"
#include "params/ResultForwarding.hh"
#include "params/ResultPathRule.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5::o3
{

class CPU;

class ResultPathRule : public SimObject
{
  public:
    const bool anyProducer, anyConsumer;
    const IntExecutionFamily producer, consumer;
    const int producerPort, consumerPort, sourceIndex, delay;
    const IntResultRole result;
    ResultPathRule(const ResultPathRuleParams &p);
    bool matchesProducer(IntExecutionFamily family, unsigned port,
                         IntResultRole role) const;
    bool matchesConsumer(IntExecutionFamily family, unsigned port,
                         RegIndex source) const;
};

class ExecutionPort : public SimObject
{
    Tick acceptedAt;
    struct Stats : statistics::Group
    {
        statistics::Scalar accepted;
        Stats(statistics::Group *parent);
    } stats;

  public:
    const unsigned number;
    ExecutionPort(const ExecutionPortParams &p);
    bool available(Tick now) const { return acceptedAt != now; }
    void accept(Tick now);
};

class ExecutionPorts : public SimObject
{
    std::array<ExecutionPort *, 12> ports{};
    std::unordered_map<unsigned, unsigned> cursors;
    struct Stats : statistics::Group
    {
        statistics::Vector accepted;
        Stats(statistics::Group *parent);
    } stats;

  public:
    ExecutionPorts(const ExecutionPortsParams &p);
    int probe(IntExecutionFamily family, uint16_t candidates, Tick now,
              uint16_t eligible = 0xfff) const;
    void commit(IntExecutionFamily family, uint16_t candidates,
                unsigned port, Tick now);
    ExecutionPort *get(unsigned number) const { return ports.at(number); }
};

class ResultForwarding : public SimObject
{
  public:
    struct Reference
    {
        DynInstPtr producer;
        RegIndex destination = 0;
        IntExecutionFamily family = IntExecutionFamily::None;
        unsigned port = 12;
        IntResultRole role = IntResultRole::Other;
        Tick nominal = 0;
        uint64_t version = 0;
        bool tracked = false;
    };

  private:
    std::unordered_map<RegIndex, Reference> producers;
    struct Waiter { DynInstPtr inst; Tick when; };
    std::unordered_map<uint64_t, Waiter> waiters;
    const std::vector<ResultPathRule *> paths;
    CPU *cpu = nullptr;
    EventFunctionWrapper wakeEvent;
    struct Stats : statistics::Group
    {
        statistics::Scalar deferred, wakeups, cancelled, unissuedCancelled;
        statistics::Vector lateSources;
        Stats(statistics::Group *parent);
    };
    mutable Stats stats;
    void armWake();
    void wakeConsumers();
    void checkDrain();

  public:
    ResultForwarding(const ResultForwardingParams &p);
    void bindCPU(CPU *owner);
    void track(const DynInstPtr &inst, bool managed);
    Reference source(PhysRegIdPtr reg) const;
    bool qualifies(const DynInstPtr &inst, unsigned consumer_port, Tick now) const;
    Tick availableAt(const DynInstPtr &inst, unsigned consumer_port) const;
    int earliestAdjustment(const DynInstPtr &inst, unsigned producer_port) const;
    void defer(const DynInstPtr &inst, Tick when);
    void clearWait(const DynInstPtr &inst);
    void release(const DynInstPtr &inst);
    void cancel(const DynInstPtr &inst);
    void reset();
    bool empty() const;
    DrainState drain() override;
};

class IntExecutionResource : public SimObject
{
    const IntExecutionFamily family;
    const Cycles issueInterval;
    const Cycles completionDelay;
    const Cycles prfDelay;
    std::array<Tick, 12> nextAcceptance{};
    struct Operation
    {
        DynInstPtr inst;
        bool completed = false;
        bool written = false;
        bool sent = false;
        unsigned events = 3;
    };
    std::list<std::shared_ptr<Operation>> operations;
    struct Stats : statistics::Group
    {
        statistics::Scalar admitted, values, completed, written;
        Stats(statistics::Group *parent);
    } stats;
    void finishEvent(const std::shared_ptr<Operation> &operation);

  public:
    IntExecutionResource(const IntExecutionResourceParams &p);
    virtual bool accepts(IntExecutionFamily requested, unsigned port, Tick now) const;
    void admit(CPU *cpu, ResultForwarding *forwarding, const DynInstPtr &inst,
               unsigned port, std::function<void()> available,
               std::function<void()> complete);
    bool empty() const { return operations.empty(); }
    DrainState drain() override;
};

class IntegerALU : public IntExecutionResource
{
    ExecutionPort *const port;

  public:
    IntegerALU(const IntegerALUParams &p);
    ExecutionPort *connection() const { return port; }
    bool accepts(IntExecutionFamily requested, unsigned number, Tick now) const override;
};

} // namespace gem5::o3
#endif
