package difftest.gateway

import scala.collection.mutable.ListBuffer
import difftest._
import chisel3._

class CoreGateway {
  val gatewayList = ListBuffer.empty[(Data, Int, String)]
  def addOne(bundle: Data, delay: Int, name: String): Unit = {
    gatewayList += ((bundle, delay, name))
  }

  def PrintGateway(): Unit = {
    println("Difftest Gateway List:")
    gatewayList.foreach { case (bundle, delay, name) =>
      println(s"Delay: $delay, Bundle: ${bundle}, Name: $name")
    }
  }

  def getOne(name: String): Data = {
    gatewayList.filter(_._3 == name).head._1
  }
}

object CoreGateway {
  val gateway = new CoreGateway()
  
  def PrintGateway(): Unit = {
    gateway.PrintGateway()
  }

  def addOne(bundle: Data, delay: Int, name: String): Unit = {
    gateway.addOne(bundle, delay, name)
  }

  def getOne(name: String): Data = {
    println(name)
    gateway.getOne(name)
  }
}

class CoreGatewayBundle extends Bundle {
  // Rob
  val instrCommitDelayCnt: Int = 3
  val instrCommit = Vec(8, new DiffInstrCommit(128))

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
  val intWriteback = Vec(9, new DiffIntWriteback(128))
  
  val fpWritebackDelayCnt: Int = 0
  val fpWriteback = Vec(9, new DiffFpWriteback(160))
  
  val vecWritebackDelayCnt: Int = 0
  val vecWriteback = Vec(9, new DiffVecWriteback(160))

  val vecV0WritebackDelayCnt: Int = 0
  val vecV0Writeback = Vec(7, new DiffVecV0Writeback(22))

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

  val hcsrStateDelayCnt: Int = 0
  val hcsrState = new DiffHCSRState

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
        (hcsrState, hcsrStateDelayCnt) :+
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

