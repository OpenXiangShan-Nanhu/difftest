package difftest

/*=================================================================
  1.simv/emu/gsim
    monitor通过xmr获取各模块信号
    GatewayEndpoint中包含各种DPIC
  2.fpga
    monitor通过xmr获取各模块信号
    GatewayEndpoint中包含Endpoint、Batch、XDMA等ip的集成等
  3.pldm
    monitor通过xmr获取各模块信号
    GatewayEndpoint中包含Endpoint、Batch等组件
=================================================================*/

import chisel3._
import chisel3.util._
import chisel3.experimental._
import difftest._
import difftest.common._
import difftest.monitor.CoreGateway
import difftest.gateway._
import difftest.monitor._
import difftest.fpga._
import chisel3.experimental.dataview.DataViewable
import difftest.gateway.Gateway.isFPGA

class LNDifftestTop(
  coreNum       : Int,
  MEM_BASE      : String
) extends Module {
  
  val monitorSeq = Seq.tabulate(coreNum)(idx => Module(new DifftestMonitor(idx, s"XlnFpgaTop.soc", "")))
  for(idx <- 0 until coreNum) {
    monitorSeq(idx).suggestName(s"difftest_core_gateway_$idx")
    dontTouch(monitorSeq(idx).io)
  }

  private val difftestMacros = Seq(
    s"DEBUG_MEM_BASE 0x${MEM_BASE}",
    s"DEFAULT_EMU_RAM_SIZE 0x${(16L * 1024 * 1024 * 1024).toHexString}UL",
    s"NUM_CORES ${coreNum}"
  )
  private val probeSeq = monitorSeq.map(_.io.probe.getInstanceSeq).flatten

  val fpgaHostReset = WireInit(false.B)
  val fpgaDiffEnable = WireInit(true.B)

  val gateway = withClockAndReset(clock, (reset.asBool || fpgaHostReset).asAsyncReset) {
    CoreGateway.collect(probeSeq)
  }

  val difftest = IO(new DifftestTopIO)
  difftest.exit := gateway.exit.getOrElse(0.U)
  difftest.step := gateway.step.getOrElse(0.U)
  difftest.uart := DontCare

  val difftest_fpga = IO(new DifftestFpgaIO(gateway.fpgaIO.isDefined))

  difftest_fpga.from_host_axis.foreach(_.ready := true.B)

  // Required signals for LogPerfControl
  val timer = RegInit(0.U(64.W))
  timer := timer + 1.U
  dontTouch(timer)

  val log_enable = difftest.logCtrl.enable(timer)
  dontTouch(log_enable)

  gateway.refClock.foreach(_ := difftest_fpga.ref_clock.getOrElse(false.B))

  // IO: difftest_fpga_*
  gateway.fpgaIO.foreach { fpgaIO =>
    val cfgResetReq = WireInit(false.B)
    val cfgReset = withClockAndReset(difftest_fpga.ref_clock, difftest_fpga.ref_reset) {
      val cnt = RegInit(0.U(4.W))
      when(cfgResetReq && cnt === 0.U) {
        cnt := 10.U
      }.elsewhen(cnt =/= 0.U) {
        cnt := cnt - 1.U
      }
      cnt =/= 0.U
    }
    withClockAndReset(difftest_fpga.ref_clock.get, difftest_fpga.ref_reset.get || cfgReset) {
      val cfg = Module(new XDMAConfigBar)
      cfgResetReq := cfg.io.cfgReset
      val ctrl = cfg.io.hostCtrl
      val host = Module(new HostEndpoint(fpgaIO.bits.getWidth, Gateway.hostAxisWidth))

      host.io.difftest.valid := fpgaIO.valid && ctrl.diffEnable
      host.io.difftest.bits := fpgaIO.bits
      fpgaIO.ready := Mux(ctrl.diffEnable, host.io.difftest.ready, true.B)
      host.io.pcie_clock := difftest_fpga.pcie_clock.get

      host.io.to_host_axis <> difftest_fpga.to_host_axis.get

      cfg.io.axilite <> difftest_fpga.config_axilite.get

      difftest_fpga.host_ctrl.foreach(_ := ctrl)
      fpgaHostReset := ctrl.reset
      fpgaDiffEnable := ctrl.diffEnable
      gateway.fpgaSquashEnable.foreach(_ := ctrl.enableSquash)

      cfg.io.memCtrl.memStatus := 0.U
      difftest_fpga.from_host_axis.foreach(_.ready := false.B)

      val mem_axi = WireInit(difftest_fpga.noc_ddr_port.get)
      val memCtrl = Module(new DifftestMemCtrl(mem_axi, Gateway.hostAxisWidth, baseAddr = 0x80000000L))

      memCtrl.io.ctrl <> cfg.io.memCtrl
      memCtrl.io.pcie_clock := difftest_fpga.pcie_clock.get
      memCtrl.io.h2c <> difftest_fpga.from_host_axis.get
      memCtrl.io.cpu <> difftest_fpga.noc_ddr_port.get
      difftest_fpga.to_ddr.foreach(_ <> memCtrl.io.mem)
    }

    gateway.clockEnable.foreach { clockEnable =>
      difftest_fpga.clock_enable.foreach(_ := clockEnable || fpgaHostReset || !fpgaDiffEnable)
    }
  }

  DifftestModule.generateCppHeader(
    "XiangShan",
    gateway.instances,
    gateway.cppMacros ++ difftestMacros,
    gateway.structPacked.getOrElse(false),
    gateway.structAligned.getOrElse(false),
  )
  if (gateway.cppExtModule.getOrElse(false)) {
    DifftestModule.generateCppExtModules()
  }
  FileControl.write(gateway.vMacros.map(m => s"`define $m"), "DifftestMacros.svh")
  // Profile.generateJson(cpu, interfaces.toSeq)
}

class DifftestFpgaIO(hasFpgaIO: Boolean) extends Bundle {
  val to_host_axis    = if(hasFpgaIO) Some(new AXI4Stream(Gateway.hostAxisWidth))                        else None
  val from_host_axis  = if(hasFpgaIO) Some(Flipped(new AXI4Stream(Gateway.hostAxisWidth)))            else None
  val config_axilite  = if(hasFpgaIO) Some(Flipped(new AXI4LiteBundle(32, 32)))                       else None
  val pcie_clock      = if(hasFpgaIO) Some(Input(Clock()))                                            else None
  val ref_clock       = if(hasFpgaIO) Some(Input(Clock()))                                            else None
  val ref_reset       = if(hasFpgaIO) Some(Input(Bool()))                                             else None
  val host_ctrl       = if(hasFpgaIO) Some(Output(new XDMAHostCtrlIO))                                else None
  val noc_ddr_port    = if(hasFpgaIO) Some(Flipped(new AXI4Bundle(addrWidth = 34, dataWidth = 256)))  else None
  val to_ddr          = if(hasFpgaIO) Some(new AXI4Bundle(addrWidth = 34, dataWidth = 256))              else None
  val clock_enable    = if(hasFpgaIO) Some(Output(Bool()))                                            else None
}

trait DifftestIOHelper {
  def difftestIODrv: DifftestFpgaIO 
  lazy val difftest_io = IO(new DifftestFpgaIO(true))
  def connectDifftestTopIO(): Unit = {
    difftest_io <> difftestIODrv
    dontTouch(difftest_io)
  }
}