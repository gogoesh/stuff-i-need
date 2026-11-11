`timescale 1ns/1ps
module alu_tb;

    reg  [31:0] A, B;
    reg  [2:0]  F;
    wire [31:0] Y;
    wire        Cout, Overflow, Zero;

    reg  [31:0] expY;
    reg         expC, expV, expZ;

    reg  [103:0] testvectors [0:15];
    integer      vectornum, errors;

    alu #(.N(32)) dut (.A(A), .B(B), .F(F), .Y(Y),
                       .Cout(Cout), .Overflow(Overflow), .Zero(Zero));

    initial begin
        $readmemh("alu.tv", testvectors);
        vectornum = 0;
        errors    = 0;

        while (vectornum < 16) begin
            {F, A, B, expY, expC, expV, expZ} =
                {testvectors[vectornum][102:100],
                 testvectors[vectornum][99:4],
                 testvectors[vectornum][2:0]};
            #10;
            if (Y !== expY || Cout !== expC || Overflow !== expV || Zero !== expZ) begin
                errors = errors + 1;
                $display("FAIL vector %0d: F=%b A=%h B=%h | Y=%h C=%b V=%b Z=%b | expected Y=%h C=%b V=%b Z=%b",
                         vectornum, F, A, B, Y, Cout, Overflow, Zero, expY, expC, expV, expZ);
            end
            vectornum = vectornum + 1;
        end

        $display("%0d tests completed with %0d errors", vectornum, errors);
        $stop;
    end
endmodule
