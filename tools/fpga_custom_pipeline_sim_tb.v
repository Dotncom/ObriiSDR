`timescale 1ns/1ps

module fpga_custom_pipeline_sim_tb;
    reg clk = 0;
    always #5 clk = ~clk;

    reg reset = 1;
    reg in_valid = 0;
    reg signed [15:0] in_i = 0;
    reg signed [15:0] in_q = 0;
    wire out_valid;
    wire out_trigger;
    wire signed [15:0] out_i;
    wire signed [15:0] out_q;

    obrii_iq_pipeline dut (
        .clk(clk), .reset(reset), .in_valid(in_valid),
        .in_i(in_i), .in_q(in_q),
        .out_valid(out_valid), .out_trigger(out_trigger),
        .out_i(out_i), .out_q(out_q)
    );

    initial begin
        @(negedge clk);
        reset = 0;
        in_valid = 1;
        in_i = 1000;
        in_q = 100;
        @(posedge clk); #1;
        if (out_valid !== 0) $fatal(1, "first sample must not emit decimator output");

        @(negedge clk);
        in_i = 3000;
        in_q = 300;
        @(posedge clk); #1;
        if (out_valid !== 1 || out_i !== 2200 || out_q !== 200 || out_trigger !== 0)
            $fatal(1, "two custom stages or first decimation failed");

        @(negedge clk);
        in_valid = 0;
        @(posedge clk); #1;
        if (out_valid !== 0) $fatal(1, "valid gap emitted an output");

        @(negedge clk);
        in_valid = 1;
        in_i = -1000;
        in_q = -100;
        @(posedge clk); #1;
        if (out_valid !== 0) $fatal(1, "decimator counter was not reset");

        @(negedge clk);
        in_valid = 0;
        @(posedge clk); #1;
        if (out_valid !== 0) $fatal(1, "gap between samples emitted an output");

        @(negedge clk);
        in_valid = 1;
        in_i = -3000;
        in_q = -300;
        @(posedge clk); #1;
        if (out_valid !== 1 || out_i !== -1800 || out_q !== -200)
            $fatal(1, "second decimation after valid gap failed");
        $display("FPGA custom pipeline simulation PASS");
        $finish;
    end
endmodule
