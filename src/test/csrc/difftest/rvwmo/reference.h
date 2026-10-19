#ifndef DIFFTEST_RVWMO_REFERENCE_H
#define DIFFTEST_RVWMO_REFERENCE_H
#include "event.h"
#include <cstddef>
#include <vector>

namespace rvwmo {

struct ReferenceAccess {
  uint64_t address = 0;
  std::vector<uint8_t> bytes;
};

// Per-instruction bridge. Begin only after independently validating the load.
// Returning true on a malformed request prevents silent private-RAM fallback;
// finish() reports the mismatch before the reference result can be accepted.
class ReferenceRead {
public:
  CheckResult begin(std::vector<ReferenceAccess> accesses);
  CheckResult finish();
  static bool read(void *context, uint64_t address, size_t size, uint8_t *bytes);

private:
  std::vector<ReferenceAccess> accesses_;
  size_t next_ = 0;
  bool active_ = false;
  CheckResult error_;
};
} // namespace rvwmo
#endif
