// SPDX-License-Identifier: GPL-3.0-or-later
module ngage_light
(
	input             clk,
	input             ce_pix,
	input             frame,
	input       [8:0] k,
	input             byp,
	input      [23:0] rgb_in,
	output reg [23:0] rgb_out
);

reg [8:0] k_f = 9'h100;
reg       byp_f = 1'b0;
// a backlight lights the whole panel at once: take a new level only at the frame start, never mid-picture
always @(posedge clk) if (ce_pix & frame) begin
	k_f   <= k;
	byp_f <= byp;
end

wire  [8:0] keff = (byp_f | k_f[8]) ? 9'h100 : k_f;
wire [16:0] pr = rgb_in[23:16] * keff;
wire [16:0] pg = rgb_in[15:8] * keff;
wire [16:0] pb = rgb_in[7:0] * keff;

always @(posedge clk) rgb_out <= {pr[15:8], pg[15:8], pb[15:8]};

endmodule
