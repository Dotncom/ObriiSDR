`timescale 1ns/1ps

module fpga_logic_sim_tb;
    reg clk = 0;
    always #5 clk = ~clk;

    reg reset = 1;
    reg in_valid = 0;
    reg signed [15:0] in_i = 0;
    reg signed [15:0] in_q = 0;
    wire out_valid;
    wire signed [15:0] out_i;
    wire signed [15:0] out_q;

    obrii_user_logic dut (
        .clk(clk), .reset(reset), .in_valid(in_valid),
        .in_i(in_i), .in_q(in_q),
        .out_valid(out_valid), .out_i(out_i), .out_q(out_q)
    );

    initial begin
        @(negedge clk);
        reset = 0;
        in_valid = 1;
        in_i = 1000;
        in_q = -2000;
        @(posedge clk); #1;
        if (out_valid !== 1 || out_i !== 1100 || out_q !== -2000)
            $fatal(1, "first sample or IQ alignment failed");

        @(negedge clk);
        in_valid = 0;
        in_i = 32000;
        in_q = 1200;
        @(posedge clk); #1;
        if (out_valid !== 0 || out_i !== 1100 || out_q !== -2000)
            $fatal(1, "invalid interval changed the registered sample");

        @(negedge clk);
        in_valid = 1;
        in_i = -1200;
        in_q = 500;
        @(posedge clk); #1;
        if (out_valid !== 1 || out_i !== -1100 || out_q !== 500)
            $fatal(1, "sample after valid gap failed");
        $display("FPGA user logic simulation PASS");
        $finish;
    end
endmodule
