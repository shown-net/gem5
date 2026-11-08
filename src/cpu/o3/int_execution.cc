// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/int_execution.hh"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/fu_pool.hh"
#include "sim/eventq.hh"

namespace gem5::o3
{

namespace
{
using VectorDomain = ResultForwarding::VectorDomain;

VectorDomain
vectorDomain(const DynInstPtr &inst, unsigned port, Cycles latency)
{
    const auto op = inst->opClass();
    if ((op == FloatAddOp || op == SimdFloatAddOp) &&
        (port == 1 || port == 5) && latency == Cycles(3))
        return VectorDomain::FastAdder;
    if ((op == FloatMultAccOp || op == SimdFloatMultAccOp) &&
        port <= 1 && latency == Cycles(4))
        return VectorDomain::Fma;
    if ((op == FloatMultOp || op == SimdFloatMultOp) &&
        port <= 1 && latency == Cycles(4))
        return VectorDomain::Mul;
    if ((op == SimdPackOp || op == SimdInsertOp) &&
        (port == 1 || port == 5) &&
        (latency == Cycles(1) || latency == Cycles(3)))
        return VectorDomain::Shuffle;
    bool integer_source = false, vector_source = false;
    for (RegIndex i = 0; i < inst->numSrcRegs(); ++i) {
        integer_source |= inst->srcRegIdx(i).is(IntRegClass);
        vector_source |= inst->srcRegIdx(i).is(FloatRegClass) ||
                         inst->srcRegIdx(i).is(VecRegClass);
    }
    if (port == 0 && latency == Cycles(3) && vector_source &&
        inst->numDestRegs(IntRegClass))
        return VectorDomain::V2I;
    if (port == 5 && latency == Cycles(1) && integer_source &&
        (inst->numDestRegs(FloatRegClass) || inst->numDestRegs(VecRegClass)))
        return VectorDomain::I2V;
    const bool simd = op == SimdAddOp || op == SimdAddAccOp ||
        op == SimdAluOp || op == SimdCmpOp || op == SimdCvtOp ||
        op == SimdMiscOp || op == SimdMultOp || op == SimdMultAccOp ||
        op == SimdShiftOp || op == SimdShiftAccOp ||
        op == SimdFloatAluOp || op == SimdFloatCmpOp ||
        op == SimdFloatCvtOp || op == SimdFloatMiscOp;
    if (simd && port <= 1 && latency == Cycles(1))
        return VectorDomain::Simd01;
    if (simd && port == 5 &&
        (latency == Cycles(1) || latency == Cycles(3)))
        return VectorDomain::Simd5;
    return VectorDomain::None;
}

int
vectorBypass(VectorDomain producer, VectorDomain consumer)
{
    // Intel 248966-051 Table 2-5, printed p.2-14. The Fast Adder row's
    // port label differs from the port table; the selected 1/5 execution
    // mapping determines that unit. Unidentified lowering forms use their
    // ordinary result latency, never an inferred table match.
    static constexpr int delay[8][7] = {
        {0, 1, 1, 1, 0, 0, 0}, // SIMD ports 0/1, latency 1
        {1, 0, 1, 0, 0, 0, 0}, // FMA
        {1, 0, 1, 0, 0, 0, 0}, // MUL
        {1, 0, 1,-1, 0, 0, 0}, // Fast Adder
        {0, 1, 1, 1, 0, 0, 0}, // SIMD port 5
        {0, 0, 1, 0, 0, 0, 0}, // Shuffle
        {0, 0, 1, 0, 0, 0, 0}, // Vector to integer
        {0, 1, 1, 0, 0, 0, 0}, // Integer to vector
    };
    if (producer == VectorDomain::None || consumer == VectorDomain::None ||
        consumer == VectorDomain::I2V)
        return 0;
    return delay[unsigned(producer) - 1][unsigned(consumer) - 1];
}

IntExecutionFamily
parseFamily(const std::string &name)
{
    if (name == "Any" || name == "None") return IntExecutionFamily::None;
    if (name == "ALU") return IntExecutionFamily::ALU;
    if (name == "SHFT") return IntExecutionFamily::SHFT;
    if (name == "SlowInt") return IntExecutionFamily::SlowInt;
    if (name == "BM") return IntExecutionFamily::BM;
    if (name == "Branch") return IntExecutionFamily::Branch;
    if (name == "Divider") return IntExecutionFamily::Divider;
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
      ADD_STAT(accepted, statistics::units::Count::get(), "Accepted work by port"),
      ADD_STAT(acceptedByFamily, statistics::units::Count::get(),
               "Shared grants by execution family and port")
{
    accepted.init(12);
    acceptedByFamily.init(7, 12);
    const char *families[] = {"none", "alu", "shft", "slow", "bm", "branch", "divider"};
    for (unsigned i = 0; i < 7; ++i) acceptedByFamily.subname(i, families[i]);
    for (unsigned i = 0; i < 12; ++i)
    {
        accepted.subname(i, "p" + std::to_string(i));
        acceptedByFamily.ysubname(i, "p" + std::to_string(i));
    }
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
    stats.acceptedByFamily[unsigned(family)][port]++;
}

ResultForwarding::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(deferred, statistics::units::Count::get(), "Consumers deferred by result paths"),
      ADD_STAT(wakeups, statistics::units::Count::get(), "Result-path wake events"),
      ADD_STAT(cancelled, statistics::units::Count::get(), "Cancelled result producers"),
      ADD_STAT(unissuedCancelled, statistics::units::Count::get(), "Cancelled unissued result producers"),
      ADD_STAT(lateSources, statistics::units::Count::get(), "Source bindings after producer PRF writeback")
{
    lateSources.init(7);
    const char *names[] = {"None", "ALU", "SHFT", "SlowInt", "BM", "Branch", "Divider"};
    for (unsigned i = 0; i < 7; ++i) lateSources.subname(i, names[i]);
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
        const bool vector = !managed && !inst->bypassesIQ() &&
            (inst->isFloating() || inst->isVector() ||
             inst->opClass() == FloatAddOp || inst->opClass() == SimdFloatAddOp);
        if (!managed && !vector) {
            producers.erase(reg->flatIndex());
            continue;
        }
        Reference reference;
        if (vector)
            reference.vectorResult = std::make_shared<VectorResultState>();
        else
            reference.producer = inst;
        reference.destination = i;
        reference.family = inst->staticInst->intExecution().family;
        reference.role = inst->staticInst->intResultRole(i);
        reference.version = inst->seqNum;
        reference.tracked = true;
        producers[reg->flatIndex()] = std::move(reference);
    }
}

void
ResultForwarding::dispatched(const DynInstPtr &inst, unsigned port,
                             Cycles latency)
{
    for (RegIndex i = 0; i < inst->numDestRegs(); ++i) {
        auto found = producers.find(inst->renamedDestIdx(i)->flatIndex());
        if (found == producers.end() || found->second.version != inst->seqNum ||
            !found->second.vectorResult)
            continue;
        auto &result = *found->second.vectorResult;
        result.domain = vectorDomain(inst, port, latency);
        result.nominalAt = cpu->clockEdge(latency);
    }
}

ResultForwarding::Reference
ResultForwarding::source(PhysRegIdPtr reg) const
{
    if (reg->isAlwaysReady())
        return {};
    auto found = producers.find(reg->flatIndex());
    if (found == producers.end()) return {};
    if (!found->second.producer && !found->second.vectorResult)
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
        if (ref.vectorResult) {
            if (!ref.vectorResult->ready || ref.vectorResult->cancelled)
                return std::numeric_limits<Tick>::max();
            const Cycles latency = inst->iq ?
                inst->iq->fuPool()->getOpLatency(inst->opClass()) : Cycles(0);
            const auto consumer = vectorDomain(inst, consumer_port, latency);
            int adjustment = vectorBypass(ref.vectorResult->domain, consumer);
            // These legacy SSE forms use the instruction package's complete
            // 4-cycle MUL latency. Native destination/second-source MUL
            // chains and non-identity ADD/MUL chains confirm no additional
            // cycle on these paths. Keep other Table 2-5 cells unchanged.
            const auto &name = inst->staticInst->getName();
            if (consumer == VectorDomain::Mul &&
                (ref.vectorResult->domain == VectorDomain::Mul ||
                 ref.vectorResult->domain == VectorDomain::FastAdder) &&
                (name == "mxmmmulf" || name == "mxmmmuls"))
                adjustment = 0;
            const Tick delta = std::abs(adjustment) * cpu->clockPeriod();
            const Tick at = adjustment < 0 ?
                ref.vectorResult->nominalAt - delta :
                ref.vectorResult->nominalAt + delta;
            ready = std::max(ready, std::max(at, ref.vectorResult->readyAt));
            ++source;
            continue;
        }
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
            nominal = ref.producer->resultNominalAt(ref.destination);
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
        panic_if(ref.producer && at < ref.producer->resultAvailableAt(ref.destination),
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
        if (inst->isSquashed() || inst->getFault() != NoFault ||
            (found->second.vectorResult && !inst->isIssued())) {
            if (found->second.vectorResult)
                found->second.vectorResult->cancelled = true;
            producers.erase(found);
        } else {
            auto &reference = found->second;
            if (reference.vectorResult) {
                panic_if(inst->lastWakeDependents == Tick(-1),
                         "Vector result released before wakeup");
                reference.vectorResult->readyAt = inst->lastWakeDependents;
                reference.vectorResult->ready = true;
            } else {
                reference.port = inst->resultProducerPort();
                reference.nominal = inst->resultNominalAt(i);
                reference.producer = nullptr;
            }
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
            if (found->second.vectorResult)
                found->second.vectorResult->cancelled = true;
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
        if (reference.producer || (reference.vectorResult &&
            !reference.vectorResult->ready && !reference.vectorResult->cancelled))
            return false;
    return true;
}

void
ResultForwarding::checkDrain()
{
    if (drainState() == DrainState::Draining && empty()) signalDrainDone();
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
    fatal_if(p.family < 1 || p.family > 5 || issueInterval < Cycles(1),
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
    const bool fused = inst->fusedFollower();
    panic_if(!fused && !accepts(inst->staticInst->intExecution().family, port, curTick()),
             "Integer execution service accepted an invalid grant");
    if (!fused) nextAcceptance[port] = cpu->clockEdge(issueInterval);
    const Cycles latency(fused ? 0 : inst->staticInst->intExecution().resultLatency);
    const int64_t evaluation = int64_t(uint64_t(latency)) +
        forwarding->earliestAdjustment(inst, port);
    panic_if(evaluation < (fused ? 0 : 1), "Result path requires a value before a valid execution cycle");
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

IntegerDivider::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(admitted, statistics::units::Count::get(), "Integer divide instructions admitted"),
      ADD_STAT(evaluated, statistics::units::Count::get(), "Integer divide values formed once"),
      ADD_STAT(cancelled, statistics::units::Count::get(), "Integer divide instructions cancelled before completion"),
      ADD_STAT(actions, statistics::units::Count::get(), "Preparation, divide launch and two further p1 action grants")
{
    actions.init(4);
}

IntegerDivider::IntegerDivider(const IntegerDividerParams &p)
    : SimObject(p), serviceEvent([this] { service(); }, name() + ".service", false,
                               Event::Priority(0)), stats(this)
{}

void
IntegerDivider::releaseAction(const DynInstPtr &inst)
{
    if (inst->isInIQ() && inst->iq->releaseOneAction(inst) == 0)
        inst->clearInIQ();
}

bool
IntegerDivider::issue(CPU *owner, ResultForwarding *forwarding, FUPool *pool,
    const DynInstPtr &inst, std::function<void()> available,
    std::function<void()> complete, std::function<void()> issued_action)
{
    fatal_if(cpu && cpu != owner, "Integer divider shared between different cores");
    cpu = owner;
    const auto info = inst->staticInst->intDivide();
    assert((info.bytes == 4 || info.bytes == 8) && inst->numDestRegs() == 2);
    const unsigned candidates = inst->boundExecutionPort >= 0 ?
        1u << inst->boundExecutionPort : inst->staticInst->intExecution().ports;
    unsigned eligible = 0;
    for (unsigned p = 0; p < 12; ++p)
        if ((candidates & (1u << p)) && pool->portAvailable(p, curTick()) &&
            forwarding->availableAt(inst, p) <= curTick())
            eligible |= 1u << p;
    const int port = pool->sharedPorts()->probe(IntExecutionFamily::Divider,
                                               candidates, curTick(), eligible);
    if (port < 0) return false;
    pool->sharedPorts()->commit(IntExecutionFamily::Divider, candidates, port, curTick());
    cpu->recordExecutedUops(1, inst->threadNumber);
    cpu->recordDispatchedPort(port);
    pool->releasePortBinding(inst);
    inst->divisionManaged = true;
    auto op = std::make_shared<Operation>();
    op->inst = inst;
    op->pool = pool;
    op->forwarding = forwarding;
    op->available = available;
    op->complete = complete;
    op->issuedAction = issued_action;
    op->admitted = curTick();
    op->inputs = {inst->renamedSrcIdx(info.lowSource)->valueReadyAt(),
                  inst->renamedSrcIdx(info.highSource)->valueReadyAt(),
                  inst->renamedSrcIdx(info.divisorSource)->valueReadyAt()};
    for (const Tick at : op->inputs)
        panic_if(at == MaxTick, "Divide input has no published physical value");
    operations.push_back(op);
    ++stats.admitted;
    ++stats.actions[0];
    releaseAction(inst);
    arm();
    return true;
}

void
IntegerDivider::arm()
{
    if (!operations.empty() && !serviceEvent.scheduled())
        schedule(serviceEvent, cpu->clockEdge(Cycles(1)));
}

void
IntegerDivider::finish(const std::shared_ptr<Operation> &op)
{
    if (op->inst->isSquashed() || op->inst->getFault() != NoFault) {
        if (op->inst->isInIQ()) op->inst->clearInIQ();
        op->forwarding->cancel(op->inst);
        ++stats.cancelled;
    } else {
        op->forwarding->release(op->inst);
    }
    op->complete();
    operations.remove(op);
    cpu->wakeCPU();
    if (operations.empty()) {
        if (serviceEvent.scheduled()) deschedule(serviceEvent);
        if (drainState() == DrainState::Draining) signalDrainDone();
    }
}

void
IntegerDivider::service()
{
    const Tick now = curTick();
    const Tick period = cpu->clockPeriod();
    for (auto it = operations.begin(); it != operations.end();) {
        const auto op = *it++;
        auto inst = op->inst;
        if (inst->isSquashed()) { finish(op); continue; }
        const auto info = inst->staticInst->intDivide();
        if (!op->launched) {
            if (now < nextLaunch || now < op->admitted + period ||
                !op->pool->portAvailable(1, now)) continue;
            op->pool->sharedPorts()->commit(IntExecutionFamily::Divider, 2, 1, now);
            cpu->recordExecutedUops(1, inst->threadNumber);
            cpu->recordDispatchedPort(1);
            op->issuedAction();
            ++stats.actions[1];
            nextLaunch = now + period * (info.bytes == 4 ? 6 : 10);
            op->launched = true;
            // EMR input -> output matrices, ordered low dividend, high
            // dividend, divisor. They are parameter priors, not an SPR
            // value-dependent duration function or a sum of duop latencies.
            const std::array<std::array<unsigned, 3>, 2> matrix = info.bytes == 4 ?
                std::array<std::array<unsigned, 3>, 2>{{{11,15,10}, {15,15,14}}} :
                std::array<std::array<unsigned, 3>, 2>{{{14,18,14}, {18,18,18}}};
            const Tick wait = now - (op->admitted + period);
            for (unsigned result = 0; result < 2; ++result) {
                const auto &row = matrix[result];
                Tick ready = op->admitted + period * *std::min_element(row.begin(), row.end());
                for (unsigned input = 0; input < 3; ++input)
                    ready = std::max(ready, op->inputs[input] + period * row[input]);
                op->outputs[result] = ready + wait;
            }
            std::array<Tick, 2> times;
            times[info.quotientDest] = op->outputs[0];
            times[info.remainderDest] = op->outputs[1];
            inst->prepareDivideValues(times);
            inst->evaluateResultValues(now);
            ++stats.evaluated;
            releaseAction(inst);
            if (inst->getFault() != NoFault) { finish(op); continue; }
        }
        // The published duop list gives three p1 actions but does not give
        // their internal graph. Use a bounded sequence of port grants, then
        // apply the independent input/output matrix to result publication.
        if (op->internalStages < 2 && op->pool->portAvailable(1, now)) {
            op->pool->sharedPorts()->commit(IntExecutionFamily::Divider, 2, 1, now);
            cpu->recordExecutedUops(1, inst->threadNumber);
            cpu->recordDispatchedPort(1);
            op->issuedAction();
            ++stats.actions[2 + op->internalStages];
            ++op->internalStages;
            releaseAction(inst);
        }
        for (unsigned result = 0; result < 2; ++result) {
            if (op->internalStages != 2 || (op->transferred & (1u << result)) ||
                now < op->outputs[result]) continue;
            const unsigned dest = result == 0 ? info.quotientDest : info.remainderDest;
            op->transferred |= 1u << result;
            inst->publishDivideResult(dest, now);
            op->available();
            cpu->schedule(new EventFunctionWrapper([this, op, dest, result] {
                if (!op->inst->isSquashed() && op->inst->getFault() == NoFault)
                    op->inst->writeResultValue(dest);
                op->written |= 1u << result;
                if (op->written == 3) finish(op);
            }, name() + ".prf", true, Event::Priority(1)), now);
        }
    }
    arm();
}

DrainState
IntegerDivider::drain()
{
    return operations.empty() ? DrainState::Drained : DrainState::Draining;
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
