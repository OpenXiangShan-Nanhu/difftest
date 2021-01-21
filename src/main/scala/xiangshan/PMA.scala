package xiangshan

import chisel3._
import chisel3.util._
import utils._
import Chisel.experimental.chiselName
import xiangshan.cache.{DCache, HasDCacheParameters, DCacheParameters, ICache, ICacheParameters, L1plusCache, L1plusCacheParameters, PTW, Uncache}

object MemMap {
  def apply (base: String, top: String, width: String, description: String, mode: String): ((String, String), Map[String, String]) = {
    ((base, top) -> Map(
      "width" -> width, // 0 means no limitation
      "description" -> description,
      "mode" -> mode,
    ))
  }
}

object AddressSpace {
  def MemMapList = List(
    //     Base address      Top address       Width  Description    Mode (RWXIDSAC)
    MemMap("h00_0000_0000", "h00_0FFF_FFFF",   "h0", "Reserved",    ""),
    MemMap("h00_1000_0000", "h00_1FFF_FFFF",   "h0", "QSPI_Flash",  "RX"),
    MemMap("h00_2000_0000", "h00_2FFF_FFFF",   "h0", "Reserved",    ""),
    MemMap("h00_3000_0000", "h00_3000_FFFF",   "h0", "DMA",         "RW"),
    MemMap("h00_3001_0000", "h00_3004_FFFF",   "h0", "GPU",         "RWC"),
    MemMap("h00_3005_0000", "h00_3005_FFFF",   "h0", "USB",         "RW"),
    MemMap("h00_3006_0000", "h00_3006_FFFF",   "h0", "SDMMC",       "RW"),
    MemMap("h00_3007_0000", "h00_30FF_FFFF",   "h0", "Reserved",    ""),
    MemMap("h00_3100_0000", "h00_3100_FFFF",   "h0", "QSPI",        "RW"),
    MemMap("h00_3101_0000", "h00_3101_FFFF",   "h0", "GMAC",        "RW"),
    MemMap("h00_3102_0000", "h00_3102_FFFF",   "h0", "HDMI",        "RW"),
    MemMap("h00_3103_0000", "h00_3103_FFFF",   "h0", "HDMI_PHY",    "RW"),
    MemMap("h00_3104_0000", "h00_3105_FFFF",   "h0", "DP",          "RW"),
    MemMap("h00_3106_0000", "h00_3106_FFFF",   "h0", "DDR0",        "RW"),
    MemMap("h00_3107_0000", "h00_3107_FFFF",   "h0", "DDR0_PHY",    "RW"),
    MemMap("h00_3108_0000", "h00_3108_FFFF",   "h0", "DDR1",        "RW"),
    MemMap("h00_3109_0000", "h00_3109_FFFF",   "h0", "DDR1_PHY",    "RW"),
    MemMap("h00_310A_0000", "h00_310A_FFFF",   "h0", "IIS",         "RW"),
    MemMap("h00_310B_0000", "h00_310B_FFFF",   "h0", "UART0",       "RW"),
    MemMap("h00_310C_0000", "h00_310C_FFFF",   "h0", "UART1",       "RW"),
    MemMap("h00_310D_0000", "h00_310D_FFFF",   "h0", "IIC0",        "RW"),
    MemMap("h00_310E_0000", "h00_310E_FFFF",   "h0", "IIC1",        "RW"),
    MemMap("h00_310F_0000", "h00_310F_FFFF",   "h0", "IIC2",        "RW"),
    MemMap("h00_3110_0000", "h00_3110_FFFF",   "h0", "GPIO",        "RW"),
    MemMap("h00_3111_0000", "h00_3111_FFFF",   "h0", "CRU",         "RW"),
    MemMap("h00_3112_0000", "h00_37FF_FFFF",   "h0", "Reserved",    ""),
    MemMap("h00_3800_0000", "h00_3800_FFFF",   "h0", "CLINT",       "RW"),
    MemMap("h00_3801_0000", "h00_3BFF_FFFF",   "h0", "Reserved",    ""),
    MemMap("h00_3C00_0000", "h00_3FFF_FFFF",   "h0", "PLIC",        "RW"),
    MemMap("h00_4000_0000", "h00_4FFF_FFFF",   "h0", "PCIe0",       "RW"),
    MemMap("h00_5000_0000", "h00_5FFF_FFFF",   "h0", "PCIe1",       "RW"),
    MemMap("h00_6000_0000", "h00_6FFF_FFFF",   "h0", "PCIe2",       "RW"),
    MemMap("h00_7000_0000", "h00_7FFF_FFFF",   "h0", "PCIe3",       "RW"),
    MemMap("h00_8000_0000", "h1F_FFFF_FFFF",   "h0", "DDR",         "RWXIDSA"),
  )

  def printMemmap(){
    println("-------------------- memory map --------------------")
    for(i <- MemMapList){
      println(i._1._1 + "->" + i._1._2 + " width " + (if(i._2.get("width").get == "0") "unlimited" else i._2.get("width").get) + " " + i._2.get("description").get + " [" + i._2.get("mode").get + "]")
    }
    println("----------------------------------------------------")
  }

  def genMemmapMatchVec(addr: UInt): UInt = {
    VecInit(MemMapList.map(i => {
      i._1._1.U <= addr && addr < i._1._2.U
    }).toSeq).asUInt
  }

  def queryMode(matchVec: UInt): UInt = {
    Mux1H(matchVec, VecInit(MemMapList.map(i => {
      PMAMode.strToMode(i._2.get("mode").get)
    }).toSeq))
  }

  def queryWidth(matchVec: UInt): UInt = {
    Mux1H(matchVec, VecInit(MemMapList.map(i => {
      i._2.get("width").get.U
    }).toSeq))
  }

  def memmapAddrMatch(addr: UInt): (UInt, UInt) = {
    val matchVec = genMemmapMatchVec(addr)
    (queryMode(matchVec), queryWidth(matchVec))
  }

  def isDMMIO(addr: UInt): Bool = !PMAMode.dcache(memmapAddrMatch(addr)._1)
  def isIMMIO(addr: UInt): Bool = !PMAMode.icache(memmapAddrMatch(addr)._1)

  def isConfigableAddr(addr: UInt): Bool = {
    VecInit(MemMapList.map(i => {
      i._1._1.U <= addr && addr < i._1._2.U && (i._2.get("mode").get.toUpperCase.indexOf("C") >= 0).B
    }).toSeq).asUInt.orR
  }
}

class PMAChecker extends XSModule with HasDCacheParameters
{
  val io = IO(new Bundle() {
    val paddr = Input(UInt(VAddrBits.W))
    val mode = Output(PMAMode())
    val widthLimit = Output(UInt(8.W)) // TODO: fixme
    val updateCConfig = Input(Valid(Bool()))
  })

  val enableConfigableCacheZone = RegInit(false.B)
  val updateCConfig = RegNext(RegNext(RegNext(io.updateCConfig)))
  when(updateCConfig.valid) {
    enableConfigableCacheZone := updateCConfig.bits
  }

  val (mode, widthLimit) = AddressSpace.memmapAddrMatch(io.paddr)
  io.mode := Mux(AddressSpace.isConfigableAddr(io.paddr) && enableConfigableCacheZone, mode | PMAMode.D, mode)
  io.widthLimit := widthLimit
}