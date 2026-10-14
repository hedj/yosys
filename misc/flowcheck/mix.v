// Exercises the passes flowcheck has contracts for: muxed multipliers,
// dividers, adders and shifts (share/opt_share), a pmux (muxpack/pmux2shiftx),
// memory, an FSM, comparisons (alumacc) and wide AND/OR trees.
module mix (input clk, rst, input [2:0] op, input [15:0] a, b, c, d, input [3:0] sh, ra, wa,
            input we, output reg [31:0] y, output reg [15:0] q, output reg [1:0] st, output r);
  reg [15:0] mem [0:15];
  always @(posedge clk) begin
    if (we) mem[wa] <= a;
    q <= mem[ra];
  end
  always @(posedge clk)
    case (op)
      3'd0: y <= a * b;
      3'd1: y <= c * d;
      3'd2: y <= a / b;
      3'd3: y <= c / d;
      3'd4: y <= (a << sh) + (b << sh);
      3'd5: y <= (c << sh) - d;
      3'd6: y <= {16'b0, a + b};
      default: y <= {16'b0, c + d};
    endcase
  always @(posedge clk)
    if (rst) st <= 0;
    else case (st)
      0: if (a < b) st <= 1;
      1: if (c == d) st <= 2;
      2: st <= 3;
      3: if (a[0]) st <= 0;
    endcase
  assign r = &(a | b) ^ |(c & d);
endmodule
