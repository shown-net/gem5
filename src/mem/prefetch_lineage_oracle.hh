/* Facts-only prefetch service lineage; never a retired PMU value. */
#ifndef __MEM_PREFETCH_LINEAGE_ORACLE_HH__
#define __MEM_PREFETCH_LINEAGE_ORACLE_HH__

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace gem5::prefetch_lineage
{
struct Witness
{
    std::uint32_t context;
    std::uint64_t sequence, attempt;
    int llc_outcome;
    // Immediate response-message sender, not the ultimate data tier.
    // Sender: unknown=0, shared LLC controller=1, private peer=2.
    // Response: unknown=0, DATA=1, DATA_EXCLUSIVE=2.
    int response_sender_kind = 0, response_sender_id = -1, response_kind = 0;
};

// Unique means an unambiguous identity join, including distinct followers of
// one transaction. Multiplicity is not ambiguity; conflicting identities,
// repeated service edges and storage overflow are.
enum class Join { Rejected, Unique, Ambiguous };
enum class Edge { Rejected, Recorded, Ambiguous };
enum class Reject : unsigned char {
    None,
    StaleInvalidated,
    Uniqueness,
};

struct Follower {
    std::uint32_t context = 0;
    std::uint64_t sequence = 0, attempt = 0;
};

struct FinishDiagnosis {
    std::optional<Witness> witness;
    std::vector<Witness> witnesses;
    Reject reject = Reject::None;
    std::vector<Follower> followers;
    // One follower/lookup but zero or multiple responses: the legacy witness
    // is still returned (Task 3 contract) but estimated rules reject it.
    bool responseAmbiguous = false;
};

// Identifiers are host-oracle state, never target credits or FST content.
// Only an explicit TBE token and the request's propagated token may join.
class Oracle
{
    struct Transaction
    {
        int core;
        std::uint64_t line;
        unsigned lookups = 0, followers = 0, responses = 0;
        bool invalidated = false;
        bool followerConflict = false;
        Witness witness{};
        std::vector<Follower> followerIds{};
    };
    std::map<std::uint64_t, Transaction> live_;
    std::map<std::pair<int, std::uint64_t>, std::uint64_t> keys_;
    std::uint64_t next_ = 1, overflows_ = 0, follower_overflows_ = 0;
    std::size_t max_live_;

    Transaction *find(std::uint64_t id, int core, std::uint64_t line)
    {
        const auto i = live_.find(id);
        return i != live_.end() && i->second.core == core &&
                       i->second.line == line ? &i->second : nullptr;
    }

  public:
    explicit Oracle(std::size_t max_live) : max_live_(max_live) {}
    std::uint64_t begin(int core, std::uint64_t line)
    {
        if (core < 0 || !next_ || keys_.count({core, line})) return 0;
        if (live_.size() >= max_live_) { ++overflows_; return 0; }
        const auto id = next_++;
        live_.emplace(id, Transaction{core, line});
        keys_.emplace(std::make_pair(core, line), id);
        return id;
    }
    bool attach(std::uint64_t id, int core, std::uint64_t line,
                std::uint32_t context, std::uint64_t sequence,
                std::uint64_t attempt)
    {
        auto *tx = find(id, core, line);
        if (!tx || !attempt) return false;
        if (++tx->followers == 1) {
            tx->witness.context = context;
            tx->witness.sequence = sequence;
            tx->witness.attempt = attempt;
        }
        return true;
    }
    bool lookup(std::uint64_t id, int core, std::uint64_t line, int outcome)
    {
        auto *tx = find(id, core, line);
        if (!tx || outcome < 0 || outcome > 4) return false;
        if (++tx->lookups == 1) tx->witness.llc_outcome = outcome;
        return true;
    }
    bool invalidate(std::uint64_t id, int core, std::uint64_t line)
    {
        auto *tx = find(id, core, line);
        if (!tx) return false;
        tx->invalidated = true;
        return true;
    }
    std::vector<Follower>
    followersOf(std::uint64_t id, int core, std::uint64_t line) const
    {
        const auto i = live_.find(id);
        if (i == live_.end() || i->second.core != core ||
            i->second.line != line)
            return {};
        return i->second.followerIds;
    }
    bool response(std::uint64_t id, int core, std::uint64_t line,
                  int sender_kind, int sender_id, int response_kind)
    {
        auto *tx = find(id, core, line);
        if (!tx) return false;
        if (++tx->responses == 1) {
            if ((sender_kind == 1 || sender_kind == 2) && sender_id >= 0) {
                tx->witness.response_sender_kind = sender_kind;
                tx->witness.response_sender_id = sender_id;
            }
            if (response_kind == 1 || response_kind == 2)
                tx->witness.response_kind = response_kind;
        } else {
            tx->witness.response_sender_kind = 0;
            tx->witness.response_sender_id = -1;
            tx->witness.response_kind = 0;
        }
        return true;
    }
    // Bound host storage independently of workload size. Never silently drop
    // the ninth identity and certify the first eight as a complete chain.
    static constexpr std::size_t MaxFollowers = 8;

    Join attachDiagnosed(std::uint64_t id, int core, std::uint64_t line,
                         std::uint32_t context, std::uint64_t sequence,
                         std::uint64_t attempt, Follower &previous)
    {
        auto *tx = find(id, core, line);
        if (!tx || !attempt) return Join::Rejected;
        for (const auto &f : tx->followerIds) {
            if (f.context == context && f.sequence == sequence &&
                f.attempt == attempt)
                return tx->followerConflict ? Join::Ambiguous : Join::Unique;
            if ((f.context == context && f.sequence == sequence) ||
                f.attempt == attempt) {
                tx->followerConflict = true;
                previous = f;
            }
        }
        if (++tx->followers == 1) {
            tx->witness.context = context;
            tx->witness.sequence = sequence;
            tx->witness.attempt = attempt;
        }
        if (tx->followerIds.size() < MaxFollowers) {
            tx->followerIds.push_back({context, sequence, attempt});
        } else {
            ++follower_overflows_;
            tx->followerConflict = true;
        }
        return tx->followerConflict ? Join::Ambiguous : Join::Unique;
    }

    Edge lookupDiagnosed(std::uint64_t id, int core, std::uint64_t line,
                         int outcome)
    {
        auto *tx = find(id, core, line);
        if (!tx || outcome < 0 || outcome > 4) return Edge::Rejected;
        const bool ambiguous = tx->lookups != 0;
        if (++tx->lookups == 1) tx->witness.llc_outcome = outcome;
        return ambiguous ? Edge::Ambiguous : Edge::Recorded;
    }

    Edge responseDiagnosed(std::uint64_t id, int core, std::uint64_t line,
                           int sender_kind, int sender_id, int response_kind)
    {
        auto *tx = find(id, core, line);
        if (!tx) return Edge::Rejected;
        const bool ambiguous = tx->responses != 0;
        if (++tx->responses == 1) {
            if ((sender_kind == 1 || sender_kind == 2) && sender_id >= 0) {
                tx->witness.response_sender_kind = sender_kind;
                tx->witness.response_sender_id = sender_id;
            }
            if (response_kind == 1 || response_kind == 2)
                tx->witness.response_kind = response_kind;
        } else {
            tx->witness.response_sender_kind = 0;
            tx->witness.response_sender_id = -1;
            tx->witness.response_kind = 0;
        }
        return ambiguous ? Edge::Ambiguous : Edge::Recorded;
    }

    FinishDiagnosis finishDiagnosed(std::uint64_t id, int core,
                                    std::uint64_t line)
    {
        FinishDiagnosis diagnosis;
        const auto *tx = find(id, core, line);
        if (tx) {
            if (tx->invalidated)
                diagnosis.reject = Reject::StaleInvalidated;
            else if (tx->followerConflict || tx->lookups > 1)
                diagnosis.reject = Reject::Uniqueness;
            if (tx->lookups == 1 && tx->followers &&
                !tx->invalidated && !tx->followerConflict) {
                if (tx->followers == 1) diagnosis.witness = tx->witness;
                for (const auto &f : tx->followerIds) {
                    auto w = tx->witness;
                    w.context = f.context;
                    w.sequence = f.sequence;
                    w.attempt = f.attempt;
                    diagnosis.witnesses.push_back(w);
                }
                diagnosis.responseAmbiguous = tx->responses != 1;
            }
            diagnosis.followers = tx->followerIds;
            keys_.erase({core, line});
            live_.erase(id);
        }
        return diagnosis;
    }

    std::optional<Witness> finish(std::uint64_t id, int core,
                                  std::uint64_t line)
    {
        const auto *tx = find(id, core, line);
        if (!tx) return std::nullopt;
        const auto result = tx->lookups == 1 && tx->followers == 1 &&
                                    !tx->invalidated
                                ? std::optional<Witness>(tx->witness)
                                : std::nullopt;
        keys_.erase({core, line});
        live_.erase(id);
        return result;
    }
    std::size_t live() const { return live_.size(); }
    std::uint64_t overflows() const { return overflows_; }
    std::uint64_t followerOverflows() const { return follower_overflows_; }
};

inline Oracle &globalOracle()
{
    // Host observation bound only; unrelated to modeled TBE capacities.
    static Oracle oracle(65536);
    return oracle;
}
} // namespace gem5::prefetch_lineage
#endif
