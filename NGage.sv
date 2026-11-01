module emu
(
	`include "sys/emu_ports.vh"
);

///////// Default values for ports not used in this core /////////

assign ADC_BUS  = 'Z;
assign USER_OUT = '1;
assign {UART_RTS, UART_TXD, UART_DTR} = 0;
assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;

assign VGA_F1 = 0;
assign VGA_SCALER  = 0;
assign VGA_DISABLE = 0;
assign HDMI_FREEZE = 0;
assign HDMI_BLACKOUT = 0;
assign HDMI_BOB_DEINT = 0;

assign AUDIO_S   = 1;
wire [15:0] hyb_audio_l, hyb_audio_r;
assign AUDIO_L   = hyb_audio_l;
assign AUDIO_R   = hyb_audio_r;
assign AUDIO_MIX = 0;

assign LED_DISK  = {1'b1, sd_rd | sd_wr};
assign LED_POWER = 0;
assign LED_USER  = ioctl_download;
assign BUTTONS   = 0;

wire [1:0] ar = status[2:1];
wire ar_rot = (status[5:4] == 2'd1) || (status[5:4] == 2'd2);
reg crop_l = 1'b1;
wire [11:0] ar_x = (!ar) ? (crop_l ? (ar_rot ? 12'd208 : 12'd176) : 12'd4) : (ar - 1'd1);
wire [11:0] ar_y = (!ar) ? (crop_l ? (ar_rot ? 12'd176 : 12'd208) : 12'd3) : 12'd0;

`include "build_id.v"
localparam CONF_STR = {
	"NGage;;",
	"-;",
	// hidden by menumask bit 0 = 0; Main still takes the load address from this F entry
	"h0F,ROMBIN,Load ROM,39000000;",
	"S0,ZIP,Load game;",
	"-;",
	"O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[12:11],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"O[5:4],Orientation,Vert,Horz,Horz180;",
	"O[6],Crop,On,Off;",
	"-;",
	"O[9],Real Backlight,Off,On;",
	"O[10],LCD ghosting,Off,Phone;",
	"-;",
	"O[8],Turbo,Off,On;",
	"-;",
	"R0,Reset;",
	"J,OK,LSoft,RSoft,C,Green,Red,1,2,3,4,5,6,7,8,9,*,0,#;",
	"jn,Start,L,R,Select,,,,X,,,A,,B,Y;",
	"jp,Start,L,R,Select,,,,X,,,A,,B,Y;",
	"V,v",`BUILD_DATE
};

wire forced_scandoubler;
wire [1:0] buttons;
wire [127:0] status;

wire        ioctl_download;
wire [15:0] ioctl_index;
wire        ioctl_wr;
wire [26:0] ioctl_addr;
wire  [7:0] ioctl_dout;
wire        ioctl_wait;

wire [21:0] gamma_bus;

wire [31:0] sd_lba;
wire        sd_rd, sd_wr, sd_ack;
wire [13:0] sd_buff_addr;
wire  [7:0] sd_buff_dout, sd_buff_din;
wire        sd_buff_wr, img_mounted;
wire [63:0] img_size;
wire        img_readonly;
wire [15:0] dbg_sd_rd_count;
wire [25:1] dma_addr;
wire [15:0] dma_din, dma_dout;
wire        dma_wrl, dma_wrh, dma_req, dma_ack;

wire [31:0] joystick_0;
wire [10:0] ps2_key;
wire [25:0] keys;
wire [31:0] dbg_keys;

wire [15:0] hyb_rumble;
wire  [8:0] hyb_light;
hps_io #(.CONF_STR(CONF_STR), .VDNUM(1)) hps_io
(
	.clk_sys(clk_cpu),
	.HPS_BUS(HPS_BUS),
	.EXT_BUS(),
	.gamma_bus(gamma_bus),

	.forced_scandoubler(forced_scandoubler),

	.buttons(buttons),
	.status(status),
	.joystick_0(joystick_0),
	.ps2_key(ps2_key),

	.ioctl_download(ioctl_download),
	.ioctl_index(ioctl_index),
	.ioctl_wr(ioctl_wr),
	.ioctl_addr(ioctl_addr),
	.ioctl_dout(ioctl_dout),
	.ioctl_wait(ioctl_wait),

	.status_menumask(16'd0),
	.joystick_0_rumble(hyb_rumble),
	.sd_lba('{sd_lba}),
	.sd_blk_cnt('{6'd0}),
	.sd_rd(sd_rd),
	.sd_wr(sd_wr),
	.sd_ack(sd_ack),
	.sd_buff_addr(sd_buff_addr),
	.sd_buff_dout(sd_buff_dout),
	.sd_buff_din('{sd_buff_din}),
	.sd_buff_wr(sd_buff_wr),
	.img_mounted(img_mounted),
	.img_readonly(img_readonly),
	.img_size(img_size)
);

///////////////////////   CLOCKS   ///////////////////////////////

wire clk_sys; // feeds sdram.sv, 100 MHz
wire clk_cpu; // everything the CPU touches, 50 MHz
wire pll_locked;
wire [63:0] reconfig_to_pll = 64'd0;
wire [63:0] reconfig_from_pll;

pll pll
(
	.refclk(CLK_50M),
	.rst(0),
	.outclk_0(clk_sys),
	.outclk_1(clk_cpu),
	.locked(pll_locked),
	.reconfig_to_pll(reconfig_to_pll),
	.reconfig_from_pll(reconfig_from_pll)
);

wire reset = RESET | status[0] | buttons[1] | ~pll_locked;

// hold the CPU in reset while a ROM download is in progress. There is only
// one loadable file (CONF_STR's single "F,ROMBIN,..."), and the index
// MiSTer or the .mgl hands this core for that entry need not be 0,
// so the gate does not look at ioctl_index.
// It goes by ioctl_download/ioctl_wr alone,
// the same signals ngage_rom_size below uses
// to measure the loaded size.
reg rom_download_d;
always @(posedge clk_cpu) rom_download_d <= ioctl_download;

// bytes actually loaded, for the flash chip0/chip1/chip2 image split in
// ngage_soc; held until the next download starts (updated only on
// ioctl_wr, which only pulses during a download).
wire [24:0] rom_size;
ngage_rom_size rom_size_ld
(
	.clk(clk_cpu),
	.ioctl_download(ioctl_download),
	.ioctl_wr(ioctl_wr),
	.ioctl_addr(ioctl_addr[24:0]),
	.rom_size(rom_size)
);

wire [31:0] dbg_data_adr;
wire [31:0] dbg_thr_cur;
wire [31:0] dbg_thr_cnt;
wire [31:0] dbg_lcd_wr;
wire [31:0] dbg_fault;
wire [15:0] lcd_pix_addr;
wire [11:0] lcd_pix_data;

wire [25:2] cpu_sdram_addr;
wire  [3:0] cpu_sdram_be;
wire [31:0] cpu_sdram_din, cpu_sdram_dout;
wire        cpu_sdram_req, cpu_sdram_ack;

sdram sdram
(
	.SDRAM_DQ(SDRAM_DQ),
	.SDRAM_A(SDRAM_A),
	.SDRAM_DQML(SDRAM_DQML),
	.SDRAM_DQMH(SDRAM_DQMH),
	.SDRAM_BA(SDRAM_BA),
	.SDRAM_nCS(SDRAM_nCS),
	.SDRAM_nWE(SDRAM_nWE),
	.SDRAM_nRAS(SDRAM_nRAS),
	.SDRAM_nCAS(SDRAM_nCAS),
	.SDRAM_CLK(SDRAM_CLK),
	.SDRAM_CKE(SDRAM_CKE),

	.init(~pll_locked),
	.clk(clk_sys),

	.addr0(cpu_sdram_addr),
	.be0(cpu_sdram_be),
	.din0(cpu_sdram_din),
	.dout0(cpu_sdram_dout),
	.req0(cpu_sdram_req),
	.ack0(cpu_sdram_ack),

	.addr1(25'd0),
	.wrl1(1'b0),
	.wrh1(1'b0),
	.din1(16'd0),
	.dout1(),
	.req1(1'b0),
	.ack1(),

	.addr2(25'd0),
	.wrl2(1'b0),
	.wrh2(1'b0),
	.din2(16'd0),
	.dout2(),
	.req2(1'b0),
	.ack2()
);

ngage_input input_map
(
	.clk(clk_cpu),
	.reset(reset | ioctl_download),
	.joy(joystick_0),
	.ps2_key(ps2_key),
	.keys(keys)
);

// pll_locked is synchronized once; ngage_hyb_top adds 2 flops to hyb_lock_s[1], so [3] tracks its
// AXI reset cycle for cycle and gates the handshakes, no beat is accepted while the slave is in reset
reg [3:0] hyb_lock_s = 4'b0000;
always @(posedge clk_cpu) hyb_lock_s <= {hyb_lock_s[2:0], pll_locked};
assign HYB_AXI_CLK  = clk_cpu;
assign HYB_AXI_RSTN = hyb_lock_s[3];

wire hyb_awready, hyb_wready, hyb_arready;
assign HYB_AXI_AWREADY = hyb_awready & HYB_AXI_RSTN;
assign HYB_AXI_WREADY  = hyb_wready  & HYB_AXI_RSTN;
assign HYB_AXI_ARREADY = hyb_arready & HYB_AXI_RSTN;

ngage_hyb_top hyb
(
	.clk(clk_cpu),
	.reset(reset),
	.pll_locked(hyb_lock_s[1]),

	.ioctl_download(ioctl_download),
	.ioctl_wr(ioctl_wr),
	.ioctl_addr(ioctl_addr[24:0]),
	.ioctl_dout(ioctl_dout),
	.ioctl_wait(ioctl_wait),
	.rom_size(rom_size),

	.sd_lba(sd_lba),
	.sd_rd(sd_rd),
	.sd_wr(sd_wr),
	.sd_ack(sd_ack),
	.sd_buff_addr(sd_buff_addr[8:0]),
	.sd_buff_dout(sd_buff_dout),
	.sd_buff_din(sd_buff_din),
	.sd_buff_wr(sd_buff_wr),
	.img_mounted(img_mounted),
	.img_size(img_size),

	.keys(keys),
	.lcd_pix_addr(lcd_pix_addr),
	.lcd_pix_data(lcd_pix_data),
	.vsync(~VGA_VS),
	.osd_reset(status[0] | buttons[1]),
	.turbo(status[8]),
	.wx(1'b1),

	.sdram_addr(cpu_sdram_addr),
	.sdram_be(cpu_sdram_be),
	.sdram_din(cpu_sdram_din),
	.sdram_dout(cpu_sdram_dout),
	.sdram_req(cpu_sdram_req),
	.sdram_ack(cpu_sdram_ack),

	.DDRAM_CLK(DDRAM_CLK),
	.DDRAM_BUSY(DDRAM_BUSY),
	.DDRAM_BURSTCNT(DDRAM_BURSTCNT),
	.DDRAM_ADDR(DDRAM_ADDR),
	.DDRAM_DOUT(DDRAM_DOUT),
	.DDRAM_DOUT_READY(DDRAM_DOUT_READY),
	.DDRAM_RD(DDRAM_RD),
	.DDRAM_DIN(DDRAM_DIN),
	.DDRAM_BE(DDRAM_BE),
	.DDRAM_WE(DDRAM_WE),

	.axi_awaddr(HYB_AXI_AWADDR),
	.axi_awvalid(HYB_AXI_AWVALID & HYB_AXI_RSTN),
	.axi_awready(hyb_awready),
	.axi_wdata(HYB_AXI_WDATA),
	.axi_wstrb(HYB_AXI_WSTRB),
	.axi_wvalid(HYB_AXI_WVALID & HYB_AXI_RSTN),
	.axi_wready(hyb_wready),
	.axi_bresp(HYB_AXI_BRESP),
	.axi_bvalid(HYB_AXI_BVALID),
	.axi_bready(HYB_AXI_BREADY),
	.axi_araddr(HYB_AXI_ARADDR),
	.axi_arvalid(HYB_AXI_ARVALID & HYB_AXI_RSTN),
	.axi_arready(hyb_arready),
	.axi_rdata(HYB_AXI_RDATA),
	.axi_rresp(HYB_AXI_RRESP),
	.axi_rvalid(HYB_AXI_RVALID),
	.axi_rready(HYB_AXI_RREADY),

	.audio_l(hyb_audio_l),
	.audio_r(hyb_audio_r),
	.rumble(hyb_rumble),
	.light(hyb_light),

	.irq(HYB_IRQ),
	.fiq(HYB_FIQ),
	.dbg0(dbg_fault),
	.dbg1(dbg_thr_cur),
	.dbg2(dbg_thr_cnt),
	.dbg3(dbg_data_adr),
	.dbg_sd_rd_count(dbg_sd_rd_count),
	.dbg_lcd_wr(dbg_lcd_wr),
	.lcd_fps(),
	.dbg_keys(dbg_keys)
);

///////////////////////   VIDEO   //////////////////////////////////
// 320x240 at 15.6 kHz / 59.6 Hz so composite and RGB TVs sync. The
// 176x208 LCD framebuffer (RGB444) is drawn at full size from IMG_X0, IMG_Y0
// (status[5:4] rotates it); the rest of the frame is blank.

localparam H_ACTIVE = 320, H_FP = 16, H_SYNC = 32, H_BP = 32;
localparam V_ACTIVE = 240, V_FP = 4,  V_SYNC = 3,  V_BP = 15;
localparam H_TOTAL = H_ACTIVE + H_FP + H_SYNC + H_BP;
localparam V_TOTAL = V_ACTIVE + V_FP + V_SYNC + V_BP;

localparam LCD_W  = 176;
localparam LCD_H  = 208;
localparam IMG_X0 = 72; // centered: (320 - 176) / 2
localparam IMG_Y0 = 16;


// clk_cpu is 50 MHz; divide by 8 for a 6.25 MHz pixel enable
reg [2:0] pix_div;
wire ce_pix = (pix_div == 3'd0);
always @(posedge clk_cpu) pix_div <= pix_div + 1'd1;

reg [9:0] hc;
reg [9:0] vc;
always @(posedge clk_cpu) if (ce_pix) begin
	if (hc == H_TOTAL-1) begin
		hc <= 0;
		vc <= (vc == V_TOTAL-1) ? 10'd0 : vc + 1'd1;
	end else hc <= hc + 1'd1;
end

// Crop changes DE and ARX/ARY only between frames, so the framework detector never measures a mixed frame
always @(posedge clk_cpu) if (ce_pix && hc == 10'd0 && vc == 10'd0) crop_l <= !status[6];


wire        in_img;
wire [15:0] rot_addr;
ngage_vid_rot vid_rot
(
	.clk(clk_cpu),
	.ce_pix(ce_pix),
	.hc(hc),
	.vc(vc),
	.orientation(status[5:4]),
	.in_img(in_img),
	.addr(rot_addr),
	.x_end(),
	.y0()
);

// one ce_pix-deep pipeline: the framebuffer read (second BRAM port) takes
// a cycle, so image/blank/sync all get delayed together to line
// back up with lcd_pix_data once it arrives.
reg        in_img_d, hblank_d, vblank_d, hsync_d, vsync_d;
reg [15:0] lcd_pix_addr_r;
always @(posedge clk_cpu) if (ce_pix) begin
	in_img_d       <= in_img;
	hblank_d       <= (hc >= H_ACTIVE);
	vblank_d       <= (vc >= V_ACTIVE);
	hsync_d        <= (hc >= H_ACTIVE+H_FP && hc < H_ACTIVE+H_FP+H_SYNC);
	vsync_d        <= (vc >= V_ACTIVE+V_FP && vc < V_ACTIVE+V_FP+V_SYNC);
	lcd_pix_addr_r <= rot_addr;
end
assign lcd_pix_addr = lcd_pix_addr_r;

assign VGA_HS = ~hsync_d;
assign VGA_VS = ~vsync_d;
// Crop On: the raster is the picture. The analog OSD is sized from the DE width and the
// scaler takes the same DE, so Crop Off gives the OSD the full raster.
assign VGA_DE = crop_l ? in_img_d : ~(hblank_d | vblank_d);
video_freak video_freak
(
	.CLK_VIDEO(CLK_VIDEO),
	.CE_PIXEL(CE_PIXEL),
	.VGA_VS(VGA_VS),
	.HDMI_WIDTH(HDMI_WIDTH),
	.HDMI_HEIGHT(HDMI_HEIGHT),
	.VGA_DE(),
	.VIDEO_ARX(VIDEO_ARX),
	.VIDEO_ARY(VIDEO_ARY),
	.VGA_DE_IN(VGA_DE),
	.ARX(ar_x),
	.ARY(ar_y),
	.CROP_SIZE(12'd0),
	.CROP_OFF(5'd0),
	.SCALE({1'b0, status[12:11]})
);
assign CLK_VIDEO = clk_cpu;
assign CE_PIXEL  = ce_pix;
assign VGA_SL    = 0;

wire [3:0] pix_r4 = lcd_pix_data[11:8];
wire [3:0] pix_g4 = lcd_pix_data[7:4];
wire [3:0] pix_b4 = lcd_pix_data[3:0];

wire [23:0] img_rgb = in_img_d ? {pix_r4, pix_r4, pix_g4, pix_g4, pix_b4, pix_b4} : 24'd0;
wire [23:0] gh_rgb;
ngage_ghost ghost
(
	.clk(clk_cpu),
	.ce_pix(ce_pix),
	.frame(ce_pix && hc == 10'd0 && vc == 10'd0),
	.enable(status[10]),
	.in_img(in_img_d),
	.addr(lcd_pix_addr_r),
	.pix(lcd_pix_data),
	.rgb_in(img_rgb),
	.rgb_out(gh_rgb)
);
wire [23:0] lit_rgb;
ngage_light light
(
	.clk(clk_cpu),
	.ce_pix(ce_pix),
	.frame(ce_pix && hc == 10'd0 && vc == 10'd0),
	.k(hyb_light),
	.byp(~status[9]),
	.rgb_in(gh_rgb),
	.rgb_out(lit_rgb)
);
wire  [7:0] out_r = lit_rgb[23:16];
wire  [7:0] out_g = lit_rgb[15:8];
wire  [7:0] out_b = lit_rgb[7:0];

assign VGA_R = VGA_DE ? out_r : 8'd0;
assign VGA_G = VGA_DE ? out_g : 8'd0;
assign VGA_B = VGA_DE ? out_b : 8'd0;

endmodule
