#ifndef DIFFTEST_RVWMO_PPO_H
#define DIFFTEST_RVWMO_PPO_H
#include "dependency.h"
#include <array>
#include <map>

namespace rvwmo {

// Consumes architectural instructions in each hart's retirement order. Samples
// must describe the final accepted attempt. Pending calls make no state changes
// and must be retried with complete timestamps before retiring a younger event.
class PpoChecker {
public:
  CheckResult retire(const RetiredInstruction &instruction);
  CheckResult finalize() const;
  void reset_hart(uint32_t hart, uint64_t epoch);
  void release_writer(const OperationId &writer);
  size_t address_count() const;

private:
  struct ReadSummary {
    OperationId writer;
    DependencySummary time;
  };
  struct ByteState {
    DependencySummary access;
    std::array<ReadSummary, 2> reads;
  };
  struct HartState {
    uint64_t epoch = 0;
    uint64_t last_uid = 0;
    bool has_retired = false;
    bool pending = false;
    uint64_t pending_uid = 0;
    DependencyTracker dependencies;
    DependencySummary all, reads, writes, acquire, rcsc, control, address_prefix;
    DependencySummary fence_read, fence_write;
    std::map<uint64_t, ByteState> bytes;
    std::map<OperationId, DependencySummary> store_dependencies;
    std::map<OperationId, DependencySummary> reservation_reads;
  };
  std::map<uint32_t, HartState> harts_;
  CheckResult check_operation(HartState &hart, const MemoryOperation &operation, const DependencyMetadata &metadata);
  void update_operation(HartState &hart, const MemoryOperation &operation, const DependencyMetadata &metadata);
};
} // namespace rvwmo
#endif
