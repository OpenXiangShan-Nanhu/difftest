#ifndef DIFFTEST_RVWMO_EVENT_H
#define DIFFTEST_RVWMO_EVENT_H

#include <cstdint>
#include <string>
#include <vector>

namespace rvwmo {

struct OperationId {
  uint32_t hart = 0;
  uint64_t epoch = 0;
  uint64_t uid = 0;
  uint32_t subaccess = 0;
  bool operator==(const OperationId &other) const {
    return hart == other.hart && epoch == other.epoch && uid == other.uid && subaccess == other.subaccess;
  }
  bool operator!=(const OperationId &other) const {
    return !(*this == other);
  }
  bool operator<(const OperationId &other) const {
    if (hart != other.hart)
      return hart < other.hart;
    if (epoch != other.epoch)
      return epoch < other.epoch;
    if (uid != other.uid)
      return uid < other.uid;
    return subaccess < other.subaccess;
  }
};

struct ObservationTime {
  uint64_t rank = 0;
  bool valid = false;
};

enum class CheckStatus {
  Pass,
  Violation,
  Pending,
  Unsupported,
  Invalid
};
struct CheckResult {
  CheckStatus status = CheckStatus::Pass;
  unsigned rule = 0;
  std::string message;
  explicit operator bool() const {
    return status == CheckStatus::Pass;
  }
};

// Register numbers 0..31 are integer registers; adapters may use other numbers
// for floating-point, vector and CSR dependency carriers.
struct DependencyMetadata {
  bool provided = false;
  bool complete = true;
  std::vector<unsigned> sources;
  std::vector<unsigned> destinations;
  // Optional per-destination carriers. Empty outer vector uses sources above;
  // an empty inner vector explicitly clears that destination's dependencies.
  std::vector<std::vector<unsigned>> destination_sources;
  std::vector<unsigned> address_sources;
  std::vector<unsigned> data_sources;
  bool carries_to_destination = true;
  bool control_transfer = false;
  bool fence = false;
  uint8_t fence_predecessor = 0;
  uint8_t fence_successor = 0;
};

// ModeledWitness identifies the source required by the fixed-order read-value
// axiom. It does not claim to identify a physical cache/forwarding data source.
enum class ReadSourceKind {
  Unavailable,
  ModeledWitness,
  Observed
};

struct MemoryOperation {
  OperationId id;
  uint32_t attempt = 0;
  uint64_t address = 0;
  std::vector<uint8_t> byte_mask;
  bool load = false;
  bool store = false;
  bool atomic = false;
  bool lr = false;
  bool sc = false;
  bool acquire = false;
  bool release = false;
  bool rcsc = false;
  bool has_paired_lr = false;
  OperationId paired_lr;
  ObservationTime read_time;
  ObservationTime write_time;
  ReadSourceKind source_kind = ReadSourceKind::Observed;
  // One writer per byte_mask element, including initialization writers.
  std::vector<OperationId> read_sources;
};

struct RetiredInstruction {
  uint32_t hart = 0;
  uint64_t epoch = 0;
  uint64_t uid = 0;
  uint64_t pc = 0;
  uint32_t instruction = 0;
  bool failed_sc = false;
  std::vector<MemoryOperation> memory;
  DependencyMetadata dependencies;
};

} // namespace rvwmo
#endif
