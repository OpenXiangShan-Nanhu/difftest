/***************************************************************************************
* Copyright (c) 2020-2023 Institute of Computing Technology, Chinese Academy of Sciences
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

package difftest.common

import chisel3._
import chisel3.experimental.ExtModule
import chisel3.util._

private trait HasMemInitializer { this: ExtModule =>
  def mem_decl: String
  def mem_target: String

  private val initializer =
    """
      |  string bin_file;
      |  integer memory_image = 0, n_read = 0, byte_read = 1;
      |  byte data;
      |  initial begin
      |    if ($test$plusargs("workload")) begin
      |      $value$plusargs("workload=%s", bin_file);
      |      memory_image = $fopen(bin_file, "rb");
      |    if (memory_image == 0) begin
      |      $display("Error: failed to open %s", bin_file);
      |      $finish;
      |    end
      |    foreach (`MEM_TARGET[i]) begin
      |      if (byte_read == 0) break;
      |      for (integer j = 0; j < 32; j++) begin
      |        byte_read = $fread(data, memory_image);
      |        if (byte_read == 0) break;
      |        n_read += 1;
      |        `MEM_TARGET[i][j * 8 +: 8] = data;
      |      end
      |    end
      |    $fclose(memory_image);
      |    $display("%m: load %d bytes from %s.", n_read, bin_file);
      |  end
      |end
      |""".stripMargin
  val mem_init =
    s"""
       |`ifdef DISABLE_DIFFTEST_RAM_DPIC
       |`ifdef PALLADIUM
       |  initial $$ixc_ctrl("tb_import", "$$display");
       |`endif // PALLADIUM
       |$mem_decl
       |`define MEM_TARGET $mem_target
       |$initializer
       |`endif // DISABLE_DIFFTEST_RAM_DPIC
       |""".stripMargin
}

private trait HasReadPort { this: ExtModule =>
  val r = IO(new Bundle {
    val enable = Input(Bool())
    val index = Input(UInt(64.W))
    val data = Output(UInt(256.W))
  })

  val r_dpic =
    """
      |`ifndef DISABLE_DIFFTEST_RAM_DPIC
      |import "DPI-C" function longint difftest_ram_read(input longint rIdx);
      |`endif // DISABLE_DIFFTEST_RAM_DPIC
      |""".stripMargin

  val r_if =
    """
      |input             r_enable,
      |input      [63:0] r_index,
      |output reg [255:0] r_data,
      |""".stripMargin

  val r_func =
    """
      |  r_data = 0;
      |`ifndef DISABLE_DIFFTEST_RAM_DPIC
      |  if (r_enable) begin
      |    for (integer i = 0; i < 4; i++) begin
      |      r_data[i*64 +: 64] = difftest_ram_read((r_index << 2) + i);
      |    end
      |  end
      |`else
      |  if (r_enable) r_data = `MEM_TARGET[r_index];
      |`endif // DISABLE_DIFFTEST_RAM_DPIC
      |""".stripMargin

  val r_cpp_arg =
    """
      |uint8_t   r_enable,
      |uint64_t  r_index,
      |uint64_t& r_data""".stripMargin

  val r_cpp_func = "if (r_enable) r_data = difftest_ram_read(r_index);"

  def read(enable: Bool, index: UInt): UInt = {
    r.enable := enable
    r.index := index
    RegEnable(r.data, r.enable)
  }
}

private trait HasWritePort { this: ExtModule =>
  val w = IO(new Bundle {
    val enable = Input(Bool())
    val index = Input(UInt(64.W))
    val data = Input(UInt(256.W))
    val mask = Input(UInt(256.W))
  })

  val w_dpic =
    """
      |`ifndef DISABLE_DIFFTEST_RAM_DPIC
      |import "DPI-C" function void difftest_ram_write
      |(
      |  input  longint index,
      |  input  longint data,
      |  input  longint mask
      |);
      |`endif // DISABLE_DIFFTEST_RAM_DPIC
      |""".stripMargin

  val w_if =
    """
      |input         w_enable,
      |input  [63:0] w_index,
      |input  [255:0] w_data,
      |input  [255:0] w_mask,
      |""".stripMargin

  val w_func =
    """
      |`ifndef DISABLE_DIFFTEST_RAM_DPIC
      |if (w_enable) begin
      |  for (integer i = 0; i < 4; i++) begin
      |    difftest_ram_write((w_index << 2) + i, w_data[i*64 +: 64], w_mask[i*64 +: 64]);
      |  end
      |end
      |`else
      |if (w_enable) begin
      |  `MEM_TARGET[w_index] <= (w_data & w_mask) | (`MEM_TARGET[w_index] & ~w_mask);
      |end
      |`endif // DISABLE_DIFFTEST_RAM_DPIC
      |""".stripMargin

  val w_cpp_arg =
    """
      |uint8_t  w_enable,
      |uint64_t w_index,
      |uint64_t w_data,
      |uint64_t w_mask""".stripMargin

  val w_cpp_func = "if(w_enable) difftest_ram_write(w_index, w_data, w_mask);"

  def write(enable: Bool, index: UInt, data: UInt, mask: UInt): HasWritePort = {
    w.enable := enable
    w.index := index
    w.data := data
    w.mask := mask
    this
  }
}

abstract private class MemHelper extends ExtModule with HasExtModuleInline with HasMemInitializer

private class MemRWHelper extends MemHelper with HasReadPort with HasWritePort {
  val clock = IO(Input(Clock()))

  def read(i: Int, enable: Bool, index: UInt): UInt = {
    r(i).enable := enable
    r(i).index := index
    Mux(r(i).async, RegEnable(r(i).data, r(i).enable), r(i).data)
  }
  def write(i: Int, enable: Bool, index: UInt, data: UInt, mask: UInt): Unit = {
    w(i).enable := enable
    w(i).index := index
    w(i).data := data
    w(i).mask := mask
  }

  def mem_decl: String =
    """
      |// 8GB memory
      |`define RAM_SIZE (256 * 1024 * 1024)
      |reg [255:0] memory [0 : `RAM_SIZE - 1];
      |""".stripMargin

  def mem_target: String = "memory"

  override def desiredName: String = s"Mem${nr}R${nw}WHelper"

  val cppExtModule =
    s"""
       |static void $desiredName(
       |${r_cpp_arg(nr)},
       |${w_cpp_arg(nw)}
       |) {
       |  ${r_cpp_body(nr)}
       |  ${w_cpp_body(nw)}
       |}
       |""".stripMargin
  difftest.DifftestModule.createCppExtModule(desiredName, cppExtModule, Some("\"ram.h\""))

  setInline(
    s"$desiredName.v",
    s"""
       |`ifdef SYNTHESIS
       |  `define DISABLE_DIFFTEST_RAM_DPIC
       |`endif
       |module $desiredName #(
       |  parameter RAM_SIZE
       |)(
       |  input clock,
       |  ${r_sv_interface(nr)},
       |  ${w_sv_interface(nw)}
       |);
       |  $mem_init
       |  ${r_sv_body(nr)}
       |  ${w_sv_body(nw)}
       |endmodule
     """.stripMargin,
  )
}

abstract class DifftestMem(size: BigInt, lanes: Int, bits: Int) extends Module {
  require(bits == 8 && lanes % 32 == 0, "supports 256-bits aligned byte access only")
  require(lanes == 32, "supports 32 lanes only")
  protected val n_helper = lanes / 32
  private val helper = Seq.fill(n_helper)(Module(new MemRWHelper))

  val read = IO(new Bundle {
    val valid = Input(Bool())
    val index = Input(UInt(64.W))
    val data = Output(Vec(lanes / 32, UInt(256.W)))
  })
  val write = IO(Input(new Bundle {
    val valid = Bool()
    val index = UInt(64.W)
    val data = Vec(lanes / 32, UInt(256.W))
    val mask = Vec(lanes / 32, UInt(256.W))
  }))

  read.data := helper.zipWithIndex.map { case (h, i) =>
    h.clock := clock
    h.read(
      enable = !reset.asBool && read.valid,
      index = read.index * n_helper.U + i.U,
    )
  }

  helper.zipWithIndex.foreach { case (h, i) =>
    h.clock := clock
    h.write(
      enable = !reset.asBool && write.valid,
      index = write.index * n_helper.U + i.U,
      data = write.data(i),
      mask = write.mask(i),
    )
  }

  private var r_index = 0
  def read(addr: UInt, en: Bool): Vec[UInt] = {
    val port = read(r_index)
    r_index += 1
    port.valid := en
    port.index := addr
    port.data
  }

  def readAndHold(addr: UInt, en: Bool): Vec[UInt] = {
    val port = read(r_index)
    r_index += 1
    port.valid := en
    port.index := addr
    Mux(RegNext(en), port.data, RegEnable(port.data, RegNext(en))).asTypeOf(Vec(lanes, UInt(bits.W)))
  }

  private var w_index = 0
  def write(addr: UInt, data: Seq[UInt], mask: Seq[Bool]): Unit = {
    val port = write(w_index)
    w_index += 1
    port.valid := true.B
    port.index := addr
    port.data := VecInit(data).asTypeOf(port.data)
    require(data.length == lanes, s"data Vec[UInt] should have the length of $lanes")
    require(mask.length == lanes, s"mask Vec[Bool] should have the length of $lanes")
    require(data.head.getWidth == bits, s"data should have the width of $bits")
    port.mask := VecInit(mask.map(m => Fill(bits, m))).asTypeOf(port.mask)
  }
}

private class DifftestMem1P(size: BigInt, lanes: Int, bits: Int) extends DifftestMem(size, lanes, bits, 1, 1) {
  assert(!read.head.valid || !write.head.valid, "read and write come at the same cycle")
}

private class DifftestMemMP(size: BigInt, lanes: Int, bits: Int, nr: Int, nw: Int)
  extends DifftestMem(size, lanes, bits, nr, nw) {
  override def desiredName: String = s"DifftestMem${nr}R${nw}W"
}

object DifftestMem {
  private def setDefaultIOs(mod: DifftestMem): DifftestMem = {
    mod.read := DontCare
    mod.read.foreach(_.valid := false.B)
    mod.write := DontCare
    mod.write.foreach(_.valid := false.B)
    mod
  }

  def apply(size: BigInt, beatBytes: Int): DifftestMem = apply(size, beatBytes, 8)

  // only for compatibility
  def apply(
    size: BigInt,
    lanes: Int,
    bits: Int,
    synthesizable: Boolean = false,
    singlePort: Boolean = true,
  ): DifftestMem = apply(size, lanes, bits, 1, 1)

  def apply(
    size: BigInt,
    lanes: Int,
    bits: Int,
    nr: Int,
    nw: Int,
  ): DifftestMem = setDefaultIOs(Module(new DifftestMemMP(size, lanes, bits, nr, nw)))
}
