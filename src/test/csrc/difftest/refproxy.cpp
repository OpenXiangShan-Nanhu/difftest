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

#include "refproxy.h"
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <vector>

uint8_t *ref_golden_mem = NULL;
const char *difftest_ref_so = NULL;

#define SPIKE_ENV_VARIABLE "SPIKE_HOME"
#define SPIKE_SO_FILENAME  "difftest/build/riscv64-spike-so"

#define check_and_assert(func)                             \
  do {                                                     \
    if (!func) {                                           \
      printf("Failed loading %s: %s\n", #func, dlerror()); \
      fflush(stdout);                                      \
      assert(func);                                        \
    }                                                      \
  } while (0);


SpikeProxy::SpikeProxy(int coreid, size_t ram_size)
    : handler(load_handler()) {

#define GetRefFunc(dummy, ref_func, ret, ...) load_function<RefFunc((*), ret, __VA_ARGS__)>(#ref_func)
#define LoadRefFunc(this_func, ref_func, ret, ...)  this_func = GetRefFunc(, ref_func, ret, __VA_ARGS__);
#define CheckRefFunc(this_func, ref_func, ret, ...) check_and_assert(this_func);

  REF_BASE(LoadRefFunc)
  REF_BASE(CheckRefFunc)
  REF_OPTIONAL(LoadRefFunc)

  if (NUM_CORES > 1) {
    check_and_assert(ref_set_mhartid);
    ref_set_mhartid(coreid);

    check_and_assert(ref_put_gmaddr);
    check_and_assert(ref_golden_mem);
    ref_put_gmaddr(ref_golden_mem);
  }

  // set ram_size before ref_init()
  if (ram_size > 0) {
    check_and_assert(ref_set_ramsize);
    ref_set_ramsize(ram_size);
  }

  ref_init();
}

void *SpikeProxy::load_handler() {
  const char *env = SPIKE_ENV_VARIABLE;
  const char *file_path = SPIKE_SO_FILENAME;
  Info("The reference model is %s\n", difftest_ref_so);
  int mode = RTLD_LAZY | RTLD_DEEPBIND;
  void *so_handler = (NUM_CORES > 1) ? dlmopen(LM_ID_NEWLM, difftest_ref_so, mode) : dlopen(difftest_ref_so, mode);
  check_and_assert(so_handler);
  return so_handler;
}

template <typename T> T SpikeProxy::load_function(const char *func_name) {
  check_and_assert(handler);
  return reinterpret_cast<T>(dlsym(handler, func_name));
}

SpikeProxy::~SpikeProxy() {
  if (ref_close) {
    ref_close();
  }
  if (handler) {
    dlclose(handler);
  }
}

void SpikeProxy::regcpy(DiffTestState *dut) {
  memcpy(&regs_int, dut->regs_int.value, 32 * sizeof(uint64_t));
  memcpy(&regs_fp, dut->regs_fp.value, 32 * sizeof(uint64_t));
  memcpy(&csr, &dut->csr, sizeof(csr));
  pc = dut->commit[0].pc;
#ifdef CONFIG_DIFFTEST_HCSRSTATE
  memcpy(&hcsr, &dut->hcsr, sizeof(hcsr));
#endif // CONFIG_DIFFTEST_HCSRSTATE
#ifdef CONFIG_DIFFTEST_ARCHVECREGSTATE
  memcpy(&regs_vec, &dut->regs_vec.value, sizeof(regs_vec));
#endif // CONFIG_DIFFTEST_ARCHVECREGSTATE
#ifdef CONFIG_DIFFTEST_VECCSRSTATE
  memcpy(&vcsr, &dut->vcsr, sizeof(vcsr));
#endif // CONFIG_DIFFTEST_VECCSRSTATE
#ifdef CONFIG_DIFFTEST_FPCSRSTATE
  memcpy(&fcsr, &dut->fcsr, sizeof(fcsr));
#endif // CONFIG_DIFFTEST_FPCSRSTATE
#ifdef CONFIG_DIFFTEST_TRIGGERCSRSTATE
  memcpy(&triggercsr, &dut->triggercsr, sizeof(triggercsr));
#endif //CONFIG_DIFFTEST_TRIGGERCSRSTATE
  ref_regcpy(&regs_int, DUT_TO_REF, false);
};

int SpikeProxy::compare(DiffTestState *dut) {
#define PROXY_COMPARE(field) memcmp(&(dut->field), &(field), sizeof(field))

  const int results[] = {PROXY_COMPARE(regs_int),
                         PROXY_COMPARE(regs_fp),
#ifdef CONFIG_DIFFTEST_ARCHVECREGSTATE
                         PROXY_COMPARE(regs_vec),
#endif // CONFIG_DIFFTEST_ARCHVECREGSTATE
#ifdef CONFIG_DIFFTEST_VECCSRSTATE
                         PROXY_COMPARE(vcsr),
#endif // CONFIG_DIFFTEST_VECCSRSTATE
#ifdef CONFIG_DIFFTEST_FPCSRSTATE
                         PROXY_COMPARE(fcsr),
#endif // CONFIG_DIFFTEST_FPCSRSTATE
#ifdef CONFIG_DIFFTEST_HCSRSTATE
                         PROXY_COMPARE(hcsr),
#endif // CONFIG_DIFFTEST_HCSRSTATE
#ifdef CONFIG_DIFFTEST_TRIGGERCSRSTATE
                         PROXY_COMPARE(triggercsr),
#endif // CONFIG_DIFFTEST_TRIGGERCSRSTATE
                         PROXY_COMPARE(csr)

  };
  for (int i = 0; i < sizeof(results) / sizeof(int); i++) {
    if (results[i]) {
      // There may be some waive rules for CSRs
      if (i == sizeof(results) / sizeof(int) - 1) {
        // If mismatches are cleared, we sync the states back to REF.
        if (do_csr_waive(dut) && !PROXY_COMPARE(csr)) {
          sync(true);
          return 0;
        }
      }
      return 1;
    }
  }
  return 0;
};

void SpikeProxy::display(DiffTestState *dut) {
  if (dut) {
#define PROXY_COMPARE_AND_DISPLAY(field, field_names)                     \
  do {                                                                    \
    uint64_t *_ptr_dut = (uint64_t *)(&((dut)->field));                   \
    uint64_t *_ptr_ref = (uint64_t *)(&(field));                          \
    for (int i = 0; i < sizeof(field) / sizeof(uint64_t); i++) {          \
      if (_ptr_dut[i] != _ptr_ref[i]) {                                   \
        Info(                                                             \
            "%7s different at pc = 0x%010lx, right= 0x%016lx, "           \
            "wrong = 0x%016lx\n",                                         \
            field_names[i], dut->commit[0].pc, _ptr_ref[i], _ptr_dut[i]); \
      }                                                                   \
    }                                                                     \
  } while (0);

    PROXY_COMPARE_AND_DISPLAY(regs_int, regs_name_int)
    PROXY_COMPARE_AND_DISPLAY(regs_fp, regs_name_fp)
    PROXY_COMPARE_AND_DISPLAY(csr, regs_name_csr)
#ifdef CONFIG_DIFFTEST_HCSRSTATE
    PROXY_COMPARE_AND_DISPLAY(hcsr, regs_name_hcsr)
#endif // CONFIG_DIFFTEST_HCSRSTATE
#ifdef CONFIG_DIFFTEST_ARCHVECREGSTATE
    PROXY_COMPARE_AND_DISPLAY(regs_vec, regs_name_vec)
#endif // CONFIG_DIFFTEST_ARCHVECREGSTATE
#ifdef CONFIG_DIFFTEST_VECCSRSTATE
    PROXY_COMPARE_AND_DISPLAY(vcsr, regs_name_vec_csr)
#endif // CONFIG_DIFFTEST_VECCSRSTATE
#ifdef CONFIG_DIFFTEST_FPCSRSTATE
    PROXY_COMPARE_AND_DISPLAY(fcsr, regs_name_fp_csr)
#endif // CONFIG_DIFFTEST_FPCSRSTATE
#ifdef CONFIG_DIFFTEST_TRIGGERCSRSTATE
    PROXY_COMPARE_AND_DISPLAY(triggercsr, regs_name_triggercsr)
#endif // CONFIG_DIFFTEST_TRIGGERCSRSTATE
  } else {
    ref_reg_display();
  }
};

void SpikeProxy::flash_init(const uint8_t *flash_base, size_t size, const char *flash_bin) {
  if (load_flash_bin_v2) {
    load_flash_bin_v2(flash_base, size);
  } else if (load_flash_bin) {
    // This API is deprecated because flash_bin may be an empty pointer
    load_flash_bin(flash_bin, size);
  } else {
    std::cout << "Require load_flash_bin or load_flash_bin_v2 to initialize the flash" << std::endl;
    assert(0);
  }
}

bool SpikeProxy::do_csr_waive(DiffTestState *dut) {
#define CSR_WAIVE(field, mapping)                             \
  do {                                                        \
    uint64_t v = mapping(csr.field);                          \
    if (csr.field != dut->csr.field && dut->csr.field == v) { \
      csr.field = v;                                          \
      has_waive = true;                                       \
    }                                                         \
  } while (0);

  bool has_waive = false;
  return has_waive;
}
