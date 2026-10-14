// A memory that has the same name as picorv32's register file, so that
// mapping it interns the same $memory\cpuregs$... names picorv32 will use.
module throwaway(input clk, input [3:0] wa, ra, input we, input [15:0] wd, output reg [15:0] q);
  reg [15:0] cpuregs [0:15];
  always @(posedge clk) begin if (we) cpuregs[wa] <= wd; q <= cpuregs[ra]; end
endmodule
