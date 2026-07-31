package difftest.monitor

import scala.collection._
import chisel3._
import chisel3.util._
import difftest._
import difftest.gateway._

class DifftestMonitorBundle(
  commitWidth   : Int,
  intPhyRegNum  : Int,
  fpPhyRegNum   : Int,
  vecPhyRegNum  : Int,
  v0PhyRegNum   : Int
) extends Bundle {
  // BasicDiff
  val instrCommit                 = Vec(commitWidth, new DiffInstrCommit(Seq(intPhyRegNum, fpPhyRegNum, vecPhyRegNum, v0PhyRegNum).max))
  val trapEvent                   = new DiffTrapEvent
  val phyIntRegState              = new DiffPhyIntRegState(intPhyRegNum)
  val phyFpRegState               = new DiffPhyFpRegState(fpPhyRegNum)
  val phyVecRegState              = new DiffPhyVecRegState(2 * (vecPhyRegNum + v0PhyRegNum))
  val archEvent                   = new DiffArchEvent
  val csrState                    = new DiffCSRState
  val debugMode                   = new DiffDebugMode
  val triggerCSRState             = new DiffTriggerCSRState
  val fpCSRState                  = new DiffFpCSRState
  val nonRegInterruptPendingEvent = new DiffNonRegInterruptPendingEvent
  val lrscEvent                   = new DiffLrScEvent
  val archIntRenameTable          = new DiffArchIntRenameTable(intPhyRegNum)
  val archFpRenameTable           = new DiffArchFpRenameTable(vecPhyRegNum)
  val archVecRenameTable          = new DiffArchVecRenameTable(2 * (vecPhyRegNum + v0PhyRegNum))

  def getInstanceSeq: Seq[(DifftestBundle, Int)] = Seq((archEvent, 3)) ++
    (instrCommit.map(c => (c, 3)))    :+
    (trapEvent, 0)                    :+
    (phyIntRegState, 2)               :+
    (phyFpRegState, 2)                :+
    (phyVecRegState, 2)               :+
    (csrState, 0)                     :+
    (debugMode, 0)                    :+
    (triggerCSRState, 0)              :+
    (fpCSRState, 0)                   :+
    (nonRegInterruptPendingEvent, 0)  :+
    (lrscEvent, 0)                    :+
    (archIntRenameTable, 2)           :+
    (archFpRenameTable, 2)            :+
    (archVecRenameTable, 2)         
}

class DifftestMonitor(
  ccid          : Int,
  path          : String,
  prefix        : String,
  commitWidth   : Int,
  intPhyRegNum  : Int,
  fpPhyRegNum   : Int,
  vecPhyRegNum  : Int,
  v0PhyRegNum   : Int
  ) extends BlackBox with HasBlackBoxInline {

  override def desiredName = prefix + s"DifftestMonitor${ccid}"
  
  val io = IO(new Bundle {
    val clock = Output(Clock())
    val reset = Output(Bool())
    val probe = Output(new DifftestMonitorBundle(
      commitWidth   = commitWidth,
      intPhyRegNum  = intPhyRegNum,
      fpPhyRegNum   = fpPhyRegNum,
      vecPhyRegNum  = vecPhyRegNum,
      v0PhyRegNum   = v0PhyRegNum
    ))
  })

  private val cc = s"$path.cc_$ccid"
  private val portQueue = mutable.Queue[String]()

  private def opStr(name:String, data:Data):String = {
    val width = data.getWidth
    val range = if(width == 1) "         " else f" [${width - 1}%2d: 0] "
    s"  output$range$name"
  }

  private def parseIO(name:String, data:Data): Unit = {
    data match {
      case b: Bundle => b.elements.foreach({case(n, d) => parseIO(s"${name}_$n", d)})
      case v: Vec[Data] => v.zipWithIndex.foreach({case(d, i) => parseIO(s"${name}_$i", d)})
      case _ => if(data.getWidth != 0) portQueue.addOne(opStr(name, data))
    }
  }

  io.probe.elements.foreach({case(n, d) => parseIO(s"probe_$n", d)})

  private val portDecl = portQueue.mkString(",\n")
  private val xmrQueue = mutable.Queue[String]()
  
  private def paStr(sink:String, src:String):String = {
    s"  assign $sink = $src;"
  }

  private def xmrIO(data:Data, sink:String, src:String): Unit = {
    data match {
      case b: DifftestBaseBundle => b.elements.foreach({
        case (n, d) => if((b.hasValid && n.takeRight(5) == "valid") || n.takeRight(7) == "hasTrap") {
          xmrIO(d, s"${sink}_$n", s"${src}_$n & ~$cc.tile.core.reset")
        } else {
          xmrIO(d, s"${sink}_$n", s"${src}_$n")
        }
      })
      case v: Vec[Data] => v.zipWithIndex.foreach({case(d, i) => xmrIO(d, s"${sink}_$i", s"${src}_$i")})
      case _ => if(data.getWidth != 0) xmrQueue.addOne(paStr(sink, src))
    }
  }

  for((n, d) <- io.probe.elements) {
    d match {
      case v: Vec[Data] => v.zipWithIndex.foreach({ case(d, i) =>
        val srcKey = s"difftest${n.capitalize}_$i"
        val srcName = CoreGateway.getOne(srcKey).pathName.replace("inner.", "inner_").replace(s"${prefix}CpuCluster", cc)
        val sinkName = s"probe_${n}_$i"
        xmrIO(d, sinkName, srcName)
      })
      case d: Data =>
        val srcKey = s"difftest${n.capitalize}"
        val srcName = CoreGateway.getOne(srcKey).pathName.replace("inner.", "inner_").replace(s"${prefix}CpuCluster", cc)
        val sinkName = s"probe_$n"
        xmrIO(d, sinkName, srcName)
    }
  }
  private val assignmentDecl = xmrQueue.mkString("\n")

  setInline(s"$desiredName.sv",
    s"""
       |module $desiredName (
       |$portDecl,
       |  output         reset,
       |  output         clock
       |);
       |$assignmentDecl
       |  assign reset = $cc.tile.core.reset;
       |  assign clock = $cc.tile.core.clock;
       |endmodule""".stripMargin)
}
