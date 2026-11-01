// SoC glue: address decode for the map the firmware
// probes. sdram.sv port 0 is 32-bit: every SDRAM access
// is one transaction through the S_SD state below.
module ngage_soc #(
	parameter [15:0] FLASH_MFG_ID = 16'h0001,
	parameter [15:0] FLASH_DEV_ID = 16'h257E,
	parameter [7:0]  CFI_SIZE     = 8'h17
)
(
	input             clk,
	input             reset,

	input      [25:0] keys,
	output     [31:0] dbg_keys,

	output     [31:0] dbg_pc,
	input       [4:0] dbg_regsel,
	output     [31:0] dbg_regval,
	output            cpu_halt,

	// sdram.sv port 0, 32-bit word addressed, byte enables set for writes only
	output     [25:2] sdram_addr,
	output      [3:0] sdram_be,
	output     [31:0] sdram_din,
	input      [31:0] sdram_dout,
	output            sdram_req,
	input             sdram_ack,

	// LCD framebuffer, read at pixel rate by the video generator
	input      [15:0] lcd_pix_addr,
	output     [11:0] lcd_pix_data,

	// bytes actually loaded by ioctl: chip 0 reads SDRAM 0..rom_size-1
	input      [24:0] rom_size,

	// diagnostics: last data address, and a frozen trail of the PC leading
	// into the first instruction fetch whose data came back 0xFFFFFFFF
	// (any region, not just literal open bus: an unmapped flash sub-range
	// or an empty chip reads the same way) -- see the freeze block below
	output     [31:0] dbg_data_adr,
	output     [31:0] dbg_openbus_fetch_adr,
	output     [31:0] dbg_pc_ring1,
	output     [31:0] dbg_pc_ring2,
	output     [31:0] dbg_thr_cur,
	output     [31:0] dbg_thr_cnt,
	output     [31:0] dbg_lcd_wr,
	output     [31:0] dbg_fault,

	// MMC card and its DMA channels 0..6
	output     [15:0] dbg_sd_rd_count,
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
	output     [25:1] dma_addr,
	output     [15:0] dma_din,
	output            dma_wrl,
	output            dma_wrh,
	output            dma_req,
	input      [15:0] dma_dout,
	input             dma_ack
	,
	input      [31:0] hyb_mem_adr,
	input             hyb_mem_rnw,
	input             hyb_mem_ena,
	input       [1:0] hyb_mem_acc,
	input      [31:0] hyb_mem_dout,
	output     [31:0] hyb_mem_din,
	output            hyb_mem_done,
	output            hyb_irq,
	output            hyb_fiq,
	output      [6:0] hyb_dma_pend,
	output      [2:0] hyb_flash_array,
	output            hyb_rf_clr
);

wire [31:0] mem_adr_raw;
wire        mem_rnw_raw, mem_ena_raw, mem_seq, mem_code_raw, mem_lock;
wire  [1:0] mem_acc_raw;
wire [31:0] mem_dout_raw;
reg  [31:0] mem_din;
reg         mem_done;
wire        mem_abort = 1'b0; // the firmware probes freely; nothing aborts
reg irq_r = 1'b0;
wire [31:0] va_adr, va_dout;
wire        va_wr;
wire        fault_ena;
wire [31:0] fault_far;

assign mem_adr_raw  = hyb_mem_adr;
assign mem_rnw_raw  = hyb_mem_rnw;
assign mem_ena_raw  = hyb_mem_ena;
assign mem_acc_raw  = hyb_mem_acc;
assign mem_dout_raw = hyb_mem_dout;
assign mem_seq      = 1'b0;
assign mem_code_raw = 1'b0;
assign mem_lock     = 1'b0;
assign dbg_pc       = 32'd0;
assign dbg_regval   = 32'd0;
assign cpu_halt     = 1'b0;
assign va_adr       = 32'd0;
assign va_dout      = 32'd0;
assign va_wr        = 1'b0;
assign fault_ena    = 1'b0;
assign fault_far    = 32'd0;
assign hyb_mem_din  = mem_din;
assign hyb_mem_done = mem_done;
assign hyb_irq      = irq_r;
reg    fiq_r        = 1'b0;
assign hyb_fiq      = fiq_r;

// The bus contract says ena is a one-cycle pulse and a well-behaved master
// waits for mem_done before issuing the next one, but the real CPU pipeline
// can issue the next ena a cycle earlier than that (overlapping the tail of
// the in-flight request), while this sequencer only ever looked at mem_ena
// in S_IDLE: a pulse arriving during S_SD/S_OTHER was silently lost,
// and the CPU then took the in-flight request's completion (with whatever
// stale mem_din was left over, e.g. a leftover 0xFFFFFFFF) as if it were
// the answer to the request that got dropped. Park one such request and
// replay it into the same S_IDLE decode as soon as the FSM goes idle.
// The accepted request is registered into mem_* and held until the next
// accept: a live mux in front of the decode made Quartus drop every
// register file to flip-flops (build 17) and let S_SD/S_OTHER see the
// parked request's acc/adr instead of the in-flight one.
localparam S_IDLE = 0, S_SD = 1, S_OTHER = 3;
reg [1:0] state = S_IDLE;

reg        pending_req  = 1'b0;
reg [31:0] pending_adr, pending_dout;
reg        pending_rnw, pending_code;
reg  [1:0] pending_acc;

reg [31:0] mem_adr, mem_dout;
reg        mem_rnw, mem_code;
reg  [1:0] mem_acc;
reg        mem_ena = 1'b0; // one-cycle pulse: mem_* now hold a new request

wire accept = (state == S_IDLE) & ~mem_ena & (mem_ena_raw | pending_req);

always @(posedge clk) begin
	mem_ena <= 1'b0;
	if (reset) begin
		pending_req <= 1'b0;
	end else begin
		if (accept) begin
			mem_ena     <= 1'b1;
			mem_adr     <= pending_req ? pending_adr  : mem_adr_raw;
			mem_rnw     <= pending_req ? pending_rnw  : mem_rnw_raw;
			mem_acc     <= pending_req ? pending_acc  : mem_acc_raw;
			mem_dout    <= pending_req ? pending_dout : mem_dout_raw;
			mem_code    <= pending_req ? pending_code : mem_code_raw;
			pending_req <= 1'b0;
		end
		if (mem_ena_raw & ~(accept & ~pending_req)) begin
			pending_req  <= 1'b1;
			pending_adr  <= mem_adr_raw;
			pending_rnw  <= mem_rnw_raw;
			pending_acc  <= mem_acc_raw;
			pending_dout <= mem_dout_raw;
			pending_code <= mem_code_raw;
		end
	end
end

// ---- address decode -------------------------------------------------
wire sel_flash0 = (mem_adr[31:25] == 7'b0000000);                       // 0x0000_0000-0x01FF_FFFF
wire sel_flash1 = (mem_adr[31:25] == 7'b0000001);                       // 0x0200_0000-0x03FF_FFFF
wire sel_flash2 = (mem_adr[31:25] == 7'b0000010);                       // 0x0400_0000-0x05FF_FFFF
wire sel_tc     = (mem_adr >= 32'h0800_0000) && (mem_adr <= 32'h0800_001F);
// the whole 16MB the firmware probes is RAM, but the part behind it is 8MB,
// so bit 23 folds like an unwired A23; open bus up there let the loader's
// size probe read back its own 0xFFFFFFFF marker and declare 16MB
wire sel_cs2ram = (mem_adr[31:24] == 8'h0A);
wire sel_cs3    = (mem_adr >= 32'h0C10_0000) && (mem_adr <= 32'h0C1F_FFFF);
wire sel_echo1k = (mem_adr >= 32'h0D00_0000) && (mem_adr <= 32'h0DFF_FFFF);
wire sel_sdram  = (mem_adr[31:24] == 8'h10);                             // legacy window, unchanged
wire sel_sram   = (mem_adr >= 32'h2000_0000) && (mem_adr < 32'h2003_0000);

// companion register file sub-blocks, offset within the 0x0C10_0000 page
wire [19:0] cs3_off  = mem_adr[19:0];
wire        sel_asic0 = sel_cs3 && (cs3_off >= 20'h20000) && (cs3_off <= 20'h200FF);
wire        sel_asic1 = sel_cs3 && (cs3_off >= 20'h49000) && (cs3_off <= 20'h49FFF);
wire        sel_gpio1 = sel_cs3 && (cs3_off >= 20'h4F000) && (cs3_off <= 20'h4FFFF);
wire        sel_gpio2 = sel_cs3 && (cs3_off >= 20'h50000) && (cs3_off <= 20'h50FFF);
wire        sel_lcd   = sel_cs3 && (cs3_off >= 20'h21200) && (cs3_off <= 20'h2120F);
wire        sel_mmc   = sel_cs3 && (cs3_off >= 20'h20600) && (cs3_off <= 20'h2063F);
wire        sel_dmach = sel_cs3 && (cs3_off >= 20'h20300) && (cs3_off <= 20'h204BF);
// keypad L7 row control page, 0x0C120C00..0x0C120CFF: left the
// sel_ram2m echo so the model's L7 override and drive write can reach it
wire        sel_kpc   = sel_cs3 && (cs3_off >= 20'h20C00) && (cs3_off <= 20'h20CFF);
wire  [4:0] dmach_n   = cs3_off[10:6] - 5'd12; // channel 0..6
wire [31:0] mmc_rdata;
wire  [6:0] mmc_irq;
wire        lcd_dma_we, lcd_dma_rdy;
wire [31:0] lcd_dma_wdata;

// interrupt controller 0x0C120100..0x0C12027F, six banks of 0x40 (IRQ 0/1/2, FIQ 3/4/5);
// only +08/+0C/+10/+14 have logic, every other offset stays plain RAM (echo)
wire        sel_intc_rng = sel_cs3 && (cs3_off >= 20'h20100) && (cs3_off <= 20'h2027F);
wire        sel_intc     = sel_intc_rng && (cs3_off[5:2] >= 4'd2) && (cs3_off[5:2] <= 4'd5);
wire  [2:0] ic_bank      = cs3_off[9:6] - 4'd4;
// timers 0x0C120A00 + 0x20*k: +04 mode, +08 expired (write clears), +10 count, +18 reload
wire        sel_tmr_rng  = sel_cs3 && (cs3_off[19:8] == 12'h20A);
wire        sel_tmr      = sel_tmr_rng && (cs3_off[4:2] == 3'd1 || cs3_off[4:2] == 3'd2 || cs3_off[4:2] == 3'd4 || cs3_off[4:2] == 3'd6);
wire  [2:0] tmr_k        = cs3_off[7:5];

// 0x0C000000..0x0C1FFFFF is 2MB RAM except the register windows above
wire sel_cs3_full = (mem_adr >= 32'h0C00_0000) && (mem_adr <= 32'h0C1F_FFFF);
wire sel_ram2m = sel_cs3_full & ~(sel_asic0 | sel_asic1 | sel_gpio1 | sel_gpio2 | sel_lcd | sel_intc | sel_tmr | sel_mmc | sel_kpc);

// diagnostics: an 8-entry ring of the last distinct dbg_pc values (pushed
// whenever dbg_pc changes) plus the last data address, both frozen forever
// the first time an instruction fetch's returned data is 0xFFFFFFFF (from
// ANY region: literal open bus, an unmapped flash sub-range, or an empty
// chip all read that way), so the board shows the PC trail leading there
// instead of just the runaway address. The actual freeze trigger lives in
// the S_SD/S_OTHER completion block further down (mem_done_d/mem_din_d),
// since that is where the request's outcome is known.
reg [31:0] pc_ring [0:7];
reg [31:0] pc_prev = 32'd0;
reg        pc_frozen = 1'b0;
reg        cand_valid = 1'b0;
reg [31:0] cand_adr;
reg [31:0] dbg_data_adr_r          = 32'd0;
reg [31:0] dbg_openbus_fetch_adr_r = 32'd0;
reg [31:0] dbg_pc_ring1_r          = 32'd0;
reg [31:0] dbg_pc_ring2_r          = 32'd0;

// latched request info for the freeze check: mem_adr/mem_code are not
// guaranteed stable past the S_IDLE cycle (same reason word_base etc. get
// latched instead of read live at S_SD/S_OTHER, see the FSM below).
// mem_done_d/mem_din_d trail mem_done/mem_din by one cycle so the check
// always reads their settled value instead of racing the same nonblocking
// step that sets them in the FSM.
reg        req_code_latch;
reg [31:0] req_adr_latch;
reg        mem_done_d;
reg [31:0] mem_din_d;

always @(posedge clk) begin
	if (reset) begin
		pc_frozen <= 1'b0;
		cand_valid <= 1'b0;
		pc_prev   <= 32'd0;
	end else if (!pc_frozen) begin
		if (dbg_pc != pc_prev) begin
			pc_ring[7] <= pc_ring[6]; pc_ring[6] <= pc_ring[5]; pc_ring[5] <= pc_ring[4];
			pc_ring[4] <= pc_ring[3]; pc_ring[3] <= pc_ring[2]; pc_ring[2] <= pc_ring[1];
			pc_ring[1] <= pc_ring[0]; pc_ring[0] <= dbg_pc;
			pc_prev    <= dbg_pc;
		end
		// row 4 shows the live PC until the freeze
		dbg_data_adr_r <= dbg_pc;
		// a 0xFFFFFFFF fetch is often just prefetch past a return (0x27FC mov pc,lr
		// then data at 0x2800): freeze only once the pipeline reaches it + 8
		if (mem_done_d & req_code_latch & (mem_din_d == 32'hFFFFFFFF)) begin
			cand_valid <= 1'b1;
			cand_adr   <= req_adr_latch;
		end
		if (cand_valid && dbg_pc == cand_adr + 32'd8) begin
			pc_frozen               <= 1'b1;
			dbg_openbus_fetch_adr_r <= cand_adr;
			dbg_pc_ring1_r <= pc_ring[2];
			dbg_pc_ring2_r <= pc_ring[3];
		end
	end
end
assign dbg_data_adr          = dbg_data_adr_r;
assign dbg_openbus_fetch_adr = dbg_openbus_fetch_adr_r;
assign dbg_pc_ring1          = dbg_pc_ring1_r;
assign dbg_pc_ring2          = dbg_pc_ring2_r;

// VA 0x80006740 is ekern's current-thread variable; every write there is
// a context switch
localparam [31:0] THREAD_CUR_VA = 32'h8000_6740;
reg [31:0] thr_cur = 32'd0, thr_cnt = 32'd0, lcd_wr = 32'd0;
reg [7:0]  flt_cnt = 8'd0;
reg [23:0] flt_far = 24'd0;
always @(posedge clk) begin
	if (reset) begin thr_cur <= 32'd0; thr_cnt <= 32'd0; lcd_wr <= 32'd0; flt_cnt <= 8'd0; flt_far <= 24'd0; end
	else begin
		if (va_wr && va_adr[31:2] == THREAD_CUR_VA[31:2]) begin
			thr_cur <= va_dout;
			thr_cnt <= thr_cnt + 32'd1;
		end
		// accepted DMA words too, so row 3 and STAT show DMA screen updates
		if ((sel_lcd && mem_ena && !mem_rnw) || (lcd_dma_we && lcd_dma_rdy)) lcd_wr <= lcd_wr + 32'd1;
		// only data aborts pulse fault_ena; prefetch
		// aborts set fault_far/fsr too but never this strobe
		if (fault_ena) begin
			if (flt_cnt != 8'hFF) flt_cnt <= flt_cnt + 8'd1;
			flt_far <= fault_far[31:8];
		end
	end
end
assign dbg_thr_cur = thr_cur;
assign dbg_thr_cnt = thr_cnt;
assign dbg_lcd_wr  = lcd_wr;
assign dbg_fault   = {flt_cnt, flt_far};

// byte-lane helper for a 32-bit-wide target, from acc + adr[1:0]
function automatic [3:0] belanes(input [1:0] acc, input [1:0] a10);
	case (acc)
		2'b00:   belanes = 4'b0001 << a10[1:0];
		2'b01:   belanes = a10[1] ? 4'b1100 : 4'b0011;
		default: belanes = 4'b1111;
	endcase
endfunction

wire [3:0] lanes = belanes(mem_acc, mem_adr[1:0]);
wire [31:0] wlane = shiftwr(32'd0, mem_acc, mem_adr[1:0], mem_dout);

// LSB-aligned byte/halfword read shift: acc picks the width, adr[1:0]
// (lowbits) the lane.
function automatic [31:0] shiftrd(input [31:0] raw, input [1:0] acc, input [1:0] lowbits);
	case (acc)
		2'b00: shiftrd = {24'd0, (raw >> (lowbits * 8)) & 32'hFF};
		2'b01: shiftrd = lowbits[1] ? {16'd0, raw[31:16]} : {16'd0, raw[15:0]};
		default: shiftrd = raw;
	endcase
endfunction

// merge an LSB-aligned byte/halfword store into an old 32-bit word
function automatic [31:0] shiftwr(input [31:0] old, input [1:0] acc, input [1:0] lowbits, input [31:0] wd);
	reg [31:0] v;
	begin
		v = old;
		case (acc)
			2'b00: case (lowbits)
				2'b00: v[7:0]   = wd[7:0];
				2'b01: v[15:8]  = wd[7:0];
				2'b10: v[23:16] = wd[7:0];
				default: v[31:24] = wd[7:0];
			endcase
			2'b01: if (lowbits[1]) v[31:16] = wd[15:0]; else v[15:0] = wd[15:0];
			default: v = wd;
		endcase
		shiftwr = v;
	end
endfunction

// SRAM 48Kx32 as four byte arrays: Quartus 17 infers M10K for this shape,
// the packed [31:0] with per-byte writes went to registers in build 2.
reg [7:0] sram0 [0:49151];
reg [7:0] sram1 [0:49151];
reg [7:0] sram2 [0:49151];
reg [7:0] sram3 [0:49151];
wire [15:0] sram_idx = mem_adr[17:2];
wire sram_we = sel_sram & mem_ena & ~mem_rnw;
reg [31:0] sram_q;
always @(posedge clk) begin
	if (sram_we & lanes[0]) sram0[sram_idx] <= mem_dout[7:0];
	if (sram_we & lanes[1]) sram1[sram_idx] <= mem_dout[15:8];
	if (sram_we & lanes[2]) sram2[sram_idx] <= mem_dout[23:16];
	if (sram_we & lanes[3]) sram3[sram_idx] <= mem_dout[31:24];
	sram_q <= {sram3[sram_idx], sram2[sram_idx], sram1[sram_idx], sram0[sram_idx]};
end

// each soc reset zeroes the register files as a fresh core load does; the top
// holds reset until the 1024-entry sweep ends, so a one-clock OSD pulse clears too
reg  [10:0] rf_clr_cnt = 11'd0;
reg         rf_rst_d = 1'b0;
wire        rf_clr = ~rf_clr_cnt[10];
wire  [9:0] rf_clr_idx = rf_clr_cnt[9:0];
always @(posedge clk) begin
	rf_rst_d <= reset;
	if (reset & ~rf_rst_d) rf_clr_cnt <= 11'd0;
	else if (rf_clr) rf_clr_cnt <= rf_clr_cnt + 11'd1;
end
assign hyb_rf_clr = rf_clr;

// traffic controller: 8 words, plain echo except reg 0 always reads ready
wire [2:0] tc_idx = mem_adr[4:2];
wire       tc_we  = sel_tc & mem_ena & ~mem_rnw;
wire [31:0] tc_m;
ngage_regfile #(.AW(3)) tc_rf (.clk(clk), .clr(rf_clr), .clr_idx(rf_clr_idx), .we(tc_we), .lanes(lanes), .idx(tc_idx), .wd(wlane), .q(tc_m));
wire [31:0] tc_q = (tc_idx == 3'd0) ? (tc_m | 32'h00000800) : tc_m;
wire [31:0] tc_rdata = shiftrd(tc_q, mem_acc, mem_adr[1:0]);

// companion ASIC 0x0C120000 block: 32-word echo, 0x0C120020 poll override
wire [4:0] asic0_idx = cs3_off[6:2];
wire       asic0_we  = sel_asic0 & mem_ena & ~mem_rnw;
wire [31:0] asic0_m;
ngage_regfile #(.AW(5)) asic0_rf (.clk(clk), .clr(rf_clr), .clr_idx(rf_clr_idx), .we(asic0_we), .lanes(lanes), .idx(asic0_idx), .wd(wlane), .q(asic0_m));
wire [31:0] asic0_q = (asic0_idx == 5'd8) ? 32'h0C000000 : asic0_m;
wire [31:0] asic0_rdata = shiftrd(asic0_q, mem_acc, mem_adr[1:0]);

// companion ASIC 0x0C149000 block: 8-word echo, always OR 1 on read (ready)
wire [2:0] asic1_idx = cs3_off[4:2];
wire       asic1_we  = sel_asic1 & mem_ena & ~mem_rnw;
wire [31:0] asic1_m;
ngage_regfile #(.AW(3)) asic1_rf (.clk(clk), .clr(rf_clr), .clr_idx(rf_clr_idx), .we(asic1_we), .lanes(lanes), .idx(asic1_idx), .wd(wlane), .q(asic1_m));
wire [31:0] asic1_q = asic1_m | 32'h00000001;
wire [31:0] asic1_rdata = shiftrd(asic1_q, mem_acc, mem_adr[1:0]);

// 16-bit GPIO echoes: small wrapping register files, address aliased on
// the low bits like the 0x0D000000 block below (log nothing, keep cheap)
wire [7:0] gpio1_idx = cs3_off[9:2];
wire       gpio1_we  = sel_gpio1 & mem_ena & ~mem_rnw;
wire [31:0] gpio1_m;
ngage_regfile #(.AW(8)) gpio1_rf (.clk(clk), .clr(rf_clr), .clr_idx(rf_clr_idx), .we(gpio1_we), .lanes(lanes), .idx(gpio1_idx), .wd(wlane), .q(gpio1_m));
wire [31:0] gpio1_q = gpio1_m;
wire [31:0] gpio1_rdata = shiftrd(gpio1_q, mem_acc, mem_adr[1:0]);

wire [9:0] gpio2_idx = cs3_off[11:2];
// the companion serial port lives at word slots 0x108 (0x0C150420 cmd /
// 0x0C150422 status) and 0x109 (0x0C150424 / 0x0C150426 data); everything
// else in this page stays the plain echo array
wire       sel_ser_cmdstat = sel_gpio2 & (gpio2_idx == 10'h108);
wire       sel_ser_data    = sel_gpio2 & (gpio2_idx == 10'h109);
wire       gpio2_we  = sel_gpio2 & mem_ena & ~mem_rnw & ~sel_ser_cmdstat & ~sel_ser_data;
wire [31:0] gpio2_m;
ngage_regfile #(.AW(10)) gpio2_rf (.clk(clk), .clr(rf_clr), .clr_idx(rf_clr_idx), .we(gpio2_we), .lanes(lanes), .idx(gpio2_idx), .wd(wlane), .q(gpio2_m));

// keypad model: one instance overrides both the gpio2 pin/status words
// and the L7 page below, whichever the current transaction selects
wire  [5:0] kpc_idx = cs3_off[7:2];
wire        kpc_we  = sel_kpc & mem_ena & ~mem_rnw;
wire        kp_we   = mem_ena & ~mem_rnw & (sel_gpio2 | sel_kpc) & ~sel_ser_cmdstat & ~sel_ser_data;
wire [9:0]  kp_word_idx = sel_gpio2 ? gpio2_idx : {4'd0, kpc_idx};
wire [31:0] kp_ovr_mask, kp_ovr_val;
wire        kp_irq;
ngage_keypad kp
(
	.clk(clk), .reset(reset), .keys(keys),
	.we(kp_we), .sel_g2(sel_gpio2), .sel_kpc(sel_kpc), .word_idx(kp_word_idx), .lanes(lanes), .wlane(wlane),
	.ovr_mask(kp_ovr_mask), .ovr_val(kp_ovr_val), .irq(kp_irq), .dbg_keys(dbg_keys)
);

// euart1.pdd busy-wait on 0x0C150514 (idx 0x145) polls bit 2, else plain echo
wire [31:0] gpio2_base = gpio2_m | ((gpio2_idx == 10'h145) ? 32'h00000004 : 32'h0);
wire [31:0] gpio2_q = (gpio2_base & ~kp_ovr_mask) | (kp_ovr_val & kp_ovr_mask);
wire [31:0] gpio2_rdata = shiftrd(gpio2_q, mem_acc, mem_adr[1:0]);

// keypad L7 page: 64-word echo, override at idx 1 (read) / idx 5 (drive)
wire [31:0] kpc_m;
ngage_regfile #(.AW(6)) kpc_rf (.clk(clk), .clr(rf_clr), .clr_idx(rf_clr_idx), .we(kpc_we), .lanes(lanes), .idx(kpc_idx), .wd(wlane), .q(kpc_m));
wire [31:0] kpc_q = (kpc_m & ~kp_ovr_mask) | (kp_ovr_val & kp_ovr_mask);
wire [31:0] kpc_rdata = shiftrd(kpc_q, mem_acc, mem_adr[1:0]);

// companion serial port model: command (0x420), status (0x422, bit8 is
// the read flag and not part of the register number, bit10 goes ready 4
// clocks after a write to 0x422), data (0x426) into a 256-entry halfword
// table selected by the low 8 bits of the last value written to 0x422
// (register 0x136 and register 0x36 are the same table slot). Power-on
// defaults 0x80=0x0400, 0x23=0x0030, 0x3A=0x0030, else 0; writes to any
// register simply store, reads return whatever is stored, except 0x3A
// which is an ADC result muxed on the channel last selected via 0x39
// (see ser_adc_ctl below).
reg [31:0] ser_cmdstat;
reg  [2:0] ser_ready_ctr;
reg  [7:0] ser_reg_idx;
reg [15:0] ser_data [0:255];
reg [31:0] ser_dataword;
reg [15:0] ser_data_disp;
reg  [7:0] ser_adc_ctl;
reg        ser_adc_hit;

integer ser_data_i;
initial begin
	for (ser_data_i = 0; ser_data_i < 256; ser_data_i = ser_data_i + 1) ser_data[ser_data_i] = 16'h0000;
	ser_data[8'h80] = 16'h0400;
	ser_data[8'h23] = 16'h0030;
	ser_data[8'h3A] = 16'h0030;
end

wire ser_touches_hi     = (mem_acc == 2'b10) || mem_adr[1];
wire ser_cmdstat_we     = sel_ser_cmdstat & mem_ena & ~mem_rnw;
wire ser_dataword_we    = sel_ser_data    & mem_ena & ~mem_rnw;

always @(posedge clk) begin
	if (ser_cmdstat_we) begin
		ser_cmdstat <= shiftwr(ser_cmdstat, mem_acc, mem_adr[1:0], mem_dout);
		if (ser_touches_hi) begin
			ser_reg_idx   <= (mem_acc == 2'b10) ? mem_dout[23:16] : mem_dout[7:0];
			ser_ready_ctr <= 3'd4;
		end
	end
	if (ser_ready_ctr != 3'd0) ser_ready_ctr <= ser_ready_ctr - 3'd1;

	if (ser_dataword_we) begin
		ser_dataword <= shiftwr(ser_dataword, mem_acc, mem_adr[1:0], mem_dout);
		if (ser_touches_hi) begin
			ser_data[ser_reg_idx] <= (mem_acc == 2'b10) ? mem_dout[31:16] : mem_dout[15:0];
			// reg 0x39 selects the ADC channel, 0x3A returns the conversion;
			// channel 8 (cbus, attribute 0x23 battery BSI) reads 480 = cbus
			// class 1 normal battery, other channels read the stored 0x3A value
			if (ser_reg_idx == 8'h39)
				ser_adc_ctl <= (mem_acc == 2'b10) ? mem_dout[23:16] : mem_dout[7:0];
		end
	end
	ser_data_disp <= ser_data[ser_reg_idx];
	ser_adc_hit   <= (ser_reg_idx == 8'h3A) && (ser_adc_ctl[7:4] == 4'h8);
end

wire [31:0] ser_cmdstat_read = (ser_ready_ctr == 3'd0) ? (ser_cmdstat | 32'h04000000) : (ser_cmdstat & ~32'h04000000);
wire [31:0] ser_cmdstat_rdata = shiftrd(ser_cmdstat_read, mem_acc, mem_adr[1:0]);
wire [31:0] ser_dataword_read = {ser_adc_hit ? 16'h01E0 : ser_data_disp, ser_dataword[15:0]};
wire [31:0] ser_data_rdata = shiftrd(ser_dataword_read, mem_acc, mem_adr[1:0]);

// 26 MHz from 50 MHz: 13/25 fractional tick, one accumulator shared by all timers
reg [4:0] acc26 = 5'd0;
reg       ce26  = 1'b0;
always @(posedge clk)
	if (acc26 >= 5'd12) begin acc26 <= acc26 - 5'd12; ce26 <= 1'b1; end
	else                begin acc26 <= acc26 + 5'd13; ce26 <= 1'b0; end

// interrupt controller and 8 timers;
// IRQ is bank 0 pending & enabled only, no FIQ source exists to route
reg [5:0][31:0] ic_en; reg [7:0][31:0] tm_cnt, tm_rel; reg [7:0][1:0] tm_mode; reg [7:0] tm_flag;
wire intc_we = sel_intc & mem_ena & ~mem_rnw; wire tmr_we = sel_tmr & mem_ena & ~mem_rnw;
integer k;
always @(posedge clk)
	if (reset) begin
		ic_en <= 192'd0; tm_cnt <= 256'd0; tm_rel <= 256'd0; tm_mode <= 16'd0; tm_flag <= 8'd0;
	end else begin
		for (k = 0; k < 8; k = k + 1) if (ce26 && tm_mode[k] != 2'd0) begin
			if (tm_cnt[k] == 32'd0) begin
				tm_flag[k] <= 1'b1;
				if (tm_mode[k] >= 2'd2) tm_cnt[k] <= tm_rel[k]; else tm_cnt[k] <= tm_cnt[k] - 32'd1; // bit1 of a 2-bit field
			end else tm_cnt[k] <= tm_cnt[k] - 32'd1;
		end
		if (intc_we) case (cs3_off[5:2])
			4'd2: ic_en[ic_bank] <= ic_en[ic_bank] | wlane;
			4'd3: ic_en[ic_bank] <= ic_en[ic_bank] & ~wlane;
			default: ;
		endcase
		if (tmr_we) case (cs3_off[4:2])
			3'd1: tm_mode[tmr_k] <= wlane[1:0];
			3'd2: tm_flag[tmr_k] <= 1'b0;
			3'd4: tm_cnt[tmr_k] <= wlane;
			3'd6: tm_rel[tmr_k] <= wlane;
			default: ;
		endcase
	end
always @(posedge clk) irq_r <= |(tm_flag & ic_en[0][7:0]) | |(mmc_irq & ic_en[0][12:6]) | (kp_irq & ic_en[0][25]);
// FIQ bank 3 sees the timers only (the guest FIQ handler polls timer 3); banks 4 and 5 have no source
always @(posedge clk) fiq_r <= |(tm_flag & ic_en[3][7:0]);

function automatic [5:0] lowest1(input [31:0] v);
	integer i;
	begin
		lowest1 = 6'd0;
		for (i = 31; i >= 0; i = i - 1) if (v[i]) lowest1 = i + 1;
	end
endfunction
wire [31:0] ic_pend = (ic_bank == 3'd0) ? ({24'd0, tm_flag} | {19'd0, mmc_irq, 6'd0} | {6'd0, kp_irq, 25'd0}) :
                      (ic_bank == 3'd3) ? {24'd0, tm_flag} : 32'd0;
wire [31:0] intc_q = (cs3_off[5:2] == 4'd4) ? ic_pend : (cs3_off[5:2] == 4'd5) ? {26'd0, lowest1(ic_pend & ic_en[ic_bank])} : ic_en[ic_bank];
wire [31:0] intc_rdata = shiftrd(intc_q, mem_acc, mem_adr[1:0]);
wire [31:0] tmr_q = (cs3_off[4:2] == 3'd1) ? {30'd0, tm_mode[tmr_k]} : (cs3_off[4:2] == 3'd2) ? {31'd0, tm_flag[tmr_k]} : (cs3_off[4:2] == 3'd4) ? tm_cnt[tmr_k] : tm_rel[tmr_k];
wire [31:0] tmr_rdata = shiftrd(tmr_q, mem_acc, mem_adr[1:0]);

// echo register file at 0x0D000000..0x0DFFFFFF: small, aliased, no logging
wire [9:0] echo1k_idx = mem_adr[11:2];
wire       echo1k_we  = sel_echo1k & mem_ena & ~mem_rnw;
wire [31:0] echo1k_m;
ngage_regfile #(.AW(10)) echo1k_rf (.clk(clk), .clr(rf_clr), .clr_idx(rf_clr_idx), .we(echo1k_we), .lanes(lanes), .idx(echo1k_idx), .wd(wlane), .q(echo1k_m));
wire [31:0] echo1k_q = echo1k_m;
wire [31:0] echo1k_rdata = shiftrd(echo1k_q, mem_acc, mem_adr[1:0]);

// LCD command port
wire [31:0] lcd_rdata;
ngage_lcd lcd
(
	.clk(clk),
	.reset(reset),
	.sel(sel_lcd),
	.ena(mem_ena),
	.rnw(mem_rnw),
	.off34(mem_adr[3:2]),
	.wdata(mem_dout),
	.rdata(lcd_rdata),
	.pix_addr(lcd_pix_addr),
	.pix_data(lcd_pix_data),
	.dma_we(lcd_dma_we),
	.dma_wdata(lcd_dma_wdata),
	.dma_rdy(lcd_dma_rdy)
);

// MMC card controller and its DMA channels 0..6
ngage_mmc mmc
(
	.clk(clk), .reset(reset),
	.sel_mmc(sel_mmc), .sel_dma(sel_dmach), .dma_k(dmach_n[2:0]),
	.ena(mem_ena), .rnw(mem_rnw),
	.off(cs3_off[5:0]), .wdata(mem_dout),
	.rdata(mmc_rdata), .irq(mmc_irq), .rd_count(dbg_sd_rd_count),
	.sd_lba(sd_lba), .sd_rd(sd_rd), .sd_wr(sd_wr), .sd_ack(sd_ack),
	.sd_buff_addr(sd_buff_addr), .sd_buff_dout(sd_buff_dout), .sd_buff_din(sd_buff_din),
	.sd_buff_wr(sd_buff_wr), .img_mounted(img_mounted), .img_size(img_size),
	.dma_addr(dma_addr), .dma_din(dma_din), .dma_dout(dma_dout),
	.dma_wrl(dma_wrl), .dma_wrh(dma_wrh), .dma_req(dma_req), .dma_ack(dma_ack),
	.lcd_we(lcd_dma_we), .lcd_wdata(lcd_dma_wdata), .lcd_rdy(lcd_dma_rdy)
);

// flash command FSMs: off is the byte offset inside each chip's 32MB
// window, independent per chip. Three real 8MB x16 devices on CS0 hold the
// 18.6MB ROM image spread across file-offset ranges per the firmware's
// memory-map table: chip0 carries file 0..0x6FFFFF
// at PA offset 0..0x6FFFFF and file 0x1300000.. at PA offset 0x700000..
// 0x7FFFFF; chip1 carries file 0x700000..0xAFFFFF at PA offset 0..0x3FFFFF;
// chip2 carries file 0xB00000..0x12FFFFF at PA offset 0..0x7FFFFF. Anything
// outside those PA sub-ranges, or past rom_size, is unmapped (0xFFFFFFFF).
wire flash0_array_mode, flash1_array_mode, flash2_array_mode;
wire [31:0] flash0_id, flash1_id, flash2_id;
ngage_flash #(.FLASH_MFG_ID(FLASH_MFG_ID), .FLASH_DEV_ID(FLASH_DEV_ID), .CFI_SIZE(CFI_SIZE)) flash0
(
	.clk(clk), .reset(reset),
	.sel(sel_flash0), .ena(mem_ena), .rnw(mem_rnw),
	.off(mem_adr[24:0]), .wdata(mem_dout),
	.array_mode(flash0_array_mode), .id_rdata(flash0_id)
);
ngage_flash #(.FLASH_MFG_ID(FLASH_MFG_ID), .FLASH_DEV_ID(FLASH_DEV_ID), .CFI_SIZE(CFI_SIZE)) flash1
(
	.clk(clk), .reset(reset),
	.sel(sel_flash1), .ena(mem_ena), .rnw(mem_rnw),
	.off(mem_adr[24:0]), .wdata(mem_dout),
	.array_mode(flash1_array_mode), .id_rdata(flash1_id)
);
ngage_flash #(.FLASH_MFG_ID(FLASH_MFG_ID), .FLASH_DEV_ID(FLASH_DEV_ID), .CFI_SIZE(CFI_SIZE)) flash2
(
	.clk(clk), .reset(reset),
	.sel(sel_flash2), .ena(mem_ena), .rnw(mem_rnw),
	.off(mem_adr[24:0]), .wdata(mem_dout),
	.array_mode(flash2_array_mode), .id_rdata(flash2_id)
);
wire [31:0] flash0_rdata = shiftrd(flash0_array_mode ? 32'hFFFFFFFF : flash0_id, mem_acc, mem_adr[1:0]);
wire [31:0] flash1_rdata = shiftrd(flash1_array_mode ? 32'hFFFFFFFF : flash1_id, mem_acc, mem_adr[1:0]);
wire [31:0] flash2_rdata = shiftrd(flash2_array_mode ? 32'hFFFFFFFF : flash2_id, mem_acc, mem_adr[1:0]);

wire [24:0] chip_off = mem_adr[24:0]; // byte offset inside whichever chip is selected

wire        chip0_regA      = chip_off < 25'h700000;
wire        chip0_regB      = (chip_off >= 25'h700000) && (chip_off < 25'h800000);
wire        chip0_mapped    = chip0_regA | chip0_regB;
wire [24:0] chip0_sdram_off = chip0_regA ? chip_off : (25'h1300000 + (chip_off - 25'h700000));
wire        chip0_valid     = chip0_mapped && (chip0_sdram_off < rom_size);

wire        chip1_mapped    = chip_off < 25'h400000;
wire [24:0] chip1_sdram_off = 25'h700000 + chip_off;
wire        chip1_valid     = chip1_mapped && (chip1_sdram_off < rom_size);

wire        chip2_mapped    = chip_off < 25'h800000;
wire [24:0] chip2_sdram_off = 25'hB00000 + chip_off;
wire        chip2_valid     = chip2_mapped && (chip2_sdram_off < rom_size);

// requests that go through SDRAM: valid flash array reads on any of the
// three chips, the two new RAM windows and the untouched legacy window
wire flash0_do_sdram = sel_flash0 & mem_rnw & flash0_array_mode & chip0_valid;
wire flash1_do_sdram = sel_flash1 & mem_rnw & flash1_array_mode & chip1_valid;
wire flash2_do_sdram = sel_flash2 & mem_rnw & flash2_array_mode & chip2_valid;
wire go_sdram = flash0_do_sdram | flash1_do_sdram | flash2_do_sdram | sel_cs2ram | sel_ram2m | sel_sdram;

// everything answered in one cycle without touching SDRAM
wire [31:0] other_rdata =
	sel_tc                             ? tc_rdata     :
	sel_mmc                            ? mmc_rdata    :
	sel_asic0                          ? asic0_rdata  :
	sel_intc                           ? intc_rdata   :
	sel_tmr                            ? tmr_rdata    :
	sel_asic1                          ? asic1_rdata  :
	sel_gpio1                          ? gpio1_rdata  :
	sel_ser_cmdstat                    ? ser_cmdstat_rdata :
	sel_ser_data                       ? ser_data_rdata    :
	sel_kpc                            ? kpc_rdata    :
	sel_gpio2                          ? gpio2_rdata  :
	sel_lcd                            ? lcd_rdata    :
	sel_echo1k                         ? echo1k_rdata :
	sel_sram                           ? sram_q       :
	sel_flash0                         ? flash0_rdata :
	sel_flash1                         ? flash1_rdata :
	sel_flash2                         ? flash2_rdata :
	32'hFFFFFFFF; // sel_none: open bus

reg       req_toggle = 1'b0;
reg [24:0] word_base;
reg [31:0] wr_data;
reg  [3:0] wr_be;

// SDRAM byte offsets for the RAM windows, in words (bit 0 of the byte
// offset is folded into the halfword address, same as the existing window).
// 32MB chip layout: ROM at byte 0 (20MB reserved), 8MB RAM at 0x1400000,
// 1MB RAM at 0x1C00000; nothing overlaps or wraps past the 24-bit word bus.
localparam [24:0] SDRAM_WORD_OFF   = 25'h0800000; // legacy 0x10000000 window, 16MB in
localparam [24:0] RAM8M_WORD_OFF   = 25'h0A00000; // 0x0A000000 window, byte offset 0x1400000
localparam [24:0] RAM2M_WORD_OFF   = 25'h1200000; // 0x0C000000 window, byte offset 0x1C00000

reg wr_en; // latched at S_IDLE with word_base/wr_data, not read combinationally later

// clk_cpu and clk_sys share one PLL (one exclusive group in sys/sys_top.sdc), so this STA-timed
// path is sampled once, as sdram.sv samples req0; dout beat 0 is written with ack0 at READY, beat 1
// one clk_sys later, both held >= 6 clk_sys after that, so they are stable when the flopped ack matches.
reg sdram_ack_sync;
always @(posedge clk) sdram_ack_sync <= sdram_ack;
wire sdram_ack_s = sdram_ack_sync;

// every window offset is a multiple of 4 bytes, so word_base[0] == mem_adr[1] and a word
// access (acc 10 with lowbits 00) never starts at an odd halfword
assign sdram_addr = word_base[24:1];
assign sdram_din  = wr_data;
assign sdram_be   = wr_en ? wr_be : 4'b0000;
assign sdram_req  = req_toggle;

always @(posedge clk) begin
	mem_done <= 1'b0;

	if (reset) begin
		state      <= S_IDLE;
		req_toggle <= sdram_ack_s; // sdram.sv's ack has no reset of its own
	end else case (state)
		S_IDLE: if (mem_ena) begin
			req_code_latch <= mem_code;
			req_adr_latch  <= mem_adr;
			if (go_sdram) begin
				word_base  <= (sel_flash0 ? {1'b0, chip0_sdram_off[24:1]} :
				               sel_flash1 ? {1'b0, chip1_sdram_off[24:1]} :
				               sel_flash2 ? {1'b0, chip2_sdram_off[24:1]} :
				               sel_cs2ram ? RAM8M_WORD_OFF + {2'd0, mem_adr[23:1]} :
				               sel_ram2m  ? RAM2M_WORD_OFF + {5'd0, mem_adr[20:1]} :
				               SDRAM_WORD_OFF + {2'b0, mem_adr[23:1]});
				wr_data    <= (mem_acc == 2'b10) ? mem_dout : mem_acc[0] ? {2{mem_dout[15:0]}} : {4{mem_dout[7:0]}};
				wr_be      <= (mem_acc == 2'b10) ? 4'b1111 : mem_acc[0] ? (mem_adr[1] ? 4'b1100 : 4'b0011) : 4'b0001 << mem_adr[1:0];
				// all three flash chips are read only: writes reach the sequencer but never assert sdram_be
				wr_en      <= ~mem_rnw & ~sel_flash0 & ~sel_flash1 & ~sel_flash2;
				req_toggle <= ~req_toggle;
				state      <= S_SD;
			end else begin
				state <= S_OTHER;
			end
		end

		S_SD: if (sdram_ack_s == req_toggle) begin
			mem_din  <= (mem_acc == 2'b10) ? sdram_dout : shiftrd(sdram_dout, {1'b0, mem_acc[0]}, mem_adr[1:0]);
			mem_done <= 1'b1;
			state    <= S_IDLE;
		end

		S_OTHER: begin
			mem_din  <= other_rdata;
			mem_done <= 1'b1;
			state    <= S_IDLE;
		end

		default: state <= S_IDLE;
	endcase

	// one cycle behind mem_done/mem_din so the freeze check below always
	// sees the settled completion of the request that req_adr_latch/
	// req_code_latch describe (mem_din is written in the same nonblocking
	// step as mem_done above, so reading it in the same cycle would race)
	mem_done_d <= mem_done;
	mem_din_d  <= mem_din;
end

assign hyb_dma_pend    = mmc_irq;
assign hyb_flash_array = {flash2_array_mode, flash1_array_mode, flash0_array_mode};

endmodule

// 32-bit register file as four byte arrays with lane enables: the
// read-modify-write form (shiftwr on the stored word) needs an async read
// and Quartus 17 then builds it from registers
module ngage_regfile #(parameter AW = 8)
(
	input             clk,
	input             clr,
	input       [9:0] clr_idx,
	input             we,
	input       [3:0] lanes,
	input  [AW-1:0]   idx,
	input      [31:0] wd,
	output reg [31:0] q
);
reg [7:0] m0 [0:(1<<AW)-1];
reg [7:0] m1 [0:(1<<AW)-1];
reg [7:0] m2 [0:(1<<AW)-1];
reg [7:0] m3 [0:(1<<AW)-1];
// the sweep owns the port: one address for write and read keeps the single-port RAM
wire [AW-1:0] a = clr ? clr_idx[AW-1:0] : idx;
always @(posedge clk) begin
	if (clr | (we & lanes[0])) m0[a] <= clr ? 8'd0 : wd[7:0];
	if (clr | (we & lanes[1])) m1[a] <= clr ? 8'd0 : wd[15:8];
	if (clr | (we & lanes[2])) m2[a] <= clr ? 8'd0 : wd[23:16];
	if (clr | (we & lanes[3])) m3[a] <= clr ? 8'd0 : wd[31:24];
	q <= {m3[a], m2[a], m1[a], m0[a]};
end
endmodule
