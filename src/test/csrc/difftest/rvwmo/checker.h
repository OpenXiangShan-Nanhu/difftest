#ifndef DIFFTEST_RVWMO_CHECKER_H
#define DIFFTEST_RVWMO_CHECKER_H

#include "memory.h"
#include "ppo.h"
#include <deque>
#include <set>

namespace rvwmo {

struct RetiredLoad {
  OperationId id;
  std::vector<uint8_t> bytes;
};

// Event transport and REF execution remain outside this class. Store bytes must
// already be independently REF-checked when record_store is called.
class Checker {
public:
  explicit Checker(Memory &memory) : memory_(memory) {}
  // Allocates a collision-free MemoryWriter for full epoch/UID/subaccess identity.
  CheckResult memory_writer(const OperationId &operation, MemoryWriter &writer);
  CheckResult record_store(const MemoryStore &store, const MemoryOperation &operation);
  CheckResult observe_global(uint64_t address, const std::vector<uint8_t> &bytes, const std::vector<uint8_t> &mask,
                             const std::vector<MemoryWriter> &writers, const std::vector<MemoryWriter> &completed,
                             ObservationTime time);
  CheckResult observe_global(uint64_t address, const std::vector<uint8_t> &bytes, const std::vector<uint8_t> &mask,
                             const std::vector<MemoryWriter> &writers, const std::vector<MemoryWriter> &completed,
                             const std::vector<ObservationTime> &completion_times);
  CheckResult observe_load(const MemoryOperation &operation, uint64_t po,
                           const std::vector<std::optional<MemoryWriter>> &forwarded);
  CheckResult cancel_load(const OperationId &operation, uint32_t attempt);
  // Caller must prove that caches/observers cannot reference this writer again.
  // Local pending, snapshot and queued-retirement references are checked here.
  CheckResult release_store(const OperationId &operation);
  // Enqueues retirement without blocking event acquisition. Pending is not an
  // error: poll after later global-completion or source-store events arrive.
  CheckResult retire(const RetiredInstruction &instruction, const std::vector<RetiredLoad> &loads);
  CheckResult poll();
  CheckResult finalize();
  size_t pending_retirements() const;
  size_t pending_loads() const {
    return loads_.size();
  }
  static OperationId initialization_writer(uint64_t byte_address);

private:
  struct LoadSample {
    MemoryOperation operation;
    MemorySnapshot snapshot;
  };
  struct Retirement {
    RetiredInstruction instruction;
    std::vector<RetiredLoad> loads;
  };
  using LoadKey = std::pair<OperationId, uint32_t>;
  Memory &memory_;
  PpoChecker ppo_;
  std::map<MemoryWriter, OperationId> writers_;
  std::map<OperationId, MemoryWriter> operation_writers_;
  std::map<uint32_t, uint64_t> next_writer_uid_;
  std::map<OperationId, MemoryOperation> stores_;
  std::set<OperationId> checked_stores_;
  std::set<OperationId> completed_stores_;
  std::map<LoadKey, LoadSample> loads_;
  std::map<uint32_t, std::deque<Retirement>> retirements_;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> last_enqueued_;
  CheckResult failure_;
  CheckResult drain(uint32_t hart);
  CheckResult prepare(Retirement &retirement);
  CheckResult remember_failure(CheckResult result);
};

} // namespace rvwmo
#endif
