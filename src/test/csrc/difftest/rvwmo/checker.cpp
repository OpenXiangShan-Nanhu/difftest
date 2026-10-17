#include "checker.h"
#include <limits>

namespace rvwmo {
namespace {
CheckResult invalid(const std::string &message) {
  return {CheckStatus::Invalid, 0, message};
}
CheckResult pending(const std::string &message) {
  return {CheckStatus::Pending, 0, message};
}
CheckResult accepted_event(CheckResult result) {
  return result.status == CheckStatus::Pending ? CheckResult{} : result;
}
} // namespace

CheckResult Checker::remember_failure(CheckResult result) {
  if (result.status != CheckStatus::Pass && result.status != CheckStatus::Pending)
    failure_ = result;
  return result;
}

OperationId Checker::initialization_writer(uint64_t address) {
  return {std::numeric_limits<uint32_t>::max(), 0, address, 0};
}

CheckResult Checker::memory_writer(const OperationId &operation, MemoryWriter &writer) {
  if (!failure_)
    return failure_;
  auto found = operation_writers_.find(operation);
  if (found != operation_writers_.end()) {
    writer = found->second;
    return {};
  }
  auto &next = next_writer_uid_[operation.hart];
  while (writers_.count({operation.hart, next})) {
    if (next == std::numeric_limits<uint64_t>::max())
      return remember_failure(invalid("memory writer identity space exhausted"));
    ++next;
  }
  writer = {operation.hart, next};
  operation_writers_[operation] = writer;
  writers_[writer] = operation;
  return {};
}

CheckResult Checker::record_store(const MemoryStore &store, const MemoryOperation &operation) {
  if (!failure_)
    return failure_;
  if (!operation.store || store.writer.hart != operation.id.hart)
    return remember_failure(invalid("store model identity differs from observed operation"));
  if (store.address != operation.address || store.mask != operation.byte_mask)
    return remember_failure(invalid("store model address/mask differs from observed operation"));
  auto prior_writer = writers_.find(store.writer);
  auto prior_operation = operation_writers_.find(operation.id);
  if (stores_.count(operation.id) || (prior_writer != writers_.end() && prior_writer->second != operation.id) ||
      (prior_operation != operation_writers_.end() && !(prior_operation->second == store.writer)))
    return remember_failure(invalid("duplicate store identity or MemoryWriter identity collision"));
  auto error = memory_.commit_local(store);
  if (!error.empty())
    return remember_failure(invalid(error));
  writers_[store.writer] = operation.id;
  operation_writers_[operation.id] = store.writer;
  stores_[operation.id] = operation;
  return accepted_event(poll());
}

CheckResult Checker::observe_global(uint64_t address, const std::vector<uint8_t> &bytes,
                                    const std::vector<uint8_t> &mask, const std::vector<MemoryWriter> &writers,
                                    const std::vector<MemoryWriter> &completed, ObservationTime time) {
  if (completed.size() > 1)
    return remember_failure(invalid("merged completion requires one logical timestamp per store"));
  return observe_global(address, bytes, mask, writers, completed, std::vector<ObservationTime>(completed.size(), time));
}

CheckResult Checker::observe_global(uint64_t address, const std::vector<uint8_t> &bytes,
                                    const std::vector<uint8_t> &mask, const std::vector<MemoryWriter> &writers,
                                    const std::vector<MemoryWriter> &completed,
                                    const std::vector<ObservationTime> &completion_times) {
  if (!failure_)
    return failure_;
  if (completed.size() != completion_times.size())
    return remember_failure(invalid("logical completion timestamp count differs from completed stores"));
  for (size_t i = 0; i < completed.size(); ++i) {
    const auto &writer = completed[i];
    const auto &time = completion_times[i];
    if (!time.valid)
      return pending("global store completion has no observation rank");
    auto found = writers_.find(writer);
    if (found == writers_.end())
      return pending("global completion precedes REF-verified store record");
    const auto &operation = stores_.at(found->second);
    if (operation.write_time.valid && operation.write_time.rank != time.rank)
      return remember_failure(invalid("store has inconsistent global-completion timestamps"));
  }
  auto error = memory_.commit_global(address, bytes, mask, writers, completed);
  if (!error.empty())
    return remember_failure(invalid(error));
  for (size_t i = 0; i < completed.size(); ++i) {
    const auto &operation = writers_.at(completed[i]);
    stores_.at(operation).write_time = completion_times[i];
    completed_stores_.insert(operation);
  }
  return accepted_event(poll());
}

CheckResult Checker::observe_load(const MemoryOperation &operation, uint64_t po,
                                  const std::vector<std::optional<MemoryWriter>> &forwarded) {
  if (!failure_)
    return failure_;
  if (!operation.load)
    return remember_failure(invalid("load observation lacks read effect"));
  if (!operation.read_time.valid)
    return pending("load observation has no rank");
  LoadKey key{operation.id, operation.attempt};
  if (loads_.count(key))
    return remember_failure(invalid("duplicate load attempt observation"));
  MemorySnapshot snapshot;
  auto error = memory_.sample(operation.id.hart, po, operation.read_time.rank, operation.address, operation.byte_mask,
                              forwarded, snapshot);
  if (!error.empty())
    return remember_failure(invalid(error));
  loads_.emplace(key, LoadSample{operation, std::move(snapshot)});
  return accepted_event(poll());
}

CheckResult Checker::cancel_load(const OperationId &operation, uint32_t attempt) {
  if (!failure_)
    return failure_;
  auto found = loads_.find({operation, attempt});
  if (found == loads_.end())
    return remember_failure(invalid("cancellation references unknown load attempt"));
  auto error = memory_.release(found->second.snapshot.id);
  if (!error.empty())
    return remember_failure(invalid(error));
  loads_.erase(found);
  return {};
}

CheckResult Checker::release_store(const OperationId &operation) {
  if (!failure_)
    return failure_;
  if (!checked_stores_.count(operation) || !completed_stores_.count(operation))
    return remember_failure(invalid("writer release requires global completion and checked retirement"));
  auto references = [&](const MemoryOperation &sample) {
    if (sample.id == operation)
      return true;
    for (const auto &source: sample.read_sources)
      if (source == operation)
        return true;
    return false;
  };
  for (const auto &sample: loads_)
    if (references(sample.second.operation))
      return pending("writer still referenced by a captured load");
  for (const auto &hart: retirements_)
    for (const auto &retirement: hart.second)
      for (const auto &sample: retirement.instruction.memory)
        if (references(sample))
          return pending("writer still referenced by queued retirement");
  ppo_.release_writer(operation);
  stores_.erase(operation);
  writers_.erase(operation_writers_.at(operation));
  operation_writers_.erase(operation);
  checked_stores_.erase(operation);
  completed_stores_.erase(operation);
  return {};
}

CheckResult Checker::prepare(Retirement &retirement) {
  for (auto &operation: retirement.instruction.memory) {
    if (operation.store) {
      auto store = stores_.find(operation.id);
      if (store == stores_.end())
        return pending("retired store awaits independently verified store record");
      operation.write_time = store->second.write_time;
      if (!operation.write_time.valid)
        return pending("retired store awaits global-completion timestamp");
    }
    if (!operation.load)
      continue;
    auto load = loads_.find({operation.id, operation.attempt});
    if (load == loads_.end())
      return pending("retired load awaits final-attempt observation");
    const auto &observed = load->second.operation;
    if (operation.address != observed.address || operation.byte_mask != observed.byte_mask)
      return invalid("retirement address/mask differs from observed final load attempt");
    operation.read_time = observed.read_time;
    operation.read_sources = observed.read_sources;
    if (operation.read_sources.size() != operation.byte_mask.size())
      return pending("load lacks actual byte-writer provenance; model sources cannot replace it");
    auto error = memory_.resolve(load->second.snapshot);
    if (error == "forwarded source store has not locally committed" ||
        error.find("forwarding identity required") != std::string::npos)
      return pending(error);
    if (!error.empty())
      return {CheckStatus::Violation, 0, error};
    bool actual_found = false;
    for (const auto &actual: retirement.loads)
      if (actual.id == operation.id) {
        if (actual_found || actual.bytes.size() != operation.byte_mask.size())
          return invalid("duplicate or incorrectly sized retired load value");
        actual_found = true;
      }
    if (!actual_found)
      return pending("retired load awaits its actual value bytes");
  }
  return {};
}

CheckResult Checker::drain(uint32_t hart) {
  auto &queue = retirements_[hart];
  while (!queue.empty()) {
    auto &front = queue.front();
    auto result = prepare(front);
    if (!result)
      return remember_failure(result);
    // PPO must succeed before any value comparison. A value failure is terminal;
    // retry never applies already accepted PPO state a second time.
    result = ppo_.retire(front.instruction);
    if (!result)
      return remember_failure(result);
    for (const auto &operation: front.instruction.memory) {
      if (!operation.load)
        continue;
      auto &sample = loads_.at({operation.id, operation.attempt});
      for (const auto &actual: front.loads)
        if (actual.id == operation.id) {
          auto error = memory_.compare(sample.snapshot, actual.bytes);
          if (!error.empty())
            return remember_failure({CheckStatus::Violation, 0, error});
        }
    }
    for (const auto &operation: front.instruction.memory) {
      if (!operation.load)
        continue;
      auto found = loads_.find({operation.id, operation.attempt});
      auto error = memory_.release(found->second.snapshot.id);
      if (!error.empty())
        return remember_failure(invalid(error));
      loads_.erase(found);
    }
    for (const auto &operation: front.instruction.memory)
      if (operation.store)
        checked_stores_.insert(operation.id);
    queue.pop_front();
  }
  return {};
}

CheckResult Checker::retire(const RetiredInstruction &instruction, const std::vector<RetiredLoad> &loads) {
  if (!failure_)
    return failure_;
  auto last = last_enqueued_.find(instruction.hart);
  if (last != last_enqueued_.end() &&
      (last->second.first != instruction.epoch || last->second.second >= instruction.uid))
    return remember_failure(invalid("retirement queue requires increasing UID in one hart epoch"));
  last_enqueued_[instruction.hart] = {instruction.epoch, instruction.uid};
  retirements_[instruction.hart].push_back({instruction, loads});
  return drain(instruction.hart);
}

CheckResult Checker::poll() {
  if (!failure_)
    return failure_;
  CheckResult result;
  for (auto &entry: retirements_) {
    auto current = drain(entry.first);
    if (current.status != CheckStatus::Pass && current.status != CheckStatus::Pending)
      return current;
    if (current.status == CheckStatus::Pending)
      result = current;
  }
  return result;
}

size_t Checker::pending_retirements() const {
  size_t result = 0;
  for (const auto &entry: retirements_)
    result += entry.second.size();
  return result;
}

CheckResult Checker::finalize() {
  auto result = poll();
  if (!result)
    return result;
  if (checked_stores_.size() != stores_.size())
    return pending("recorded stores have not all completed PPO retirement checks");
  if (pending_retirements() || !loads_.empty() || memory_.pending_snapshots() || memory_.pending_stores())
    return pending("RVWMO finalization has outstanding retirements, samples or stores");
  return ppo_.finalize();
}
} // namespace rvwmo
