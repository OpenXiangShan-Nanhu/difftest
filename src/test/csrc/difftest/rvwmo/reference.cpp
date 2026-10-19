#include "reference.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace rvwmo {
CheckResult ReferenceRead::begin(std::vector<ReferenceAccess> accesses) {
  if (active_)
    return {CheckStatus::Invalid, 0, "reference memory instruction was not finalized"};
  if (accesses.empty())
    return {CheckStatus::Invalid, 0, "reference load has no validated access"};
  for (const auto &access: accesses)
    if (access.bytes.empty() || access.bytes.size() - 1 > std::numeric_limits<uint64_t>::max() - access.address)
      return {CheckStatus::Invalid, 0, "invalid validated reference access"};
  accesses_ = std::move(accesses);
  next_ = 0;
  error_ = {};
  active_ = true;
  return {};
}

bool ReferenceRead::read(void *context, uint64_t address, size_t size, uint8_t *bytes) {
  auto &self = *static_cast<ReferenceRead *>(context);
  if (!self.active_)
    return false;
  if (self.next_ >= self.accesses_.size() || self.accesses_[self.next_].address != address ||
      self.accesses_[self.next_].bytes.size() != size || !bytes) {
    self.error_ = {CheckStatus::Invalid, 0, "reference address, width or access count differs from validated load"};
    if (bytes)
      std::fill(bytes, bytes + size, 0);
    return true;
  }
  const auto &access = self.accesses_[self.next_++];
  std::copy(access.bytes.begin(), access.bytes.end(), bytes);
  return true;
}

CheckResult ReferenceRead::finish() {
  if (!active_)
    return {CheckStatus::Invalid, 0, "reference memory instruction was not prepared"};
  auto result = error_;
  if (result && next_ != accesses_.size())
    result = {CheckStatus::Invalid, 0, "reference did not consume every validated load access"};
  accesses_.clear();
  next_ = 0;
  active_ = false;
  return result;
}
} // namespace rvwmo
