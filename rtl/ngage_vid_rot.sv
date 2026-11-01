// SPDX-License-Identifier: GPL-3.0-or-later
module ngage_vid_rot
(
	input             clk,
	input             ce_pix,
	input       [9:0] hc,
	input       [9:0] vc,
	input       [1:0] orientation,
	output            in_img,
	output     [15:0] addr,
	output      [9:0] x_end,
	output      [9:0] y0
);

// 0 Vert, 1 Horz (90 deg CW), 2 Horz180 (90 deg CCW), 3 shows Vert
reg [1:0] ori = 2'd0;
always @(posedge clk) if (ce_pix && hc == 10'd0 && vc == 10'd0) ori <= orientation;

wire       rot = (ori == 2'd1) || (ori == 2'd2);
wire [9:0] x0  = rot ? 10'd56 : 10'd72;
assign      y0  = rot ? 10'd32 : 10'd16;
wire [9:0] w   = rot ? 10'd208 : 10'd176;
wire [9:0] h   = rot ? 10'd176 : 10'd208;

assign x_end = x0 + w;

assign in_img = (hc >= x0) && (hc < x0 + w) && (vc >= y0) && (vc < y0 + h);

wire [9:0] xo = hc - x0;
wire [9:0] yo = vc - y0;

wire [9:0] row = (ori == 2'd1) ? 10'd207 - xo : (ori == 2'd2) ? xo : yo;
wire [9:0] col = (ori == 2'd1) ? yo : (ori == 2'd2) ? 10'd175 - yo : xo;

assign addr = in_img ? row * 16'd176 + col : 16'd0;

endmodule
