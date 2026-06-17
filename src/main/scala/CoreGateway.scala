package difftest.gateway

import scala.collection.mutable
import difftest._
import chisel3._

object CoreGateway {
  private val gatewayMap = mutable.HashMap[String, (Data, Int)]()

  def PrintGateway(): Unit = {
    println("Difftest Gateway List:")
    gatewayMap.foreach { case (name, (bundle, delay)) =>
      println(s"Delay: $delay, Bundle: ${bundle}, Name: $name")
    }
  }

  def addOne(bundle: Data, delay: Int, name: String): Unit = {
    require(!gatewayMap.contains(name.toUpperCase), s"difftest signals $name is already recorded!")
    val elm = name.toUpperCase -> (bundle, delay)
    gatewayMap.addOne(elm)
  }

  def getOne(name: String): Data = {
    require(gatewayMap.contains(name.toUpperCase), s"difftest signals $name is not recorded!")
    gatewayMap(name.toUpperCase)._1
  }
}

class CoreGatewayBundle(
  val commitWidth: Int,
  val intWbPortNum: Int,
  val intPhyRegNum: Int,
  val fpWbPortNum: Int,
  val fpPhyRegNum: Int,
  val vecWbPortNum: Int,
  val vecPhyRegNum: Int,
  val v0WbPortNum: Int,
  val v0PhyRegNum: Int,
) extends Bundle {
  val phyRegNumMax: Int = Array(intPhyRegNum, fpPhyRegNum, vecPhyRegNum, v0PhyRegNum).max
  // Rob
  val instrCommitDelayCnt: Int = 3
  val instrCommit = Vec(commitWidth, new DiffInstrCommit(phyRegNumMax))

  // val loadEventDelayCnt: Int = 3
  // val loadEvent = Vec(8, new DiffLoadEvent)

  val trapEventDelayCnt: Int = 0
  val trapEvent = new DiffTrapEvent

  // DataPath
  val archIntRegDelayCnt: Int = 2
  val archIntRegState = new DiffArchIntRegState

  val archFpRegDelayCnt: Int = 2
  val archFpRegState = new DiffArchFpRegState

  // WbDataPath
  val intWritebackDelayCnt: Int = 0
  val intWriteback = Vec(intWbPortNum, new DiffIntWriteback(intPhyRegNum))

  val fpWritebackDelayCnt: Int = 0
  val fpWriteback = Vec(fpWbPortNum, new DiffFpWriteback(fpPhyRegNum))

  val vecWritebackDelayCnt: Int = 0
  val vecWriteback = Vec(vecWbPortNum, new DiffVecWriteback(vecPhyRegNum))

  val vecV0WritebackDelayCnt: Int = 0
  val vecV0Writeback = Vec(v0WbPortNum, new DiffVecV0Writeback(v0PhyRegNum))

  // NewCSR
  val archEventDelayCnt: Int = 3                                                                                               
  val archEvent = new DiffArchEvent

  val csrStateDelayCnt: Int = 0
  val csrState = new DiffCSRState

  val debugModeDelayCnt: Int = 0
  val debugMode = new DiffDebugMode

  val triggerCSRStateDelayCnt: Int = 0
  val triggerCSRState = new DiffTriggerCSRState

  val fpCSRStateDelayCnt: Int = 0
  val fpCSRState = new DiffFpCSRState

  // val hcsrStateDelayCnt: Int = 0
  // val hcsrState = new DiffHCSRState

  val nonRegInterruptPendingEventDelayCnt: Int = 0
  val nonRegInterruptPendingEvent = new DiffNonRegInterruptPendingEvent

  // // DCache
  // val dcacheMissQueueRefillEventDelayCnt: Int = 0
  // val dcacheMissQueueRefillEvent = new DiffRefillEvent

  // val dcacheCMOInvalEventDelayCnt: Int = 0
  // val dcacheCMOInvalEvent = new DiffCMOInvalEvent

  // // ICache
  // val icacheMissQueueRefillEventDelayCnt: Int = 0
  // val icacheMissQueueRefillEvent = new DiffRefillEvent

  // val icacheRefillEventDelayCnt: Int = 0
  // val icacheRefillEvent = Vec(4, new DiffRefillEvent)

  // // L2TLB
  // val ptwRefillEventDelayCnt: Int = 0
  // val ptwRefillEvent = new DiffRefillEvent

  // val ptwL2TLBEventDelayCnt: Int = 0
  // val ptwL2TLBEvent = Vec(2, new DiffL2TLBEvent)

  // // ITLB
  // val itlbL1TLBEventDelayCnt: Int = 0
  // val itlbL1TLBEvent = Vec(3, new DiffL1TLBEvent)

  // // DTLB
  // val dtlbL1TLBEventDelayCnt: Int = 0
  // val dtlbL1TLBEvent = Vec(6, new DiffL1TLBEvent)

  // Atomic
  // val atomicEventDelayCnt: Int = 0
  // val atomicEvent = new DiffAtomicEvent

  val lrscEventDelayCnt: Int = 0
  val lrscEvent = new DiffLrScEvent

  // SBuffer
  // val sbufferEventDelayCnt: Int = 1
  // val sbufferEvent = Vec(2, new DiffSbufferEvent)

  // val sbufferStoreEventDelayCnt: Int = 2
  // val sbufferStoreEvent = Vec(2, new DiffStoreEvent)

  // val sbufferWlineStoreEventDelayCnt: Int = 2
  // val sbufferWlineStoreEvent = Vec(2, Vec(4, new DiffStoreEvent))

  def getInstanceSeq: Seq[(DifftestBundle, Int)] = {
    Seq((archEvent, archEventDelayCnt)) ++
      instrCommit.map(c => (c, instrCommitDelayCnt)) ++
      intWriteback.map(wb => (wb, intWritebackDelayCnt)) ++
      fpWriteback.map(wb => (wb, fpWritebackDelayCnt)) ++
      vecWriteback.map(wb => (wb, vecWritebackDelayCnt)) ++
      vecV0Writeback.map(wb => (wb, vecV0WritebackDelayCnt)) :+
      (csrState, csrStateDelayCnt) :+
      // (hcsrState, hcsrStateDelayCnt) :+
      (debugMode, debugModeDelayCnt) :+
      (triggerCSRState, triggerCSRStateDelayCnt) :+
      (fpCSRState, fpCSRStateDelayCnt) :+
      (archIntRegState, archIntRegDelayCnt) :+
      (archFpRegState, archFpRegDelayCnt) :+
      (nonRegInterruptPendingEvent, nonRegInterruptPendingEventDelayCnt) :+
      (trapEvent, trapEventDelayCnt) :+
      (lrscEvent, lrscEventDelayCnt)
  }
}
