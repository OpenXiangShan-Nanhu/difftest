`timescale 1ps/1ps
module it_clock_profiles_tb;
  wire noc_clock, core_clock, noc_out;
  wire [3:0] cpu_out;
  bit reset = 1;
  bit uart_valid = 0;
  byte uart_data = 0;
  string profile = "sync";
  int noc_period = 500, core_period = 500, core_phase = 0;
  int noc_edges = 0, core_edges = 0, expected_cycles = 0;
  time last_noc, last_core, first_noc, first_core;

`ifdef IT_CLOCK_CONFIG
  ITClockGen gen (.noc_clock(noc_clock), .core_clock(core_clock));
  wire endpoint_clock = core_clock;
`else
  bit noc_source = 0, core_source = 0;
  assign noc_clock = noc_source;
  assign core_clock = core_source;
  wire endpoint_clock = noc_clock;
  initial begin
    #2500;
    forever #250 noc_source = ~noc_source;
  end
  initial begin
    #2500;
    forever #375 core_source = ~core_source;
  end
`endif

  SystemExtensionWrapper #(.NR_CC(4)) wrapper (
    .i_top_clk(noc_clock), .i_core_clk(core_clock), .i_top_rst(reset),
    .o_noc_clk(noc_out), .o_cpu_clk(cpu_out)
  );
  DifftestEndpoint endpoint (
    .clock(endpoint_clock), .reset(reset),
`ifdef IT_CLOCK_CONFIG
    .io_clock(noc_clock),
`endif
    .difftest_exit(64'b0), .difftest_step(1'b1),
    .difftest_uart_out_valid(uart_valid), .difftest_uart_out_ch(uart_data)
  );

  initial begin
`ifdef IT_CLOCK_CONFIG
    void'($value$plusargs("IT_CLOCK_PROFILE=%s", profile));
    case (profile)
      "sync": ;
      "core2_noc1": noc_period = 1000;
      "core1_noc2": core_period = 1000;
      "core3_noc2": noc_period = 750;
      "phase90": core_phase = 125;
      default: ; // The generator must reject an unknown profile itself.
    endcase
`else
    core_period = 750;
`endif
    #20000 reset = 0;
    #20000 reset = 1;
    #10000 reset = 0;
    #20000;
    if (noc_edges < 20 || core_edges < 20) $fatal(1, "missing clocks");
    if (first_noc != 2500 + noc_period / 2 ||
        first_core != 2500 + core_period / 2 + core_phase)
      $fatal(1, "unexpected startup or phase");
    $display("CLOCK_COMPONENT_PASS profile=%s", profile);
    $finish;
  end

  initial begin : uart_stimulus
    string message = "ITUART!\n";
    wait (!reset);
    for (int i = 0; i < message.len(); i++) begin
      @(negedge noc_clock);
      uart_valid = 1;
      uart_data = message.getc(i);
    end
    @(negedge noc_clock);
    uart_valid = 0;
  end

  always @(posedge noc_clock) begin
    if (noc_edges == 0) first_noc = $time;
    else if ($time - last_noc != noc_period) $fatal(1, "NoC period");
    last_noc = $time;
    noc_edges++;
    #1;
    if (noc_out !== noc_clock) $fatal(1, "NoC connection");
  end
  always @(posedge core_clock) begin
    if (core_edges == 0) first_core = $time;
    else if ($time - last_core != core_period) $fatal(1, "Core period");
    last_core = $time;
    core_edges++;
  end
  always @(noc_clock or core_clock) begin
    #1;
`ifdef IT_CLOCK_CONFIG
    if (cpu_out !== {4{core_clock}}) $fatal(1, "Core connection");
`else
    if (cpu_out !== {4{noc_clock}}) $fatal(1, "legacy Core connection");
`endif
  end
  always @(posedge endpoint_clock) begin
    expected_cycles = reset ? 0 : expected_cycles + 1;
    #1;
    if (endpoint.n_cycles !== expected_cycles) $fatal(1, "endpoint cycle count");
  end
endmodule
