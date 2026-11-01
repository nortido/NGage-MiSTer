// LCD command port at 0x0C121200/04/08.
// Not a framebuffer: the panel is driven by register writes plus a 12bpp
// (4:4:4, two pixels/word) pixel burst through register 0x5C. We shadow
// that burst into two 176x208/2 M10Ks (even/odd pixel banks) so the video
// side has something to show; one write port and one read port each keeps
// Quartus from turning the framebuffer into flip-flops.
module ngage_lcd
(
	input             clk,
	input             reset,

	input             sel,
	input             ena,
	input             rnw,
	input       [1:0] off34, // adr[3:2]: 0=+0x00, 1=+0x04, 2=+0x08
	input      [31:0] wdata,
	output     [31:0] rdata,

	input      [15:0] pix_addr,
	output     [11:0] pix_data,

	// DMA data port writes (ngage_mmc), taken only when the CPU is not writing
	input             dma_we,
	input      [31:0] dma_wdata,
	output            dma_rdy
);

localparam FB_SIZE  = 176*208;
localparam FB_WORDS = FB_SIZE/2; // one entry per even/odd pixel pair
localparam [7:0] PANEL_ID = 8'h48;

reg [31:0] cmd_reg, ctrl_reg;
reg  [7:0] last_reg;
reg        in_index_phase;   // last +0x00 write was the plain 0x09200000 index op
reg        burst_phase;      // last +0x00 write carries bit31 (long auto-inc write)
reg        id_pending;       // register 0x66 was selected: next +0x08 read is the panel ID
reg  [15:0] wptr;
reg  [15:0] row_start, col_start;
reg  [1:0]  win_params; // how many param bytes of the current 0x75/0x15 seen

// even/odd pixel banks: burst writes push pixel0 to the even bank and
// pixel1 to the odd bank at the same word index (wptr/2). Bursts start on
// even columns; an odd window start (win_base odd) is not supported and
// would land the pair in the wrong bank.
reg [11:0] fb_even [0:FB_WORDS-1];
reg [11:0] fb_odd  [0:FB_WORDS-1];
reg [11:0] fb_even_q, fb_odd_q;
reg        pix_lsb_d;

wire        we0 = sel & ena & ~rnw & (off34 == 2'd0);
wire        we1 = sel & ena & ~rnw & (off34 == 2'd1);
wire        we2 = sel & ena & ~rnw & (off34 == 2'd2);
wire        re2 = sel & ena &  rnw & (off34 == 2'd2);
// one write port per bank: a DMA word waits out any CPU write cycle, and
// outside a burst it is dropped like a stray data write, never stalled
assign      dma_rdy  = ~(sel & ena & ~rnw);
wire        dma_acc  = dma_we & dma_rdy;
wire [31:0] fb_wd    = dma_acc ? dma_wdata : wdata;
wire        burst_we = (we2 | dma_acc) & (burst_phase | (last_reg == 8'h5C)) & ~in_index_phase;

wire [15:0] win_base = row_start * 16'd176 + col_start;
wire        win_ok   = win_base < FB_SIZE[15:0];

always @(posedge clk) begin
	if (reset) begin
		cmd_reg        <= 32'd0;
		ctrl_reg       <= 32'd0;
		last_reg       <= 8'd0;
		in_index_phase <= 1'b0;
		burst_phase    <= 1'b0;
		id_pending     <= 1'b0;
		wptr           <= 16'd0;
		row_start      <= 16'd0;
		col_start      <= 16'd0;
		win_params     <= 2'd0;
	end else begin
		if (we0) begin
			cmd_reg        <= wdata;
			in_index_phase <= ~wdata[18] & ~wdata[31];
			burst_phase    <= wdata[31];
			if (wdata[18] & ~wdata[31] & (last_reg == 8'h75 || last_reg == 8'h15))
				win_params <= 2'd0;
		end
		if (we1) ctrl_reg <= wdata;

		if (we2) begin
			if (in_index_phase) begin
				// register-select data word: bits [7:0]
				last_reg <= wdata[7:0];
				if (wdata[7:0] == 8'h5C)
					wptr <= win_ok ? win_base : 16'd0;
				id_pending <= (wdata[7:0] == 8'h66);
			end else if (~burst_we && (last_reg == 8'h75 || last_reg == 8'h15)) begin
				// PASET/CASET, packed start | (end<<20); only the first word
				// (start) is honoured, end is ignored; CASET's panel column 2
				// is frame column 0, PASET has no row offset
				if (win_params == 2'd0) begin
					if (last_reg == 8'h75) row_start <= {8'd0, wdata[7:0]};
					else                   col_start <= (wdata[7:0] < 8'd2) ? 16'd0 : {8'd0, wdata[7:0] - 8'd2};
				end
				win_params <= win_params + 2'd1;
			end
		end else if (re2 && id_pending) begin
			id_pending <= 1'b0; // one-shot: ID once, then zero
		end

		if (burst_we) wptr <= (wptr + 16'd2 >= FB_SIZE[15:0]) ? 16'd0 : wptr + 16'd2;
	end
end

always @(posedge clk) if (burst_we) fb_even[wptr[15:1]] <= fb_wd[11:0];
always @(posedge clk) if (burst_we) fb_odd[wptr[15:1]]  <= fb_wd[27:16];

always @(posedge clk) begin
	fb_even_q <= fb_even[pix_addr[15:1]];
	fb_odd_q  <= fb_odd[pix_addr[15:1]];
	pix_lsb_d <= pix_addr[0];
end
assign pix_data = pix_lsb_d ? fb_odd_q : fb_even_q;

assign rdata = (off34 == 2'd0) ? (cmd_reg | 32'h40000000) :
               (off34 == 2'd1) ? (ctrl_reg | 32'h00000004) :
               id_pending      ? {24'd0, PANEL_ID} :
               32'h00000000; // data readback otherwise: not modelled, return 0

endmodule
