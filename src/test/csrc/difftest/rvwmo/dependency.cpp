#include "dependency.h"

namespace rvwmo {
void DependencySummary::include(const DependencySummary &other) {
  if (other.valid && (!valid || other.rank > rank))
    *this = other;
}
DependencySummary DependencyTracker::read(const std::vector<unsigned> &registers) const {
  DependencySummary result;
  for (auto reg: registers) {
    auto found = registers_.find(reg);
    if (reg && found != registers_.end())
      result.include(found->second);
  }
  return result;
}
void DependencyTracker::write(const std::vector<unsigned> &registers, const DependencySummary &summary) {
  for (auto reg: registers)
    if (reg)
      registers_[reg] = summary;
}

CheckResult decode_dependencies(uint32_t inst, DependencyMetadata &m) {
  m = {};
  m.provided = true;
  const unsigned rd = (inst >> 7) & 31, rs1 = (inst >> 15) & 31, rs2 = (inst >> 20) & 31;
  if ((inst & 3) != 3) {
    const unsigned quadrant = inst & 3, funct = (inst >> 13) & 7;
    const unsigned compact1 = 8 + ((inst >> 7) & 7), compact2 = 8 + ((inst >> 2) & 7);
    if (quadrant == 0 && funct == 0) {
      m.sources = {2};
      m.destinations = {compact2};
    } else if (quadrant == 0 && (funct == 2 || funct == 3)) {
      m.sources = m.address_sources = {compact1};
      m.destinations = {compact2};
      m.carries_to_destination = false;
    } else if (quadrant == 0 && (funct == 6 || funct == 7)) {
      m.address_sources = {compact1};
      m.data_sources = {compact2};
      m.sources = {compact1, compact2};
    } else if (quadrant == 1 && (funct == 0 || funct == 1 || funct == 3)) {
      m.sources = {rd};
      m.destinations = {rd};
      if (funct == 3 && rd != 2)
        m.sources.clear();
    } else if (quadrant == 1 && funct == 2) {
      m.destinations = {rd};
    } else if (quadrant == 1 && funct == 4) {
      m.sources = {compact1};
      m.destinations = {compact1};
      if (((inst >> 10) & 3) == 3)
        m.sources.push_back(compact2);
    } else if (quadrant == 1 && funct == 5) { /* C.J */
    } else if (quadrant == 1 && (funct == 6 || funct == 7)) {
      m.sources = {compact1};
      m.control_transfer = true;
    } else if (quadrant == 2 && funct == 0) {
      m.sources = {rd};
      m.destinations = {rd};
    } else if (quadrant == 2 && (funct == 2 || funct == 3)) {
      m.sources = m.address_sources = {2};
      m.destinations = {rd};
      m.carries_to_destination = false;
    } else if (quadrant == 2 && (funct == 6 || funct == 7)) {
      m.address_sources = {2};
      m.data_sources = {(inst >> 2) & 31};
      m.sources = {2, (inst >> 2) & 31};
    } else if (quadrant == 2 && funct == 4) {
      const unsigned src = (inst >> 2) & 31;
      if (src) {
        m.sources = {src};
        if (inst & 0x1000)
          m.sources.push_back(rd);
        m.destinations = {rd};
      } else if (rd) {
        m.sources = {rd};
        m.control_transfer = true;
        m.carries_to_destination = false;
        if (inst & 0x1000)
          m.destinations = {1};
      }
    } else
      return {CheckStatus::Unsupported, 0, "compressed opcode requires dependency metadata"};
    return {};
  }
  switch (inst & 127) {
    case 0x03:
      m.sources = m.address_sources = {rs1};
      m.destinations = {rd};
      m.carries_to_destination = false;
      break;
    case 0x23:
      m.sources = {rs1, rs2};
      m.address_sources = {rs1};
      m.data_sources = {rs2};
      break;
    case 0x13:
    case 0x1b:
      m.sources = {rs1};
      m.destinations = {rd};
      break;
    case 0x33:
    case 0x3b:
      m.sources = {rs1, rs2};
      m.destinations = {rd};
      break;
    case 0x37:
    case 0x17:
    case 0x6f: m.destinations = {rd}; break;
    case 0x67:
      m.sources = {rs1};
      m.destinations = {rd};
      m.control_transfer = true;
      m.carries_to_destination = false;
      break;
    case 0x63:
      m.sources = {rs1, rs2};
      m.control_transfer = true;
      break;
    case 0x0f:
      if (((inst >> 12) & 7) != 0)
        return {CheckStatus::Unsupported, 0, "non-data fence requires adapter metadata"};
      if ((inst >> 28) != 0)
        return {CheckStatus::Unsupported, 0, "non-base fence requires adapter metadata"};
      m.fence = true;
      m.fence_predecessor = (inst >> 24) & 15;
      m.fence_successor = (inst >> 20) & 15;
      break;
    case 0x2f: {
      unsigned funct = inst >> 27;
      m.address_sources = {rs1};
      m.sources = {rs1};
      m.destinations = {rd};
      if (funct != 2) {
        m.data_sources = {rs2};
        m.sources.push_back(rs2);
      }
      m.carries_to_destination = false;
      break;
    }
    default: return {CheckStatus::Unsupported, 0, "opcode requires explicit complete dependency metadata"};
  }
  return {};
}
} // namespace rvwmo
