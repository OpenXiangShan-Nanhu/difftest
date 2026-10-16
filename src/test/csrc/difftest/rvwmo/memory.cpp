#include "memory.h"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace rvwmo {
namespace {
std::string check_range(uint64_t address, size_t size) {
  if (size == 0)
    return "empty memory access";
  if (size - 1 > std::numeric_limits<uint64_t>::max() - address)
    return "memory address overflow";
  return {};
}
std::string byte_error(const char *reason, uint64_t address) {
  return std::string(reason) + " at byte address " + std::to_string(address);
}
} // namespace

bool MemoryWriter::operator==(const MemoryWriter &other) const {
  return hart == other.hart && uid == other.uid;
}
bool MemoryWriter::operator<(const MemoryWriter &other) const {
  return hart < other.hart || (hart == other.hart && uid < other.uid);
}
Memory::Memory(ReadGlobal read, WriteGlobal write) : read_(std::move(read)), write_(std::move(write)) {
  if (!read_ || !write_)
    throw std::invalid_argument("GlobalMem callbacks are required");
}

std::string Memory::commit_local(const MemoryStore &store) {
  auto error = check_range(store.address, store.bytes.size());
  if (!error.empty())
    return error;
  if (store.mask.size() != store.bytes.size())
    return "store mask size mismatch";
  if (std::none_of(store.mask.begin(), store.mask.end(), [](uint8_t byte) { return byte != 0; }))
    return "store has no enabled bytes";
  if (store.writer.uid == 0)
    return "store UID zero is reserved";
  if (stores_.count(store.writer))
    return "duplicate live store UID";
  stores_.emplace(store.writer, StoreRecord{store, true});
  // An early load sample can precede retirement of any of its older stores.
  // Preserve the newest eligible byte version until that sample is checked.
  for (auto &entry: snapshots_) {
    auto &sample = entry.second;
    if (sample.hart != store.writer.hart || sample.po <= store.po)
      continue;
    for (size_t i = 0; i < store.mask.size(); ++i) {
      uint64_t at = store.address + i;
      if (!store.mask[i] || at < sample.address || at - sample.address >= sample.latest_local_po.size())
        continue;
      auto &latest = sample.latest_local_po[at - sample.address];
      if (!latest || *latest < store.po)
        latest = store.po;
    }
  }
  for (size_t i = 0; i < store.mask.size(); ++i) {
    if (store.mask[i])
      local_[store.writer.hart][store.address + i].push_back(store.writer);
  }
  return {};
}

std::string Memory::commit_global(uint64_t address, const std::vector<uint8_t> &bytes, const std::vector<uint8_t> &mask,
                                  const std::vector<MemoryWriter> &writers,
                                  const std::vector<MemoryWriter> &completed) {
  auto error = check_range(address, bytes.size());
  if (!error.empty())
    return error;
  if (mask.size() != bytes.size() || writers.size() != bytes.size())
    return "global transaction size mismatch";
  std::set<MemoryWriter> members(completed.begin(), completed.end());
  if (members.size() != completed.size() || members.empty())
    return "invalid global completion members";
  for (const auto &writer: members) {
    auto found = stores_.find(writer);
    if (found == stores_.end() || !found->second.pending)
      return "global completion has no pending local store";
  }
  // Validate the whole transaction before changing either memory model.
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (!mask[i])
      continue;
    if (!members.count(writers[i]))
      return "global byte writer is not a completion member";
    const auto &store = stores_.at(writers[i]).store;
    uint64_t at = address + i;
    if (at < store.address || at - store.address >= store.bytes.size())
      return byte_error("global writer address mismatch", at);
    size_t offset = at - store.address;
    if (!store.mask[offset] || store.bytes[offset] != bytes[i])
      return byte_error("global store value mismatch", at);
  }
  // Each completed byte must be represented, possibly overwritten by a later member.
  for (const auto &writer: members) {
    const auto &store = stores_.at(writer).store;
    for (size_t i = 0; i < store.bytes.size(); ++i) {
      if (!store.mask[i])
        continue;
      uint64_t at = store.address + i;
      if (at < address || at - address >= mask.size() || !mask[at - address])
        return byte_error("completion omitted a local store byte", at);
      const auto &last = stores_.at(writers[at - address]).store;
      if (last.writer.hart != writer.hart || last.po < store.po)
        return byte_error("merged completion discarded a newer local store", at);
    }
  }
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (!mask[i])
      continue;
    write_(address + i, bytes[i]);
    global_writers_[address + i] = writers[i];
  }
  for (const auto &writer: members)
    retire_store(writer);
  return {};
}

std::string Memory::sample(uint32_t hart, uint64_t po, uint64_t time, uint64_t address,
                           const std::vector<uint8_t> &mask, const std::vector<std::optional<MemoryWriter>> &forwarded,
                           MemorySnapshot &snapshot) {
  auto error = check_range(address, mask.size());
  if (!error.empty())
    return error;
  if (!forwarded.empty() && forwarded.size() != mask.size())
    return "forwarding size mismatch";
  for (const auto &writer: forwarded) {
    if (writer && (writer->hart != hart || writer->uid == 0))
      return "invalid local forwarding identity";
  }
  if (next_snapshot_ == 0)
    return "snapshot identity exhausted";
  snapshot = MemorySnapshot{next_snapshot_++, hart, po, time, address, mask, {}};
  snapshot.bytes.resize(mask.size());
  SnapshotRecord record;
  record.hart = hart;
  record.po = po;
  record.address = address;
  record.latest_local_po.resize(mask.size());
  for (size_t i = 0; i < mask.size(); ++i) {
    if (!mask[i])
      continue;
    auto &byte = snapshot.bytes[i];
    uint64_t at = address + i;
    byte.value = read_(at);
    auto global = global_writers_.find(at);
    if (global != global_writers_.end())
      byte.writer = global->second;
    const MemoryStore *newest = nullptr;
    auto local = local_.find(hart);
    if (local != local_.end()) {
      auto versions = local->second.find(at);
      if (versions != local->second.end()) {
        for (const auto &writer: versions->second) {
          const auto &store = stores_.at(writer).store;
          if (store.po < po && (!newest || store.po > newest->po))
            newest = &store;
        }
      }
    }
    if (newest) {
      byte.value = newest->bytes[at - newest->address];
      byte.writer = newest->writer;
      record.latest_local_po[i] = newest->po;
      byte.sampled_local_po = newest->po;
    }
    if (!forwarded.empty() && forwarded[i]) {
      // Keep an independent source-store reference, not the DUT's returned value.
      byte.writer = forwarded[i];
      byte.deferred = true;
      byte.local_forward = true;
      record.sources.push_back(*forwarded[i]);
      ++references_[*forwarded[i]];
    }
  }
  snapshots_.emplace(snapshot.id, std::move(record));
  return {};
}

std::string Memory::resolve(MemorySnapshot &snapshot) const {
  auto active = snapshots_.find(snapshot.id);
  if (active == snapshots_.end())
    return "unknown or released memory snapshot";
  if (snapshot.bytes.size() != snapshot.mask.size())
    return "invalid memory snapshot size";
  for (size_t i = 0; i < snapshot.bytes.size(); ++i) {
    auto &byte = snapshot.bytes[i];
    if (!snapshot.mask[i])
      continue;
    if (!byte.local_forward) {
      auto latest = active->second.latest_local_po[i];
      if (latest && (!byte.sampled_local_po || *latest > *byte.sampled_local_po))
        return byte_error("older store retired after sampling; forwarding identity required", snapshot.address + i);
      continue;
    }
    if (!byte.writer)
      return "forwarded byte has no source identity";
    auto found = stores_.find(*byte.writer);
    if (found == stores_.end())
      return "forwarded source store has not locally committed";
    const auto &store = found->second.store;
    uint64_t at = snapshot.address + i;
    if (store.writer.hart != snapshot.hart || store.po >= snapshot.po)
      return byte_error("forwarding source is not an older local store", at);
    auto latest = active->second.latest_local_po[i];
    if (latest && store.po < *latest)
      return byte_error("forwarding bypassed a newer local store", at);
    if (at < store.address || at - store.address >= store.bytes.size() || !store.mask[at - store.address])
      return byte_error("forwarding source does not write load byte", at);
    byte.value = store.bytes[at - store.address];
    byte.deferred = false;
  }
  return {};
}
std::string Memory::compare(MemorySnapshot &snapshot, const std::vector<uint8_t> &actual) const {
  if (actual.size() != snapshot.bytes.size())
    return "load value size mismatch";
  auto error = resolve(snapshot);
  if (!error.empty())
    return error;
  for (size_t i = 0; i < actual.size(); ++i) {
    if (snapshot.mask[i] && actual[i] != snapshot.bytes[i].value)
      return byte_error("load value mismatch", snapshot.address + i);
  }
  return {};
}
void Memory::collect_store(const MemoryWriter &writer) {
  auto found = stores_.find(writer);
  if (found != stores_.end() && !found->second.pending && !references_.count(writer))
    stores_.erase(found);
}
void Memory::retire_store(const MemoryWriter &writer) {
  auto &record = stores_.at(writer);
  auto &addresses = local_.at(writer.hart);
  for (size_t i = 0; i < record.store.mask.size(); ++i) {
    if (!record.store.mask[i])
      continue;
    auto found = addresses.find(record.store.address + i);
    auto &versions = found->second;
    versions.erase(std::remove(versions.begin(), versions.end(), writer), versions.end());
    if (versions.empty())
      addresses.erase(found);
  }
  if (addresses.empty())
    local_.erase(writer.hart);
  record.pending = false;
  collect_store(writer);
}
std::string Memory::release(uint64_t snapshot_id) {
  auto found = snapshots_.find(snapshot_id);
  if (found == snapshots_.end())
    return "unknown or already released memory snapshot";
  for (const auto &writer: found->second.sources) {
    auto ref = references_.find(writer);
    if (--ref->second == 0)
      references_.erase(ref);
    collect_store(writer);
  }
  snapshots_.erase(found);
  return {};
}
size_t Memory::pending_stores() const {
  return std::count_if(stores_.begin(), stores_.end(), [](const auto &entry) { return entry.second.pending; });
}
} // namespace rvwmo
