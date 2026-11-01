// SPDX-License-Identifier: GPL-3.0-or-later
module ngage_ghost #(parameter [4:0] A_NUM = 5'd21)
(
	input         clk,
	input         ce_pix,
	input         frame,
	input         enable,
	input         in_img,
	input  [15:0] addr,
	input  [11:0] pix,
	input  [23:0] rgb_in,
	output [23:0] rgb_out
);

reg on_f = 1'b0;
always @(posedge clk) if (ce_pix & frame) on_f <= enable;

reg [17:0] st [0:36607];
reg [17:0] st_q;
always @(posedge clk) st_q <= st[addr];

reg c1 = 1'b0, c2 = 1'b0, c3 = 1'b0;
always @(posedge clk) begin
	c1 <= ce_pix;
	c2 <= c1;
	c3 <= c2;
end

// rise only: ceil(A_NUM * d / 32) is >= 1 and <= d, so a rise never sticks below t nor passes it
function automatic [5:0] rise(input [5:0] s, input [3:0] c);
	reg  [5:0] t, d;
	reg [10:0] p;
	integer i;
	begin
		t = {c, 2'b00};
		d = t - s;
		p = 11'd31;
		for (i = 0; i < 5; i = i + 1)
			if (A_NUM[i]) p = p + ({5'd0, d} << i);
		rise = (t > s) ? s + p[10:5] : t;
	end
endfunction

function automatic [7:0] lvl(input [5:0] s);
	reg [9:0] m;
	begin
		m = {s, 4'd0} + {4'd0, s};
		lvl = m[9:2];
	end
endfunction

wire [5:0] n_r = rise(st_q[17:12], pix[11:8]);
wire [5:0] n_g = rise(st_q[11:6], pix[7:4]);
wire [5:0] n_b = rise(st_q[5:0], pix[3:0]);

reg [23:0] g_q = 24'd0;
reg [17:0] w_q = 18'd0;
always @(posedge clk) if (c2) begin
	g_q <= in_img ? {lvl(n_r), lvl(n_g), lvl(n_b)} : 24'd0;
	w_q <= on_f ? {n_r, n_g, n_b} : {pix[11:8], 2'b00, pix[7:4], 2'b00, pix[3:0], 2'b00};
end

always @(posedge clk) if (c3 & in_img) st[addr] <= w_q;

assign rgb_out = on_f ? g_q : rgb_in;

endmodule
