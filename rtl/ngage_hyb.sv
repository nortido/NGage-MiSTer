// SPDX-License-Identifier: GPL-3.0-or-later
module ngage_hyb #(parameter integer FPS_SECOND = 50000000, parameter integer RUMBLE_HOLD_MS = 2000)
(
	input             clk,
	input             rst,

	input      [29:0] axi_awaddr,
	input             axi_awvalid,
	output            axi_awready,
	input      [31:0] axi_wdata,
	input       [3:0] axi_wstrb,
	input             axi_wvalid,
	output            axi_wready,
	output      [1:0] axi_bresp,
	output            axi_bvalid,
	input             axi_bready,
	input      [29:0] axi_araddr,
	input             axi_arvalid,
	output            axi_arready,
	output     [31:0] axi_rdata,
	output      [1:0] axi_rresp,
	output            axi_rvalid,
	input             axi_rready,

	output reg [31:0] mem_adr,
	output reg        mem_rnw,
	output reg        mem_ena,
	output reg  [1:0] mem_acc,
	output reg [31:0] mem_dout,
	input      [31:0] mem_din,
	input             mem_done,
	input             soc_held,
	output            soc_run,

	input             dl_active,
	input             rom_loaded,
	input             card_present,
	input      [24:0] rom_size,
	input             osd_req,
	input             turbo,
	input             wx,
	input       [2:0] flash_array,
	input       [6:0] dma_pend,
	input             irq_level,
	input      [31:0] dma_drops,
	input             vsync,
	input      [31:0] lcd_wr,
	output reg [15:0] lcd_fps,

	output     [31:0] dbg0,
	output     [31:0] dbg1,
	output     [31:0] dbg2,
	output     [31:0] dbg3,

	output     [31:0] diag_addr,
	output     [63:0] diag_wdata,
	output            diag_peek,
	output            diag_poke,
	input      [63:0] diag_rdata,
	input             diag_busy,
	output     [25:0] keys_or,
	output     [15:0] rumble,
	output      [8:0] light,

	output            aud_en,
	output            aud_stereo,
	output     [15:0] aud_prime,
	output     [31:0] aud_step,
	output     [17:0] aud_wr,
	input      [17:0] aud_rd,
	input      [31:0] aud_under
);

localparam [31:0] MAGIC   = 32'h4E474831;
localparam [31:0] VERSION = 32'h00000004;
localparam [31:0] TMO_DATA = 32'hDEADBEEF;
localparam [11:0] TMO_LAST = 12'd4095;
localparam integer RMB_HOLD = FPS_SECOND / 1000 * RUMBLE_HOLD_MS;

localparam [9:0] R_MAGIC = 10'd0, R_VERSION = 10'd1, R_SCRATCH = 10'd2, R_CTRL = 10'd3, R_STATUS = 10'd4,
                 R_ROM_SIZE = 10'd5, R_PAHI = 10'd6, R_TIMEOUTS = 10'd7, R_HELD = 10'd8,
                 R_DBG0 = 10'd9, R_DBG1 = 10'd10, R_DBG2 = 10'd11, R_DBG3 = 10'd12,
                 R_LCD_FPS = 10'd13, R_IRQ_LEVEL = 10'd14, R_DMA_DROPS = 10'd15,
                 R_DDR_ADDR = 10'd16, R_DDR_WLO = 10'd17, R_DDR_WHI = 10'd18, R_DDR_RLO = 10'd19,
                 R_DDR_RHI = 10'd20, R_DDR_CMD = 10'd21, R_KEYS_OR = 10'd22,
                 R_AUD_CTL = 10'd23, R_AUD_STEP = 10'd24, R_AUD_WR = 10'd25, R_AUD_RD = 10'd26, R_AUD_UNDER = 10'd27,
                 R_RUMBLE = 10'd28, R_LIGHT = 10'd29;

localparam [1:0] ST_IDLE = 2'd0, ST_EXEC = 2'd1, ST_FWD = 2'd2, ST_RESP = 2'd3;
reg  [1:0] st;
reg        aw_have, w_have, is_rd;
reg [29:0] aw_a, a;
reg [31:0] w_d, d, rdata_r;

assign axi_awready = (st == ST_IDLE) & ~aw_have;
assign axi_wready  = (st == ST_IDLE) & ~w_have;
assign axi_arready = (st == ST_IDLE) & ~aw_have & ~w_have & ~axi_awvalid & ~axi_wvalid;
assign axi_bvalid  = (st == ST_RESP) & ~is_rd;
assign axi_rvalid  = (st == ST_RESP) & is_rd;
assign axi_bresp   = 2'b00;
assign axi_rresp   = 2'b00;
assign axi_rdata   = rdata_r;

reg [31:0] scratch, timeouts, held, dbg_r [0:3];
reg        run, osd_flag;
reg  [6:0] pahi;
reg [11:0] tmo;
reg        issued, stale;

reg        vs_d;
reg [31:0] lw_snap, sec_cnt;
reg [15:0] fps_cnt;
wire       vs_rise = vsync & ~vs_d;
wire       fps_inc = vs_rise & (lcd_wr != lw_snap);

reg [25:0] kor;
assign keys_or = kor;

reg [15:0] rmb;
reg [31:0] rmb_cnt;
assign rumble = rmb;

reg  [8:0] lt;
assign light = lt;

reg        au_en, au_stereo;
reg [15:0] au_prime;
reg [31:0] au_step;
reg [17:0] au_wr;
assign aud_en     = au_en;
assign aud_stereo = au_stereo;
assign aud_prime  = au_prime;
assign aud_step   = au_step;
assign aud_wr     = au_wr;

`ifdef NGAGE_HYB_DIAG
reg [31:0] dg_addr, dg_wlo, dg_whi;
reg        dg_peek, dg_poke;
assign diag_addr  = dg_addr;
assign diag_wdata = {dg_whi, dg_wlo};
assign diag_peek  = dg_peek;
assign diag_poke  = dg_poke;
`else
assign diag_addr  = 32'd0;
assign diag_wdata = 64'd0;
assign diag_peek  = 1'b0;
assign diag_poke  = 1'b0;
`endif

assign soc_run = run;
assign dbg0 = dbg_r[0];
assign dbg1 = dbg_r[1];
assign dbg2 = dbg_r[2];
assign dbg3 = dbg_r[3];

reg turbo_q = 1'b0;
always @(posedge clk) turbo_q <= rst ? 1'b0 : turbo;
reg wx_q = 1'b0;
always @(posedge clk) wx_q <= rst ? 1'b0 : wx;

wire [31:0] status = {14'd0, wx_q, turbo_q, 1'b0, dma_pend, 1'b0, flash_array, card_present, osd_flag, rom_loaded, dl_active};

reg [31:0] reg_q;
always @* begin
	case (a[11:2])
		R_MAGIC:     reg_q = MAGIC;
		R_VERSION:   reg_q = VERSION;
		R_SCRATCH:   reg_q = scratch;
		R_CTRL:      reg_q = {31'd0, run};
		R_STATUS:    reg_q = status;
		R_ROM_SIZE:  reg_q = {7'd0, rom_size};
		R_PAHI:      reg_q = {25'd0, pahi};
		R_TIMEOUTS:  reg_q = timeouts;
		R_HELD:      reg_q = held;
		R_DBG0:      reg_q = dbg_r[0];
		R_DBG1:      reg_q = dbg_r[1];
		R_DBG2:      reg_q = dbg_r[2];
		R_DBG3:      reg_q = dbg_r[3];
		R_LCD_FPS:   reg_q = {16'd0, lcd_fps};
		R_IRQ_LEVEL: reg_q = {31'd0, irq_level};
		R_DMA_DROPS: reg_q = dma_drops;
		R_KEYS_OR:   reg_q = {6'd0, kor};
		R_AUD_CTL:   reg_q = {au_prime, 14'd0, au_stereo, au_en};
		R_AUD_STEP:  reg_q = au_step;
		R_AUD_WR:    reg_q = {14'd0, au_wr};
		R_AUD_RD:    reg_q = {14'd0, aud_rd};
		R_AUD_UNDER: reg_q = aud_under;
		R_RUMBLE:    reg_q = {16'd0, rmb};
		R_LIGHT:     reg_q = {23'd0, lt};
`ifdef NGAGE_HYB_DIAG
		R_DDR_ADDR:  reg_q = dg_addr;
		R_DDR_WLO:   reg_q = dg_wlo;
		R_DDR_WHI:   reg_q = dg_whi;
		R_DDR_RLO:   reg_q = diag_rdata[31:0];
		R_DDR_RHI:   reg_q = diag_rdata[63:32];
		R_DDR_CMD:   reg_q = {31'd0, diag_busy};
`endif
		default:     reg_q = 32'd0;
	endcase
end

wire reg_wr = (st == ST_EXEC) & a[29] & ~is_rd;

always @(posedge clk) begin
	mem_ena <= 1'b0;
`ifdef NGAGE_HYB_DIAG
	dg_peek <= 1'b0;
	dg_poke <= 1'b0;
`endif
	if (rst) begin
		st <= ST_IDLE; aw_have <= 1'b0; w_have <= 1'b0; is_rd <= 1'b0;
		scratch <= 32'd0; timeouts <= 32'd0; held <= 32'd0;
		dbg_r[0] <= 32'd0; dbg_r[1] <= 32'd0; dbg_r[2] <= 32'd0; dbg_r[3] <= 32'd0;
		run <= 1'b0; osd_flag <= 1'b0; pahi <= 7'd0; issued <= 1'b0; stale <= 1'b0; kor <= 26'd0;
		au_en <= 1'b0; au_stereo <= 1'b0; au_prime <= 16'd0; au_step <= 32'd0; au_wr <= 18'd0;
		rmb <= 16'd0; rmb_cnt <= 32'd0; lt <= 9'h100;
`ifdef NGAGE_HYB_DIAG
		dg_addr <= 32'd0; dg_wlo <= 32'd0; dg_whi <= 32'd0;
`endif
	end else begin
		if (soc_held) stale <= 1'b0;
		if (rmb_cnt != 32'd0) rmb_cnt <= rmb_cnt - 32'd1;
		else rmb <= 16'd0;

		case (st)
			ST_IDLE: begin
				if (axi_awvalid & axi_awready) begin aw_have <= 1'b1; aw_a <= axi_awaddr; end
				if (axi_wvalid & axi_wready) begin w_have <= 1'b1; w_d <= axi_wdata; end
				if (aw_have & w_have) begin
					aw_have <= 1'b0; w_have <= 1'b0;
					is_rd <= 1'b0; a <= aw_a; d <= w_d; st <= ST_EXEC;
				end else if (axi_arvalid & axi_arready) begin
					is_rd <= 1'b1; a <= axi_araddr; st <= ST_EXEC;
				end
			end
			ST_EXEC: begin
				if (a[29]) begin
					rdata_r <= reg_q;
					st <= ST_RESP;
				end else if (soc_held) begin
					held <= held + 32'd1;
					rdata_r <= 32'd0;
					st <= ST_RESP;
				end else begin
					mem_adr  <= {pahi, a[24:2], a[26:25]};
					mem_rnw  <= is_rd;
					mem_acc  <= a[28:27];
					mem_dout <= d;
					tmo <= 12'd0;
					st <= ST_FWD;
				end
			end
			ST_FWD: begin
				tmo <= tmo + 12'd1;
				if (stale) begin
					if (mem_done) stale <= 1'b0;
				end else if (!issued) begin
					mem_ena <= 1'b1;
					issued <= 1'b1;
				end
				if (issued & mem_done) begin
					issued <= 1'b0;
					rdata_r <= mem_din;
					st <= ST_RESP;
				end else if (tmo == TMO_LAST) begin
					// the soc may still answer later: that done belongs to no transaction
					issued <= 1'b0;
					stale <= ((stale & ~mem_done) | issued) & ~soc_held;
					timeouts <= timeouts + 32'd1;
					rdata_r <= TMO_DATA;
					st <= ST_RESP;
				end
			end
			default: begin
				if ((is_rd & axi_rready) | (~is_rd & axi_bready)) st <= ST_IDLE;
			end
		endcase

		if (reg_wr) case (a[11:2])
			R_SCRATCH: scratch <= d;
			R_CTRL:    run <= d[0];
			R_PAHI:    pahi <= d[6:0];
			R_DBG0:    dbg_r[0] <= d;
			R_DBG1:    dbg_r[1] <= d;
			R_DBG2:    dbg_r[2] <= d;
			R_DBG3:    dbg_r[3] <= d;
			R_KEYS_OR: kor <= d[25:0];
			R_AUD_CTL:  begin au_en <= d[0]; au_stereo <= d[1]; au_prime <= d[31:16]; end
			R_AUD_STEP: au_step <= d;
			R_AUD_WR:   au_wr <= d[17:0];
			R_RUMBLE:   begin rmb <= d[15:0]; rmb_cnt <= RMB_HOLD - 1; end
			R_LIGHT:    lt <= d[8:0];
`ifdef NGAGE_HYB_DIAG
			R_DDR_ADDR: dg_addr <= d;
			R_DDR_WLO:  dg_wlo <= d;
			R_DDR_WHI:  dg_whi <= d;
			R_DDR_CMD:  begin dg_peek <= (d == 32'd1); dg_poke <= (d == 32'd2); end
`endif
			default: ;
		endcase
		if (soc_held) begin au_en <= 1'b0; rmb <= 16'd0; lt <= 9'h100; end

		if (osd_req) osd_flag <= 1'b1;
		else if (reg_wr && a[11:2] == R_STATUS && d[2]) osd_flag <= 1'b0;
	end
end

always @(posedge clk) begin
	if (rst) begin
		vs_d <= 1'b0; lw_snap <= 32'd0; sec_cnt <= 32'd0; fps_cnt <= 16'd0; lcd_fps <= 16'd0;
	end else begin
		vs_d <= vsync;
		if (vs_rise) lw_snap <= lcd_wr;
		if (sec_cnt == FPS_SECOND - 1) begin
			sec_cnt <= 32'd0;
			lcd_fps <= fps_cnt + {15'd0, fps_inc};
			fps_cnt <= 16'd0;
		end else begin
			sec_cnt <= sec_cnt + 32'd1;
			fps_cnt <= fps_cnt + {15'd0, fps_inc};
		end
	end
end

endmodule
