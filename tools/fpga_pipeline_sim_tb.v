`timescale 1ns/1ps

module fpga_pipeline_sim_tb;
reg clk = 0;
reg reset = 1;
reg in_valid = 0;
reg signed [15:0] in_i = 0;
reg signed [15:0] in_q = 0;
wire out_valid;
wire out_trigger;
wire signed [15:0] out_i;
wire signed [15:0] out_q;
integer sample;
integer cycles = 0;
integer strong_checks = 0;
integer quiet_checks = 0;

obrii_iq_pipeline dut (
    .clk(clk), .reset(reset), .in_valid(in_valid),
    .in_i(in_i), .in_q(in_q),
    .out_valid(out_valid), .out_trigger(out_trigger),
    .out_i(out_i), .out_q(out_q)
);

always #5 clk = ~clk;

initial begin
    repeat (3) @(negedge clk);
    reset = 0;
    for (sample = 0; sample < 80; sample = sample + 1) begin
        @(negedge clk);
        in_valid = 1;
        if (sample < 40) begin
            case (sample % 4)
                0: begin in_i = 10000; in_q = 0; end
                1: begin in_i = 0; in_q = 10000; end
                2: begin in_i = -10000; in_q = 0; end
                3: begin in_i = 0; in_q = -10000; end
            endcase
        end else begin
            in_i = 0;
            in_q = 0;
        end
    end
    @(negedge clk);
    in_valid = 0;
    repeat (8) @(negedge clk);
    if (strong_checks < 4 || quiet_checks < 4)
        $fatal(1, "insufficient strong/quiet output samples: %0d/%0d", strong_checks, quiet_checks);
    $display("FPGA IQ pipeline simulation passed: %0d strong, %0d quiet", strong_checks, quiet_checks);
    $finish;
end

always @(posedge clk) begin
    cycles = cycles + 1;
    #1;
    if (out_trigger && !out_valid)
        $fatal(1, "trigger without valid output");
    if (out_valid && cycles >= 29 && cycles <= 43) begin
        if (out_i < 9800 || out_i > 10200 || out_q < -30 || out_q > 30 || !out_trigger)
            $fatal(1, "DDC/FIR/decimator/trigger strong sample mismatch: I=%0d Q=%0d event=%0d", out_i, out_q, out_trigger);
        strong_checks = strong_checks + 1;
    end
    if (out_valid && cycles >= 69 && cycles <= 83) begin
        if (out_i < -30 || out_i > 30 || out_q < -30 || out_q > 30 || out_trigger)
            $fatal(1, "quiet sample mismatch: I=%0d Q=%0d event=%0d", out_i, out_q, out_trigger);
        quiet_checks = quiet_checks + 1;
    end
end
endmodule
