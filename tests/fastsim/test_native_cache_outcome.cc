// Regression for the request-local and identity views of a Ruby lookup.
#include <cassert>
#include <memory>

#include "mem/ruby/slicc_interface/RubySlicc_Util.hh"

namespace gem5::Gem5Internal
{
__thread Tick *_curTickPtr = nullptr;
}

int
main()
{
    using namespace gem5;
    Tick tick = 1234;
    Gem5Internal::_curTickPtr = &tick;
    constexpr uint32_t context = 7;
    constexpr uint64_t sequence = 19;
    TaoTraceNativeAccessRegistry::enableContext(context);

    auto identity_only = std::make_shared<Request>(
        Addr{0x2000}, 8, Request::Flags{}, RequestorID{1});
    identity_only->setContext(context);
    identity_only->setReqInstSeqNum(sequence - 1);
    ruby::taotraceCacheOutcomeByIdentity(true, context, sequence - 1, 0, 1);
    assert(!identity_only->getExtension<TaoTraceRubyResponseExtension>());
    assert(TaoTraceNativeAccessRegistry::snapshot(context, sequence - 1)
               .hierarchy.tagMisses[0] == 1);

    auto request = std::make_shared<Request>(Addr{0x1000}, 8, Request::Flags{},
                                             RequestorID{1});
    request->setContext(context);
    request->setReqInstSeqNum(sequence);
    ruby::nativeCacheOutcome(request, true, context, sequence, 0, 1);

    const auto local = request->getExtension<TaoTraceRubyResponseExtension>();
    const auto identity = TaoTraceNativeAccessRegistry::snapshot(context, sequence);
    assert(local && local->hierarchy.accesses[0] == 1);
    assert(local->hierarchy.tagMisses[0] == 1);
    assert(local->hasL1MissTick && local->firstL1MissTick == tick);
    assert(identity.found && identity.hierarchy.accesses[0] == 1);
    assert(identity.hierarchy.tagMisses[0] == 1);

    // A message without a request must still use its captured identity.
    ruby::nativeCacheOutcome({}, true, context, sequence + 1, 0, 1);
    const auto fallback = TaoTraceNativeAccessRegistry::snapshot(
        context, sequence + 1);
    assert(fallback.found && fallback.hierarchy.accesses[0] == 1);
    assert(fallback.hierarchy.tagMisses[0] == 1);

    ruby::nativeCacheOutcome({}, false, context, sequence + 2, 0, 1);
    assert(!TaoTraceNativeAccessRegistry::snapshot(context, sequence + 2).found);
}
