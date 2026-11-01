// SPDX-License-Identifier: GPL-3.0-or-later
module ngage_hyb_aud #(parameter integer CLK_HZ = 50000000)
(
	input             clk,
	input             rst,
	input             srst,

	input             en,
	input             stereo,
	input      [15:0] prime,
	input      [31:0] step,
	input      [17:0] wr,
	output reg [17:0] rd,
	output reg [31:0] under,

	output reg        ddr_rd,
	output     [28:0] ddr_word,
	input             ddr_busy,
	input             ddr_valid,
	input      [63:0] ddr_rdata,

	output reg [15:0] audio_l,
	output reg [15:0] audio_r
);

localparam [31:0] BASE  = 32'h38E00000;
localparam [31:0] RATE  = 32'd48000;
localparam [31:0] TLIM  = CLK_HZ - 48000;
localparam [1:0]  S_IDLE = 2'd0, S_PRIME = 2'd1, S_PLAY = 2'd2;

assign ddr_word = {BASE[31:18], rd[17:3]};

reg [31:0] tacc;
reg        tick;
always @(posedge clk) begin
	if (rst) begin
		tacc <= 32'd0; tick <= 1'b0;
	end else if (tacc >= TLIM) begin
		tacc <= tacc - TLIM; tick <= 1'b1;
	end else begin
		tacc <= tacc + RATE; tick <= 1'b0;
	end
end

reg   [1:0] st;
reg         v0, v1, pend;
reg  [63:0] w0, w1;
reg   [1:0] fi;
reg  [31:0] ph;

wire [17:0] avail  = wr - {rd[17:3], 3'b000};
wire        primed = v0 & v1 & ({2'b00, avail[17:3]} + 17'd2 >= {1'b0, prime});
wire        last   = stereo ? fi[0] : (fi == 2'd3);
wire [127:0] cat   = {w1, w0};
wire  [6:0] oa     = stereo ? {fi, 5'd0} : {1'b0, fi, 4'd0};
wire  [6:0] ob     = oa + (stereo ? 7'd32 : 7'd16);
wire [15:0] a_l    = cat[oa +: 16];
wire [15:0] a_r    = cat[oa + 7'd16 +: 16];
wire [15:0] b_l    = cat[ob +: 16];
wire [15:0] b_r    = cat[ob + 7'd16 +: 16];
wire [16:0] d_l    = {b_l[15], b_l} - {a_l[15], a_l};
wire [16:0] d_r    = {b_r[15], b_r} - {a_r[15], a_r};
wire [32:0] ph_n   = {1'b0, ph} + {1'b0, step};

reg         mbusy, mch;
reg   [3:0] mc;
reg  [15:0] fr, frk;
reg  [32:0] md, acc;
reg  [15:0] xa, xr, xo;
reg  [16:0] xd;
wire [32:0] acc_n = acc + (fr[0] ? md : 33'd0);
wire [16:0] res   = {xa[15], xa} + acc_n[32:16];

always @(posedge clk) begin
	ddr_rd <= 1'b0;
	if (rst) under <= 32'd0;

	if (rst | srst | ~en) begin
		st <= S_IDLE; v0 <= 1'b0; v1 <= 1'b0; pend <= 1'b0; mbusy <= 1'b0;
		audio_l <= 16'd0; audio_r <= 16'd0;
		if (rst | srst) rd <= 18'd0;
	end else if (st == S_IDLE) begin
		st <= S_PRIME; rd <= wr; fi <= stereo ? {1'b0, wr[2]} : wr[2:1]; ph <= 32'd0;
	end else begin
		// the in-flight read of a cleared block still completes in the arbiter; pend stays 0 for it
		if (~pend & ~ddr_busy & ~(v0 & v1) & (avail[17:3] != 15'd0)) begin
			ddr_rd <= 1'b1; pend <= 1'b1;
		end
		if (pend & ddr_valid) begin
			pend <= 1'b0; rd <= {rd[17:3] + 15'd1, 3'b000};
			if (v0) begin w1 <= ddr_rdata; v1 <= 1'b1; end
			else begin w0 <= ddr_rdata; v0 <= 1'b1; end
		end
		if (st == S_PRIME & primed) st <= S_PLAY;

		if (mbusy) begin
			acc <= acc_n; md <= {md[31:0], 1'b0}; fr <= {1'b0, fr[15:1]}; mc <= mc + 4'd1;
			if (mc == 4'd15) begin
				if (~mch & stereo) begin
					xo <= res[15:0]; xa <= xr; md <= {{16{xd[16]}}, xd}; fr <= frk; acc <= 33'd0; mch <= 1'b1;
				end else begin
					audio_l <= mch ? xo : res[15:0]; audio_r <= res[15:0]; mbusy <= 1'b0;
				end
			end
		end

		if (tick) begin
			if (st == S_PLAY & (~last | v1)) begin
				mbusy <= 1'b1; mch <= 1'b0; mc <= 4'd0; acc <= 33'd0;
				fr <= ph[31:16]; frk <= ph[31:16];
				md <= {{16{d_l[16]}}, d_l}; xa <= a_l; xr <= a_r; xd <= d_r;
				ph <= ph_n[31:0];
				if (ph_n[32]) begin
					if (last) begin w0 <= w1; v1 <= 1'b0; fi <= 2'd0; end
					else fi <= fi + 2'd1;
				end
			end else begin
				audio_l <= 16'd0; audio_r <= 16'd0;
				if (st == S_PLAY) begin under <= under + 32'd1; st <= S_PRIME; end
			end
		end
	end
end

endmodule
