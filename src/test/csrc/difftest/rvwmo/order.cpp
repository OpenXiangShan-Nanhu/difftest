#include "order.h"
#include <limits>
#include <utility>

namespace rvwmo {
CheckResult ObservedOrder::enqueue(EventStamp stamp, Consumer consumer) {
  if (!failure_)
    return failure_;
  if (!consumer || (watermark_ && stamp.cycle <= *watermark_))
    return failure_ = {CheckStatus::Invalid, 0, "missing consumer or observation older than completed watermark"};
  Key key{stamp.cycle, stamp.phase, stamp.order};
  if (events_.count(key))
    return failure_ = {CheckStatus::Invalid, 0, "ambiguous observation order; do not break ties using hart ID"};
  events_.emplace(key, Entry{stamp, std::move(consumer)});
  return {};
}

CheckResult ObservedOrder::advance(uint64_t completed_cycle) {
  if (!failure_)
    return failure_;
  if (watermark_ && completed_cycle < *watermark_)
    return failure_ = {CheckStatus::Invalid, 0, "observation watermark moved backwards"};
  watermark_ = completed_cycle;
  while (!events_.empty() && std::get<0>(events_.begin()->first) <= completed_cycle) {
    auto &entry = events_.begin()->second;
    if (!entry.stamp.ordered)
      return {CheckStatus::Pending, 0, "observation has no qualified cross-hart ordering stamp"};
    if (next_rank_ == std::numeric_limits<uint64_t>::max())
      return failure_ = {CheckStatus::Invalid, 0, "memory observation rank exhausted"};
    auto result = entry.consumer({next_rank_, true});
    if (result.status == CheckStatus::Pending)
      return result;
    if (!result)
      return failure_ = result;
    ++next_rank_;
    events_.erase(events_.begin());
  }
  return {};
}

CheckResult ObservedOrder::finalize() {
  if (!failure_)
    return failure_;
  if (!events_.empty())
    return {CheckStatus::Pending, 0, "memory observations remain unprocessed"};
  return {};
}
} // namespace rvwmo
