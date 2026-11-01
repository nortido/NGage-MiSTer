// SPDX-License-Identifier: GPL-3.0-or-later
module ngage_hyb_ddr
(
	input             clk,
	input             rst,

	input             ioctl_download,
	input             ioctl_wr,
	input      [24:0] ioctl_addr,
	input       [7:0] ioctl_dout,
	output            ioctl_wait,
	input      [24:0] rom_size,
	output reg [24:0] rom_size_q,
	output reg        dl_active,
	output reg        rom_loaded,

	input      [25:1] dma_addr,
	input      [15:0] dma_din,
	input             dma_wrl,
	input             dma_wrh,
	input             dma_req,
	output reg [15:0] dma_dout,
	output reg        dma_ack,
	output reg [31:0] dma_drops,

	input      [31:0] diag_addr,
	input      [63:0] diag_wdata,
	input             diag_peek,
	input             diag_poke,
	output reg [63:0] diag_rdata,
	output            diag_busy,

	input      [28:0] aud_word,
	input             aud_rd,
	output            aud_busy,
	output reg [63:0] aud_rdata,
	output reg        aud_valid,

	input             DDRAM_BUSY,
	output      [7:0] DDRAM_BURSTCNT,
	output     [28:0] DDRAM_ADDR,
	input      [63:0] DDRAM_DOUT,
	input             DDRAM_DOUT_READY,
	output            DDRAM_RD,
	output     [63:0] DDRAM_DIN,
	output      [7:0] DDRAM_BE,
	output            DDRAM_WE
);

localparam [31:0] ROM_BASE   = 32'h39000000;
localparam [31:0] RAM16_BASE = 32'h3B000000;
localparam [31:0] RAM2M_BASE = 32'h38800000;

localparam [1:0] OWN_LD = 2'd0, OWN_DMA = 2'd1, OWN_DIAG = 2'd2, OWN_AUD = 2'd3;
localparam [1:0] Q_IDLE = 2'd0, Q_WANT = 2'd1, Q_PORT = 2'd2;
reg        p_we, p_rd, p_rpend;
reg  [1:0] p_own;
reg [28:0] p_addr;
reg [63:0] p_din;
reg  [7:0] p_be;
wire       p_idle  = ~p_we & ~p_rd & ~p_rpend;
wire       wr_acc  = p_we & ~DDRAM_BUSY;
wire       rd_done = p_rpend & DDRAM_DOUT_READY;

assign DDRAM_BURSTCNT = 8'd1;
assign DDRAM_ADDR     = p_addr;
assign DDRAM_DIN      = p_din;
assign DDRAM_BE       = p_be;
assign DDRAM_WE       = p_we;
assign DDRAM_RD       = p_rd;

// RESET and status[0] stay high during an upload, so the loader resets only on
// PLL loss and download start; Main polls ioctl_wait slowly, hence wait at 48 of 64
reg        dl_d;
wire       ld_rst = rst | (ioctl_download & ~dl_d);
reg [32:0] fifo [0:63];
reg  [6:0] wptr, rptr;
wire [6:0] ld_count = wptr - rptr;
wire [6:0] wbase    = ld_rst ? 7'd0 : wptr;
assign ioctl_wait = ld_count >= 7'd48;

reg        pk_valid, fl_valid;
reg [21:0] pk_word, fl_word;
reg [63:0] pk_data, fl_data;
reg  [7:0] pk_be, fl_be;

wire [32:0] head   = fifo[rptr[5:0]];
wire [21:0] h_word = head[32:11];
wire  [2:0] h_lane = head[10:8];
wire        h_same = pk_valid & (pk_word == h_word);
wire  [7:0] h_be   = 8'd1 << h_lane;
wire  [7:0] m_be   = (h_same ? pk_be : 8'd0) | h_be;
reg  [63:0] m_data;
integer     i;
always @* for (i = 0; i < 8; i = i + 1) m_data[8*i +: 8] = h_be[i] ? head[7:0] : pk_data[8*i +: 8];

wire pop      = (ld_count != 7'd0) & ~fl_valid & ~ld_rst;
wire take_ld  = p_idle & fl_valid;
wire ld_drained = ~ioctl_download & (ld_count == 7'd0) & ~pk_valid & ~fl_valid & ~(p_we & (p_own == OWN_LD));

always @(posedge clk) begin
	dl_d <= rst ? 1'b0 : ioctl_download;
	if (ioctl_wr) fifo[wbase[5:0]] <= {ioctl_addr, ioctl_dout};
	wptr <= wbase + {6'd0, ioctl_wr};
	rptr <= ld_rst ? 7'd0 : rptr + {6'd0, pop};

	if (ld_rst) begin
		pk_valid <= 1'b0; fl_valid <= 1'b0;
		dl_active <= ~rst; rom_loaded <= 1'b0; rom_size_q <= 25'd0;
	end else begin
		if (take_ld) fl_valid <= 1'b0;
		if (pop) begin
			if (pk_valid & ~h_same) begin
				fl_valid <= 1'b1; fl_word <= pk_word; fl_data <= pk_data; fl_be <= pk_be;
				pk_word <= h_word; pk_data <= m_data; pk_be <= m_be;
			end else if (m_be == 8'hFF) begin
				fl_valid <= 1'b1; fl_word <= h_word; fl_data <= m_data; fl_be <= m_be;
				pk_valid <= 1'b0;
			end else begin
				pk_valid <= 1'b1; pk_word <= h_word; pk_data <= m_data; pk_be <= m_be;
			end
		end else if (~ioctl_download & (ld_count == 7'd0) & pk_valid & ~fl_valid) begin
			fl_valid <= 1'b1; fl_word <= pk_word; fl_data <= pk_data; fl_be <= pk_be;
			pk_valid <= 1'b0;
		end
		if (dl_active & ld_drained) begin
			dl_active <= 1'b0; rom_loaded <= 1'b1; rom_size_q <= rom_size;
		end
	end
end

wire [25:0] dm_b     = {dma_addr, 1'b0};
wire        dm_ram16 = (dm_b >= 26'h1400000) && (dm_b < 26'h2400000);
wire        dm_ram2m = (dm_b >= 26'h2400000) && (dm_b < 26'h2600000);
wire  [8:0] dm_pg    = dm_b[20:12];
wire        dm_regpg = dm_ram2m && (dm_pg == 9'h120 || dm_pg == 9'h121 || dm_pg == 9'h149 || dm_pg == 9'h14F || dm_pg == 9'h150);
wire        dm_ok    = (dm_ram16 | dm_ram2m) & ~dm_regpg;
wire [31:0] dm_ddr   = dm_ram16 ? RAM16_BASE + {6'd0, dm_b - 26'h1400000} : RAM2M_BASE + {11'd0, dm_b[20:0]};
wire        dm_wr    = dma_wrl | dma_wrh;
wire  [7:0] dm_be    = {6'd0, dma_wrh, dma_wrl} << {dm_b[2:1], 1'b0};
reg   [1:0] dm_st;
reg   [1:0] dm_hw;
// a DMA read with dma_din[0] set takes its halfword from the beat of the last DMA read
// without a DDR acess: ngage_mmc LCD reads inside one 64-bit beat
reg  [63:0] dm_beat;
wire        dm_lat   = ~dm_wr & dma_din[0];
wire        take_dma = p_idle & ~fl_valid & (dm_st == Q_WANT);
reg   [1:0] au_st;
reg  [28:0] au_word;
wire        take_au  = p_idle & ~fl_valid & (dm_st != Q_WANT) & (au_st == Q_WANT);
assign      aud_busy = au_st != Q_IDLE;

`ifdef NGAGE_HYB_DIAG
reg  [1:0] dg_st;
reg        dg_wr;
reg [28:0] dg_word;
reg [63:0] dg_w;
wire       take_dg = p_idle & ~fl_valid & (dm_st != Q_WANT) & (au_st != Q_WANT) & (dg_st == Q_WANT);
assign diag_busy = dg_st != Q_IDLE;
`else
assign diag_busy = 1'b0;
`endif

always @(posedge clk) begin
	if (rst) begin
		p_we <= 1'b0; p_rd <= 1'b0; p_rpend <= 1'b0; p_own <= OWN_LD;
		dm_st <= Q_IDLE; dma_ack <= 1'b0; dma_dout <= 16'd0; dma_drops <= 32'd0; dm_beat <= 64'd0;
		diag_rdata <= 64'd0;
		au_st <= Q_IDLE; aud_valid <= 1'b0; aud_rdata <= 64'd0;
`ifdef NGAGE_HYB_DIAG
		dg_st <= Q_IDLE;
`endif
	end else begin
		if (~DDRAM_BUSY) begin p_we <= 1'b0; p_rd <= 1'b0; end
		if (DDRAM_DOUT_READY) p_rpend <= 1'b0;
		aud_valid <= 1'b0;

		if (take_ld) begin
			p_own <= OWN_LD; p_we <= 1'b1;
			p_addr <= ROM_BASE[31:3] + {7'd0, fl_word}; p_din <= fl_data; p_be <= fl_be;
		end else if (take_dma) begin
			p_own <= OWN_DMA; p_we <= dm_wr; p_rd <= ~dm_wr; p_rpend <= ~dm_wr;
			p_addr <= dm_ddr[31:3]; p_din <= {4{dma_din}}; p_be <= dm_wr ? dm_be : 8'hFF;
			dm_hw <= dm_b[2:1];
			dm_st <= Q_PORT;
		end else if (take_au) begin
			p_own <= OWN_AUD; p_rd <= 1'b1; p_rpend <= 1'b1;
			p_addr <= au_word; p_be <= 8'hFF;
			au_st <= Q_PORT;
		end
`ifdef NGAGE_HYB_DIAG
		else if (take_dg) begin
			p_own <= OWN_DIAG; p_we <= dg_wr; p_rd <= ~dg_wr; p_rpend <= ~dg_wr;
			p_addr <= dg_word; p_din <= dg_w; p_be <= 8'hFF;
			dg_st <= Q_PORT;
		end
`endif

		if ((dma_req != dma_ack) && dm_st == Q_IDLE) begin
			if (dm_ok && !dm_lat) dm_st <= Q_WANT;
			else begin
				dma_dout <= dm_ok ? dm_beat[16*dm_b[2:1] +: 16] : 16'd0;
				dma_ack <= dma_req;
				if (dm_regpg) dma_drops <= dma_drops + 32'd1;
			end
		end
		if ((wr_acc | rd_done) && p_own == OWN_DMA && dm_st == Q_PORT) begin
			if (rd_done) begin dma_dout <= DDRAM_DOUT[16*dm_hw +: 16]; dm_beat <= DDRAM_DOUT; end
			dma_ack <= dma_req;
			dm_st <= Q_IDLE;
		end

		if (aud_rd && au_st == Q_IDLE) begin au_st <= Q_WANT; au_word <= aud_word; end
		if (rd_done && p_own == OWN_AUD && au_st == Q_PORT) begin
			aud_rdata <= DDRAM_DOUT; aud_valid <= 1'b1;
			au_st <= Q_IDLE;
		end

`ifdef NGAGE_HYB_DIAG
		if ((diag_peek | diag_poke) && dg_st == Q_IDLE) begin
			dg_st <= Q_WANT; dg_wr <= diag_poke; dg_word <= diag_addr[31:3]; dg_w <= diag_wdata;
		end
		if ((wr_acc | rd_done) && p_own == OWN_DIAG && dg_st == Q_PORT) begin
			if (rd_done) diag_rdata <= DDRAM_DOUT;
			dg_st <= Q_IDLE;
		end
`endif
	end
end

endmodule
