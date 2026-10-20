package difftest

import chisel3._
import chisel3.util.experimental.BoringUtils

// The integration passes the common timebase through its instance boundary.
// Scoped construction then routes it only within that module definition.
object ObservationClock {
  private var source: Option[UInt] = None

  def withSource[T](cycle: UInt)(body: => T): T = {
    require(cycle.getWidth == 64)
    val previous = source
    source = Some(cycle)
    try body
    finally source = previous
  }

  def apply(): UInt = {
    val cycle = source.getOrElse(throw new IllegalArgumentException("memory observation clock is not connected"))
    BoringUtils.bore(cycle)
  }
}
