`ifdef IT_CLOCK_CONFIG
// Fixed profiles selected at startup; RTL clock gates remain active.
module ITClockGen (
    output bit noc_clock = 0,
    output bit core_clock = 0
);
    timeunit 1ps;
    timeprecision 1ps;
    string profile = "sync";
    int noc_half_ps = 250;
    int core_half_ps = 250;
    int core_phase_ps = 0;

    initial begin
        void'($value$plusargs("IT_CLOCK_PROFILE=%s", profile));
        case (profile)
            "sync": ;
            "core2_noc1": noc_half_ps = 500;
            "core1_noc2": core_half_ps = 500;
            "core3_noc2": noc_half_ps = 375;
            "phase90": core_phase_ps = 125;
            default: $fatal(1, "IT_CLOCK_CONFIG unknown profile '%s'", profile);
        endcase
        $display("IT_CLOCK_CONFIG rev=20261126 profile=%s core_period_ps=%0d noc_period_ps=%0d core_phase_ps=%0d core_MHz=%0.3f noc_MHz=%0.3f",
                 profile, 2 * core_half_ps, 2 * noc_half_ps, core_phase_ps,
                 500000.0 / core_half_ps, 500000.0 / noc_half_ps);
        // Preserve the original TB startup delay and first edge for sync mode.
        #2500;
        fork
            forever #(noc_half_ps) noc_clock = ~noc_clock;
            begin
                #(core_phase_ps);
                forever #(core_half_ps) core_clock = ~core_clock;
            end
        join
    end
endmodule
`endif
