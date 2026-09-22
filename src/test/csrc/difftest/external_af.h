#ifndef DIFFTEST_EXTERNAL_AF_H
#define DIFFTEST_EXTERNAL_AF_H

#include <cstdint>
#include <deque>

// One record for EVERY architectural trap, including interrupts and untagged
// exceptions. Never search by address: repeated PCs must not reuse old evidence.
class ExternalAfTracker {
public:
  struct Trap {
    uint64_t pc;
    uint64_t paddr;
    uint64_t vaddr;
    uint32_t exception;
    uint32_t interrupt;
    uint32_t source;
  };

  void reset() { traps.clear(); failed = false; }
  bool push(const Trap &trap) {
    if (traps.size() >= 64) {
      failed = true;
      return false;
    }
    traps.push_back(trap);
    return true;
  }
  bool consume(uint64_t pc, uint32_t exception, uint32_t interrupt, Trap &trap) {
    if (failed || traps.empty()) return false;
    trap = traps.front();
    traps.pop_front();
    if (trap.pc != pc || trap.exception != exception || trap.interrupt != interrupt) {
      failed = true;
      return false;
    }
    return true;
  }
  bool broken() const { return failed; }
  static bool permits(const Trap &trap) {
    if (trap.interrupt || !trap.source) return false;
    if (trap.exception == 1) return (trap.source & ~uint32_t(3)) == 0;
    if (trap.exception == 5) return trap.source == 8;
    if (trap.exception == 7) return trap.source == 4;
    return false;
  }

private:
  std::deque<Trap> traps;
  bool failed = false;
};

#endif
