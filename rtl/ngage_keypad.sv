module ngage_keypad
(
	input             clk,
	input             reset,
	input      [25:0] keys,

	input             we,
	input             sel_g2,
	input             sel_kpc,
	input       [9:0] word_idx,
	input       [3:0] lanes,
	input      [31:0] wlane,

	output     [31:0] ovr_mask,
	output     [31:0] ovr_val,
	output            irq,
	output     [31:0] dbg_keys
);

// drive state, one bit per logical line 0..15 (2,3,10,11 unused); idle
// reset has rows 0,1,4,5,6,7 driven
reg [15:0] driven = 16'h00F3;

reg [25:0] keys_d = 26'd0;
always @(posedge clk) keys_d <= keys;

wire matrix_edge = |(keys[19:0] & ~keys_d[19:0]);

// row/col active: driven directly, or a pressed key's other line is driven
wire row0_a  = driven[0]  | (keys[0]  & driven[8])  | (keys[2]  & driven[9]);
wire row1_a  = driven[1]  | (keys[1]  & driven[8])  | (keys[3]  & driven[9]);
wire row4_a  = driven[4]  | (keys[4]  & driven[12]) | (keys[8]  & driven[13]) | (keys[12] & driven[14]) | (keys[16] & driven[15]);
wire row5_a  = driven[5]  | (keys[5]  & driven[12]) | (keys[9]  & driven[13]) | (keys[13] & driven[14]) | (keys[17] & driven[15]);
wire row6_a  = driven[6]  | (keys[6]  & driven[12]) | (keys[10] & driven[13]) | (keys[14] & driven[14]) | (keys[18] & driven[15]);
wire row7_a  = driven[7]  | (keys[7]  & driven[12]) | (keys[11] & driven[13]) | (keys[15] & driven[14]) | (keys[19] & driven[15]);
wire col8_a  = driven[8]  | (keys[0]  & driven[0])  | (keys[1]  & driven[1]);
wire col9_a  = driven[9]  | (keys[2]  & driven[0])  | (keys[3]  & driven[1]);
wire col12_a = driven[12] | (keys[4]  & driven[4])  | (keys[5]  & driven[5])  | (keys[6]  & driven[6])  | (keys[7]  & driven[7]);
wire col13_a = driven[13] | (keys[8]  & driven[4])  | (keys[9]  & driven[5])  | (keys[10] & driven[6])  | (keys[11] & driven[7]);
wire col14_a = driven[14] | (keys[12] & driven[4])  | (keys[13] & driven[5])  | (keys[14] & driven[6])  | (keys[15] & driven[7]);
wire col15_a = driven[15] | (keys[16] & driven[4])  | (keys[17] & driven[5])  | (keys[18] & driven[6])  | (keys[19] & driven[7]);

reg row0_q, row1_q, row4_q, row5_q, row6_q, row7_q;
reg col8_q, col9_q, col12_q, col13_q, col14_q, col15_q;
reg l16_q, l17_q, l18_q, l19_q, l20_q, l21_q;
always @(posedge clk) begin
	row0_q <= row0_a; row1_q <= row1_a; row4_q <= row4_a; row5_q <= row5_a; row6_q <= row6_a; row7_q <= row7_a;
	col8_q <= col8_a; col9_q <= col9_a; col12_q <= col12_a; col13_q <= col13_a; col14_q <= col14_a; col15_q <= col15_a;
	l16_q <= keys[20]; l17_q <= keys[21]; l18_q <= keys[22]; l19_q <= keys[23]; l20_q <= keys[24]; l21_q <= keys[25];
end

// direct-line status encoding: edge on 16..21 sets its st102 bit
wire [7:0] st102_set =
	({8{keys[20] & ~keys_d[20]}} & 8'h08) |
	({8{keys[21] & ~keys_d[21]}} & 8'h20) |
	({8{keys[22] & ~keys_d[22]}} & 8'h80) |
	({8{keys[23] & ~keys_d[23]}} & 8'h02) |
	({8{keys[24] & ~keys_d[24]}} & 8'h04) |
	({8{keys[25] & ~keys_d[25]}} & 8'h40);

wire drvA_we  = we & sel_g2  & (word_idx == 10'h100);
wire drvB_we  = we & sel_g2  & (word_idx == 10'h103);
wire drvL7_we = we & sel_kpc & (word_idx[5:0] == 6'd5) & (&lanes);
wire stat_we  = we & sel_g2  & (word_idx == 10'h040);

reg st100;
reg [7:0] st102;

always @(posedge clk) begin
	if (reset) begin
		driven <= 16'h00F3;
		st100  <= 1'b0;
		st102  <= 8'd0;
	end else begin
		if (drvA_we && lanes[2]) begin
			if (wlane[16]) driven[0] <= 1'b1;
			if (wlane[17]) driven[1] <= 1'b1;
			if (wlane[18]) driven[4] <= 1'b1;
			if (wlane[19]) driven[5] <= 1'b1;
			if (wlane[20]) driven[6] <= 1'b1;
		end
		if (drvA_we && lanes[3]) begin
			if (wlane[24]) driven[0] <= 1'b0;
			if (wlane[25]) driven[1] <= 1'b0;
			if (wlane[26]) driven[4] <= 1'b0;
			if (wlane[27]) driven[5] <= 1'b0;
			if (wlane[28]) driven[6] <= 1'b0;
		end
		if (drvB_we && lanes[2]) begin
			if (wlane[16]) driven[8]  <= 1'b1;
			if (wlane[17]) driven[9]  <= 1'b1;
			if (wlane[18]) driven[12] <= 1'b1;
			if (wlane[19]) driven[13] <= 1'b1;
			if (wlane[20]) driven[14] <= 1'b1;
			if (wlane[21]) driven[15] <= 1'b1;
		end
		if (drvB_we && lanes[3]) begin
			if (wlane[24]) driven[8]  <= 1'b0;
			if (wlane[25]) driven[9]  <= 1'b0;
			if (wlane[26]) driven[12] <= 1'b0;
			if (wlane[27]) driven[13] <= 1'b0;
			if (wlane[28]) driven[14] <= 1'b0;
			if (wlane[29]) driven[15] <= 1'b0;
		end
		if (drvL7_we) driven[7] <= wlane[19] ? 1'b0 : 1'b1;

		// set beats clear: a press edge in the same cycle as a W1C wins
		if (matrix_edge) st100 <= 1'b1;
		else if (stat_we && lanes[0] && wlane[2]) st100 <= 1'b0;

		st102 <= (st102 & ~((stat_we && lanes[2]) ? wlane[23:16] : 8'd0)) | st102_set;
	end
end

wire [31:0] status_val = {8'd0, st102, 8'd0, 5'd0, st100, 2'd0};

// build overrides explicitly per word, no generic array so the mask/val
// stay a plain mux like the rest of ngage_soc's read paths
reg [31:0] ovr_mask_r, ovr_val_r;
always @* begin
	ovr_mask_r = 32'd0;
	ovr_val_r  = 32'd0;
	if (sel_g2) begin
		case (word_idx)
			10'h040: begin ovr_mask_r = 32'h00EE000C; ovr_val_r = status_val; end
			10'h100: begin ovr_mask_r = 32'h0000001F; ovr_val_r = {27'd0, ~row6_q, ~row5_q, ~row4_q, ~row1_q, ~row0_q}; end
			10'h103: begin ovr_mask_r = 32'h0000003F; ovr_val_r = {26'd0, ~col15_q, ~col14_q, ~col13_q, ~col12_q, ~col9_q, ~col8_q}; end
			10'h082: begin
				ovr_mask_r = 32'h2C060000;
				ovr_val_r  = 32'd0;
				ovr_val_r[29] = ~l16_q; ovr_val_r[17] = ~l17_q; ovr_val_r[18] = ~l18_q; ovr_val_r[26] = ~l19_q; ovr_val_r[27] = ~l20_q;
			end
			10'h083: begin ovr_mask_r = 32'h00001000; ovr_val_r = {19'd0, ~l21_q, 12'd0}; end
			default: ;
		endcase
	end else if (sel_kpc) begin
		if (word_idx[5:0] == 6'd1) begin ovr_mask_r = 32'h00080000; ovr_val_r = {12'd0, ~row7_q, 19'd0}; end
	end
end
assign ovr_mask = ovr_mask_r;
assign ovr_val  = ovr_val_r;

assign irq = st100 | (|(st102 & 8'hEE));

reg [5:0] evt_cnt = 6'd0;
wire direct_edge = |(keys[25:20] & ~keys_d[25:20]);
always @(posedge clk) begin
	if (reset) evt_cnt <= 6'd0;
	else if (matrix_edge || direct_edge) evt_cnt <= evt_cnt + 6'd1;
end
assign dbg_keys = {evt_cnt, keys_d};

endmodule
