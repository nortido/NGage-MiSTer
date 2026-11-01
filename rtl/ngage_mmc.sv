// MMC controller (0x0C120600..0x63F) plus DMA channels 0..6: the card path
// counts bytes, but the LCD-destination channel counts 32-bit words minus 1.
// Real command/state subset, SD-MiSTer
// backed; DMA reads and writes whole sectors. New command vs continuation: 0x61C written since last start.
module ngage_mmc
(
	input             clk, reset,

	input             sel_mmc,  // cs3_off 0x600..0x63F
	input             sel_dma,  // cs3_off 0x300..0x4BF, DMA channels 0..6 (write snoop only, the RAM behind echoes reads)
	input       [2:0] dma_k,
	input             ena, rnw,
	input       [5:0] off,
	input      [31:0] wdata,
	output reg [31:0] rdata,
	output      [6:0] irq,
	output reg [15:0] rd_count,

	output reg [31:0] sd_lba,
	output reg        sd_rd,
	output reg        sd_wr,
	input             sd_ack,
	input       [8:0] sd_buff_addr,
	input       [7:0] sd_buff_dout,
	output      [7:0] sd_buff_din,
	input             sd_buff_wr,
	input             img_mounted,
	input      [63:0] img_size,

	output reg [25:1] dma_addr,
	output reg [15:0] dma_din,
	input      [15:0] dma_dout,
	output reg        dma_wrl, dma_wrh,
	output reg        dma_req,
	input             dma_ack,

	output reg        lcd_we,    // held until lcd_rdy: the CPU owns the LCD port on a write cycle
	output reg [31:0] lcd_wdata,
	input             lcd_rdy
);

localparam [2:0] CARD_IDLE = 3'd0, CARD_READY = 3'd1, CARD_IDENT = 3'd2, CARD_STBY = 3'd3, CARD_TRAN = 3'd4;

localparam [31:0] OCR_RESP = 32'h80FF8000;
localparam [31:0] CID0 = 32'h0200044E, CID1 = 32'h474D4D43, CID2 = 32'h31100000, CID3 = 32'h00019601;
localparam [31:0] CSD0 = 32'h4C26002A, CSD1 = 32'h0F59803F, CSD2 = 32'hEDB7FFFF, CSD3 = 32'h8A400001;

function automatic [31:0] r1resp(input [2:0] st);
	r1resp = ({29'd0, st} << 9) | 32'h100;
endfunction

// card / command register state. card_present survives reset (cpu_reset):
// only img_mounted/img_size (an OSD mount) decides whether a card exists.
reg        card_present = 1'b0;
reg  [2:0] card_state;
reg [31:0] resp [0:3];
reg  [1:0] resp_idx;
reg        resp_ready;
reg        idx_written;
reg  [5:0] cmd_idx;
reg [31:0] cmd_arg;
reg [31:0] cmd_reg;
reg [31:0] cur_lba;
reg  [8:0] buf_off;
reg [31:0] r608, r60C, r610, r614, r61C; // plain echo, unused by the model itself

// DMA channel registers and sector buffer
reg  [6:0] ch_src_card, ch_dst_card; // last +14 / +20 write was the card port
reg  [6:0] ch_dst_lcd;               // last +20 write was the LCD data port
reg [31:0] ch_src [0:6], ch_dst [0:6], ch_cnt [0:6];
reg  [2:0] dma_ch;
reg        dma_busy;
// a start while busy is queued, not dropped: screen updates overlap file reads
reg  [6:0] dma_pend, dma_startq;
reg        buf_valid;
reg [31:0] buf_lba;     // the sector sec_buf holds while buf_valid
reg        sd_req_pend; // a read fetch is out, from its sd_rd pulse until its sd_ack falls
// card stream as {lba, byte offset}: each card go takes its start here and moves it by its length at once
reg [40:0] st_pos;
reg [40:0] ch_pos [0:6];
reg        dma_ab;      // +08 = 0 on the running card transfer, it stops at the next safe point
integer    j;
reg [7:0]  sec_buf [0:511];
always @(posedge clk) if (sd_buff_wr) sec_buf[sd_buff_addr] <= sd_buff_dout;

localparam [3:0] D_IDLE = 4'd0, D_FETCH = 4'd1, D_RDLO = 4'd2, D_RDHI = 4'd3, D_SEND = 4'd4, D_WAIT_ACK = 4'd5;
localparam [3:0] L_REQ = 4'd6, L_WAIT = 4'd7, L_PUT = 4'd8; // SDRAM -> LCD, one 32-bit word per two halfword reads
reg  [3:0] dstate;
reg        l_hi;
reg [31:0] hw_left;
reg [31:0] cur_pa;
// an LCD frame paused between two words while a queued card transfer runs
reg        lp_on;
reg  [2:0] lp_ch;
reg [31:0] lp_left, lp_pa;
reg  [7:0] byte_lo;
reg  [8:0] rdaddr;
reg  [7:0] buf_rdat;
reg  [1:0] dma_ack_sync;
wire       dma_ack_s = dma_ack_sync[1];
reg        sd_ack_d;
wire       sd_ack_rise = sd_ack & ~sd_ack_d;
wire       sd_ack_fall = ~sd_ack & sd_ack_d;
wire [9:0] next_buf_off = {1'b0, buf_off} + 10'd2;

// PA -> SDRAM word address, same windows as ngage_soc's RAM8M_WORD_OFF/RAM2M_WORD_OFF
function automatic [24:0] pa_word(input [31:0] pa);
	if (pa[31:24] == 8'h0A) pa_word = 25'h0A00000 + {2'd0, pa[23:1]};
	else if (pa[31:21] == 11'h060) pa_word = 25'h1200000 + {5'd0, pa[20:1]};
	else pa_word = 25'd0;
endfunction
function automatic pa_valid(input [31:0] pa);
	pa_valid = (pa[31:24] == 8'h0A) || (pa[31:21] == 11'h060);
endfunction
function automatic [2:0] low7(input [6:0] v);
	integer i;
	begin
		low7 = 3'd0;
		for (i = 6; i >= 0; i = i - 1) if (v[i]) low7 = i[2:0];
	end
endfunction

wire       dma_wr    = sel_dma && ena && !rnw;
wire       start_now = dma_wr && off == 6'h08 && wdata == 32'd2 && !dma_busy && dma_startq == 7'd0;
wire       start_q   = !dma_wr && !dma_busy && dma_startq != 7'd0;
wire [6:0] card_q    = dma_startq & (ch_src_card | ch_dst_card);
// a queued card go takes the engine from an LCD frame at a word boundary
wire       start_p   = !dma_wr && dstate == L_REQ && !l_hi && dma_req == dma_ack_s && !lcd_we && card_q != 7'd0;
wire [2:0] start_k   = start_p ? low7(card_q) : (dma_startq == 7'd0) ? dma_k : low7(dma_startq);
wire       start_rd  = ch_src_card[start_k] && card_present;
// an invalid source reads as zero without an SDRAM access
wire       l_step    = (dstate == L_REQ && !pa_valid(cur_pa)) || (dstate == L_WAIT && dma_ack_s == dma_req);
wire [15:0] l_data   = pa_valid(cur_pa) ? dma_dout : 16'd0;
wire [31:0] g_hw     = (ch_cnt[dma_k] + 32'd1) >> 1;
wire [40:0] s_pos    = start_now ? st_pos : ch_pos[start_k];
wire        c_run    = dma_k == dma_ch && dstate != D_IDLE && dstate != L_REQ && dstate != L_WAIT && dstate != L_PUT;
// a read stops with no DDR write in flight: in D_FETCH or at the ack
wire        rd_stop  = dma_ab && (dstate == D_FETCH || (dstate == D_WAIT_ACK && dma_ack_s == dma_req));

// the MGL mounts the card while the soc still waits in reset for the monitor
always @(posedge clk) if (img_mounted) card_present <= (img_size != 64'd0);

// RAM -> card: halfwords into wbuf, one sd_wr per sector, the IRQ only after the last sd_ack
localparam [3:0] W_REQ = 4'd9, W_WAIT = 4'd10, W_SEND = 4'd11, W_ACK = 4'd12;
reg  [7:0] woff;
reg        w_zero; // the count ended mid-sector: the rest of the sector is zero
reg [15:0] wbuf [0:255];
reg [15:0] wbuf_q;
reg        wbuf_hi;
// ngage_hyb_ddr still holds the 64-bit beat of cur_pa from this run's last LCD read
reg        l_bv;
wire [31:0] w_hw   = (ch_cnt[start_k] + 32'd1) >> 1;
wire        start_wr = ch_dst_card[start_k] && card_present && w_hw != 32'd0;
// no fill under a live sd_ack: a drain cut short by a soc reset still reads wbuf
wire        w_fill = dstate == W_REQ && !sd_ack && (w_zero || !pa_valid(cur_pa));
wire        w_get  = dstate == W_WAIT && dma_ack_s == dma_req;
wire        w_step = w_fill | w_get;
always @(posedge clk) if (w_step) wbuf[woff] <= w_get ? dma_dout : 16'd0;
always @(posedge clk) begin wbuf_q <= wbuf[sd_buff_addr[8:1]]; wbuf_hi <= sd_buff_addr[0]; end
assign sd_buff_din = wbuf_hi ? wbuf_q[15:8] : wbuf_q[7:0];
wire        w_end  = dstate == W_ACK && sd_ack_fall && hw_left == 32'd0;
// a cancelled write still sends the sector it is on
wire        w_stop = dma_ab && dstate == W_ACK && sd_ack_fall;
wire [40:0] go_len = ch_src_card[dma_k] ? {8'd0, g_hw, 1'b0} : ch_dst_card[dma_k] ? {(g_hw + 32'd255) >> 8, 9'd0} : 41'd0;
wire        p_take   = start_p && (start_rd || start_wr);
wire        ab_end   = rd_stop || w_stop;
// every end of a card transfer; the paused LCD frame resumes from here
wire        card_end = (dstate == D_WAIT_ACK && dma_ack_s == dma_req && hw_left == 32'd1) || w_end || ab_end;
always @(posedge clk) dma_ack_sync <= {dma_ack_sync[0], dma_ack};
always @(posedge clk) buf_rdat <= sec_buf[rdaddr];
always @(*) rdaddr = (dstate == D_RDHI) ? (buf_off + 9'd1) : buf_off;

always @(posedge clk) begin
	if (reset) begin
		card_state <= CARD_IDLE;
		resp_ready <= 1'b0; resp_idx <= 2'd0; idx_written <= 1'b0;
		cmd_idx <= 6'd0; cmd_arg <= 32'd0; cmd_reg <= 32'd0;
		r608 <= 32'd0; r60C <= 32'd0; r610 <= 32'd0; r614 <= 32'd0; r61C <= 32'd0;
		cur_lba <= 32'd0; buf_off <= 9'd0;
		ch_src_card <= 7'd0; ch_dst_card <= 7'd0; ch_dst_lcd <= 7'd0; dma_ch <= 3'd0;
		dma_busy <= 1'b0; dma_pend <= 7'd0; dma_startq <= 7'd0; buf_valid <= 1'b0; sd_req_pend <= 1'b0;
		lcd_we <= 1'b0; lcd_wdata <= 32'd0; l_hi <= 1'b0;
		dstate <= D_IDLE; hw_left <= 32'd0; cur_pa <= 32'd0; byte_lo <= 8'd0;
		lp_on <= 1'b0; lp_ch <= 3'd0; lp_left <= 32'd0; lp_pa <= 32'd0;
		buf_lba <= 32'd0; st_pos <= 41'd0; dma_ab <= 1'b0;
		for (j = 0; j < 7; j = j + 1) ch_pos[j] <= 41'd0;
		sd_lba <= 32'd0; sd_rd <= 1'b0; sd_wr <= 1'b0; sd_ack_d <= 1'b0;
		dma_addr <= 25'd0; dma_din <= 16'd0; dma_wrl <= 1'b0; dma_wrh <= 1'b0; dma_req <= 1'b0;
		rd_count <= 16'd0; rdata <= 32'd0;
		woff <= 8'd0; w_zero <= 1'b0; l_bv <= 1'b0;
	end else begin
		if (img_mounted) buf_valid <= 1'b0;

		sd_ack_d <= sd_ack;
		if (sd_ack_rise) sd_rd <= 1'b0;
		// a fetch lands under its own lba, also when its read was cancelled meanwhile
		if (sd_ack_fall && sd_req_pend) begin
			buf_valid <= 1'b1; buf_lba <= sd_lba; sd_req_pend <= 1'b0; rd_count <= rd_count + 16'd1;
		end

		// command register writes
		if (sel_mmc && ena && !rnw) begin
			case (off)
				6'h1C: begin cmd_idx <= wdata[5:0]; r61C <= wdata; idx_written <= 1'b1; end
				6'h20: cmd_arg <= wdata;
				6'h08: r608 <= wdata;
				6'h0C: r60C <= wdata;
				6'h10: r610 <= wdata;
				6'h14: r614 <= wdata;
				6'h04: begin
					cmd_reg <= wdata;
					if (wdata[0]) begin
						if (idx_written) begin
							resp_idx <= 2'd0;
							case (cmd_idx)
								6'd0: begin resp_ready <= 1'b0; card_state <= CARD_IDLE; end
								6'd1: if (card_present && (card_state == CARD_IDLE || card_state == CARD_READY)) begin
									resp[0] <= OCR_RESP; resp[1] <= OCR_RESP; resp[2] <= OCR_RESP; resp[3] <= OCR_RESP;
									resp_ready <= 1'b1; card_state <= CARD_READY;
								end else begin
									resp_ready <= 1'b0; if (!card_present) card_state <= CARD_IDLE;
								end
								6'd2: if (card_present && card_state == CARD_READY) begin
									resp[0] <= CID0; resp[1] <= CID1; resp[2] <= CID2; resp[3] <= CID3;
									resp_ready <= 1'b1; card_state <= CARD_IDENT;
								end else begin
									resp_ready <= 1'b0; if (!card_present) card_state <= CARD_IDLE;
								end
								6'd3: if (card_present && card_state == CARD_IDENT) begin
									resp[0] <= r1resp(card_state); resp[1] <= r1resp(card_state);
									resp[2] <= r1resp(card_state); resp[3] <= r1resp(card_state);
									resp_ready <= 1'b1; card_state <= CARD_STBY;
								end else begin
									resp_ready <= 1'b0; if (!card_present) card_state <= CARD_IDLE;
								end
								6'd7: if (card_present) begin
									resp[0] <= r1resp(card_state); resp[1] <= r1resp(card_state);
									resp[2] <= r1resp(card_state); resp[3] <= r1resp(card_state);
									resp_ready <= 1'b1;
									card_state <= (cmd_arg != 32'd0) ? CARD_TRAN : CARD_STBY;
								end else begin
									resp_ready <= 1'b0; card_state <= CARD_IDLE;
								end
								6'd9: if (card_present && (card_state == CARD_STBY || card_state == CARD_TRAN)) begin
									resp[0] <= CSD0; resp[1] <= CSD1; resp[2] <= CSD2; resp[3] <= CSD3;
									resp_ready <= 1'b1;
								end else begin
									resp_ready <= 1'b0; if (!card_present) card_state <= CARD_IDLE;
								end
								6'd10: if (card_present && (card_state == CARD_STBY || card_state == CARD_TRAN)) begin
									resp[0] <= CID0; resp[1] <= CID1; resp[2] <= CID2; resp[3] <= CID3;
									resp_ready <= 1'b1;
								end else begin
									resp_ready <= 1'b0; if (!card_present) card_state <= CARD_IDLE;
								end
								6'd12: if (card_present) begin
									resp[0] <= r1resp(card_state); resp[1] <= r1resp(card_state);
									resp[2] <= r1resp(card_state); resp[3] <= r1resp(card_state);
									resp_ready <= 1'b1; card_state <= CARD_TRAN;
								end else begin
									resp_ready <= 1'b0; card_state <= CARD_IDLE;
								end
								6'd13: if (card_present) begin
									resp[0] <= r1resp(card_state); resp[1] <= r1resp(card_state);
									resp[2] <= r1resp(card_state); resp[3] <= r1resp(card_state);
									resp_ready <= 1'b1;
								end else begin
									resp_ready <= 1'b0; card_state <= CARD_IDLE;
								end
								6'd15: begin resp_ready <= 1'b0; card_state <= CARD_IDLE; end
								6'd16: if (card_present) begin
									resp[0] <= r1resp(card_state); resp[1] <= r1resp(card_state);
									resp[2] <= r1resp(card_state); resp[3] <= r1resp(card_state);
									resp_ready <= 1'b1;
								end else begin
									resp_ready <= 1'b0; card_state <= CARD_IDLE;
								end
								6'd17, 6'd18, 6'd24, 6'd25: if (card_present) begin
									resp[0] <= r1resp(card_state); resp[1] <= r1resp(card_state);
									resp[2] <= r1resp(card_state); resp[3] <= r1resp(card_state);
									resp_ready <= 1'b1;
									st_pos <= {9'd0, cmd_arg[31:9], 9'd0}; buf_valid <= 1'b0;
								end else begin
									resp_ready <= 1'b0; card_state <= CARD_IDLE;
								end
								default: begin
									resp_ready <= 1'b0; if (!card_present) card_state <= CARD_IDLE;
								end
							endcase
						end
						idx_written <= 1'b0;
					end else if (wdata == 32'd0 || wdata == 32'd2) begin
						resp_ready <= 1'b0; resp_idx <= 2'd0; st_pos[8:0] <= 9'd0;
					end
				end
				default: ;
			endcase
		end

		// DMA register writes
		if (sel_dma && ena && !rnw) begin
			case (off)
				6'h14: begin ch_src[dma_k] <= wdata; ch_src_card[dma_k] <= (wdata == 32'h1012_0624); end
				6'h20: begin
					ch_dst[dma_k] <= wdata;
					ch_dst_card[dma_k] <= (wdata == 32'h1012_0624);
					ch_dst_lcd[dma_k]  <= (wdata == 32'h1012_1208);
				end
				6'h2C: ch_cnt[dma_k] <= wdata;
				6'h08: begin
					dma_pend[dma_k] <= 1'b0;
					if (wdata == 32'd2) begin ch_pos[dma_k] <= st_pos; st_pos <= st_pos + go_len; end
					if (wdata == 32'd2 && !start_now) dma_startq[dma_k] <= 1'b1;
					if (wdata == 32'd0) begin dma_startq[dma_k] <= 1'b0; if (c_run) dma_ab <= 1'b1; end
				end
				default: ;
			endcase
		end

		// start at once when idle and nothing queued, else the lowest queued
		// channel once the engine is idle and no DMA register write is in flight
		if (start_now || start_q || start_p) begin
			if (start_q || start_p) dma_startq[start_k] <= 1'b0;
			if (p_take) begin lp_on <= 1'b1; lp_ch <= dma_ch; lp_left <= hw_left; lp_pa <= cur_pa; end
			if (start_rd) begin
				dma_ch <= start_k; dma_busy <= 1'b1;
				hw_left <= (ch_cnt[start_k] + 32'd1) >> 1; // byte count, card port is 8 bit
				cur_pa <= ch_dst[start_k]; dstate <= D_FETCH; {cur_lba, buf_off} <= s_pos;
			end else if (start_wr) begin
				dma_ch <= start_k; dma_busy <= 1'b1;
				hw_left <= w_hw; // byte count, card port is 8 bit
				cur_pa <= ch_src[start_k]; woff <= 8'd0; w_zero <= 1'b0; dstate <= W_REQ; {cur_lba, buf_off} <= s_pos;
			end else if (ch_src_card[start_k] || ch_dst_card[start_k]) begin
				dma_pend[start_k] <= 1'b1; // no card or write: read only, drop data
			end else if (ch_dst_lcd[start_k]) begin
				dma_ch <= start_k; dma_busy <= 1'b1;
				hw_left <= ch_cnt[start_k] + 32'd1; // 32-bit words: the kernel's LCD shift is 2
				cur_pa <= ch_src[start_k]; l_hi <= 1'b0; dstate <= L_REQ;
			end
		end

		// reads
		if (ena && rnw) begin
			if (sel_mmc) begin
				case (off)
					6'h04: rdata <= cmd_reg & ~32'h1;
					6'h08: rdata <= r608;
					6'h0C: rdata <= r60C;
					6'h10: rdata <= r610;
					6'h14: rdata <= r614;
					6'h18: rdata <= resp_ready ? 32'h100 : 32'd0;
					6'h1C: rdata <= r61C;
					6'h20: begin rdata <= resp[resp_idx]; resp_idx <= resp_idx + 2'd1; end
					default: rdata <= 32'd0;
				endcase
			end
		end

		// DMA FSM for the channel in dma_ch: fetch a sector via sd_*, then
		// copy halfwords into SDRAM through the toggle-handshake port
		case (dstate)
			D_IDLE: ;
			D_FETCH: begin
				if (buf_valid && buf_lba == cur_lba) dstate <= D_RDLO;
				else if (!sd_req_pend) begin sd_lba <= cur_lba; sd_rd <= 1'b1; sd_req_pend <= 1'b1; buf_valid <= 1'b0; end
			end
			D_RDLO: dstate <= D_RDHI;
			D_RDHI: begin byte_lo <= buf_rdat; dstate <= D_SEND; end
			D_SEND: begin
				dma_din  <= {buf_rdat, byte_lo};
				dma_addr <= pa_word(cur_pa);
				dma_wrl  <= pa_valid(cur_pa);
				dma_wrh  <= pa_valid(cur_pa);
				dma_req  <= ~dma_req;
				dstate   <= D_WAIT_ACK;
			end
			D_WAIT_ACK: if (dma_ack_s == dma_req) begin
				dma_wrl <= 1'b0; dma_wrh <= 1'b0;
				cur_pa  <= cur_pa + 32'd2;
				if (next_buf_off == 10'd512) begin
					buf_off <= 9'd0; buf_valid <= 1'b0; cur_lba <= cur_lba + 1'b1;
				end else buf_off <= next_buf_off[8:0];
				if (hw_left == 32'd1) begin
					dma_busy <= 1'b0; dma_pend[dma_ch] <= 1'b1; dstate <= D_IDLE;
				end else begin
					hw_left <= hw_left - 32'd1;
					dstate  <= (next_buf_off == 10'd512) ? D_FETCH : D_RDLO;
				end
			end
			L_REQ: if (pa_valid(cur_pa) && !p_take) begin
				dma_addr <= pa_word(cur_pa);
				dma_din  <= {15'd0, l_bv}; // served from that beat, no DDR access
				dma_req  <= ~dma_req;
				dstate   <= L_WAIT;
			end
			L_WAIT: ;
			L_PUT: if (lcd_rdy) begin
				lcd_we <= 1'b0;
				if (hw_left == 32'd1) begin
					dma_busy <= 1'b0; dma_pend[dma_ch] <= 1'b1; dstate <= D_IDLE;
				end else begin
					hw_left <= hw_left - 32'd1;
					dstate  <= L_REQ;
				end
			end
			W_REQ: if (!sd_ack && !w_zero && pa_valid(cur_pa)) begin
				dma_addr <= pa_word(cur_pa);
				dma_din  <= 16'd0;
				dma_req  <= ~dma_req;
				dstate   <= W_WAIT;
			end
			W_WAIT: ;
			// sd_lba still names the fetch of a cancelled read until it lands
			W_SEND: if (!sd_req_pend) begin sd_lba <= cur_lba; sd_wr <= 1'b1; dstate <= W_ACK; end
			W_ACK: begin
				if (sd_ack_rise) sd_wr <= 1'b0;
				if (sd_ack_fall) begin
					cur_lba <= cur_lba + 1'b1;
					if (hw_left == 32'd0) begin dma_busy <= 1'b0; dma_pend[dma_ch] <= 1'b1; dstate <= D_IDLE; end
					else dstate <= W_REQ;
				end
			end
			default: dstate <= D_IDLE;
		endcase

		if (ab_end) begin dma_busy <= 1'b0; dma_pend[dma_ch] <= 1'b0; dstate <= D_IDLE; end
		if (card_end) dma_ab <= 1'b0;
		if (card_end && lp_on) begin
			lp_on <= 1'b0; dma_busy <= 1'b1;
			dma_ch <= lp_ch; hw_left <= lp_left; cur_pa <= lp_pa; l_hi <= 1'b0; dstate <= L_REQ;
		end

		// SDRAM halfword j is pixel j, so the first read is the low half
		if (l_step && !p_take) begin
			cur_pa <= cur_pa + 32'd2;
			l_hi   <= ~l_hi;
			if (l_hi) begin lcd_wdata[31:16] <= l_data; lcd_we <= 1'b1; dstate <= L_PUT; end
			else begin lcd_wdata[15:0] <= l_data; dstate <= L_REQ; end
		end
		// a card transfer or an idle engine may replace the beat in the port
		if (dstate != L_REQ && dstate != L_WAIT && dstate != L_PUT) l_bv <= 1'b0;
		else if (l_step) l_bv <= cur_pa[2:1] != 2'd3;

		if (w_step) begin
			woff <= woff + 8'd1;
			if (!w_zero) begin
				cur_pa  <= cur_pa + 32'd2;
				hw_left <= hw_left - 32'd1;
				if (hw_left == 32'd1) w_zero <= 1'b1;
			end
			dstate <= (woff == 8'hFF) ? W_SEND : W_REQ;
		end
	end
end

assign irq = dma_pend;

endmodule
