#ifndef DIFFTEST_RVWMO_ORDER_H
#define DIFFTEST_RVWMO_ORDER_H

#include "event.h"
#include <functional>
#include <map>
#include <optional>
#include <tuple>

namespace rvwmo {

// A stamp describes the observation, not when its delayed DPI packet arrived.
// Within one cycle/phase the producer must supply a cross-hart arbitration order.
struct EventStamp {
  uint64_t cycle = 0;
  uint32_t phase = 0;
  uint32_t order = 0;
  bool ordered = false;
};

class ObservedOrder {
public:
  using Consumer = std::function<CheckResult(ObservationTime)>;
  CheckResult enqueue(EventStamp stamp, Consumer consumer);
  // Producer guarantees every event through this cycle has arrived. Consumers
  // returning Pending retain their event; younger events cannot pass it.
  CheckResult advance(uint64_t completed_cycle);
  CheckResult finalize();
  size_t pending_events() const {
    return events_.size();
  }

private:
  using Key = std::tuple<uint64_t, uint32_t, uint32_t>;
  struct Entry {
    EventStamp stamp;
    Consumer consumer;
  };
  std::map<Key, Entry> events_;
  std::optional<uint64_t> watermark_;
  uint64_t next_rank_ = 1;
  CheckResult failure_;
};

} // namespace rvwmo
#endif
