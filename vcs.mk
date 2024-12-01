#***************************************************************************************
# Copyright (c) 2024 Beijing Institute of Open Source Chip (BOSC)
# Copyright (c) 2020-2024 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2020-2021 Peng Cheng Laboratory
#
# DiffTest is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
#
# See the Mulan PSL v2 for more details.
#***************************************************************************************

VCS           = vcs
VCS_TOP       = tb_top
VCS_DIR   	  = $(SIM_DIR)/vcs

VCS_TARGET    = $(abspath $(VCS_DIR)/comp/simv)
VCS_BUILD_DIR = $(abspath $(VCS_DIR)/comp/simv-compile)
VCS_RUN_DIR   = $(abspath $(VCS_DIR)/$(RUN_BIN))

VCS_CSRC_DIR   = $(abspath ./src/test/csrc/vcs)
VCS_CONFIG_DIR = $(abspath ./config)

VCS_CXXFILES  = $(SIM_CXXFILES) $(shell find $(VCS_CSRC_DIR) -name "*.cpp")
VCS_CXXFLAGS  = $(SIM_CXXFLAGS) -I$(VCS_CSRC_DIR) -DNUM_CORES=$(NUM_CORES)
VCS_LDFLAGS   = -Wl,--no-as-needed $(SIM_LDFLAGS) -lpthread -ldl

# DiffTest support
ifneq ($(NO_DIFF),1)
VCS_FLAGS    += +define+DIFFTEST
endif

ifeq ($(RELEASE),1)
VCS_FLAGS    += +define+SNPS_FAST_SIM_FFV +define+USE_RF_DEBUG
endif

# core soft rst
ifeq ($(WORKLOAD_SWITCH),1)
VCS_FLAGS    += +define+ENABLE_WORKLOAD_SWITCH
endif

ifeq ($(SYNTHESIS), 1)
VCS_FLAGS    += +define+SYNTHESIS +define+TB_NO_DPIC
else
ifeq ($(DISABLE_DIFFTEST_RAM_DPIC), 1)
VCS_FLAGS    += +define+DISABLE_DIFFTEST_RAM_DPIC
endif
ifeq ($(DISABLE_DIFFTEST_FLASH_DPIC), 1)
VCS_FLAGS    += +define+DISABLE_DIFFTEST_FLASH_DPIC
endif
endif

# if fsdb is considered
# CONSIDER_FSDB ?= 0
ifeq ($(CONSIDER_FSDB),1)
EXTRA = +define+CONSIDER_FSDB
# if VERDI_HOME is not set automatically after 'module load', please set manually.
ifndef VERDI_HOME
$(error VERDI_HOME is not set. Try whereis verdi, abandon /bin/verdi and set VERID_HOME manually)
else
NOVAS_HOME = $(VERDI_HOME)
NOVAS = $(NOVAS_HOME)/share/PLI/VCS/LINUX64
EXTRA += -P $(NOVAS)/novas.tab $(NOVAS)/pli.a
endif
endif

ifeq ($(VCS),verilator)
VCS_FLAGS += --exe --cc --main --top-module $(VCS_TOP) -Wno-WIDTH --max-num-width 150000
VCS_FLAGS += --instr-count-dpi 1 --timing +define+VERILATOR_5
VCS_FLAGS += -Mdir $(VCS_BUILD_DIR)  --compiler gcc
VCS_CXXFLAGS += -std=c++20
else
VCS_FLAGS += -full64 +v2k -timescale=1ns/1ns -sverilog -debug_access+all +lint=TFIPC-L
VCS_FLAGS += -lca -fgp -kdb +nospecify +notimingcheck
VCS_FLAGS += -Mdir=$(VCS_BUILD_DIR) -j200
VCS_FLAGS += +define+VCS
ifeq ($(ENABLE_XPROP),1)
VCS_FLAGS += -xprop
else
# randomize all undefined signals (instead of using X)
VCS_FLAGS += +vcs+initreg+random
endif
VCS_CXXFLAGS += -std=c++11 -static
endif

VCS_FLAGS += -o $(VCS_TARGET) -l $(VCS_DIR)/comp/vcs.log
ifneq ($(ENABLE_XPROP),1)
VCS_FLAGS += +define+RANDOMIZE_GARBAGE_ASSIGN
VCS_FLAGS += +define+RANDOMIZE_INVALID_ASSIGN
VCS_FLAGS += +define+RANDOMIZE_MEM_INIT
VCS_FLAGS += +define+RANDOMIZE_REG_INIT
endif
# manually set RANDOMIZE_DELAY to avoid VCS from incorrect random initialize
# NOTE: RANDOMIZE_DELAY must NOT be rounded to 0
VCS_FLAGS += +define+RANDOMIZE_DELAY=1
# SRAM lib defines
VCS_FLAGS += +define+UNIT_DELAY +define+no_warning
# search build for other missing verilog files
VCS_FLAGS += -y $(RTL_DIR) +libext+.v +libext+.sv
# search generated-src for verilog included files
VCS_FLAGS += +incdir+$(GEN_VSRC_DIR)
# C++ flags
VCS_FLAGS += -CFLAGS "$(VCS_CXXFLAGS)" -LDFLAGS "$(VCS_LDFLAGS)"
# enable fsdb dump
VCS_FLAGS += $(EXTRA)

VCS_VSRC_DIR = $(abspath ./src/test/vsrc/vcs)
VCS_VFILES   = $(SIM_VSRC) $(shell find $(VCS_VSRC_DIR) -name "*.v")
$(VCS_TARGET): $(SIM_TOP_V) $(VCS_CXXFILES) $(VCS_VFILES)
	$(shell if [ ! -e $(VCS_DIR)/comp ];then mkdir -p $(VCS_DIR)/comp; fi)
	$(VCS) $(VCS_FLAGS) $(SIM_TOP_V) $(VCS_CXXFILES) $(VCS_VFILES)
ifeq ($(VCS),verilator)
	$(MAKE) -s -C $(VCS_BUILD_DIR) -f V$(VCS_TOP).mk
endif

simv: $(VCS_TARGET)

RUN_BIN_DIR ?= $(ABS_WORK_DIR)/ready-to-run/
RUN_OPTS := +workload=$(RUN_BIN_DIR)/$(RUN_BIN)

ifeq ($(TRACE),1)
ifeq ($(CONSIDER_FSDB),1)
RUN_OPTS += +dump-wave=fsdb
else
RUN_OPTS += +dump-wave=vpd
endif
endif

ifneq ($(REF_SO),)
RUN_OPTS += +diff=$(REF_SO)
endif

ifeq ($(NO_DIFF),1)
RUN_OPTS += +no-diff
endif

RUN_OPTS += -no_save -assert finish_maxfail=30 -assert global_finish_maxfail=10000
RUN_OPTS += -fgp=num_threads:4,num_fsdb_threads:4

simv-run:
	$(shell if [ ! -e $(VCS_RUN_DIR) ]; then mkdir -p $(VCS_RUN_DIR); fi)
	touch $(VCS_RUN_DIR)/sim.log
	$(shell if [ -e $(VCS_RUN_DIR)/simv ]; then rm -f $(VCS_RUN_DIR)/simv; fi)
	$(shell if [ -e $(VCS_RUN_DIR)/simv.daidir ]; then rm -rf $(VCS_RUN_DIR)/simv.daidir; fi)
	ln -s $(VCS_TARGET) $(VCS_RUN_DIR)/simv
	ln -s $(VCS_DIR)/comp/simv.daidir $(VCS_RUN_DIR)/simv.daidir
	cd $(VCS_RUN_DIR) && (./simv $(RUN_OPTS) 2> assert.log | tee sim.log)

vcs-clean:
	rm -rf simv csrc DVEfiles simv.daidir stack.info.* ucli.key $(VCS_BUILD_DIR)
