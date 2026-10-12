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

#include "spikedasm.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static bool is_tested = false, is_valid = false;
static std::string dasm_program;
static char dasm_result[256];

static std::string shell_quote(const std::string &value) {
  std::string quoted = "'";
  for (char c : value) {
    quoted += c == '\'' ? "'\\''" : std::string(1, c);
  }
  return quoted + "'";
}

static bool execute_dasm_cmd(uint64_t inst) {
  char input[64];
  snprintf(input, sizeof(input), "printf 'DASM(%016" PRIx64 ")\\n' | ", inst);
  const std::string command = input + shell_quote(dasm_program) + " 2>/dev/null";
  dasm_result[0] = '\0';
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return false;
  }
  const bool got_output = fgets(dasm_result, sizeof(dasm_result), pipe) != nullptr;
  const int status = pclose(pipe);
  dasm_result[strcspn(dasm_result, "\r\n")] = '\0';
  return status == 0 && got_output && dasm_result[0] && !strstr(dasm_result, "DASM(");
}

bool spike_valid() {
  if (!is_tested) {
    is_tested = true;
    const char *configured = getenv("SPIKE_DASM");
    dasm_program = configured && configured[0] ? configured : "spike-dasm";
    is_valid = execute_dasm_cmd(0x00000013);
    if (is_valid) {
      printf("DiffTest disassembly enabled: %s\n", dasm_program.c_str());
    } else {
      printf("DiffTest disassembly unavailable: cannot run %s; set SPIKE_DASM to an executable "
             "spike-dasm path or add its directory to PATH\n", dasm_program.c_str());
    }
  }
  return is_valid;
}

const char *spike_dasm(uint64_t inst) {
  if (!spike_valid() || !execute_dasm_cmd(inst)) {
    snprintf(dasm_result, sizeof(dasm_result), "disassembly unavailable");
  }
  return dasm_result;
}

// This is an example usage. Not used.
static int usage() {
  uint64_t input = 0x10500073L;

  if (spike_valid()) {
    printf("%s", spike_dasm(input));
  }

  return 0;
}
