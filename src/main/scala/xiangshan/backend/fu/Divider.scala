package xiangshan.backend.fu

import chisel3._
import chisel3.util._
import xiangshan._
import xiangshan.utils._
import xiangshan.backend._

import xiangshan.backend.fu.FunctionUnit._

class Divider(len: Int) extends FunctionUnit(divCfg) {
  val io = IO(new MulDivIO(len))

  def abs(a: UInt, sign: Bool): (Bool, UInt) = {
    val s = a(len - 1) && sign
    (s, Mux(s, -a, a))
  }

  val s_idle :: s_log2 :: s_shift :: s_compute :: s_finish :: Nil = Enum(5)
  val state = RegInit(s_idle)
  val newReq = (state === s_idle) && io.in.fire()

  val (a, b) = (io.in.bits.src1, io.in.bits.src2)
  val divBy0 = b === 0.U(len.W)

  val shiftReg = Reg(UInt((1 + len * 2).W))
  val hi = shiftReg(len * 2, len)
  val lo = shiftReg(len - 1, 0)

  val (aSign, aVal) = abs(a, io.in.bits.ctrl.sign)
  val (bSign, bVal) = abs(b, io.in.bits.ctrl.sign)
  val aSignReg = RegEnable(aSign, newReq)
  val qSignReg = RegEnable((aSign ^ bSign) && !divBy0, newReq)
  val bReg = RegEnable(bVal, newReq)
  val aValx2Reg = RegEnable(Cat(aVal, "b0".U), newReq)
  val ctrlReg = RegEnable(io.in.bits.ctrl, newReq)

  val cnt = Counter(len)
  when (newReq) {
    state := s_log2
  } .elsewhen (state === s_log2) {
    // `canSkipShift` is calculated as following:
    //   bEffectiveBit = Log2(bVal, XLEN) + 1.U
    //   aLeadingZero = 64.U - aEffectiveBit = 64.U - (Log2(aVal, XLEN) + 1.U)
    //   canSkipShift = aLeadingZero + bEffectiveBit
    //     = 64.U - (Log2(aVal, XLEN) + 1.U) + Log2(bVal, XLEN) + 1.U
    //     = 64.U + Log2(bVal, XLEN) - Log2(aVal, XLEN)
    //     = (64.U | Log2(bVal, XLEN)) - Log2(aVal, XLEN)  // since Log2(bVal, XLEN) < 64.U
    val canSkipShift = (64.U | Log2(bReg)) - Log2(aValx2Reg)
    // When divide by 0, the quotient should be all 1's.
    // Therefore we can not shift in 0s here.
    // We do not skip any shift to avoid this.
    cnt.value := Mux(divBy0, 0.U, Mux(canSkipShift >= (len-1).U, (len-1).U, canSkipShift))
    state := s_shift
  } .elsewhen (state === s_shift) {
    shiftReg := aValx2Reg << cnt.value
    state := s_compute
  } .elsewhen (state === s_compute) {
    val enough = hi.asUInt >= bReg.asUInt
    shiftReg := Cat(Mux(enough, hi - bReg, hi)(len - 1, 0), lo, enough)
    cnt.inc()
    when (cnt.value === (len-1).U) { state := s_finish }
  } .elsewhen (state === s_finish) {
    when(io.out.ready){
      state := s_idle
    }
  }

  when(state=/=s_idle && ctrlReg.uop.brTag.needFlush(io.redirect)){
    state := s_idle
  }

  val r = hi(len, 1)
  val resQ = Mux(qSignReg, -lo, lo)
  val resR = Mux(aSignReg, -r, r)

  val xlen = io.out.bits.data.getWidth
  val res = Mux(ctrlReg.isHi, resR, resQ)
  io.out.bits.data := Mux(ctrlReg.isW, SignExt(res(31,0),xlen), res)
  io.out.bits.uop := ctrlReg.uop

  io.out.valid := state === s_finish
  io.in.ready := state === s_idle
}