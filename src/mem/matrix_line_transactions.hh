#ifndef __MEM_MATRIX_LINE_TRANSACTIONS_HH__
#define __MEM_MATRIX_LINE_TRANSACTIONS_HH__

#include <cassert>
#include <cstdint>
#include <map>
#include <utility>

namespace gem5::matrix
{

/** Bounded ordering state only; cache data and permissions remain in Cache. */
class LineTransactionTable
{
  public:
    using Key = std::pair<uint64_t, bool>;
    enum class Kind : uint8_t { None, Store, CRead };
    enum class Phase : uint8_t { None, Prepare, Access };
    enum class Admission { Allowed, Conflict, Prior, Capacity };

    struct Transaction
    {
        int port;
        uint64_t id;
        Kind kind;
        bool prepared = false;
        bool accessed = false;
        uint64_t cleanPacketId;
        uint64_t started;
    };
    struct Flight
    {
        Key key;
        Phase phase;
        uint64_t transaction;
        uint64_t started;
    };

    LineTransactionTable(unsigned transactions, unsigned requests)
        : maxTransactions(transactions), maxRequests(requests)
    {
        assert(transactions && requests);
    }

    Admission admit(Key key, int port, uint64_t id, Kind kind, Phase phase,
                    uint64_t packet_id, uint64_t now, bool direct_access = false)
    {
        assert(!direct_access || ((kind == Kind::Store || kind == Kind::CRead) &&
                                  phase == Phase::Access));
        auto it = active.find(key);
        if ((phase == Phase::Prepare || direct_access) && it == active.end()) {
            if (active.size() == maxTransactions) {
                return Admission::Capacity;
            }
            it = active.emplace(key, Transaction{
                port, id, kind, false, false, packet_id, now}).first;
        }
        if (it != active.end()) {
            auto &txn = it->second;
            if (phase == Phase::None || txn.port != port || txn.id != id ||
                txn.kind != kind) {
                return Admission::Conflict;
            }
            // Permission reuse still waits for actual earlier deliveries.
            // Until acceptance, a retry may instead require preparation.
            if (phase == Phase::Prepare || direct_access) {
                assert(!txn.prepared);
                assert(!txn.accessed);
                if (inflight.count(key)) {
                    return Admission::Prior;
                }
            } else {
                assert(txn.prepared && !txn.accessed);
            }
        } else {
            assert(phase == Phase::None);
        }
        return flights.size() < maxRequests ? Admission::Allowed :
                                             Admission::Capacity;
    }

    void accepted(const void *request, Key key, Phase phase, uint64_t id,
                  uint64_t now)
    {
        assert(flights.size() < maxRequests);
        const bool inserted = flights.emplace(
            request, Flight{key, phase, id, now}).second;
        assert(inserted);
        ++inflight[key];
        if (phase != Phase::None) {
            auto &txn = active.at(key);
            assert(txn.id == id);
            if (phase == Phase::Prepare) {
                txn.prepared = true;
            } else {
                txn.accessed = true;
            }
        }
    }

    /** Called only after successful delivery, including any response retry. */
    bool delivered(const void *request)
    {
        auto it = flights.find(request);
        if (it == flights.end()) {
            return false;
        }
        const auto flight = it->second;
        auto count = inflight.find(flight.key);
        assert(count != inflight.end() && count->second);
        if (--count->second == 0) {
            inflight.erase(count);
        }
        if (flight.phase == Phase::Access) {
            auto txn = active.find(flight.key);
            assert(txn != active.end() &&
                   txn->second.id == flight.transaction &&
                   txn->second.accessed);
            active.erase(txn);
        }
        flights.erase(it);
        return true;
    }

    const Transaction *transaction(Key key) const
    {
        auto it = active.find(key);
        return it == active.end() ? nullptr : &it->second;
    }
    const Flight *flight(const void *request) const
    {
        auto it = flights.find(request);
        return it == flights.end() ? nullptr : &it->second;
    }
    unsigned transactionCount() const { return active.size(); }
    unsigned requestCount() const { return flights.size(); }

  private:
    const unsigned maxTransactions;
    const unsigned maxRequests;
    std::map<Key, Transaction> active;
    std::map<Key, unsigned> inflight;
    std::map<const void *, Flight> flights;
};

} // namespace gem5::matrix

#endif // __MEM_MATRIX_LINE_TRANSACTIONS_HH__
