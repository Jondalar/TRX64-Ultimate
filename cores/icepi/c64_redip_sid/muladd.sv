// Multiply-add used by sid_voice (the voice DCA) and sid_filter (the op-amp).
//
// From icepi-zero-c64; the widths became parameters for U7 (2026-11-07), so sid_filter can carry
// fractional bits in its filter state while the default stays the original 16 x 16 -> 32.
// docs/specs/U7-sid-filter-precision.md

module muladd #(
  parameter AW = 16,
  parameter BW = 16,
  parameter OW = 32
)(
  input  logic signed [OW-1:0] c,
  input  logic                 s,  // 0 = add, 1 = subtract
  input  logic signed [AW-1:0] a,
  input  logic signed [BW-1:0] b,
  output logic signed [OW-1:0] o
);

logic signed [OW-1:0] m;

// AW x BW signed multiply
assign m = a * b;

// Add / subtract
assign o = s ? (c - m) : (c + m);

endmodule
