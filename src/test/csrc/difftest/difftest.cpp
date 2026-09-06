/***************************************************************************************
* Copyright (c) 2020-2023 Institute of Computing Technology, Chinese Academy of Sciences
* Copyright (c) 2020-2021 Peng Cheng Laboratory
*
* DiffTest is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*
* See the Mulan PSL v2 for more details.
***************************************************************************************/

#include "difftest.h"
#include "difftrace.h"
#include "dut.h"
#include "flash.h"
#include "goldenmem.h"
#include "ram.h"
#include "spikedasm.h"
#include <cstring>
#include <vector>
#if defined(CONFIG_DIFFTEST_SQUASH) && !defined(CONFIG_PLATFORM_FPGA)
#include "svdpi.h"
#endif // CONFIG_DIFFTEST_SQUASH && !CONFIG_PLATFORM_FPGA
#ifdef CONFIG_DIFFTEST_PERFCNT
#include "perf.h"
#endif // CONFIG_DIFFTEST_PERFCNT
#ifdef CONFIG_DIFFTEST_QUERY
#include "query.h"
#endif // CONFIG_DIFFTEST_QUERY

Difftest **difftest = NULL;

#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
namespace {

constexpr uint64_t irq_bit(unsigned cause) {
  return 1ULL << cause;
}

constexpr uint64_t kMsi = irq_bit(3);
constexpr uint64_t kSti = irq_bit(5);
constexpr uint64_t kVsti = irq_bit(6);
constexpr uint64_t kMti = irq_bit(7);
constexpr uint64_t kSei = irq_bit(9);
constexpr uint64_t kVsei = irq_bit(10);
constexpr uint64_t kMei = irq_bit(11);
constexpr uint64_t kLcofi = irq_bit(13);
constexpr uint64_t kNonRegInterruptSnapshotMask = kMsi | kSti | kVsti | kMti | kSei | kVsei | kMei | kLcofi;

NonRegInterruptPending make_pending_update(uint64_t valid_mask, uint64_t pending) {
  NonRegInterruptPending ip = {};
  ip.platformIRPMeipValid = (valid_mask & kMei) != 0;
  ip.platformIRPMeip = (pending & kMei) != 0;
  ip.platformIRPMtipValid = (valid_mask & kMti) != 0;
  ip.platformIRPMtip = (pending & kMti) != 0;
  ip.platformIRPMsipValid = (valid_mask & kMsi) != 0;
  ip.platformIRPMsip = (pending & kMsi) != 0;
  ip.platformIRPSeipValid = (valid_mask & kSei) != 0;
  ip.platformIRPSeip = (pending & kSei) != 0;
  ip.platformIRPStipValid = (valid_mask & kSti) != 0;
  ip.platformIRPStip = (pending & kSti) != 0;
  ip.platformIRPVseipValid = (valid_mask & kVsei) != 0;
  ip.platformIRPVseip = (pending & kVsei) != 0;
  ip.platformIRPVstipValid = (valid_mask & kVsti) != 0;
  ip.platformIRPVstip = (pending & kVsti) != 0;
  ip.localCounterOverflowInterruptReqValid = (valid_mask & kLcofi) != 0;
  ip.localCounterOverflowInterruptReq = (pending & kLcofi) != 0;
  return ip;
}

constexpr uint32_t kCsrOpcode = 0x73;
constexpr uint32_t kCsrMip = 0x344;
constexpr uint32_t kCsrSip = 0x144;
constexpr uint32_t kCsrMvip = 0x309;
constexpr uint32_t kCsrHip = 0x644;
constexpr uint32_t kCsrVsip = 0x244;

bool is_pending_csr_address(uint32_t address) {
  return address == kCsrMip || address == kCsrSip || address == kCsrMvip ||
         address == kCsrHip || address == kCsrVsip;
}

bool is_pending_csr_read(uint32_t instruction) {
  if ((instruction & 0x7f) != kCsrOpcode || !is_pending_csr_address((instruction >> 20) & 0xfff)) {
    return false;
  }

  const uint32_t funct3 = (instruction >> 12) & 0x7;
  switch (funct3) {
  case 0x1: // CSRRW: rd=x0 suppresses the read
  case 0x5: // CSRRWI: rd=x0 suppresses the read
    return ((instruction >> 7) & 0x1f) != 0;
  case 0x2: // CSRRS
  case 0x3: // CSRRC
  case 0x6: // CSRRSI
  case 0x7: // CSRRCI
    return true;
  default:
    return false;
  }
}

bool is_pending_csr_write(uint32_t instruction) {
  if ((instruction & 0x7f) != kCsrOpcode || !is_pending_csr_address((instruction >> 20) & 0xfff)) {
    return false;
  }

  const uint32_t funct3 = (instruction >> 12) & 0x7;
  if (funct3 == 0x1 || funct3 == 0x5) { // CSRRW/CSRRWI always write
    return true;
  }
  if (funct3 == 0x2 || funct3 == 0x3 || funct3 == 0x6 || funct3 == 0x7) {
    return ((instruction >> 15) & 0x1f) != 0;
  }
  return false;
}

} // namespace
#endif

int difftest_init() {
#ifdef CONFIG_DIFFTEST_PERFCNT
  difftest_perfcnt_init();
#endif // CONFIG_DIFFTEST_PERFCNT
#ifdef CONFIG_DIFFTEST_IOTRACE
  difftest_iotrace_init();
#endif // CONFIG_DIFFTEST_IOTRACE
#ifdef CONFIG_DIFFTEST_QUERY
  difftest_query_init();
#endif // CONFIG_DIFFTEST_QUERY
  diffstate_buffer_init();
  difftest = new Difftest *[NUM_CORES];
  for (int i = 0; i < NUM_CORES; i++) {
    difftest[i] = new Difftest(i);
    difftest[i]->dut = diffstate_buffer[i]->get(0, 0);
  }
  return 0;
}

int init_nemuproxy(size_t ramsize = 0) {
  for (int i = 0; i < NUM_CORES; i++) {
    difftest[i]->update_nemuproxy(i, ramsize);
  }
  return 0;
}

int difftest_state() {
  for (int i = 0; i < NUM_CORES; i++) {
    if (difftest[i]->get_trap_valid()) {
      return difftest[i]->get_trap_code();
    }
    if (difftest[i]->proxy && difftest[i]->proxy->get_status()) {
      return difftest[i]->proxy->get_status();
    }
  }
  return -1;
}

int difftest_nstep(int step, bool enable_diff) {
#if CONFIG_DIFFTEST_ZONESIZE > 1
  difftest_switch_zone();
#endif // CONFIG_DIFFTEST_ZONESIZE
  for (int i = 0; i < step; i++) {
    if (enable_diff) {
      if (difftest_step())
        return STATE_ABORT;
    } else {
      difftest_set_dut();
      if(difftest_check_trap())
        return STATE_GOODTRAP;
    }
    int status = difftest_state();
    if (status != STATE_RUNNING)
      return status;
  }
  return STATE_RUNNING;
}

void difftest_switch_zone() {
  for (int i = 0; i < NUM_CORES; i++) {
    diffstate_buffer[i]->switch_zone();
  }
}
void difftest_set_dut() {
  for (int i = 0; i < NUM_CORES; i++) {
    difftest[i]->dut = diffstate_buffer[i]->next();
  }
}
int difftest_step() {
  difftest_set_dut();
#if defined(CONFIG_DIFFTEST_QUERY) && !defined(CONFIG_DIFFTEST_BATCH)
  difftest_query_step();
#endif // CONFIG_DIFFTEST_QUERY
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
  // Take all cores' snapshots before any core updates the shared GoldenMem.
  for (int i = 0; i < NUM_CORES; i++) {
    difftest[i]->load_snapshot_record();
  }
#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
  for (int i = 0; i < NUM_CORES; i++) {
    int ret = difftest[i]->step();
    if (ret) {
      return ret;
    }
  }
  return 0;
}

int difftest_check_trap() {
  for (int i = 0; i < NUM_CORES; i++) {
    if(difftest[i]->dut->event.valid && (difftest[i]->dut->event.exceptionInst & 0xffff) == 0x6b) {
      return 1;
    }
  }
  return 0;
}

void difftest_trace_read() {
  for (int i = 0; i < NUM_CORES; i++) {
    difftest[i]->trace_read();
  }
}

void difftest_trace_write(int step) {
  for (int i = 0; i < NUM_CORES; i++) {
    difftest[i]->trace_write(step);
  }
}

void difftest_finish() {
#ifdef CONFIG_DIFFTEST_PERFCNT
  uint64_t cycleCnt = difftest[0]->get_trap_event()->cycleCnt;
  difftest_perfcnt_finish(cycleCnt);
#endif // CONFIG_DIFFTEST_PERFCNT
#ifdef CONFIG_DIFFTEST_IOTRACE
  difftest_iotrace_free();
#endif // CONFIG_DIFFTEST_IOTRACE
#ifdef CONFIG_DIFFTEST_QUERY
  difftest_query_finish();
#endif // CONFIG_DIFFTEST_QUERY
  diffstate_buffer_free();
  for (int i = 0; i < NUM_CORES; i++) {
    delete difftest[i];
  }
  delete[] difftest;
  difftest = NULL;
}

#if defined(CONFIG_DIFFTEST_SQUASH) && !defined(CONFIG_PLATFORM_FPGA)
svScope squashScope;
void set_squash_scope() {
  squashScope = svGetScope();
}

extern "C" void set_squash_enable(int enable);
void difftest_squash_enable(int enable) {
  if (squashScope == NULL) {
    printf("Error: Could not retrieve squash scope, set first\n");
    assert(squashScope);
  }
  svSetScope(squashScope);
  set_squash_enable(enable);
}
#endif // CONFIG_DIFFTEST_SQUASH && !CONFIG_PLATFORM_FPGA

#ifdef CONFIG_DIFFTEST_REPLAY
svScope replayScope;
void set_replay_scope() {
  replayScope = svGetScope();
}

extern "C" void set_replay_head(int head);
void difftest_replay_head(int head) {
  if (replayScope == NULL) {
    printf("Error: Could not retrieve replay scope, set first\n");
    assert(replayScope);
  }
  svSetScope(replayScope);
  set_replay_head(head);
}
#endif // CONFIG_DIFFTEST_REPLAY

Difftest::Difftest(int coreid) : id(coreid) {
  state = new DiffState();
#ifdef CONFIG_DIFFTEST_REPLAY
  state_ss = (DiffState *)malloc(sizeof(DiffState));
#endif // CONFIG_DIFFTEST_REPLAY
}

Difftest::~Difftest() {
  delete state;
  delete difftrace;
  if (proxy) {
    delete proxy;
  }
#ifdef CONFIG_DIFFTEST_REPLAY
  free(state_ss);
  if (proxy_reg_ss) {
    free(proxy_reg_ss);
  }
#endif // CONFIG_DIFFTEST_REPLAY
}

#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
void Difftest::load_snapshot_record() {
  for (int i = 0; i < CONFIG_DIFF_LOAD_SNAPSHOT_WIDTH; i++) {
    auto &event = dut->load_snapshot[i];
    if (!event.valid) {
      continue;
    }
    event.valid = 0;

    auto &snapshots = load_snapshots[event.robidx];
    if (event.clear) {
      snapshots.clear();
      continue;
    }

    LoadGoldenMemSnapshot snapshot;
    snapshot.paddr = event.paddr;

    for (size_t byte = 0; byte < load_snapshot_bytes; byte++) {
      if (!(event.mask & (1U << byte))) {
        continue;
      }
      uint64_t byte_addr = event.paddr + byte;
      if (!in_pmem(byte_addr)) {
        continue;
      }
      uint64_t data = 0;
      read_goldenmem(byte_addr, &data, 1);
      snapshot.data[byte] = data;
      snapshot.mask |= 1U << byte;
    }
    if (snapshot.mask != 0) {
      snapshots.push_back(snapshot);
    }
  }
}

bool Difftest::load_snapshot_matches(uint16_t robidx, uint64_t paddr, const void *data, size_t len,
                                     std::vector<uint16_t> *consumed_masks) const {
  if (len == 0 || len > load_snapshot_bytes) {
    return false;
  }

  const auto &snapshots = load_snapshots[robidx];

  if (consumed_masks != nullptr && consumed_masks->size() != snapshots.size()) {
    consumed_masks->assign(snapshots.size(), 0);
  }

  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t sample_index = snapshots.size(); sample_index > 0; sample_index--) {
    const auto &snapshot = snapshots[sample_index - 1];
    if (paddr < snapshot.paddr) {
      continue;
    }
    size_t offset = paddr - snapshot.paddr;
    if (offset >= load_snapshot_bytes || len > load_snapshot_bytes - offset) {
      continue;
    }

    bool match = true;
    for (size_t i = 0; i < len; i++) {
      uint16_t byte_mask = 1U << (offset + i);
      if (!(snapshot.mask & byte_mask) || bytes[i] != snapshot.data[offset + i] ||
          (consumed_masks != nullptr && ((*consumed_masks)[sample_index - 1] & byte_mask))) {
        match = false;
        break;
      }
    }
    if (match) {
      if (consumed_masks != nullptr) {
        for (size_t i = 0; i < len; i++) {
          (*consumed_masks)[sample_index - 1] |= 1U << (offset + i);
        }
      }
      return true;
    }
  }
  return false;
}

void Difftest::clear_load_snapshot(uint16_t robidx) {
  load_snapshots[robidx].clear();
}

void Difftest::clear_all_load_snapshots() {
  for (auto &snapshots: load_snapshots) {
    snapshots.clear();
  }
}

#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT

#if defined(CONFIG_DIFFTEST_LOADEVENT) && defined(CONFIG_DIFFTEST_ARCHVECREGSTATE)
bool enable_vec_load_goldenmem_check = true;
#endif // CONFIG_DIFFTEST_LOADEVENT && CONFIG_DIFFTEST_ARCHVECREGSTATE

void Difftest::update_nemuproxy(int coreid, size_t ram_size = 0) {
  proxy = new REF_PROXY(coreid, ram_size);
#if defined(CONFIG_DIFFTEST_LOADEVENT) && defined(CONFIG_DIFFTEST_ARCHVECREGSTATE)
  enable_vec_load_goldenmem_check = proxy->check_ref_vec_load_goldenmem();
#endif // CONFIG_DIFFTEST_LOADEVENT && CONFIG_DIFFTEST_ARCHVECREGSTATE
#ifdef CONFIG_DIFFTEST_REPLAY
  proxy_reg_size = proxy->get_reg_size();
  proxy_reg_ss = (uint8_t *)malloc(proxy_reg_size);
#endif // CONFIG_DIFFTEST_REPLAY
}

#ifdef CONFIG_DIFFTEST_REPLAY
bool Difftest::can_replay() {
  auto info = dut->trace_info;
  return info.valid && !info.in_replay && info.trace_size > 1;
}

bool Difftest::in_replay_range() {
  auto info = dut->trace_info;
  if (!info.valid || !info.in_replay || info.trace_size > 1)
    return false;
  int pos = info.trace_head;
  int head = replay_status.trace_head;
  int tail = (head + replay_status.trace_size - 1) % CONFIG_DIFFTEST_REPLAY_SIZE;
  if (tail < head) { // consider ring queue
    return (pos <= tail) || (pos >= head);
  } else {
    return (pos >= head) && (pos <= tail);
  }
}

void Difftest::replay_snapshot() {
  memcpy(state_ss, state, sizeof(DiffState));
  memcpy(proxy_reg_ss, &proxy->regs_int, proxy_reg_size);
  proxy->ref_csrcpy(squash_csr_buf, REF_TO_DUT);
  proxy->ref_store_log_reset();
  proxy->set_store_log(true);
  goldenmem_store_log_reset();
  goldenmem_set_store_log(true);
}

void Difftest::do_replay() {
  auto info = dut->trace_info;
  replay_status.in_replay = true;
  replay_status.trace_head = info.trace_head;
  replay_status.trace_size = info.trace_size;
  memcpy(state, state_ss, sizeof(DiffState));
  memcpy(&proxy->regs_int, proxy_reg_ss, proxy_reg_size);
  proxy->ref_regcpy(&proxy->regs_int, DUT_TO_REF, false);
  proxy->ref_csrcpy(squash_csr_buf, DUT_TO_REF);
  proxy->ref_store_log_restore();
  goldenmem_store_log_restore();
  difftest_replay_head(info.trace_head);
  // clear buffered queue
#ifdef CONFIG_DIFFTEST_STOREEVENT
  while (!store_event_queue.empty())
    store_event_queue.pop();
#endif // CONFIG_DIFFTEST_STOREEVENT
#if defined(CONFIG_DIFFTEST_LOADEVENT) && defined(CONFIG_DIFFTEST_SQUASH)
  while (!load_event_queue.empty())
    load_event_queue.pop();
#endif
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
  clear_all_load_snapshots();
#endif
}
#endif // CONFIG_DIFFTEST_REPLAY

int Difftest::step() {
#ifdef CONFIG_DIFFTEST_REPLAY
  static int replay_step = 0;
  if (replay_status.in_replay) {
    if (!in_replay_range()) {
      return 0;
    } else {
      replay_step++;
      if (replay_step > replay_status.trace_size) {
        Info("*** DUT run out of replay range, failed to get error location ***\n");
        return 1;
      }
    }
  }
  bool canReplay = can_replay();
  if (canReplay) {
    replay_snapshot();
  } else {
    proxy->set_store_log(false);
    goldenmem_set_store_log(false);
  }
  int ret = check_all();
  if (ret && canReplay) {
    Info("\n**** Start replay for more accurate error location ****\n");
    do_replay();
    return 0;
  } else {
    return ret;
  }
#else
  return check_all();
#endif // CONFIG_DIFFTEST_REPLAY
}

inline int Difftest::check_all() {
  progress = false;

  if (check_timeout()) {
    return 1;
  }
  do_first_instr_commit();

  // Each cycle is checked for an store event, and recorded in queue.
  // It is checked every time an instruction is committed and queue has content.
#ifdef CONFIG_DIFFTEST_STOREEVENT
  store_event_record();
#endif

#ifdef CONFIG_DIFFTEST_SQUASH
#ifdef CONFIG_DIFFTEST_LOADEVENT
  load_event_record();
#endif // CONFIG_DIFFTEST_LOADEVENT
#endif // CONFIG_DIFFTEST_SQUASH

#ifdef DEBUG_GOLDENMEM
  if (do_golden_memory_update()) {
    return 1;
  }
#endif

#ifdef CONFIG_DIFFTEST_CMOINVALEVENT
  cmo_inval_event_record();
#endif // CONFIG_DIFFTEST_CMOINVALEVENT

  if (!has_commit) {
    return 0;
  }

#ifdef DEBUG_REFILL
  if (do_irefill_check() || do_drefill_check() || do_ptwrefill_check()) {
    return 1;
  }
#endif

#ifdef DEBUG_L2TLB
  if (do_l2tlb_check()) {
    return 1;
  }
#endif

#ifdef DEBUG_L1TLB
  if (do_l1tlb_check()) {
    return 1;
  }
#endif

#ifdef DEBUG_MODE_DIFF
  // skip load & store insts in debug mode
  // for other insts copy inst content to ref's dummy debug module
  for (int i = 0; i < DIFFTEST_COMMIT_WIDTH; i++) {
    if (DEBUG_MEM_REGION(dut->commit[i].valid, dut->commit[i].pc))
      debug_mode_copy(dut->commit[i].pc, dut->commit[i].isRVC ? 2 : 4, dut->commit[i].inst);
  }

#endif

#ifdef CONFIG_DIFFTEST_LRSCEVENT
  // sync lr/sc reg microarchitectural status to the REF
  if (dut->lrsc.valid) {
    dut->lrsc.valid = 0;
    struct SyncState sync;
    sync.sc_fail = !dut->lrsc.success;
    proxy->uarchstatus_sync((uint64_t *)&sync);
  }
#endif

#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
  do_non_reg_interrupt_pending();
  if (csr_snapshot_mismatch) {
    return 1;
  }
#endif

#ifdef CONFIG_DIFFTEST_MHPMEVENTOVERFLOWEVENT
  do_mhpmevent_overflow();
#endif
#ifdef CONFIG_DIFFTEST_CRITICALERROREVENT
  do_raise_critical_error();
#endif
#ifdef CONFIG_DIFFTEST_SYNCAIAEVENT
  do_sync_aia();
#endif
#ifdef CONFIG_DIFFTEST_SYNCCUSTOMMFLUSHPWREVENT
  do_sync_custom_mflushpwr();
#endif

  num_commit = 0; // reset num_commit this cycle to 0
  if (dut->event.valid) {
#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
    csr_read_snapshot.valid = false;
#endif
    // interrupt has a higher priority than exception
    dut->event.interrupt ? do_interrupt() : do_exception();
    dut->event.valid = 0;
    dut->commit[0].valid = 0;
  } else {
#if !defined(BASIC_DIFFTEST_ONLY) && !defined(CONFIG_DIFFTEST_SQUASH)
    if (dut->commit[0].valid) {
      dut_commit_first_pc = dut->commit[0].pc;
      ref_commit_first_pc = proxy->pc;
      if (dut_commit_first_pc != ref_commit_first_pc) {
        pc_mismatch = true;
      }
    }
#endif
    for (int i = 0; i < CONFIG_DIFF_COMMIT_WIDTH; i++) {
      if (dut->commit[i].valid) {
        if (do_instr_commit(i)) {
          return 1;
        }
#ifdef CONFIG_DIFFTEST_VECFOFSYNCEVENT
        // only fof loads (opcode=0x07, mop=00, lumop=10000) should trigger sync
        if ((dut->commit[i].instr & 0x1f0007f) == 0x1000007) do_vec_fof_sync();
#endif
#ifndef CONFIG_DIFFTEST_SQUASH
        if (do_load_check(i)) {
          return 1;
        }
        if (do_store_check()) {
          return 1;
        }
#endif // CONFIG_DIFFTEST_SQUASH
        dut->commit[i].valid = 0;
        num_commit += 1 + dut->commit[i].nFused;
      }
    }
  }

  if (update_delayed_writeback()) {
    return 1;
  }

  if (!progress) {
    return 0;
  }

  proxy->sync();

  if (num_commit > 0) {
    state->record_group(dut->commit[0].pc, num_commit);
  }

  if (apply_delayed_writeback()) {
    return 1;
  }

  if (proxy->compare(dut) || pc_mismatch
#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
      || interrupt_mismatch || csr_snapshot_mismatch
#endif
  ) {
#ifdef FUZZING
    if (in_disambiguation_state()) {
      Info("Mismatch detected with a disambiguation state at pc = 0x%lx.\n", dut->trap.pc);
      return 0;
    }
#endif
    display();
    proxy->display(dut);
#ifdef FUZZER_LIB
    stats.exit_code = SimExitCode::difftest;
#endif // FUZZER_LIB
    return 1;
  }

  return 0;
}

void Difftest::do_interrupt() {
  state->record_interrupt(dut->event.exceptionPC, dut->event.exceptionInst, dut->event.interrupt);
#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
  uint64_t restore_mask = 0;
  const uint64_t latest_pending = latest_non_reg_interrupt_pending;
  if (dut->event.interruptSnapshotValid) {
    const uint64_t cause = dut->event.interrupt;
    if (cause >= 64 || !(dut->event.interruptCandidates & irq_bit(cause))) {
      Info("Core %d interrupt candidate mismatch: cause=%lu candidates=0x%016lx\n", this->id, cause,
           dut->event.interruptCandidates);
      interrupt_mismatch = true;
    }

    const uint64_t accepted_pending = dut->event.nonRegInterruptPending;
    const uint64_t snapshot_mask = dut->event.nonRegInterruptPendingMask & kNonRegInterruptSnapshotMask;
    const uint64_t latest_mask = latest_non_reg_interrupt_pending_mask & kNonRegInterruptSnapshotMask;
    const uint64_t rewind_domain = snapshot_mask | latest_mask;
    restore_mask = (accepted_pending ^ latest_pending) & rewind_domain;
    if (restore_mask) {
      auto accepted = make_pending_update(restore_mask, accepted_pending);
      proxy->non_reg_interrupt_pending(accepted);
    }
  }
#endif
  if (dut->event.hasNMI) {
    proxy->trigger_nmi(dut->event.hasNMI, dut->event.interrupt);
  } else if (dut->event.virtualInterruptIsHvictlInject) {
    proxy->virtual_interrupt_is_hvictl_inject(dut->event.virtualInterruptIsHvictlInject);
  }
  struct InterruptDelegate intrDeleg;
  intrDeleg.irToHS = dut->event.irToHS;
  intrDeleg.irToVS = dut->event.irToVS;
  proxy->intr_delegate(intrDeleg);
  proxy->raise_intr(dut->event.interrupt | (1ULL << 63));
#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
  if (restore_mask) {
    auto latest = make_pending_update(restore_mask, latest_pending);
    proxy->non_reg_interrupt_pending(latest);
  }
#endif
  progress = true;
}

void Difftest::do_exception() {
  state->record_exception(dut->event.exceptionPC, dut->event.exceptionInst, dut->event.exception);
  if (dut->event.exception == EX_IPF || dut->event.exception == EX_LPF || dut->event.exception == EX_SPF ||
      dut->event.exception == EX_IGPF || dut->event.exception == EX_LGPF || dut->event.exception == EX_SGPF) {
    struct ExecutionGuide guide;
    guide.force_raise_exception = true;
    guide.exception_num = dut->event.exception;
    guide.mtval = dut->csr.mtval;
    guide.stval = dut->csr.stval;
#ifdef CONFIG_DIFFTEST_HCSRSTATE
    guide.mtval2 = dut->hcsr.mtval2;
    guide.htval = dut->hcsr.htval;
    guide.vstval = dut->hcsr.vstval;
#endif // CONFIG_DIFFTEST_HCSRSTATE
    guide.force_set_jump_target = false;
    proxy->guided_exec(guide);
  } else if (dut->event.exception == EX_HWE) {
    proxy->raise_intr(dut->event.exception);
  } else {
#ifdef DEBUG_MODE_DIFF
    if (DEBUG_MEM_REGION(true, dut->event.exceptionPC)) {
      debug_mode_copy(dut->event.exceptionPC, 4, dut->event.exceptionInst);
    }
#endif
    proxy->ref_exec(1);
  }

#ifdef FUZZING
  static uint64_t lastExceptionPC = 0xdeadbeafUL;
  static int sameExceptionPCCount = 0;
  if (dut->event.exceptionPC == lastExceptionPC) {
    if (sameExceptionPCCount >= 5) {
      Info("Found infinite loop at exception_pc %lx. Exiting.\n", dut->event.exceptionPC);
      dut->trap.hasTrap = 1;
      dut->trap.code = STATE_FUZZ_COND;
#ifdef FUZZER_LIB
      stats.exit_code = SimExitCode::exception_loop;
#endif // FUZZER_LIB
      return;
    }
    sameExceptionPCCount++;
  }
  if (!sameExceptionPCCount && dut->event.exceptionPC != lastExceptionPC) {
    sameExceptionPCCount = 0;
  }
  lastExceptionPC = dut->event.exceptionPC;
#endif // FUZZING

  progress = true;
}

#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
void Difftest::apply_csr_read_snapshot(int index, uint64_t &restore_mask, uint64_t &restore_pending,
                                       bool &lcofi_changed_after_snapshot) {
  restore_mask = 0;
  restore_pending = latest_non_reg_interrupt_pending;
  lcofi_changed_after_snapshot = false;

  const auto &commit = dut->commit[index];
  auto &snapshot = csr_read_snapshot;
  if (!is_pending_csr_read(commit.instr)) {
    return;
  }

  if (!snapshot.valid) {
    Info("Core %d missing CSR pending snapshot: pc=0x%016lx instr=0x%08x\n", this->id, commit.pc,
         commit.instr);
    csr_snapshot_mismatch = true;
    return;
  }

  const uint64_t snapshot_pending = snapshot.pending & kNonRegInterruptSnapshotMask;
  const uint64_t snapshot_mask = snapshot.mask & kNonRegInterruptSnapshotMask;
  const uint64_t latest_mask = latest_non_reg_interrupt_pending_mask & kNonRegInterruptSnapshotMask;
  const uint64_t rewind_domain = snapshot_mask | latest_mask;
  restore_mask = (snapshot_pending ^ latest_non_reg_interrupt_pending) & rewind_domain;
  lcofi_changed_after_snapshot = snapshot.lcofi_epoch != lcofi_update_epoch;
  if (restore_mask) {
    auto accepted = make_pending_update(restore_mask, snapshot_pending);
    proxy->non_reg_interrupt_pending(accepted);
  }
  snapshot.valid = false;
}

#endif

int Difftest::do_instr_commit(int i) {

  // store the writeback info to debug array
#ifdef BASIC_DIFFTEST_ONLY
  uint64_t commit_pc = proxy->pc;
#else
  uint64_t commit_pc = dut->commit[i].pc;
#endif
  uint64_t commit_instr = dut->commit[i].instr;
  state->record_inst(commit_pc, commit_instr, (dut->commit[i].rfwen | dut->commit[i].fpwen | dut->commit[i].vecwen),
                     dut->commit[i].wdest, get_commit_data(i), dut->commit[i].skip != 0, dut->commit[i].special & 0x1,
                     dut->commit[i].lqIdx, dut->commit[i].sqIdx, dut->commit[i].robIdx, dut->commit[i].isLoad,
                     dut->commit[i].isStore);

#ifdef FUZZING
  // isExit
  if (dut->commit[i].special & 0x2) {
    dut->trap.hasTrap = 1;
    dut->trap.code = STATE_SIM_EXIT;
#ifdef FUZZER_LIB
    stats.exit_code = SimExitCode::sim_exit;
#endif // FUZZER_LIB
    return 0;
  }
#endif // FUZZING

  progress = true;
  update_last_commit();

  // isDelayeWb
  if (dut->commit[i].special & 0x1) {
    int *status =
#ifdef CONFIG_DIFFTEST_ARCHINTDELAYEDUPDATE
        dut->commit[i].rfwen ? delayed_int :
#endif // CONFIG_DIFFTEST_ARCHINTDELAYEDUPDATE
#ifdef CONFIG_DIFFTEST_ARCHFPDELAYEDUPDATE
        dut->commit[i].fpwen ? delayed_fp
                             :
#endif // CONFIG_DIFFTEST_ARCHFPDELAYEDUPDATE
                             nullptr;
    if (status) {
      if (status[dut->commit[i].wdest]) {
        display();
        Info("The delayed register %s has already been delayed for %d cycles\n",
             (dut->commit[i].rfwen ? regs_name_int : regs_name_fp)[dut->commit[i].wdest], status[dut->commit[i].wdest]);
        raise_trap(STATE_ABORT);
        return 1;
      }
      status[dut->commit[i].wdest] = 1;
    }
  }

#ifdef DEBUG_MODE_DIFF
  if (spike_valid() && (IS_DEBUGCSR(commit_instr) || IS_TRIGGERCSR(commit_instr))) {
    Info("s0 is %016lx ", dut->regs.gpr[8]);
    Info("pc is %lx %s\n", commit_pc, spike_dasm(commit_instr));
  }
#endif

  // MMIO accessing should not be a branch or jump, just +2/+4 to get the next pc
  // to skip the checking of an instruction, just copy the reg state to reference design
  if (dut->commit[i].skip || (DEBUG_MODE_SKIP(dut->commit[i].valid, dut->commit[i].pc, dut->commit[i].inst))) {
#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
    if (is_pending_csr_read(dut->commit[i].instr)) {
      csr_read_snapshot.valid = false;
    }
#endif
    // We use the physical register file to get wdata
    proxy->skip_one(dut->commit[i].isRVC, (dut->commit[i].rfwen && dut->commit[i].wdest != 0), dut->commit[i].fpwen,
                    dut->commit[i].vecwen, dut->commit[i].wdest, get_commit_data(i), dut);
    return 0;
  }

#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
  uint64_t csr_restore_mask = 0;
  uint64_t csr_restore_pending = latest_non_reg_interrupt_pending;
  bool lcofi_changed_after_snapshot = false;
  apply_csr_read_snapshot(i, csr_restore_mask, csr_restore_pending, lcofi_changed_after_snapshot);
  if (csr_snapshot_mismatch) {
    return 1;
  }
#endif

  // Default: single step exec
  // when there's a fused instruction, let proxy execute more instructions.
  for (int j = 0; j < dut->commit[i].nFused + 1; j++) {
    proxy->ref_exec(1);
#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
    if (j == 0) {
      const bool pending_csr_write = is_pending_csr_write(dut->commit[i].instr);
      uint64_t post_csr_restore_mask = csr_restore_mask;
      if (pending_csr_write && !lcofi_changed_after_snapshot) {
        // LCOFIP is sticky architectural state. With no later hardware update,
        // the CSR result produced by REF is the newest value and must not be undone.
        post_csr_restore_mask &= ~kLcofi;
      }
      if (post_csr_restore_mask) {
        auto latest = make_pending_update(post_csr_restore_mask, csr_restore_pending);
        proxy->non_reg_interrupt_pending(latest);
      }
      if (pending_csr_write) {
        proxy->sync();
        latest_non_reg_interrupt_pending =
            (latest_non_reg_interrupt_pending & ~kLcofi) | (proxy->csr.mip & kLcofi);
      }
    }
#endif
#ifdef CONFIG_DIFFTEST_SQUASH
    commit_stamp = (commit_stamp + 1) % CONFIG_DIFFTEST_SQUASH_STAMPSIZE;
    if (do_load_check(i)) {
      return 1;
    }
    if (do_store_check()) {
      return 1;
    }
#endif // CONFIG_DIFFTEST_SQUASH
  }

  return 0;
}

void Difftest::regcpy_dut_to_ref() {
#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
  // DUT mip.SEIP is software OR external; Spike regcpy must receive only the software-owned bit.
  const uint64_t dut_mip = dut->csr.mip;
  const bool software_seip = dut->non_reg_interrupt_pending.valid
                                 ? dut->non_reg_interrupt_pending.softwareSeip
                                 : latest_software_seip;
  dut->csr.mip = (dut_mip & ~kSei) | (software_seip ? kSei : 0);
#endif
  proxy->regcpy(dut);
#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
  dut->csr.mip = dut_mip;
#endif
}

void Difftest::do_first_instr_commit() {
  if (!has_commit && dut->commit[0].valid) {
#ifndef BASIC_DIFFTEST_ONLY
    if (dut->commit[0].pc != FIRST_INST_ADDRESS) {
      return;
    }
#endif
    Info("The first instruction of core %d has commited. Difftest enabled. \n", id);
    has_commit = 1;
    nemu_this_pc = FIRST_INST_ADDRESS;

    proxy->flash_init((const uint8_t *)flash_dev.base, flash_dev.img_size, flash_dev.img_path);
    simMemory->clone_on_demand(
        [this](uint64_t offset, void *src, size_t n) {
          uint64_t dest_addr = PMEM_BASE + offset;
          proxy->mem_init(dest_addr, src, n, DUT_TO_REF);
        },
        true);
    // Use a temp variable to store the current pc of dut
    uint64_t dut_this_pc = dut->commit[0].pc;
    // NEMU should always start at FIRST_INST_ADDRESS
    dut->commit[0].pc = FIRST_INST_ADDRESS;
    regcpy_dut_to_ref();
    dut->commit[0].pc = dut_this_pc;
    // Do not reconfig simulator 'proxy->update_config(&nemu_config)' here:
    // If this is main sim thread, simulator has its own initial config
    // If this process is checkpoint wakeuped, simulator's config has already been updated,
    // do not override it.
  }
  if(has_commit && dut->commit[0].valid && dut->commit[0].pc == FIRST_INST_ADDRESS){
    regcpy_dut_to_ref();
  }
}

#if defined(CONFIG_DIFFTEST_LOADEVENT) && defined(CONFIG_DIFFTEST_ARCHVECREGSTATE)
static uint64_t get_vec_load_dut_data(Difftest *difftest, int index, DifftestLoadEvent load_event, int vdidx, int lane) {
#ifdef CONFIG_DIFFTEST_COMMITDATA
#ifdef CONFIG_DIFFTEST_SQUASH
  return load_event.vecCommitData[VLENE_64 * vdidx + lane];
#else
  return difftest->get_dut()->commit_data[index].vecData[VLENE_64 * vdidx + lane];
#endif // CONFIG_DIFFTEST_SQUASH
#else
  bool v0Wen = difftest->get_dut()->commit[index].v0wen && vdidx == 0;
  auto vecNextPdest = difftest->get_dut()->commit[index].otherwpdest[vdidx];
  uint64_t *dutRegPtr = v0Wen ? difftest->get_dut()->wb_v0[vecNextPdest].data :
                                difftest->get_dut()->wb_vec[vecNextPdest].data;
  return dutRegPtr[lane];
#endif // CONFIG_DIFFTEST_COMMITDATA
}

void Difftest::do_vec_load_check(int index, DifftestLoadEvent load_event) {
  if (!enable_vec_load_goldenmem_check) {
    return;
  }

  // ===============================================================
  //                      Comparison data
  // ===============================================================
  uint32_t vdNum = proxy->get_ref_vdNum();

  proxy->sync();

#ifdef CONFIG_DIFFTEST_SQUASH
  auto vecFirstLdest = load_event.wdest;
#else
  auto vecFirstLdest = dut->commit[index].wdest;
#endif // CONFIG_DIFFTEST_SQUASH

  bool reg_mismatch = false;
  const size_t vecRegBytes = VLENE_64 * sizeof(uint64_t);
  const size_t totalBytes = vdNum * vecRegBytes;

  for (int vdidx = 0; vdidx < vdNum; vdidx++) {
    auto vecNextLdest = vecFirstLdest + vdidx;

    for (int i = 0; i < VLENE_64; i++) {
      uint64_t dutRegData = get_vec_load_dut_data(this, index, load_event, vdidx, i);
      uint64_t *refRegPtr = proxy->arch_vecreg(VLENE_64 * vecNextLdest + i);
      reg_mismatch |= dutRegData != *refRegPtr;
    }
  }

  if (!reg_mismatch) {
    return;
  }

  if (totalBytes == 0) {
    Info("Vector Load comparison failed and no destination vector register was recorded.\n");
    return;
  }

  {
    auto packet = proxy->get_vec_goldenmem_packet();
    if (packet == nullptr) {
      Info("Vector Load comparison failed and no byte-level golden memory records were available.\n");
      return;
    }

    auto records = packet->records;
    size_t recordCount = packet->byte_count;
    if (recordCount > 4096) {
      Info("Vector Load golden memory packet byte_count overflow\n");
      return;
    }

    std::vector<uint8_t> loadMask(totalBytes, 0);
    std::vector<uint8_t> refPatchMask(totalBytes, 0);
    std::vector<uint8_t> refPatchData(totalBytes, 0);
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
    struct SnapshotCandidate {
      size_t dstByte;
      uint64_t paddr;
      uint8_t dutByte;
    };
    std::vector<SnapshotCandidate> snapshotCandidates;
    bool snapshot_eligible = true;
#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
    bool byte_mismatch = false;
    std::memset(packet->update_mask, 0, sizeof(packet->update_mask));

    for (size_t i = 0; i < recordCount; i++) {
      const auto &record = records[i];
      if (record.dst_byte == UINT64_MAX) {
        continue;
      }
      if (record.dst_byte >= totalBytes) {
        byte_mismatch = true;
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
        snapshot_eligible = false;
#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
        continue;
      }

      size_t vdidx = record.dst_byte / vecRegBytes;
      size_t regByte = record.dst_byte % vecRegBytes;
      size_t lane = regByte / sizeof(uint64_t);
      size_t laneByte = regByte % sizeof(uint64_t);
      uint64_t dutRegData = get_vec_load_dut_data(this, index, load_event, vdidx, lane);
      uint8_t dutByte = (dutRegData >> (laneByte * 8)) & 0xff;
      uint64_t *refRegPtr = proxy->arch_vecreg(VLENE_64 * (vecFirstLdest + vdidx) + lane);
      uint8_t refByte = (*refRegPtr >> (laneByte * 8)) & 0xff;

      loadMask[record.dst_byte] = 1;
      if (dutByte == refByte) {
        continue;
      }

      if (dutByte == record.golden_byte) {
        packet->update_mask[i] = 1;
        refPatchMask[record.dst_byte] = 1;
        refPatchData[record.dst_byte] = dutByte;
      } else {
        byte_mismatch = true;
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
        snapshotCandidates.push_back({record.dst_byte, record.paddr, dutByte});
#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
      }
    }

    for (int vdidx = 0; vdidx < vdNum; vdidx++) {
      auto vecNextLdest = vecFirstLdest + vdidx;
      for (int lane = 0; lane < VLENE_64; lane++) {
        uint64_t dutRegData = get_vec_load_dut_data(this, index, load_event, vdidx, lane);
        uint64_t *refRegPtr = proxy->arch_vecreg(VLENE_64 * vecNextLdest + lane);
        for (size_t byte = 0; byte < sizeof(uint64_t); byte++) {
          size_t dstByte = vdidx * vecRegBytes + lane * sizeof(uint64_t) + byte;
          if (loadMask[dstByte])
            continue;

          uint8_t dutByte = (dutRegData >> (byte * 8)) & 0xff;
          uint8_t refByte = (*refRegPtr >> (byte * 8)) & 0xff;
          byte_mismatch |= dutByte != refByte;
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
          if (dutByte != refByte) {
            snapshot_eligible = false;
          }
#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
        }
      }
    }

    if (!byte_mismatch) {
      for (size_t dstByte = 0; dstByte < totalBytes; dstByte++) {
        if (!refPatchMask[dstByte])
          continue;

        size_t vdidx = dstByte / vecRegBytes;
        size_t regByte = dstByte % vecRegBytes;
        size_t lane = regByte / sizeof(uint64_t);
        size_t laneByte = regByte % sizeof(uint64_t);
        uint64_t *refRegPtr = proxy->arch_vecreg(VLENE_64 * (vecFirstLdest + vdidx) + lane);
        auto *refBytes = reinterpret_cast<uint8_t *>(refRegPtr);
        refBytes[laneByte] = refPatchData[dstByte];
      }
      proxy->vec_update_goldenmem();
      proxy->sync(true);
      return;
    }

#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
    if (snapshot_eligible && !snapshotCandidates.empty()) {
      bool snapshot_match = true;
      // Each byte returned by one load sample can satisfy at most one vector destination byte.
      std::vector<uint16_t> consumed_snapshot_masks;
      for (const auto &candidate: snapshotCandidates) {
        if (!load_snapshot_matches(load_event.robidx, candidate.paddr, &candidate.dutByte, 1,
                                   &consumed_snapshot_masks)) {
          snapshot_match = false;
          break;
        }
        refPatchMask[candidate.dstByte] = 1;
        refPatchData[candidate.dstByte] = candidate.dutByte;
      }
      if (snapshot_match) {
        for (size_t dstByte = 0; dstByte < totalBytes; dstByte++) {
          if (!refPatchMask[dstByte])
            continue;

          size_t vdidx = dstByte / vecRegBytes;
          size_t regByte = dstByte % vecRegBytes;
          size_t lane = regByte / sizeof(uint64_t);
          size_t laneByte = regByte % sizeof(uint64_t);
          uint64_t *refRegPtr = proxy->arch_vecreg(VLENE_64 * (vecFirstLdest + vdidx) + lane);
          auto *refBytes = reinterpret_cast<uint8_t *>(refRegPtr);
          refBytes[laneByte] = refPatchData[dstByte];
        }
        // update_mask only contains bytes matched by the current GoldenMem.
        // Snapshot bytes must never be written back to the reference memory.
        proxy->vec_update_goldenmem();
        proxy->sync(true);
        return;
      }
    }
#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT

#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
    Info("Vector Load byte-level register, golden memory and execution-time snapshot mismatch\n");
#else
    Info("Vector Load byte-level register and golden memory mismatch\n");
#endif
    return;
  }
}
#endif // CONFIG_DIFFTEST_LOADEVENT && CONFIG_DIFFTEST_ARCHVECREGSTATE

int Difftest::do_load_check(int i) {
  // Handle load instruction carefully for SMP
  int result = 0;
#ifdef CONFIG_DIFFTEST_LOADEVENT
  if (NUM_CORES > 1) {
#ifdef CONFIG_DIFFTEST_SQUASH
    if (load_event_queue.empty())
      return 0;
    auto load_event = load_event_queue.front();
    if (load_event.stamp != commit_stamp)
      return 0;
    bool regWen = load_event.regWen;
    bool fpwen = load_event.fpwen;
    auto refRegPtr = proxy->arch_reg(load_event.wdest, load_event.fpwen);
    auto commitData = load_event.commitData;
#else
    auto load_event = dut->load[i];
    if (!load_event.valid)
      return 0;
    bool regWen = (dut->commit[i].rfwen && dut->commit[i].wdest != 0) || dut->commit[i].fpwen;
    bool fpwen = dut->commit[i].fpwen;
    auto refRegPtr = proxy->arch_reg(dut->commit[i].wdest, dut->commit[i].fpwen);
    auto commitData = get_commit_data(i);
#endif // CONFIG_DIFFTEST_SQUASH

#if defined(CONFIG_DIFFTEST_LOADEVENT) && defined(CONFIG_DIFFTEST_ARCHVECREGSTATE)
    if (load_event.isVLoad) {
      do_vec_load_check(i, load_event);
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
      clear_load_snapshot(load_event.robidx);
#endif
#ifdef CONFIG_DIFFTEST_SQUASH
      load_event_queue.pop();
#else
      dut->load[i].valid = 0;
#endif // CONFIG_DIFFTEST_SQUASH
      return 0;
    }
#endif // CONFIG_DIFFTEST_LOADEVENT && CONFIG_DIFFTEST_ARCHVECREGSTATE

    if (load_event.isLoad || load_event.isAtomic) {
      proxy->sync();
      if (regWen && *refRegPtr != commitData) {
        uint64_t golden;
        uint64_t golden_flag;
        uint64_t mask = 0xFFFFFFFFFFFFFFFF;
        int len = 0;
        if (load_event.isLoad) {
          switch (load_event.opType) {
            case 0:  // lb
            case 4:  // lbu
            case 16: // hlvb
            case 20: // hlvbu
              len = 1;
              break;

            case 1:  // lh
            case 5:  // lhu
            case 17: // hlvh
            case 21: // hlvhu
            case 29: // hlvxhu
              len = 2;
              break;

            case 2:  // lw
            case 6:  // lwu
            case 18: // hlvw
            case 22: // hlvwu
            case 30: // hlvxwu
              len = 4;
              break;

            case 3:  // ld
            case 19: // hlvd
              len = 8;
              break;

            default: Info("Unknown fuOpType: 0x%x\n", load_event.opType);
          }
        } else if (load_event.isAtomic) {
          if (load_event.opType % 2 == 0) {
            len = 4;
          } else { // load_event.opType % 2 == 1
            len = 8;
          }
        }
        if (len == 0) {
          result = 1;
        } else {
          read_goldenmem(load_event.paddr, &golden, len, &golden_flag);
          if (load_event.isLoad && !load_event.isAtomic && golden_flag == 0) {
            const uint64_t ref_data = *refRegPtr;
            const uint64_t data_mask = UINT64_MAX >> (64 - len * 8);
            uint64_t expected = commitData & data_mask;
            if (fpwen) {
              expected |= ~data_mask;
            } else if (!(load_event.opType & 4)) {
              const uint64_t sign_bit = UINT64_C(1) << (len * 8 - 1);
              expected = (expected ^ sign_bit) - sign_bit;
            }
            const bool invalid_format =
                expected != commitData || (fpwen && (load_event.opType < 1 || load_event.opType > 3));
            uint8_t update_mask = 0;
            uint8_t mismatch_mask = 0;
            // ponytail: byte candidates do not prove ordering; stricter checks need forwarding provenance.
            for (int byte = 0; byte < len; byte++) {
              const uint8_t dut_byte = commitData >> (byte * 8);
              if (dut_byte == uint8_t(ref_data >> (byte * 8)))
                continue;
              if (dut_byte == uint8_t(golden >> (byte * 8))) {
                update_mask |= 1U << byte;
                continue;
              }
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
              if (load_snapshot_matches(load_event.robidx, load_event.paddr + byte, &dut_byte, 1))
                continue;
#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
              mismatch_mask |= 1U << byte;
            }
            if (invalid_format || mismatch_mask) {
              Info("Scalar load mismatch: core=%d commit_pc=0x%016lx robidx=0x%x paddr=0x%016lx op=0x%x len=%d\n", id,
                   dut->commit[i].pc, load_event.robidx, load_event.paddr, load_event.opType, len);
              Info("  DUT=0x%016lx REF=0x%016lx Golden=0x%016lx mismatch_mask=0x%02x invalid_format=%d\n", commitData,
                   ref_data, golden, mismatch_mask, invalid_format);
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
              for (const auto &snapshot: load_snapshots[load_event.robidx]) {
                Info("  snapshot paddr=0x%016lx mask=0x%04x data=", snapshot.paddr, snapshot.mask);
                for (size_t byte = 0; byte < load_snapshot_bytes; byte++) {
                  Info("%02x", snapshot.data[byte]);
                }
                Info("\n");
              }
#endif // CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
              result = 1;
            } else {
              // Validate the whole result before repairing memory; snapshot bytes never update it.
              for (int byte = 0; byte < len; byte++) {
                if (!(update_mask & (1U << byte)))
                  continue;
                uint8_t data = golden >> (byte * 8);
                proxy->ref_memcpy(load_event.paddr + byte, &data, 1, DUT_TO_REF);
              }
              *refRegPtr = commitData;
              proxy->sync(true);
            }
          } else {
            if (load_event.isLoad) {
              switch (len) {
                case 1:
                  golden = (int64_t)(int8_t)golden;
                  golden_flag = (int64_t)(int8_t)golden_flag;
                  mask = (uint64_t)(0xFF);
                  break;
                case 2:
                  golden = (int64_t)(int16_t)golden;
                  golden_flag = (int64_t)(int16_t)golden_flag;
                  mask = (uint64_t)(0xFFFF);
                  break;
                case 4:
                  golden = (int64_t)(int32_t)golden;
                  golden_flag = (int64_t)(int32_t)golden_flag;
                  mask = (uint64_t)(0xFFFFFFFF);
                  break;
              }
            }
            if (golden == commitData || load_event.isAtomic) { // atomic instr carefully handled
              proxy->ref_memcpy(load_event.paddr, &golden, len, DUT_TO_REF);
              if (regWen) {
                *refRegPtr = commitData;
                proxy->sync(true);
              }
            } else if (load_event.isLoad && golden_flag != 0) {
              // goldenmem check failed, but the flag is set, so use DUT data to reset
              Info("load check of uncache mm store flag\n");
              Info("  DUT data: 0x%lx, regWen: %d, refRegPtr: %p\n", commitData, regWen, (void *)refRegPtr);
              proxy->ref_memcpy(load_event.paddr, &commitData, len, DUT_TO_REF);
              update_goldenmem(load_event.paddr, &commitData, mask, len);
              if (regWen) {
                *refRegPtr = commitData;
                proxy->sync(true);
              }
            }
          }
        }
      }
    }
#ifdef CONFIG_DIFFTEST_LOADSNAPSHOTEVENT
    clear_load_snapshot(load_event.robidx);
#endif
#ifdef CONFIG_DIFFTEST_SQUASH
    load_event_queue.pop();
#else
    dut->load[i].valid = 0;
#endif // CONFIG_DIFFTEST_SQUASH
  }
#endif // CONFIG_DIFFTEST_LOADEVENT
  return result;
}

int Difftest::do_store_check() {
#ifdef CONFIG_DIFFTEST_STOREEVENT
  while (!store_event_queue.empty()) {
    auto store_event = store_event_queue.front();
#ifdef CONFIG_DIFFTEST_SQUASH
    if (store_event.stamp != commit_stamp)
      return 0;
#endif // CONFIG_DIFFTEST_SQUASH
    auto addr = store_event.addr;
    auto data = store_event.data;
    auto mask = store_event.mask;

    if (proxy->store_commit(&addr, &data, &mask)) {
#ifdef FUZZING
      if (in_disambiguation_state()) {
        Info("Store mismatch detected with a disambiguation state at pc = 0x%lx.\n", dut->trap.pc);
        return 0;
      }
#endif
      uint64_t pc = store_event.pc;
      display();

      Info("\n==============  Store Commit Event (Core %d)  ==============\n", this->id);
      proxy->get_store_event_other_info(&pc);
      Info("Mismatch for store commits \n");
      Info("  REF commits addr 0x%016lx, data 0x%016lx, mask 0x%04x, pc 0x%016lx\n", addr, data, mask, pc);
      Info("  DUT commits addr 0x%016lx, data 0x%016lx, mask 0x%04x, pc 0x%016lx, robidx 0x%x\n", store_event.addr,
           store_event.data, store_event.mask, store_event.pc, store_event.robidx);

      store_event_queue.pop();
      return 1;
    }

    store_event_queue.pop();
  }
#endif // CONFIG_DIFFTEST_STOREEVENT
  return 0;
}

// cacheid: 0 -> icache
//          1 -> dcache
//          2 -> pagecache
//          3 -> icache PIQ refill ipf
//          4 -> icache mainPipe port0 toIFU
//          5 -> icache mainPipe port1 toIFU
//          6 -> icache ipf refill cache
//          7 -> icache mainPipe port0 read PIQ
//          8 -> icache mainPipe port1 read PIQ
int Difftest::do_refill_check(int cacheid) {
#ifdef CONFIG_DIFFTEST_REFILLEVENT
  auto dut_refill = &(dut->refill[cacheid]);
  if (!dut_refill->valid) {
    return 0;
  }
  dut_refill->valid = 0;
  static int delay = 0;
  delay = delay * 2;
  if (delay > 16) {
    return 1;
  }
  static uint64_t last_valid_addr = 0;
  char buf[512];
  char flag_buf[512];
  uint64_t realpaddr = dut_refill->addr;
  dut_refill->addr = dut_refill->addr - dut_refill->addr % 64;
  if (true) {
    last_valid_addr = dut_refill->addr;
    if (!in_pmem(dut_refill->addr)) {
      // speculated illegal mem access should be ignored
      return 0;
    }
    for (int i = 0; i < 8; i++) {
      read_goldenmem(dut_refill->addr + i * 8, &buf, 8, &flag_buf);
      if (dut_refill->data[i] != *((uint64_t *)buf)) {
#ifdef CONFIG_DIFFTEST_CMOINVALEVENT
        if (goldenmem_check_cmo_refill(dut_refill->addr, dut_refill->data)) {
          // Only bytes invalidated by CBO.INVAL may differ; later writes must match.
          Info("INFO: Sync GoldenMem using refill Data from DUT (Because of CBO.INVAL):\n");
          Info("      cacheid=%d, addr: %lx\n      Gold: ", cacheid, dut_refill->addr);
          for (int j = 0; j < 8; j++) {
            read_goldenmem(dut_refill->addr + j * 8, &buf, 8);
            Info("%016lx", *((uint64_t *)buf));
          }
          Info("\n      Core: ");
          for (int j = 0; j < 8; j++) {
            Info("%016lx", dut_refill->data[j]);
          }
          Info("\n");
          update_goldenmem(dut_refill->addr, dut_refill->data, 0xffffffffffffffffUL, 64);
          proxy->ref_memcpy(dut_refill->addr, dut_refill->data, 64, DUT_TO_REF);

          // Preserve local stores in REF without adding them to shared GoldenMem.
          auto *store_data = reinterpret_cast<uint8_t *>(dut_refill->storeData);
          for (int byte = 0; byte < 64; byte++) {
            if (cacheid == DCACHEID && dut_refill->hasStoreData && (dut_refill->storeMask & (1ULL << byte))) {
              proxy->ref_memcpy(dut_refill->addr + byte, &store_data[byte], 1, DUT_TO_REF);
            }
          }
          return 0;
        } else {
#endif // CONFIG_DIFFTEST_CMOINVALEVENT
#ifdef CONFIG_DIFFTEST_UNCACHEMMSTOREEVENT
          // in multi-core, uncache mm store may cause data inconsistencies.
          // so here needs to override the nemu value with the dut value by cacheline granularity.
          if (*((uint64_t *)flag_buf) != 0) {
            Info("INFO: Sync GoldenMem using refill Data from DUT (Because of uncache main-mem store):\n");
            Info("      cacheid=%d, addr: %lx\n      Gold: ", cacheid, dut_refill->addr);
            for (int j = 0; j < 8; j++) {
              read_goldenmem(dut_refill->addr + j * 8, &buf, 8);
              Info("%016lx", *((uint64_t *)buf));
            }
            Info("\n      Core: ");
            for (int j = 0; j < 8; j++) {
              Info("%016lx", dut_refill->data[j]);
            }
            Info("\n");
            update_goldenmem(dut_refill->addr, dut_refill->data, 0xffffffffffffffffUL, 64);
            proxy->ref_memcpy(dut_refill->addr, dut_refill->data, 64, DUT_TO_REF);
            return 0;
          }
#endif // CONFIG_DIFFTEST_UNCACHEMMSTOREEVENT
          Info("cacheid=%d,idtfr=%d,realpaddr=0x%lx: Refill test failed!\n", cacheid, dut_refill->idtfr, realpaddr);
          Info("addr: %lx\nGold: ", dut_refill->addr);
          for (int j = 0; j < 8; j++) {
            read_goldenmem(dut_refill->addr + j * 8, &buf, 8);
            Info("%016lx", *((uint64_t *)buf));
          }
          Info("\nCore: ");
          for (int j = 0; j < 8; j++) {
            Info("%016lx", dut_refill->data[j]);
          }
          Info("\n");
          // continue run some cycle before aborted to dump wave
          if (delay == 0) {
            delay = 1;
          }
          return 0;
#ifdef CONFIG_DIFFTEST_CMOINVALEVENT
        }
#endif // CONFIG_DIFFTEST_CMOINVALEVENT
      }
    }
#ifdef CONFIG_DIFFTEST_CMOINVALEVENT
    // A matching refill also consumes the shared invalidation marker.
    goldenmem_clear_cmo_inval(dut_refill->addr);
#endif
  }
#endif // CONFIG_DIFFTEST_REFILLEVENT
  return 0;
}

int Difftest::do_irefill_check() {
  int r = 0;
  r |= do_refill_check(ICACHEID);
  // r |= do_refill_check(3);
  // r |= do_refill_check(4);
  // r |= do_refill_check(5);
  // r |= do_refill_check(6);
  // r |= do_refill_check(7);
  // r |= do_refill_check(8);
  return r;
}

int Difftest::do_drefill_check() {
  return do_refill_check(DCACHEID);
}

int Difftest::do_ptwrefill_check() {
  return do_refill_check(PAGECACHEID);
}

typedef struct {
  PTE pte;
  uint8_t level;
} r_s2xlate;

r_s2xlate do_s2xlate(Hgatp *hgatp, uint64_t gpaddr) {
  PTE pte;
  uint64_t hpaddr;
  uint8_t level;
  uint64_t pg_base = hgatp->ppn << 12;
  r_s2xlate r_s2;
  if (hgatp->mode == 0) {
    r_s2.pte.ppn = gpaddr >> 12;
    r_s2.level = 0;
    return r_s2;
  }
  int max_level = hgatp->mode == 8 ? 2 : 3;
  for (level = max_level; level >= 0; level--) {
    hpaddr = pg_base + GVPNi(gpaddr, level, max_level) * sizeof(uint64_t);
    read_goldenmem(hpaddr, &pte.val, 8);
    pg_base = pte.ppn << 12;
    if (!pte.v || pte.r || pte.x || pte.w || level == 0) {
      break;
    }
  }
  r_s2.pte = pte;
  r_s2.level = level;
  return r_s2;
}

int Difftest::do_l1tlb_check() {
#ifdef CONFIG_DIFFTEST_L1TLBEVENT
  for (int i = 0; i < CONFIG_DIFF_L1TLB_WIDTH; i++) {
    if (!dut->l1tlb[i].valid) {
      continue;
    }
    dut->l1tlb[i].valid = 0;
    PTE pte;
    uint64_t paddr;
    uint8_t difftest_level;
    r_s2xlate r_s2;
    bool isNapot = false;

    Satp *satp = (Satp *)&dut->l1tlb[i].satp;
    Satp *vsatp = (Satp *)&dut->l1tlb[i].vsatp;
    Hgatp *hgatp = (Hgatp *)&dut->l1tlb[i].hgatp;
    uint8_t hasS2xlate = dut->l1tlb[i].s2xlate != noS2xlate;
    uint8_t onlyS2 = dut->l1tlb[i].s2xlate == onlyStage2;
    uint8_t hasAllStage = dut->l1tlb[i].s2xlate == allStage;
    uint64_t pg_base = (hasS2xlate ? vsatp->ppn : satp->ppn) << 12;
    int mode = hasS2xlate ? vsatp->mode : satp->mode;
    int max_level = mode == 8 ? 2 : 3;
    if (onlyS2) {
      r_s2 = do_s2xlate(hgatp, dut->l1tlb[i].vpn << 12);
      pte = r_s2.pte;
      difftest_level = r_s2.level;
    } else {
      for (difftest_level = max_level; difftest_level >= 0; difftest_level--) {
        paddr = pg_base + VPNi(dut->l1tlb[i].vpn, difftest_level) * sizeof(uint64_t);
        if (hasAllStage) {
          r_s2 = do_s2xlate(hgatp, paddr);
          uint64_t pg_mask = ((1ull << VPNiSHFT(r_s2.level)) - 1);
          if (r_s2.level == 0 && r_s2.pte.n) {
            pg_mask = ((1ull << NAPOTSHFT) - 1);
          }
          pg_base = (r_s2.pte.ppn << 12 & ~pg_mask) | (paddr & pg_mask & ~PAGE_MASK);
          paddr = pg_base | (paddr & PAGE_MASK);
        }
        read_goldenmem(paddr, &pte.val, 8);
        pg_base = pte.ppn << 12;
        if (!pte.v || pte.r || pte.x || pte.w || difftest_level == 0) {
          break;
        }
      }
      if (difftest_level > 0 && pte.v) {
        uint64_t pg_mask = ((1ull << VPNiSHFT(difftest_level)) - 1);
        pg_base = (pte.ppn << 12 & ~pg_mask) | (dut->l1tlb[i].vpn << 12 & pg_mask & ~PAGE_MASK);
      } else if (difftest_level == 0 && pte.n) {
        isNapot = true;
        uint64_t pg_mask = ((1ull << NAPOTSHFT) - 1);
        pg_base = (pte.ppn << 12 & ~pg_mask) | (dut->l1tlb[i].vpn << 12 & pg_mask & ~PAGE_MASK);
      }
      if (hasAllStage && pte.v) {
        r_s2 = do_s2xlate(hgatp, pg_base);
        pte = r_s2.pte;
        difftest_level = r_s2.level;
        if (difftest_level == 0 && pte.n) {
          isNapot = true;
        }
      }
    }
    if (isNapot) {
      dut->l1tlb[i].ppn = dut->l1tlb[i].ppn >> 4 << 4;
      pte.difftest_ppn = pte.difftest_ppn >> 4 << 4;
    } else {
      dut->l1tlb[i].ppn = dut->l1tlb[i].ppn >> difftest_level * 9 << difftest_level * 9;
    }
    if (pte.difftest_ppn != dut->l1tlb[i].ppn) {
      Info("Warning: l1tlb resp test of core %d index %d failed! vpn = %lx\n", id, i, dut->l1tlb[i].vpn);
      Info("  REF commits pte.val: 0x%lx, dut s2xlate: %d\n", pte.val, dut->l1tlb[i].s2xlate);
      Info("  REF commits ppn 0x%lx, DUT commits ppn 0x%lx\n", pte.difftest_ppn, dut->l1tlb[i].ppn);
      Info("  REF commits perm 0x%02x, level %d, pf %d\n", pte.difftest_perm, difftest_level, !pte.difftest_v);
      return 0;
    }
  }
#endif // CONFIG_DIFFTEST_L1TLBEVENT
  return 0;
}

int Difftest::do_l2tlb_check() {
#ifdef CONFIG_DIFFTEST_L2TLBEVENT
  for (int i = 0; i < CONFIG_DIFF_L2TLB_WIDTH; i++) {
    if (!dut->l2tlb[i].valid) {
      continue;
    }
    dut->l2tlb[i].valid = 0;
    Satp *satp = (Satp *)&dut->l2tlb[i].satp;
    Satp *vsatp = (Satp *)&dut->l2tlb[i].vsatp;
    Hgatp *hgatp = (Hgatp *)&dut->l2tlb[i].hgatp;
    PTE pte;
    r_s2xlate r_s2;
    r_s2xlate check_s2;
    uint64_t paddr;
    uint8_t difftest_level;
    for (int j = 0; j < 8; j++) {
      if (dut->l2tlb[i].valididx[j]) {
        uint8_t hasS2xlate = dut->l2tlb[i].s2xlate != noS2xlate;
        uint8_t onlyS2 = dut->l2tlb[i].s2xlate == onlyStage2;
        uint64_t pg_base = (hasS2xlate ? vsatp->ppn : satp->ppn) << 12;
        int mode = hasS2xlate ? vsatp->mode : satp->mode;
        int max_level = mode == 8 ? 2 : 3;
        if (onlyS2) {
          r_s2 = do_s2xlate(hgatp, dut->l2tlb[i].vpn << 12);
          uint64_t pg_mask = ((1ull << VPNiSHFT(r_s2.level)) - 1);
          uint64_t s2_pg_base = r_s2.pte.ppn << 12;
          pg_base = (s2_pg_base & ~pg_mask) | (paddr & pg_mask & ~PAGE_MASK);
          paddr = pg_base | (paddr & PAGE_MASK);
        }
        for (difftest_level = max_level; difftest_level >= 0; difftest_level--) {
          paddr = pg_base + VPNi(dut->l2tlb[i].vpn + j, difftest_level) * sizeof(uint64_t);
          if (hasS2xlate) {
            r_s2 = do_s2xlate(hgatp, paddr);
            uint64_t pg_mask = ((1ull << VPNiSHFT(r_s2.level)) - 1);
            pg_base = (r_s2.pte.ppn << 12 & ~pg_mask) | (paddr & pg_mask & ~PAGE_MASK);
            paddr = pg_base | (paddr & PAGE_MASK);
          }
          read_goldenmem(paddr, &pte.val, 8);
          if (!pte.v || pte.r || pte.x || pte.w || difftest_level == 0) {
            break;
          }
          pg_base = pte.ppn << 12;
        }

        if (hasS2xlate) {
          r_s2 = do_s2xlate(hgatp, pg_base);
          if (dut->l2tlb[i].pteidx[j])
            check_s2 = r_s2;
        }
        bool difftest_gpf = !r_s2.pte.v || (!r_s2.pte.r && r_s2.pte.w);
        bool difftest_pf = !pte.v || (!pte.r && pte.w);
        bool s1_check_fail = pte.difftest_ppn != dut->l2tlb[i].ppn[j] || pte.difftest_perm != dut->l2tlb[i].perm ||
                             pte.difftest_pbmt != dut->l2tlb[i].pbmt || difftest_level != dut->l2tlb[i].level ||
                             difftest_pf != dut->l2tlb[i].pf;
        bool s2_check_fail = hasS2xlate ? r_s2.pte.difftest_ppn != dut->l2tlb[i].s2ppn ||
                                              r_s2.pte.difftest_perm != dut->l2tlb[i].g_perm ||
                                              r_s2.pte.difftest_pbmt != dut->l2tlb[i].g_pbmt ||
                                              r_s2.level != dut->l2tlb[i].g_level || difftest_gpf != dut->l2tlb[i].gpf
                                        : false;
        if (s1_check_fail || s2_check_fail) {
          Info("Warning: L2TLB resp test of core %d index %d sector %d failed! vpn = %lx\n", id, i, j,
               dut->l2tlb[i].vpn + j);
          Info("  REF commits ppn 0x%lx, perm 0x%02x, level %d, pf %d\n", pte.difftest_ppn, pte.difftest_perm,
               difftest_level, difftest_pf);
          if (hasS2xlate)
            Info("      s2_ppn 0x%lx, g_perm 0x%02x, g_level %d, gpf %d\n", r_s2.pte.difftest_ppn,
                 r_s2.pte.difftest_perm, r_s2.level, difftest_gpf);
          Info("  DUT commits ppn 0x%lx, perm 0x%02x, level %d, pf %d\n", dut->l2tlb[i].ppn[j], dut->l2tlb[i].perm,
               dut->l2tlb[i].level, dut->l2tlb[i].pf);
          if (hasS2xlate)
            Info("      s2_ppn 0x%lx, g_perm 0x%02x, g_level %d, gpf %d\n", dut->l2tlb[i].s2ppn, dut->l2tlb[i].g_perm,
                 dut->l2tlb[i].g_level, dut->l2tlb[i].gpf);
          return 1;
        }
      }
    }
  }
#endif // CONFIG_DIFFTEST_L1TLBEVENT
  return 0;
}

inline int handle_atomic(int coreid, uint64_t atomicAddr, uint64_t atomicData, uint64_t atomicMask, uint8_t atomicFuop,
                         uint64_t atomicOut) {
  // We need to do atmoic operations here so as to update goldenMem
  if (!(atomicMask == 0xf || atomicMask == 0xf0 || atomicMask == 0xff)) {
    Info("Unrecognized mask: %lx\n", atomicMask);
    return 1;
  }

  if (atomicMask == 0xff) {
    uint64_t rs = atomicData; // rs2
    uint64_t t = atomicOut;   // original value
    uint64_t ret;
    uint64_t mem;
    read_goldenmem(atomicAddr, &mem, 8);
    if (mem != t && atomicFuop != 007 && atomicFuop != 003) { // ignore sc_d & lr_d
      Info("Core %d atomic instr mismatch goldenMem, mem: 0x%lx, t: 0x%lx, op: 0x%x, addr: 0x%lx\n", coreid, mem, t,
           atomicFuop, atomicAddr);
      return 1;
    }
    switch (atomicFuop) {
      case 002:
      case 003: ret = t; break;
      // if sc fails(aka atomicOut == 1), no update to goldenmem
      case 006:
      case 007:
        if (t == 1)
          return 0;
        ret = rs;
        break;
      case 012:
      case 013: ret = rs; break;
      case 016:
      case 017: ret = t + rs; break;
      case 022:
      case 023: ret = (t ^ rs); break;
      case 026:
      case 027: ret = t & rs; break;
      case 032:
      case 033: ret = t | rs; break;
      case 036:
      case 037: ret = ((int64_t)t < (int64_t)rs) ? t : rs; break;
      case 042:
      case 043: ret = ((int64_t)t > (int64_t)rs) ? t : rs; break;
      case 046:
      case 047: ret = (t < rs) ? t : rs; break;
      case 052:
      case 053: ret = (t > rs) ? t : rs; break;
      default: printf("Unknown atomic fuOpType: 0x%x\n", atomicFuop);
    }
    update_goldenmem(atomicAddr, &ret, atomicMask, 8);
  }

  if (atomicMask == 0xf || atomicMask == 0xf0) {
    uint32_t rs = (uint32_t)atomicData; // rs2
    uint32_t t = (uint32_t)atomicOut;   // original value
    uint32_t ret;
    uint32_t mem;
    uint64_t mem_raw;
    uint64_t ret_sel;
    atomicAddr = (atomicAddr & 0xfffffffffffffff8);
    read_goldenmem(atomicAddr, &mem_raw, 8);

    if (atomicMask == 0xf)
      mem = (uint32_t)mem_raw;
    else
      mem = (uint32_t)(mem_raw >> 32);

    if (mem != t && atomicFuop != 006 && atomicFuop != 002) { // ignore sc_w & lr_w
      Info("Core %d atomic instr mismatch goldenMem, rawmem: 0x%lx mem: 0x%x, t: 0x%x, op: 0x%x, addr: 0x%lx\n", coreid,
           mem_raw, mem, t, atomicFuop, atomicAddr);
      return 1;
    }
    switch (atomicFuop) {
      case 002:
      case 003: ret = t; break;
      // if sc fails(aka atomicOut == 1), no update to goldenmem
      case 006:
      case 007:
        if (t == 1)
          return 0;
        ret = rs;
        break;
      case 012:
      case 013: ret = rs; break;
      case 016:
      case 017: ret = t + rs; break;
      case 022:
      case 023: ret = (t ^ rs); break;
      case 026:
      case 027: ret = t & rs; break;
      case 032:
      case 033: ret = t | rs; break;
      case 036:
      case 037: ret = ((int32_t)t < (int32_t)rs) ? t : rs; break;
      case 042:
      case 043: ret = ((int32_t)t > (int32_t)rs) ? t : rs; break;
      case 046:
      case 047: ret = (t < rs) ? t : rs; break;
      case 052:
      case 053: ret = (t > rs) ? t : rs; break;
      default: printf("Unknown atomic fuOpType: 0x%x\n", atomicFuop);
    }
    ret_sel = ret;
    if (atomicMask == 0xf0)
      ret_sel = (ret_sel << 32);
    update_goldenmem(atomicAddr, &ret_sel, atomicMask, 8);
  }
  return 0;
}

void dumpGoldenMem(const char *banner, uint64_t addr, uint64_t time) {
#ifdef DEBUG_REFILL
  char buf[512];
  if (addr == 0) {
    return;
  }
  Info("============== %s =============== time = %ld\ndata: ", banner, time);
  for (int i = 0; i < 8; i++) {
    read_goldenmem(addr + i * 8, &buf, 8);
    Info("%016lx", *((uint64_t *)buf));
  }
  Info("\n");
#endif
}

#ifdef DEBUG_GOLDENMEM
int Difftest::do_golden_memory_update() {
  // Update Golden Memory info
  uint64_t cycleCnt = get_trap_event()->cycleCnt;
  static bool initDump = true;
  if (cycleCnt >= 100 && initDump) {
    initDump = false;
    dumpGoldenMem("Init", track_instr, cycleCnt);
  }

#ifdef CONFIG_DIFFTEST_UNCACHEMMSTOREEVENT
  for (int i = 0; i < CONFIG_DIFF_UNCACHE_MM_STORE_WIDTH; i++) {
    if (dut->uncache_mm_store[i].valid) {
      dut->uncache_mm_store[i].valid = 0;
      // the flag is set only in the case of multi-cores and uncache mm store
      uint8_t flag = NUM_CORES > 1 ? 1 : 0;
      update_goldenmem(dut->uncache_mm_store[i].addr, dut->uncache_mm_store[i].data, dut->uncache_mm_store[i].mask, 8,
                       flag);
      if (dut->uncache_mm_store[i].addr == track_instr) {
        dumpGoldenMem("Uncache MM Store", track_instr, cycleCnt);
      }
    }
  }
#endif // CONFIG_DIFFTEST_UNCACHEMMSTOREEVENT
#ifdef CONFIG_DIFFTEST_SBUFFEREVENT
  for (int i = 0; i < CONFIG_DIFF_SBUFFER_WIDTH; i++) {
    if (dut->sbuffer[i].valid) {
      dut->sbuffer[i].valid = 0;
      update_goldenmem(dut->sbuffer[i].addr, dut->sbuffer[i].data, dut->sbuffer[i].mask, 64);
      if (dut->sbuffer[i].addr == track_instr) {
        dumpGoldenMem("Store", track_instr, cycleCnt);
      }
    }
  }
#endif // CONFIG_DIFFTEST_SBUFFEREVENT

#ifdef CONFIG_DIFFTEST_ATOMICEVENT
  if (dut->atomic.valid) {
    dut->atomic.valid = 0;
    int ret =
        handle_atomic(id, dut->atomic.addr, dut->atomic.data, dut->atomic.mask, dut->atomic.fuop, dut->atomic.out);
    if (dut->atomic.addr == track_instr) {
      dumpGoldenMem("Atmoic", track_instr, cycleCnt);
    }
    if (ret)
      return ret;
  }
#endif // CONFIG_DIFFTEST_ATOMICEVENT

  return 0;
}
#endif

#ifdef CONFIG_DIFFTEST_STOREEVENT
void Difftest::store_event_record() {
  for (int i = 0; i < CONFIG_DIFF_STORE_WIDTH; i++) {
    if (dut->store[i].valid) {
      store_event_queue.push(dut->store[i]);
      dut->store[i].valid = 0;
    }
  }
}
#endif

#ifdef CONFIG_DIFFTEST_SQUASH
#ifdef CONFIG_DIFFTEST_LOADEVENT
void Difftest::load_event_record() {
  for (int i = 0; i < CONFIG_DIFF_LOAD_WIDTH; i++) {
    if (dut->load[i].valid) {
      load_event_queue.push(dut->load[i]);
      dut->load[i].valid = 0;
    }
  }
}
#endif // CONFIG_DIFFTEST_LOADEVENT
#endif // CONFIG_DIFFTEST_SQUASH

#ifdef CONFIG_DIFFTEST_CMOINVALEVENT
void Difftest::cmo_inval_event_record() {
  if (dut->cmo_inval.valid) {
    goldenmem_cmo_inval(dut->cmo_inval.addr);
    dut->cmo_inval.valid = 0;
  }
}
#endif // CONFIG_DIFFTEST_CMOINVALEVENT

int Difftest::check_timeout() {
  uint64_t cycleCnt = get_trap_event()->cycleCnt;
  // check whether there're any commits since the simulation starts
  if (!has_commit && cycleCnt > last_commit + first_commit_limit) {
    Info("The first instruction of core %d at 0x%lx does not commit after %lu cycles.\n", id, FIRST_INST_ADDRESS,
         first_commit_limit);
    display();
    return 1;
  }

  // NOTE: the WFI instruction may cause the CPU to halt for more than `stuck_limit` cycles.
  // We update the `last_commit` if the CPU has a WFI instruction
  // to allow the CPU to run at most `stuck_limit` cycles after WFI resumes execution.
  if (has_wfi()) {
    update_last_commit();
  }

  // check whether there're any commits in the last `stuck_limit` cycles
  if (has_commit && cycleCnt > last_commit + stuck_commit_limit) {
    Info(
        "No instruction of core %d commits for %lu cycles, maybe get stuck\n"
        "(please also check whether a fence.i instruction requires more than %lu cycles to flush the icache)\n",
        id, stuck_commit_limit, stuck_commit_limit);
    Info("Let REF run one more instruction.\n");
    proxy->ref_exec(1);
    display();
    return 1;
  }

  return 0;
}

int Difftest::update_delayed_writeback() {
#define CHECK_DELAYED_WB(wb, delayed, n, regs_name)                                                \
  do {                                                                                             \
    for (int i = 0; i < n; i++) {                                                                  \
      auto delay = dut->wb + i;                                                                    \
      if (delay->valid) {                                                                          \
        delay->valid = false;                                                                      \
        if (!delayed[delay->address]) {                                                            \
          display();                                                                               \
          Info("Delayed writeback at %s has already been committed\n", regs_name[delay->address]); \
          raise_trap(STATE_ABORT);                                                                 \
          return 1;                                                                                \
        }                                                                                          \
        if (delay->nack) {                                                                         \
          if (delayed[delay->address] > delay_wb_limit) {                                          \
            delayed[delay->address] -= 1;                                                          \
          }                                                                                        \
        } else {                                                                                   \
          delayed[delay->address] = 0;                                                             \
        }                                                                                          \
        progress = true;                                                                           \
      }                                                                                            \
    }                                                                                              \
  } while (0);

#ifdef CONFIG_DIFFTEST_ARCHINTDELAYEDUPDATE
  CHECK_DELAYED_WB(regs_int_delayed, delayed_int, CONFIG_DIFF_REGS_INT_DELAYED_WIDTH, regs_name_int)
#endif // CONFIG_DIFFTEST_ARCHINTDELAYEDUPDATE
#ifdef CONFIG_DIFFTEST_ARCHFPDELAYEDUPDATE
  CHECK_DELAYED_WB(regs_fp_delayed, delayed_fp, CONFIG_DIFF_REGS_FP_DELAYED_WIDTH, regs_name_fp)
#endif // CONFIG_DIFFTEST_ARCHFPDELAYEDUPDATE
  return 0;
}

int Difftest::apply_delayed_writeback() {
#define APPLY_DELAYED_WB(delayed, regs, regs_name)                           \
  do {                                                                       \
    static const int m = delay_wb_limit;                                     \
    for (int i = 0; i < 32; i++) {                                           \
      if (delayed[i]) {                                                      \
        if (delayed[i] > m) {                                                \
          display();                                                         \
          Info("%s is delayed for more than %d cycles.\n", regs_name[i], m); \
          raise_trap(STATE_ABORT);                                           \
          return 1;                                                          \
        }                                                                    \
        delayed[i]++;                                                        \
        dut->regs.value[i] = proxy->regs.value[i];                           \
      }                                                                      \
    }                                                                        \
  } while (0);

#ifdef CONFIG_DIFFTEST_ARCHINTDELAYEDUPDATE
  APPLY_DELAYED_WB(delayed_int, regs_int, regs_name_int)
#endif // CONFIG_DIFFTEST_ARCHINTDELAYEDUPDATE
#ifdef CONFIG_DIFFTEST_ARCHFPDELAYEDUPDATE
  APPLY_DELAYED_WB(delayed_fp, regs_fp, regs_name_fp)
#endif // CONFIG_DIFFTEST_ARCHFPDELAYEDUPDATE
  return 0;
}

void Difftest::raise_trap(int trapCode) {
  dut->trap.hasTrap = 1;
  dut->trap.code = trapCode;
}

#ifdef CONFIG_DIFFTEST_NONREGINTERRUPTPENDINGEVENT
void Difftest::do_non_reg_interrupt_pending() {
  if (dut->non_reg_interrupt_pending.valid) {
    const uint64_t raw_pending = dut->non_reg_interrupt_pending.rawPending & kNonRegInterruptSnapshotMask;
    const uint64_t raw_mask = dut->non_reg_interrupt_pending.rawPendingMask & kNonRegInterruptSnapshotMask;
    const uint64_t ownership_added = raw_mask & ~latest_non_reg_interrupt_pending_mask;
    const uint64_t ownership_removed = latest_non_reg_interrupt_pending_mask & ~raw_mask;
    const uint64_t ownership_changed = ownership_added | ownership_removed;
    const uint64_t source_update_mask = (
        (dut->non_reg_interrupt_pending.platformIRPMeipValid ? kMei : 0) |
        (dut->non_reg_interrupt_pending.platformIRPMtipValid ? kMti : 0) |
        (dut->non_reg_interrupt_pending.platformIRPMsipValid ? kMsi : 0) |
        (dut->non_reg_interrupt_pending.platformIRPSeipValid ? kSei : 0) |
        (dut->non_reg_interrupt_pending.platformIRPStipValid ? kSti : 0) |
        (dut->non_reg_interrupt_pending.platformIRPVseipValid ? kVsei : 0) |
        (dut->non_reg_interrupt_pending.platformIRPVstipValid ? kVsti : 0) |
        (dut->non_reg_interrupt_pending.localCounterOverflowInterruptReqValid ? kLcofi : 0)) & raw_mask;

    const uint64_t cache_update_mask = source_update_mask | ownership_changed;
    latest_non_reg_interrupt_pending =
        (latest_non_reg_interrupt_pending & ~cache_update_mask) | (raw_pending & cache_update_mask);
    latest_non_reg_interrupt_pending_mask = raw_mask;
    latest_software_seip = dut->non_reg_interrupt_pending.softwareSeip;

    auto ip = make_pending_update(ownership_added | source_update_mask, raw_pending);
    if (source_update_mask & kLcofi) {
      ++lcofi_update_epoch;
    }
    ip.fromAIAMeipValid = dut->non_reg_interrupt_pending.fromAIAMeipValid;
    ip.fromAIAMeip = dut->non_reg_interrupt_pending.fromAIAMeip;
    ip.fromAIASeipValid = dut->non_reg_interrupt_pending.fromAIASeipValid;
    ip.fromAIASeip = dut->non_reg_interrupt_pending.fromAIASeip;
    ip.stimeValid = dut->non_reg_interrupt_pending.stimeValid;
    ip.stime = dut->non_reg_interrupt_pending.stime;

    if (dut->non_reg_interrupt_pending.csrReadSnapshotFlush) {
      csr_read_snapshot.valid = false;
    }
    if (dut->non_reg_interrupt_pending.csrReadSnapshotValid &&
        !dut->non_reg_interrupt_pending.csrReadSnapshotFlush) {
      if (csr_read_snapshot.valid) {
        Info("Core %d overlapping CSR pending snapshots\n", this->id);
        csr_snapshot_mismatch = true;
      } else {
        csr_read_snapshot.valid = true;
        csr_read_snapshot.pending = raw_pending;
        csr_read_snapshot.mask = raw_mask;
        csr_read_snapshot.lcofi_epoch = lcofi_update_epoch;
      }
    }

    proxy->non_reg_interrupt_pending(ip);
    if (ownership_removed) {
      // time->sync() is processed after STIP inside the first update. Apply
      // released bits last so an STCE falling edge exposes retained software state.
      auto released = make_pending_update(ownership_removed, raw_pending);
      proxy->non_reg_interrupt_pending(released);
    }
    dut->non_reg_interrupt_pending.valid = 0;
  }
}
#endif

#ifdef CONFIG_DIFFTEST_MHPMEVENTOVERFLOWEVENT
void Difftest::do_mhpmevent_overflow() {
  if (dut->mhpmevent_overflow.valid) {
    proxy->mhpmevent_overflow(dut->mhpmevent_overflow.mhpmeventOverflow);
    dut->mhpmevent_overflow.valid = 0;
  }
}
#endif

#ifdef CONFIG_DIFFTEST_CRITICALERROREVENT
void Difftest::do_raise_critical_error() {
  if (dut->critical_error.valid) {
    bool ref_critical_error = proxy->raise_critical_error();
    if (ref_critical_error == dut->critical_error.criticalError) {
      Info("Core %d dump: " ANSI_COLOR_RED
           "HIT CRITICAL ERROR: please check if software cause a double trap. \n" ANSI_COLOR_RESET,
           this->id);
      raise_trap(STATE_GOODTRAP);
    } else {
      display();
      Info("Core %d dump: DUT critical_error diff REF \n", this->id);
      raise_trap(STATE_ABORT);
    }
  }
}
#endif

#ifdef CONFIG_DIFFTEST_SYNCAIAEVENT
void Difftest::do_sync_aia() {
  if (dut->sync_aia.valid) {
    struct FromAIA aia;
    aia.mtopei = dut->sync_aia.mtopei;
    aia.stopei = dut->sync_aia.stopei;
    aia.vstopei = dut->sync_aia.vstopei;
    aia.hgeip = dut->sync_aia.hgeip;
    proxy->sync_aia(aia);
    dut->sync_aia.valid = 0;
  }
}
#endif

#ifdef CONFIG_DIFFTEST_SYNCCUSTOMMFLUSHPWREVENT
void Difftest::do_sync_custom_mflushpwr() {
  if (dut->sync_custom_mflushpwr.valid) {
    proxy->sync_custom_mflushpwr(dut->sync_custom_mflushpwr.l2FlushDone);
    dut->sync_custom_mflushpwr.valid = 0;
  }
}
#endif

#ifdef CONFIG_DIFFTEST_VECFOFSYNCEVENT
void Difftest::do_vec_fof_sync() {
  for (int idx = 0; idx < CONFIG_DIFF_VEC_FOF_SYNC_WIDTH; idx++) {
    if (!dut->vec_fof_sync[idx].valid)
      continue;
    struct VecFofSyncInfo info;
    info.fofVl = dut->vec_fof_sync[idx].fofVl;
    info.fofEew = dut->vec_fof_sync[idx].fofEew;
    info.vdNum = dut->vec_fof_sync[idx].vdNum;
    info.vdRegNum = dut->vec_fof_sync[idx].vdRegNum;
    info.vdBase = dut->vec_fof_sync[idx].vdBase;
    memset(info.data, 0, sizeof(info.data));
#ifdef CONFIG_DIFFTEST_ARCHVECREGSTATE
    uint32_t totalRegs = info.vdNum * info.vdRegNum;
    for (uint32_t vd = 0; vd < totalRegs; vd++) {
      uint32_t regIdx = info.vdBase + vd;
      if (regIdx >= 32 || 2 * vd + 1 >= 16) break;
      info.data[2 * vd] = dut->regs_vec.value[regIdx * 2];
      info.data[2 * vd + 1] = dut->regs_vec.value[regIdx * 2 + 1];
    }
#endif
    proxy->vec_fof_sync(&info);
    dut->vec_fof_sync[idx].valid = 0;
  }
}
#endif

void Difftest::display() {
  Info("\n==============  In the last commit group  ==============\n");
  Info("the first commit instr pc of DUT is 0x%016lx\nthe first commit instr pc of REF is 0x%016lx\n",
       dut_commit_first_pc, ref_commit_first_pc);

  state->display(this->id);

  Info("\n==============  REF Regs  ==============\n");
  fflush(stdout);
  proxy->ref_reg_display();
  Info("privilegeMode: %lu\n", dut->csr.privilegeMode);
}

void CommitTrace::display(bool use_spike) {
  Info("%s pc %016lx inst %08x", get_type(), pc, inst);
  display_custom();
  if (use_spike) {
    Info(" %s", spike_dasm(inst));
  }
}

void CommitTrace::display_line(int index, bool use_spike, bool is_retire) {
  Info("[%02d] ", index);
  display(use_spike);
  Info("%s\n", is_retire ? " <--" : "");
}

void Difftest::display_stats() {
  auto trap = get_trap_event();
  uint64_t instrCnt = trap->instrCnt;
  uint64_t cycleCnt = trap->cycleCnt;
  double ipc = (double)instrCnt / cycleCnt;
  Info(ANSI_COLOR_MAGENTA "Core-%d instrCnt = %'" PRIu64 ", cycleCnt = %'" PRIu64 ", IPC = %lf\n" ANSI_COLOR_RESET,
       this->id, instrCnt, cycleCnt, ipc);
}

void DiffState::display(int coreid) {
  Info("\n============== Commit Group Trace (Core %d) ==============\n", coreid);
  int group_index = 0;
  while (!retire_group_queue.empty()) {
    auto retire_group = retire_group_queue.front();
    auto pc = retire_group.first;
    auto cnt = retire_group.second;
    retire_group_queue.pop();
    Info("commit group [%02d]: pc %010lx cmtcnt %d%s\n", group_index, pc, cnt,
         retire_group_queue.empty() ? " <--" : "");
    group_index++;
  }

  Info("\n============== Commit Instr Trace ==============\n");
  int commit_index = 0;
  while (!commit_trace.empty()) {
    CommitTrace *trace = commit_trace.front();
    commit_trace.pop();
    trace->display_line(commit_index, use_spike, commit_trace.empty());
    commit_index++;
  }

  fflush(stdout);
}

DiffState::DiffState() : use_spike(spike_valid()) {}
