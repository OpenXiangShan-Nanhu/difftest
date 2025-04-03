module sim_watchdog (
    input  wire        clock,
    input  wire        reset,
    input  wire        io_valid
);

logic [63:0] stuck_limit;
logic [63:0] n_lastcommit;
logic        first_commit;

initial begin
    stuck_limit = 15000;
    n_lastcommit = 15000;
    first_commit = 0;
end

always @(posedge clock) begin
    if (reset) begin
        first_commit <= 0;
    end else begin
        if ((~first_commit) && io_valid)
            first_commit <= 1;
    end
end
    
always @(posedge clock) begin
    if (~reset) begin
        if ((~first_commit) || io_valid) begin
            n_lastcommit <= stuck_limit;
        end else begin
            n_lastcommit <= n_lastcommit - 1;
            if (n_lastcommit == 0) begin
                $display("Core 0 Has no commit for %d cycles", stuck_limit);
                $finish();
            end
        end
    end
end

endmodule