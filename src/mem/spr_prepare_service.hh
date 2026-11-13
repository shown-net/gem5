// Observer-only live association for store-prepare data and its followers.
#ifndef SPR_PREPARE_SERVICE_HH
#define SPR_PREPARE_SERVICE_HH
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "mem/spr_retired_load_miss.hh"

namespace spr_retired {
struct PrepareService {
    bool pendingFill = false;
    bool dataComplete = false;
    bool invalidated = false;
    bool invalidatedBeforeData = false;
    bool invalidatedAfterData = false;
    bool replaced = false;
    uint8_t invalidationSources = 0;
    bool tbeLive = true;

    Result completedResult(bool admissionValid, bool admissionNeedsFill=false) const {
        Result result;
        if (!admissionValid || !dataComplete || invalidated) return convert(result);
        result.level[0] = pendingFill || admissionNeedsFill ? Level::PendingFill : Level::Permission;
        if (pendingFill || admissionNeedsFill) result.service = Service::DemandFollower;
        return convert(result);
    }
};

// The index owns only live TBEs; follower Requests own the service they
// actually joined. A weak line index carries invalidations after TBE release.
class PrepareServiceTracker {
    using Key = std::pair<int, uint64_t>;
    struct LineEntry { uint64_t generation; std::weak_ptr<PrepareService> service; };
    std::map<Key,LineEntry> lines_;
    std::unordered_map<uint64_t,std::shared_ptr<PrepareService>> tbes_;
    uint64_t next_ = 1;
  public:
    struct Counts {
        uint64_t begun=0, linked=0, data=0, invalidated=0;
        uint64_t replaced=0, closed=0, destroyed=0, peak=0;
    };
  private:
    Counts counts_;
  public:
    uint64_t begin(int core, uint64_t line, bool pendingFill) {
        if (tbes_.size()>=65536)
            throw std::runtime_error("SPR live prepare TBE capacity exhausted");
        Key key{core,line};
        auto prior=lines_.find(key);
        if (prior!=lines_.end()) {
            if (auto previous=prior->second.service.lock()) {
                previous->invalidated=true;
                previous->replaced=true;
                ++counts_.replaced;
            }
        } else if (lines_.size()>=65536) {
            throw std::runtime_error("SPR live prepare association capacity exhausted");
        }
        if (!next_) throw std::runtime_error("SPR prepare generation exhausted");
        const uint64_t generation=next_++;
        auto service=std::shared_ptr<PrepareService>(new PrepareService,
            [this,key,generation](PrepareService *value) {
                auto it=lines_.find(key);
                if (it!=lines_.end() && it->second.generation==generation)
                    lines_.erase(it);
                ++counts_.destroyed;
                delete value;
            });
        service->pendingFill=pendingFill;
        lines_.insert_or_assign(key,LineEntry{generation,service});
        tbes_.emplace(generation,std::move(service));
        ++counts_.begun;
        counts_.peak=std::max<uint64_t>(counts_.peak,lines_.size());
        return generation;
    }
    std::shared_ptr<PrepareService> follow(int core,uint64_t line) {
        const auto it=lines_.find({core,line});
        if (it==lines_.end()) return {};
        auto service=it->second.service.lock();
        if (!service || !service->tbeLive) return {};
        ++counts_.linked;
        return service;
    }
    std::shared_ptr<PrepareService> tbe(uint64_t generation) const {
        const auto it=tbes_.find(generation);
        return it==tbes_.end() ? nullptr : it->second;
    }
    void data(uint64_t generation) {
        const auto it=tbes_.find(generation);
        if (it!=tbes_.end() && !it->second->dataComplete) {
            it->second->dataComplete=true;
            // A real data response from this still-live generation arrives
            // after earlier invalidation ACKs. It cannot revive a replaced
            // generation; follower admission evidence is checked separately.
            if (!it->second->replaced) it->second->invalidated=false;
            ++counts_.data;
        }
    }
    void close(uint64_t generation) {
        const auto it=tbes_.find(generation);
        if (it==tbes_.end()) return;
        it->second->tbeLive=false;
        ++counts_.closed;
        tbes_.erase(it);
    }
    void invalidate(int core,uint64_t line,unsigned reason=0) {
        const auto it=lines_.find({core,line});
        if (it==lines_.end()) return;
        if (auto service=it->second.service.lock()) {
            if (!service->invalidated) ++counts_.invalidated;
            service->invalidated=true;
            if (reason<8) service->invalidationSources |= uint8_t(1u<<reason);
            if (service->dataComplete) service->invalidatedAfterData=true;
            else service->invalidatedBeforeData=true;
        }
    }
    size_t live() const { return lines_.size(); }
    size_t tbes() const { return tbes_.size(); }
    const Counts &counts() const { return counts_; }
};

// The tracker is process-lifetime; entries remove themselves on the final
// real TBE/Request reference, including canceled Request destruction.
inline PrepareServiceTracker &prepareServices() {
    static auto *tracker=new PrepareServiceTracker;
    return *tracker;
}
}
#endif
