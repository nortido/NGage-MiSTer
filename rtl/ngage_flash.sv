// AMD-style x16 flash command FSM, one instance per 32MB chip window.
// off is the byte offset inside the chip (which chip is picked by the
// caller's chip select, not by these bits). array_mode tells ngage_soc
// whether to go fetch real array data from SDRAM or hand back id_rdata.
module ngage_flash #(
	parameter [15:0] FLASH_MFG_ID = 16'h0001,
	parameter [15:0] FLASH_DEV_ID = 16'h257E,
	parameter [7:0]  CFI_SIZE     = 8'h17
)
(
	input             clk,
	input             reset,
	input             sel,
	input             ena,
	input             rnw,
	input      [24:0] off,
	input      [31:0] wdata,

	output            array_mode,
	output     [31:0] id_rdata
);

localparam ST_READ = 2'd0, ST_UNLOCK1 = 2'd1, ST_UNLOCK2 = 2'd2, ST_ID = 2'd3;

reg [1:0] state = ST_READ;
reg       cfi = 1'b0;

wire [11:0] off12 = off[11:0];
wire  [8:0] widx  = off[10:2]; // enough for autoselect (0,7) and CFI (8..24)

always @(posedge clk) begin
	if (reset) begin
		state <= ST_READ;
		cfi   <= 1'b0;
	end else if (sel & ena & ~rnw) begin
		case (state)
			// 0x90 requires the unlock sequence first, no bare autoselect entry
			ST_READ:
				if (off12 == 12'hAAA && wdata[7:0] == 8'hAA) state <= ST_UNLOCK1;
			ST_UNLOCK1:
				if (off12 == 12'h554 && wdata[7:0] == 8'h55) state <= ST_UNLOCK2;
				else state <= ST_READ;
			ST_UNLOCK2:
				if (off12 == 12'hAAA && wdata[7:0] == 8'h90) begin state <= ST_ID; cfi <= 1'b0; end
				else if (off12 == 12'hAAA && wdata[7:0] == 8'h98) begin state <= ST_ID; cfi <= 1'b1; end
				else state <= ST_READ;
			ST_ID:
				// program/erase commands land here too: accepted, ignored, ready
				if (wdata[7:0] == 8'hF0 || wdata[7:0] == 8'hFF) state <= ST_READ;
		endcase
	end
end

assign array_mode = (state == ST_READ);

reg [31:0] id_r;
always @* begin
	id_r = 32'h00000000;
	if (state == ST_ID) begin
		if (!cfi) begin
			// autoselect: mfg/dev at byte offset 0/2, extended ID at byte
			// offset 0x1C/0x1E; anything else (sector protect, byte 6, etc)
			// reads 0
			if (widx == 9'd0) id_r = {FLASH_DEV_ID, FLASH_MFG_ID};
			else if (widx == 9'd7) id_r = {16'h2500, 16'h2502}; // 0x1E, 0x1C
		end else begin
			// S29GL064N CFI table (addresses in the comment are the standard
			// CFI byte addresses; widx = address/2, low half = even address)
			case (widx)
				9'd8:  id_r = {16'h0052, 16'h0051}; // 0x11='R', 0x10='Q'
				9'd9:  id_r = {16'h0002, 16'h0059}; // 0x13=cmd set lo, 0x12='Y'
				9'd10: id_r = {16'h0040, 16'h0000}; // 0x15=0x40
				9'd13: id_r = {16'h0027, 16'h0000}; // 0x1B=0x27
				9'd14: id_r = {16'h0000, 16'h0036}; // 0x1C=0x36, 0x1D=0x00
				9'd15: id_r = {16'h0007, 16'h0000}; // 0x1E=0x00, 0x1F=0x07
				9'd16: id_r = {16'h000A, 16'h0007}; // 0x20=0x07, 0x21=0x0A
				9'd17: id_r = {16'h0003, 16'h0000}; // 0x22=0x00, 0x23=0x03
				9'd18: id_r = {16'h0004, 16'h0005}; // 0x24=0x05, 0x25=0x04
				9'd19: id_r = {8'h00, CFI_SIZE, 16'h0000}; // 0x26=0x00, 0x27=device size
				9'd20: id_r = {16'h0000, 16'h0002}; // 0x28=interface desc lo
				9'd21: id_r = {16'h0000, 16'h0005}; // 0x2A=max buffer write size
				9'd22: id_r = {16'h007F, 16'h0001}; // 0x2C=1 region, 0x2D=127 (128 sectors)
				9'd24: id_r = {16'h0000, 16'h0001}; // 0x30=sector size lo (256B units)
				9'd32: id_r = {16'h0052, 16'h0050}; // 0x40='P', 0x41='R'
				9'd33: id_r = {16'h0031, 16'h0049}; // 0x42='I', 0x43='1'
				9'd34: id_r = {16'h0000, 16'h0033}; // 0x44='3', 0x45=0x00
				9'd35: id_r = {16'h0001, 16'h0002}; // 0x46=0x02, 0x47=0x01
				9'd36: id_r = {16'h0008, 16'h0000}; // 0x48=0x00, 0x49=0x08
				9'd38: id_r = {16'h00B5, 16'h0002}; // 0x4C=0x02, 0x4D=0xB5
				9'd39: id_r = {16'h0004, 16'h00C5}; // 0x4E=0xC5, 0x4F=0x04
				9'd40: id_r = {16'h0000, 16'h0001}; // 0x50=0x01
				default: id_r = 32'h00000000;
			endcase
		end
	end
end
assign id_rdata = id_r;

endmodule
