package difftest.monitor


import scala.collection.mutable
import chisel3._
import chisel3.util._
import difftest._
import difftest.util._
import difftest.common._
import difftest.gateway._
import difftest.trace._
import difftest.DifftestModule.isFPGA

object CoreGateway {
  private var config = GatewayConfig()
  private val gatewayMap = mutable.HashMap.empty[String, (DifftestBundle, Int)]

  def difftestFPGA: Boolean = config.isFPGA

  def addOne(bundle: DifftestBundle, delay: Int, name: String): Unit = {
    val key = name.toUpperCase
    require(!gatewayMap.contains(key), s"difftest signals $name is already recorded!")
    bundle.suggestName(name)
    dontTouch(bundle)
    gatewayMap.addOne(key -> (bundle, delay))
  }

  def getOne(name: String): Data = {
    val key = name.toUpperCase
    require(gatewayMap.contains(key), s"difftest signals $name is not recorded!")
    gatewayMap(key)._1
  }

  def setCoreGatewayConfig(cfg: String): Unit = {
    cfg.foreach {
      case 'E' => config = config.copy(hasGlobalEnable    = true)
      case 'S' => config = config.copy(isSquash           = true)
      case 'R' => config = config.copy(hasReplay          = true)
      case 'Z' => config = config.copy(hasDutZone         = true)
      case 'D' => config = config.copy(isDelta            = true)
      case 'B' => config = config.copy(isBatch            = true)
      case 'I' => config = config.copy(hasInternalStep    = true)
      case 'N' => config = config.copy(isNonBlock         = true)
      case 'P' => config = config.copy(hasBuiltInPerf     = true)
      case 'T' => config = config.copy(traceDump          = true)
      case 'L' => config = config.copy(traceLoad          = true)
      case 'H' => config = config.copy(hierarchicalWiring = true)
      case 'F' => config = config.copy(isFPGA             = true)
      case 'G' => config = config.copy(isGSIM             = true)
      case 'U' => config = config.copy(softArchUpdate     = true)
      case x   => println(s"Unknown Gateway Config $x")
    }
    config.check()
  }

  def collect(instanceSeq: Seq[(DifftestBundle, Int)]): GatewayResult = {
    val instances = instanceSeq.map(_._1).toSeq
    val instanceTypesWithDelay = instanceSeq.map { case (instance, delay) =>
      (chiselTypeOf(instance), delay)
    }
    println(s"[lntop_collect] instanceSeq: ${instanceSeq}")
    val sink = if (config.needEndpoint) {
      val gatewayIn = if (config.traceLoad) {
        MixedVecInit(Trace.load(instances).toSeq.map(_.asUInt)).asUInt
      } else {
        val packed = WireInit(0.U.asTypeOf(MixedVec(instances.map(gen => UInt(gen.getWidth.W)))))
        for ((data, idx) <- packed.zipWithIndex) {
          if(config.hierarchicalWiring) {
            data := instances(idx).asUInt
          } else {
            DifftestWiring.addSink(data, s"gateway_$idx", config.hierarchicalWiring)
          }
        }
        packed.asUInt
      }
      val endpoint = Module(new GatewayEndpoint(instanceTypesWithDelay, config))
      endpoint.in := gatewayIn
      val numCores = instances.count(_.isUniqueIdentifier)
      GatewayResult(
        vMacros = Seq(s"CONFIG_DIFFTEST_INTERFACE_WIDTH ${gatewayIn.getWidth / numCores}"),
        instances = endpoint.instances,
        structPacked = Some(config.isBatch),
        structAligned = Some(config.isDelta),
        refClock = Option.when(config.hasClockGate)(endpoint.clock),
        step = Some(endpoint.step),
        fpgaIO = endpoint.fpgaIO,
        fpgaSquashEnable = endpoint.fpgaSquashEnable,
        clockEnable = endpoint.clockEnable,
      )
    } else {
      GatewayResult(instances = Gateway.getInstance(instances)) + GatewaySink.collect(config, Gateway.getInstance(instances))
    }
    sink + GatewayResult(
      cppMacros = config.cppMacros,
      vMacros = config.vMacros,
      cppExtModule = Some(config.isGSIM),
      exit = None,
    )
  }
}
