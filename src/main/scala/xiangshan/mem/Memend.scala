package xiangshan.mem

import chisel3._
import chisel3.util._
import xiangshan._
import utils._
import xiangshan.cache._
import bus.simplebus._

object LSUOpType {
  def lb   = "b000000".U
  def lh   = "b000001".U
  def lw   = "b000010".U
  def ld   = "b000011".U
  def lbu  = "b000100".U
  def lhu  = "b000101".U
  def lwu  = "b000110".U
  def ldu  = "b000111".U
  def sb   = "b001000".U
  def sh   = "b001001".U
  def sw   = "b001010".U
  def sd   = "b001011".U

  def lr      = "b100010".U
  def sc      = "b100011".U
  def amoswap = "b100001".U
  def amoadd  = "b100000".U
  def amoxor  = "b100100".U
  def amoand  = "b101100".U
  def amoor   = "b101000".U
  def amomin  = "b110000".U
  def amomax  = "b110100".U
  def amominu = "b111000".U
  def amomaxu = "b111100".U

  def isStore(func: UInt): Bool = func(3)
  def isAtom(func: UInt): Bool = func(5)

  def atomW = "010".U
  def atomD = "011".U
}

object genWmask {
  def apply(addr: UInt, sizeEncode: UInt): UInt = {
    (LookupTree(sizeEncode, List(
      "b00".U -> 0x1.U, //0001 << addr(2:0)
      "b01".U -> 0x3.U, //0011
      "b10".U -> 0xf.U, //1111
      "b11".U -> 0xff.U //11111111
    )) << addr(2, 0)).asUInt()
  }
}

object genWdata {
  def apply(data: UInt, sizeEncode: UInt): UInt = {
    LookupTree(sizeEncode, List(
      "b00".U -> Fill(8, data(7, 0)),
      "b01".U -> Fill(4, data(15, 0)),
      "b10".U -> Fill(2, data(31, 0)),
      "b11".U -> data
    ))
  }
}

class LsPipelineBundle extends XSBundle {
  val vaddr = UInt(VAddrBits.W)
  val paddr = UInt(PAddrBits.W)
  val func = UInt(6.W)
  val mask = UInt(8.W)
  val data = UInt(XLEN.W)
  val uop = new MicroOp

  val miss = Bool()
  val mmio = Bool()
  val rollback = Bool()

  val forwardMask = Vec(8, Bool())
  val forwardData = Vec(8, UInt(8.W))
}

class LoadForwardQueryIO extends XSBundle {
  val paddr = Output(UInt(PAddrBits.W))
  val mask = Output(UInt(8.W))
  val lsroqIdx = Output(UInt(LsroqIdxWidth.W))
  val pc = Output(UInt(VAddrBits.W)) //for debug
  val valid = Output(Bool()) //for debug

  val forwardMask = Input(Vec(8, Bool()))
  val forwardData = Input(Vec(8, UInt(8.W)))
}

class MemToBackendIO extends XSBundle {
  val ldin = Vec(exuParameters.LduCnt, Flipped(Decoupled(new ExuInput)))
  val stin = Vec(exuParameters.StuCnt, Flipped(Decoupled(new ExuInput)))
  val ldout = Vec(exuParameters.LduCnt, Decoupled(new ExuOutput))
  val stout = Vec(exuParameters.StuCnt, Decoupled(new ExuOutput))
  val redirect = Flipped(ValidIO(new Redirect))
  // replay all instructions form dispatch
  val replayAll = ValidIO(new Redirect)
  // replay mem instructions form Load Queue/Store Queue
  val tlbFeedback = Vec(exuParameters.LduCnt + exuParameters.LduCnt, ValidIO(new TlbFeedback))
  val commits = Flipped(Vec(CommitWidth, Valid(new RoqCommit)))
  val dp1Req = Vec(RenameWidth, Flipped(DecoupledIO(new MicroOp)))
  val lsroqIdxs = Output(Vec(RenameWidth, UInt(LsroqIdxWidth.W)))
}

class Memend extends XSModule {
  val io = IO(new Bundle{
    val backend = new MemToBackendIO
    val dmem = new SimpleBusUC(userBits = (new DcacheUserBundle).getWidth)
    val pmem = new SimpleBusUC(userBits = (new DcacheUserBundle).getWidth, addrBits = PAddrBits) // FIXME: check all simplebus's addrbits
  })

  val loadUnits = (0 until exuParameters.LduCnt).map(_ => Module(new LoadUnit))
  val storeUnits = (0 until exuParameters.StuCnt).map(_ => Module(new StoreUnit))
  val dcache = Module(new Dcache)
  // val mshq = Module(new MSHQ)
  val dtlb = Module(new TLB(Width = DTLBWidth, isDtlb = true))
  val ptw = Module(new PTW)
  val lsroq = Module(new Lsroq)
  val sbuffer = Module(new FakeSbuffer)

  dcache.io := DontCare
  ptw.io.tlb(0) <> dtlb.io.ptw
  ptw.io.tlb(1) <> DontCare //mem.io.itlb
  ptw.io.mem <> io.pmem // TODO: ptw mem access
  // mshq.io := DontCare

  for (i <- 0 until exuParameters.LduCnt) {
    loadUnits(i).io.ldin <> io.backend.ldin(i)
    loadUnits(i).io.ldout <> io.backend.ldout(i)
    loadUnits(i).io.redirect <> io.backend.redirect
    loadUnits(i).io.tlbFeedback <> io.backend.tlbFeedback(i)
    loadUnits(i).io.dcache <> dcache.io.lsu.load(i)
    loadUnits(i).io.dtlb <> dtlb.io.requestor(i)
    loadUnits(i).io.sbuffer <> sbuffer.io.forward(i)

    lsroq.io.loadIn(i) <> loadUnits(i).io.lsroq.loadIn
    lsroq.io.ldout(i) <> loadUnits(i).io.lsroq.ldout
    lsroq.io.forward(i) <> loadUnits(i).io.lsroq.forward
  }

  for (i <- 0 until exuParameters.StuCnt) {
    storeUnits(i).io.stin <> io.backend.stin(i)
    storeUnits(i).io.redirect <> io.backend.redirect
    storeUnits(i).io.tlbFeedback <> io.backend.tlbFeedback(exuParameters.LduCnt + i)
    storeUnits(i).io.dtlb <> dtlb.io.requestor(exuParameters.LduCnt + i) // FIXME
    storeUnits(i).io.lsroq <> lsroq.io.storeIn(i)
  }

  dcache.io.lsu.refill <> DontCare // TODO
  sbuffer.io.dcache <> dcache.io.lsu.store

  lsroq.io.stout <> io.backend.stout
  lsroq.io.commits <> io.backend.commits
  lsroq.io.dp1Req <> io.backend.dp1Req
  lsroq.io.lsroqIdxs <> io.backend.lsroqIdxs
  lsroq.io.brqRedirect := io.backend.redirect
  io.backend.replayAll <> lsroq.io.rollback
  dcache.io.lsu.redirect := io.backend.redirect

  lsroq.io.refill <> DontCare
  lsroq.io.refill.valid := false.B // TODO
  lsroq.io.miss <> DontCare //TODO
  // LSROQ to store buffer
  lsroq.io.sbuffer <> sbuffer.io.in
  // for ls pipeline test
  dcache.io.dmem <> io.dmem
  dcache.io.lsu.refill <> DontCare
}
