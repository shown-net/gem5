// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/int_execution.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "sim/eventq.hh"

namespace gem5::o3
{

namespace
{
IntExecutionFamily
parseFamily(const std::string &name)
{
    if (name == "Any" || name == "None") return IntExecutionFamily::None;
    if (name == "ALU") return IntExecutionFamily::ALU;
    if (name == "SHFT") return IntExecutionFamily::SHFT;
    if (name == "SlowInt") return IntExecutionFamily::SlowInt;
    if (name == "BM") return IntExecutionFamily::BM;
    fatal("Unknown result-path execution family: %s", name);
}

IntResultRole
parseRole(const std::string &name)
{
    if (name == "Any") return IntResultRole::Any;
    if (name == "GPR") return IntResultRole::GPR;
    if (name == "CF") return IntResultRole::CF;
    if (name == "OF") return IntResultRole::OF;
    if (name == "Flags") return IntResultRole::Flags;
    if (name == "Other") return IntResultRole::Other;
    fatal("Unknown result-path result role: %s", name);
}
}

ResultPathRule::ResultPathRule(const ResultPathRuleParams &p)
    : SimObject(p), anyProducer(p.producerFamily == "Any"),
      anyConsumer(p.consumerFamily == "Any"), producer(parseFamily(p.producerFamily)),
      consumer(parseFamily(p.consumerFamily)), producerPort(p.producerPort),
      consumerPort(p.consumerPort), sourceIndex(p.sourceIndex), delay(p.delay),
      result(parseRole(p.resultKind))
{
    fatal_if(producerPort < -1 || producerPort >= 12 ||
             consumerPort < -1 || consumerPort >= 12 || sourceIndex < -1,
             "Invalid result-path port or consumer operand index");
}

bool
ResultPathRule::matchesProducer(IntExecutionFamily family, unsigned port,
                                IntResultRole role) const
{
    return (anyProducer || producer == family) &&
        (producerPort < 0 || unsigned(producerPort) == port) &&
        (result == IntResultRole::Any || result == role);
}

bool
ResultPathRule::matchesConsumer(IntExecutionFamily family, unsigned port,
                                RegIndex source) const
{
    return (anyConsumer || consumer == family) &&
        (consumerPort < 0 || unsigned(consumerPort) == port) &&
        (sourceIndex < 0 || unsigned(sourceIndex) == source);
}

ExecutionPort::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(accepted, statistics::units::Count::get(), "Accepted operations")
{}

ExecutionPort::ExecutionPort(const ExecutionPortParams &p)
    : SimObject(p), acceptedAt(std::numeric_limits<Tick>::max()),
      stats(this), number(p.number)
{
    fatal_if(number >= 12, "Invalid execution port number");
}

void
ExecutionPort::accept(Tick now)
{
    panic_if(!available(now), "Execution port accepted two operations in one cycle");
    acceptedAt = now;
    stats.accepted++;
}

ExecutionPorts::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(accepted, statistics::units::Count::get(), "Accepted work by port")
{
    accepted.init(12);
    for (unsigned i = 0; i < 12; ++i)
        accepted.subname(i, "p" + std::to_string(i));
}

ExecutionPorts::ExecutionPorts(const ExecutionPortsParams &p)
    : SimObject(p), stats(this)
{
    for (auto port : p.ports) {
        fatal_if(ports[port->number], "Duplicate execution port number");
        ports[port->number] = port;
    }
}

int
ExecutionPorts::probe(IntExecutionFamily family, uint16_t candidates, Tick now,
                      uint16_t eligible) const
{
    const unsigned key = (unsigned(family) << 12) | candidates;
    auto cursor = cursors.find(key);
    const unsigned start = cursor == cursors.end() ? 0 : cursor->second;
    for (unsigned i = 0; i < 12; ++i) {
        const unsigned port = (start + i) % 12;
        if ((candidates & eligible & (1 << port)) && ports[port] &&
            ports[port]->available(now))
            return port;
    }
    return -1;
}

void
ExecutionPorts::commit(IntExecutionFamily family, uint16_t candidates,
                       unsigned port, Tick now)
{
    panic_if(port >= 12 || !(candidates & (1 << port)) || !ports[port] ||
             !ports[port]->available(now),
             "Invalid or conflicting execution-port grant");
    ports[port]->accept(now);
    cursors[(unsigned(family) << 12) | candidates] = (port + 1) % 12;
    stats.accepted[port]++;
}

ResultForwarding::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(deferred, statistics::units::Count::get(), "Consumers deferred by result paths"),
      ADD_STAT(wakeups, statistics::units::Count::get(), "Result-path wake events"),
      ADD_STAT(cancelled, statistics::units::Count::get(), "Cancelled result producers"),
      ADD_STAT(unissuedCancelled, statistics::units::Count::get(), "Cancelled unissued result producers"),
      ADD_STAT(lateSources, statistics::units::Count::get(), "Source bindings after producer PRF writeback")
{
    lateSources.init(5);
    const char *names[] = {"None", "ALU", "SHFT", "SlowInt", "BM"};
    for (unsigned i = 0; i < 5; ++i) lateSources.subname(i, names[i]);
}

ResultForwarding::ResultForwarding(const ResultForwardingParams &p)
    : SimObject(p), paths(p.paths),
      wakeEvent([this] { wakeConsumers(); }, name() + ".path_ready"), stats(this)
{}

void
ResultForwarding::bindCPU(CPU *owner)
{
    panic_if(cpu && cpu != owner, "Result forwarding shared between different cores");
    cpu = owner;
}

void
ResultForwarding::track(const DynInstPtr &inst, bool managed)
{
    for (RegIndex i = 0; i < inst->numDestRegs(); ++i) {
        auto reg = inst->renamedDestIdx(i);
        if (reg->isAlwaysReady())
            continue;
        auto found = producers.find(reg->flatIndex());
        panic_if(found != producers.end() && found->second.producer &&
                 found->second.producer != inst && !found->second.producer->isSquashed(),
                 "Result version reused before PRF writeback");
        if (!managed) {
            producers.erase(reg->flatIndex());
            continue;
        }
        Reference reference;
        reference.producer = inst;
        reference.destination = i;
        reference.family = inst->staticInst->intExecution().family;
        reference.role = inst->staticInst->intResultRole(i);
        reference.version = inst->seqNum;
        reference.tracked = true;
        producers[reg->flatIndex()] = std::move(reference);
    }
}

ResultForwarding::Reference
ResultForwarding::source(PhysRegIdPtr reg) const
{
    if (reg->isAlwaysReady())
        return {};
    auto found = producers.find(reg->flatIndex());
    if (found == producers.end()) return {};
    if (!found->second.producer)
        stats.lateSources[unsigned(found->second.family)]++;
    return found->second;
}

bool
ResultForwarding::qualifies(const DynInstPtr &inst, unsigned consumer_port, Tick now) const
{
    return availableAt(inst, consumer_port) <= now;
}

Tick
ResultForwarding::availableAt(const DynInstPtr &inst, unsigned consumer_port) const
{
    Tick ready = 0;
    RegIndex source = 0;
    for (const auto &ref : inst->sourceResults()) {
        if (!ref.tracked) { ++source; continue; }
        auto family = ref.family;
        auto port = ref.port;
        auto role = ref.role;
        Tick nominal = ref.nominal;
        if (ref.producer) {
            if (ref.producer->isSquashed() || ref.producer->getFault() != NoFault ||
                !ref.producer->hasResultValue(ref.destination))
                return std::numeric_limits<Tick>::max();
            family = ref.producer->staticInst->intExecution().family;
            port = ref.producer->resultProducerPort();
            nominal = ref.producer->resultNominalAt();
        }
        int adjustment = 0;
        bool matched = false;
        for (const auto path : paths) {
            if (!path->matchesProducer(family, port, role) ||
                !path->matchesConsumer(inst->staticInst->intExecution().family,
                                       consumer_port, source))
                continue;
            panic_if(matched, "Overlapping result-path rules for one source and port");
            matched = true;
            adjustment = path->delay;
        }
        const int64_t signed_delta = adjustment;
        panic_if(adjustment && !cpu, "Unbound result-forwarding clock");
        const Tick delta = adjustment == 0 ? 0 :
            Tick(signed_delta < 0 ? -signed_delta : signed_delta) * cpu->clockPeriod();
        panic_if(signed_delta < 0 && nominal < delta, "Result path precedes simulation start");
        const Tick at = signed_delta < 0 ? nominal - delta : nominal + delta;
        panic_if(ref.producer && at < ref.producer->resultAvailableAt(),
                 "Result path precedes production of its real value");
        ready = std::max(ready, at);
        ++source;
    }
    return ready;
}

int
ResultForwarding::earliestAdjustment(const DynInstPtr &inst, unsigned producer_port) const
{
    int earliest = 0;
    for (RegIndex i = 0; i < inst->numDestRegs(); ++i)
        for (const auto path : paths)
            if (path->matchesProducer(inst->staticInst->intExecution().family,
                                       producer_port, inst->staticInst->intResultRole(i)))
                earliest = std::min(earliest, path->delay);
    return earliest;
}

void
ResultForwarding::armWake()
{
    Tick when = std::numeric_limits<Tick>::max();
    for (auto it = waiters.begin(); it != waiters.end();) {
        if (it->second.inst->isSquashed() || it->second.inst->isIssued())
            it = waiters.erase(it);
        else { when = std::min(when, it->second.when); ++it; }
    }
    if (when == std::numeric_limits<Tick>::max()) {
        if (wakeEvent.scheduled()) deschedule(wakeEvent);
    } else {
        panic_if(when <= curTick(), "Result-path wake must be in the future");
        if (wakeEvent.scheduled()) {
            if (wakeEvent.when() != when) reschedule(wakeEvent, when);
        } else schedule(wakeEvent, when);
    }
}

void
ResultForwarding::defer(const DynInstPtr &inst, Tick when)
{
    panic_if(!cpu || when <= curTick() || when == std::numeric_limits<Tick>::max(),
             "Invalid deferred result consumer");
    auto [it, inserted] = waiters.emplace(inst->seqNum, Waiter{inst, when});
    if (inserted) stats.deferred++;
    else it->second.when = when;
    armWake();
}

void
ResultForwarding::clearWait(const DynInstPtr &inst)
{
    waiters.erase(inst->seqNum);
    armWake();
    checkDrain();
}

void
ResultForwarding::wakeConsumers()
{
    for (auto it = waiters.begin(); it != waiters.end();) {
        if (it->second.when <= curTick() || it->second.inst->isSquashed())
            it = waiters.erase(it);
        else ++it;
    }
    stats.wakeups++;
    cpu->wakeCPU();
    armWake();
    checkDrain();
}

void
ResultForwarding::release(const DynInstPtr &inst)
{
    for (RegIndex i = 0; i < inst->numDestRegs(); ++i) {
        auto reg = inst->renamedDestIdx(i);
        if (reg->isAlwaysReady())
            continue;
        auto found = producers.find(reg->flatIndex());
        if (found == producers.end() || found->second.version != inst->seqNum)
            continue;
        if (inst->isSquashed() || inst->getFault() != NoFault) {
            producers.erase(found);
        } else {
            auto &reference = found->second;
            reference.port = inst->resultProducerPort();
            reference.nominal = inst->resultNominalAt();
            reference.producer = nullptr;
        }
    }
    checkDrain();
}

void
ResultForwarding::cancel(const DynInstPtr &inst)
{
    bool removed = false;
    for (RegIndex i = 0; i < inst->numDestRegs(); ++i) {
        auto found = producers.find(inst->renamedDestIdx(i)->flatIndex());
        if (found != producers.end() && found->second.version == inst->seqNum) {
            producers.erase(found);
            removed = true;
        }
    }
    if (removed) {
        stats.cancelled++;
        if (!inst->isIssued()) stats.unissuedCancelled++;
    }
    clearWait(inst);
}

bool
ResultForwarding::empty() const
{
    if (!waiters.empty() || wakeEvent.scheduled()) return false;
    for (const auto &[index, reference] : producers)
        if (reference.producer) return false;
    return true;
}

void
ResultForwarding::checkDrain()
{
    if (empty() && drainState() == DrainState::Draining) signalDrainDone();
}

DrainState
ResultForwarding::drain()
{
    return empty() ? DrainState::Drained : DrainState::Draining;
}

void
ResultForwarding::reset()
{
    panic_if(!empty(), "Result forwarding reset before pending transfers drained");
    producers.clear();
}

IntExecutionResource::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(admitted, statistics::units::Count::get(), "Accepted operations"),
      ADD_STAT(values, statistics::units::Count::get(), "ISA result formation events"),
      ADD_STAT(completed, statistics::units::Count::get(), "Operation completion events"),
      ADD_STAT(written, statistics::units::Count::get(), "PRF writeback events")
{}

IntExecutionResource::IntExecutionResource(const IntExecutionResourceParams &p)
    : SimObject(p), family(IntExecutionFamily(p.family)),
      issueInterval(p.issueInterval), completionDelay(p.completionDelay),
      prfDelay(p.prfDelay), stats(this)
{
    fatal_if(p.family < 1 || p.family > 4 || issueInterval < Cycles(1),
             "Invalid integer execution family or issue interval");
}

bool
IntExecutionResource::accepts(IntExecutionFamily requested, unsigned port, Tick now) const
{
    return requested == family && port < 12 && nextAcceptance[port] <= now;
}

void
IntExecutionResource::finishEvent(const std::shared_ptr<Operation> &operation)
{
    if (--operation->events == 0) {
        operations.remove(operation);
        if (operations.empty() && drainState() == DrainState::Draining)
            signalDrainDone();
    }
}

void
IntExecutionResource::admit(CPU *cpu, ResultForwarding *forwarding,
                            const DynInstPtr &inst, unsigned port,
                            std::function<void()> available,
                            std::function<void()> complete)
{
    panic_if(!accepts(inst->staticInst->intExecution().family, port, curTick()),
             "Integer execution service accepted an invalid grant");
    nextAcceptance[port] = cpu->clockEdge(issueInterval);
    const Cycles latency(inst->staticInst->intExecution().resultLatency);
    const int64_t evaluation = int64_t(uint64_t(latency)) +
        forwarding->earliestAdjustment(inst, port);
    panic_if(evaluation < 1, "Result path requires a value before a valid execution cycle");
    inst->prepareResultValues(port, cpu->clockEdge(latency));
    auto operation = std::make_shared<Operation>();
    operation->inst = inst;
    operations.push_back(operation);
    stats.admitted++;
    const Tick value_at = cpu->clockEdge(Cycles(evaluation));
    auto send = [operation, complete] {
        if (operation->completed && operation->written && !operation->sent) {
            operation->sent = true;
            complete();
        }
    };
    cpu->schedule(new EventFunctionWrapper([this, cpu, operation, available, value_at] {
        auto inst = operation->inst;
        if (!inst->isSquashed() && inst->getFault() == NoFault) {
            inst->evaluateResultValues(value_at);
            stats.values++;
            if (inst->getFault() == NoFault) {
                available();
                inst->setResultsWoken();
                cpu->wakeCPU();
            }
        }
        finishEvent(operation);
    }, name() + ".value", true, Event::Default_Pri), value_at);
    cpu->schedule(new EventFunctionWrapper([this, operation, send] {
        operation->completed = true;
        stats.completed++;
        send();
        finishEvent(operation);
    }, name() + ".complete", true, Event::Priority(1)),
        cpu->clockEdge(latency + completionDelay));
    cpu->schedule(new EventFunctionWrapper([this, operation, forwarding, send] {
        auto inst = operation->inst;
        if (!inst->isSquashed() && inst->getFault() == NoFault)
            inst->writeResultValues();
        forwarding->release(inst);
        operation->written = true;
        stats.written++;
        send();
        finishEvent(operation);
    }, name() + ".prf", true, Event::Priority(2)),
        cpu->clockEdge(latency + prfDelay));
}

DrainState
IntExecutionResource::drain()
{
    return empty() ? DrainState::Drained : DrainState::Draining;
}

IntegerALU::IntegerALU(const IntegerALUParams &p)
    : IntExecutionResource(p), port(p.port)
{
    fatal_if(p.family != unsigned(IntExecutionFamily::ALU),
             "IntegerALU must serve the ALU family");
}

bool
IntegerALU::accepts(IntExecutionFamily requested, unsigned number, Tick now) const
{
    return number == port->number &&
        IntExecutionResource::accepts(requested, number, now);
}

} // namespace gem5::o3
