// See LICENSE.SiFive for license details.

#include "SimJTAG.h"
#include "common.h"
#include "remote_bitbang.h"
#include <cstdlib>

remote_bitbang_t *jtag;
bool enable_simjtag = false;
uint16_t remote_jtag_port = 23334;

void jtag_init() {
  jtag = new remote_bitbang_t(remote_jtag_port);
}

int jtag_tick(unsigned char *jtag_TCK, unsigned char *jtag_TMS, unsigned char *jtag_TDI, unsigned char *jtag_TRSTn,
              unsigned char jtag_TDO) {
  if (!enable_simjtag)
    return 0;
  if (!jtag) {
    jtag_init();
  }

  jtag->tick(jtag_TCK, jtag_TMS, jtag_TDI, jtag_TRSTn, jtag_TDO);

  return jtag->done() ? (jtag->exit_code() << 1 | 1) : 0;
}
