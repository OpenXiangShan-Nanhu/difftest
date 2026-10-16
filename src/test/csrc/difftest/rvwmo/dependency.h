#ifndef DIFFTEST_RVWMO_DEPENDENCY_H
#define DIFFTEST_RVWMO_DEPENDENCY_H
#include "event.h"
#include <map>

namespace rvwmo {
struct DependencySummary {
  bool valid = false;
  uint64_t rank = 0;
  OperationId source;
  void include(const DependencySummary &other);
};
class DependencyTracker {
public:
  DependencySummary read(const std::vector<unsigned> &registers) const;
  void write(const std::vector<unsigned> &registers, const DependencySummary &summary);

private:
  std::map<unsigned, DependencySummary> registers_;
};
CheckResult decode_dependencies(uint32_t instruction, DependencyMetadata &metadata);
} // namespace rvwmo
#endif
