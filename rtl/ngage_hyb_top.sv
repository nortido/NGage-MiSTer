// SPDX-License-Identifier: GPL-3.0-or-later
module ngage_hyb_top #(parameter integer FPS_SECOND = 50000000, parameter integer AUD_CLK_HZ = 50000000)
(
	input             clk,
	input             reset,
	input             pll_locked,

	input             ioctl_download,
	input             ioctl_wr,
	input      [24:0] ioctl_addr,
	input       [7:0] ioctl_dout,
	output            ioctl_wait,
	input      [24:0] rom_size,

	output     [31:0] sd_lba,
	output            sd_rd,
	output            sd_wr,
	input             sd_ack,
	input       [8:0] sd_buff_addr,
	input       [7:0] sd_buff_dout,
	output      [7:0] sd_buff_din,
	input             sd_buff_wr,
	input             img_mounted,
	input      [63:0] img_size,

	input      [25:0] keys,
	input      [15:0] lcd_pix_addr,
	output     [11:0] lcd_pix_data,
	input             vsync,
	input             osd_reset,
	input             turbo,
	input             wx,

	output     [25:2] sdram_addr,
	output      [3:0] sdram_be,
	output     [31:0] sdram_din,
	input      [31:0] sdram_dout,
	output            sdram_req,
	input             sdram_ack,

	output            DDRAM_CLK,
	input             DDRAM_BUSY,
	output      [7:0] DDRAM_BURSTCNT,
	output     [28:0] DDRAM_ADDR,
	input      [63:0] DDRAM_DOUT,
	input             DDRAM_DOUT_READY,
	output            DDRAM_RD,
	output     [63:0] DDRAM_DIN,
	output      [7:0] DDRAM_BE,
	output            DDRAM_WE,

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

	output     [15:0] audio_l,
	output     [15:0] audio_r,
	output     [15:0] rumble,
	output      [8:0] light,

	output            irq,
	output            fiq,
	output     [31:0] dbg0,
	output     [31:0] dbg1,
	output     [31:0] dbg2,
	output     [31:0] dbg3,
	output     [15:0] dbg_sd_rd_count,
	output     [31:0] dbg_lcd_wr,
	output     [15:0] lcd_fps,
	output     [31:0] dbg_keys
);

assign DDRAM_CLK = clk;

reg [1:0] lock_s = 2'b00;
always @(posedge clk) lock_s <= {lock_s[0], pll_locked};
wire rst = ~lock_s[1];

reg osd_d = 1'b0;
always @(posedge clk) osd_d <= osd_reset;
wire osd_pulse = osd_reset & ~osd_d;

// same rule and clock as the hybrid card_present in ngage_mmc.sv: a mount sets it, an unmount (size 0)
// clears it, no reset and no download touches it
reg card_present = 1'b0;
always @(posedge clk) if (img_mounted) card_present <= (img_size != 64'd0);

wire        soc_run, dl_active, rom_loaded, rf_clr;
wire        soc_reset = reset | ioctl_download | dl_active | ~soc_run | osd_pulse | rf_clr;

wire [31:0] fwd_adr, fwd_dout, fwd_din;
wire        fwd_rnw, fwd_ena, fwd_done;
wire  [1:0] fwd_acc;
wire        hyb_irq;
wire        hyb_fiq;
wire  [6:0] dma_pend;
wire  [2:0] flash_array;
wire [24:0] rom_size_q;
wire [31:0] dma_drops;
wire [25:0] keys_or;
wire [31:0] diag_addr;
wire [63:0] diag_wdata, diag_rdata;
wire        diag_peek, diag_poke, diag_busy;
wire        aud_en, aud_stereo, aud_rd, aud_busy, aud_valid;
wire [15:0] aud_prime;
wire [31:0] aud_step, aud_under;
wire [17:0] aud_wr, aud_rdp;
wire [28:0] aud_word;
wire [63:0] aud_rdata;

wire [25:1] dma_addr;
wire [15:0] dma_din, dma_dout;
wire        dma_wrl, dma_wrh, dma_req, dma_ack;

ngage_soc soc
(
	.clk(clk),
	.reset(soc_reset),
	.keys(keys | keys_or),
	.dbg_keys(dbg_keys),
	.dbg_pc(),
	.dbg_regsel(5'd0),
	.dbg_regval(),
	.cpu_halt(),
	.sdram_addr(sdram_addr),
	.sdram_be(sdram_be),
	.sdram_din(sdram_din),
	.sdram_dout(sdram_dout),
	.sdram_req(sdram_req),
	.sdram_ack(sdram_ack),
	.lcd_pix_addr(lcd_pix_addr),
	.lcd_pix_data(lcd_pix_data),
	.rom_size(rom_size),
	.dbg_data_adr(),
	.dbg_openbus_fetch_adr(),
	.dbg_pc_ring1(),
	.dbg_pc_ring2(),
	.dbg_thr_cur(),
	.dbg_thr_cnt(),
	.dbg_lcd_wr(dbg_lcd_wr),
	.dbg_fault(),
	.dbg_sd_rd_count(dbg_sd_rd_count),
	.sd_lba(sd_lba),
	.sd_rd(sd_rd),
	.sd_wr(sd_wr),
	.sd_ack(sd_ack),
	.sd_buff_addr(sd_buff_addr),
	.sd_buff_dout(sd_buff_dout),
	.sd_buff_din(sd_buff_din),
	.sd_buff_wr(sd_buff_wr),
	.img_mounted(img_mounted),
	.img_size(img_size),
	.dma_addr(dma_addr),
	.dma_din(dma_din),
	.dma_wrl(dma_wrl),
	.dma_wrh(dma_wrh),
	.dma_req(dma_req),
	.dma_dout(dma_dout),
	.dma_ack(dma_ack),
	.hyb_mem_adr(fwd_adr),
	.hyb_mem_rnw(fwd_rnw),
	.hyb_mem_ena(fwd_ena),
	.hyb_mem_acc(fwd_acc),
	.hyb_mem_dout(fwd_dout),
	.hyb_mem_din(fwd_din),
	.hyb_mem_done(fwd_done),
	.hyb_irq(hyb_irq),
	.hyb_fiq(hyb_fiq),
	.hyb_dma_pend(dma_pend),
	.hyb_flash_array(flash_array),
	.hyb_rf_clr(rf_clr)
);

assign irq = hyb_irq;
assign fiq = hyb_fiq;

ngage_hyb #(.FPS_SECOND(FPS_SECOND)) u_hyb
(
	.clk(clk),
	.rst(rst),
	.axi_awaddr(axi_awaddr),
	.axi_awvalid(axi_awvalid),
	.axi_awready(axi_awready),
	.axi_wdata(axi_wdata),
	.axi_wstrb(axi_wstrb),
	.axi_wvalid(axi_wvalid),
	.axi_wready(axi_wready),
	.axi_bresp(axi_bresp),
	.axi_bvalid(axi_bvalid),
	.axi_bready(axi_bready),
	.axi_araddr(axi_araddr),
	.axi_arvalid(axi_arvalid),
	.axi_arready(axi_arready),
	.axi_rdata(axi_rdata),
	.axi_rresp(axi_rresp),
	.axi_rvalid(axi_rvalid),
	.axi_rready(axi_rready),
	.mem_adr(fwd_adr),
	.mem_rnw(fwd_rnw),
	.mem_ena(fwd_ena),
	.mem_acc(fwd_acc),
	.mem_dout(fwd_dout),
	.mem_din(fwd_din),
	.mem_done(fwd_done),
	.soc_held(soc_reset),
	.soc_run(soc_run),
	.dl_active(dl_active | ioctl_download),
	.rom_loaded(rom_loaded),
	.card_present(card_present),
	.rom_size(rom_size_q),
	.osd_req(osd_pulse),
	.turbo(turbo),
	.wx(wx),
	.flash_array(flash_array),
	.dma_pend(dma_pend),
	.irq_level(hyb_irq),
	.dma_drops(dma_drops),
	.vsync(vsync),
	.lcd_wr(dbg_lcd_wr),
	.lcd_fps(lcd_fps),
	.dbg0(dbg0),
	.dbg1(dbg1),
	.dbg2(dbg2),
	.dbg3(dbg3),
	.diag_addr(diag_addr),
	.diag_wdata(diag_wdata),
	.diag_peek(diag_peek),
	.diag_poke(diag_poke),
	.diag_rdata(diag_rdata),
	.diag_busy(diag_busy),
	.keys_or(keys_or),
	.rumble(rumble),
	.light(light),
	.aud_en(aud_en),
	.aud_stereo(aud_stereo),
	.aud_prime(aud_prime),
	.aud_step(aud_step),
	.aud_wr(aud_wr),
	.aud_rd(aud_rdp),
	.aud_under(aud_under)
);

ngage_hyb_ddr u_ddr
(
	.clk(clk),
	.rst(rst),
	.ioctl_download(ioctl_download),
	.ioctl_wr(ioctl_wr),
	.ioctl_addr(ioctl_addr),
	.ioctl_dout(ioctl_dout),
	.ioctl_wait(ioctl_wait),
	.rom_size(rom_size),
	.rom_size_q(rom_size_q),
	.dl_active(dl_active),
	.rom_loaded(rom_loaded),
	.dma_addr(dma_addr),
	.dma_din(dma_din),
	.dma_wrl(dma_wrl),
	.dma_wrh(dma_wrh),
	.dma_req(dma_req),
	.dma_dout(dma_dout),
	.dma_ack(dma_ack),
	.dma_drops(dma_drops),
	.diag_addr(diag_addr),
	.diag_wdata(diag_wdata),
	.diag_peek(diag_peek),
	.diag_poke(diag_poke),
	.diag_rdata(diag_rdata),
	.diag_busy(diag_busy),
	.aud_word(aud_word),
	.aud_rd(aud_rd),
	.aud_busy(aud_busy),
	.aud_rdata(aud_rdata),
	.aud_valid(aud_valid),
	.DDRAM_BUSY(DDRAM_BUSY),
	.DDRAM_BURSTCNT(DDRAM_BURSTCNT),
	.DDRAM_ADDR(DDRAM_ADDR),
	.DDRAM_DOUT(DDRAM_DOUT),
	.DDRAM_DOUT_READY(DDRAM_DOUT_READY),
	.DDRAM_RD(DDRAM_RD),
	.DDRAM_DIN(DDRAM_DIN),
	.DDRAM_BE(DDRAM_BE),
	.DDRAM_WE(DDRAM_WE)
);

ngage_hyb_aud #(.CLK_HZ(AUD_CLK_HZ)) u_aud
(
	.clk(clk),
	.rst(rst),
	.srst(soc_reset),
	.en(aud_en),
	.stereo(aud_stereo),
	.prime(aud_prime),
	.step(aud_step),
	.wr(aud_wr),
	.rd(aud_rdp),
	.under(aud_under),
	.ddr_rd(aud_rd),
	.ddr_word(aud_word),
	.ddr_busy(aud_busy),
	.ddr_valid(aud_valid),
	.ddr_rdata(aud_rdata),
	.audio_l(audio_l),
	.audio_r(audio_r)
);

endmodule
