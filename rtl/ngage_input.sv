// MiSTer joystick_0 + ps2_key -> ngage_keypad's keys[25:0]. Same clock as
// hps_io, so no CDC chain; keys is the one register stage that ORs the
// pad with the sticky keyboard press/release state.
module ngage_input
(
	input             clk,
	input             reset,
	input      [31:0] joy,
	input      [10:0] ps2_key,
	output reg [25:0] keys
);

reg [25:0] joy_map;
always @* begin
	joy_map      = 26'd0;
	joy_map[22]  = joy[0];              // right
	joy_map[20]  = joy[1];              // left
	joy_map[23]  = joy[2];              // down
	joy_map[21]  = joy[3];              // up
	joy_map[24]  = joy[4];              // OK
	joy_map[0]   = joy[5];              // LSoft
	joy_map[4]   = joy[6];              // RSoft
	joy_map[2]   = joy[7];              // C
	joy_map[15]  = joy[8];              // Green
	joy_map[19]  = joy[9];              // Red
	joy_map[8]   = joy[10];             // 1
	joy_map[9]   = joy[11];             // 2
	joy_map[10]  = joy[12];             // 3
	joy_map[12]  = joy[13];             // 4
	joy_map[13]  = joy[14];             // 5
	joy_map[14]  = joy[15];             // 6
	joy_map[16]  = joy[16];             // 7
	joy_map[17]  = joy[17];             // 8
	joy_map[18]  = joy[18];             // 9
	joy_map[5]   = joy[19];             // star
	joy_map[11]  = joy[20];             // 0
	joy_map[6]   = joy[21];             // hash
end

// PS/2 set-2 make/break, compact layout without a numpad. The E0 arrow
// codes for 2/4/6/8 collide with the unextended numpad digits, so the
// extended bit (ps2_key[8]) must gate the case, not just the low byte.
reg [4:0] kbd_idx;
reg       kbd_hit;
always @* begin
	kbd_hit = 1'b1;
	case ({ps2_key[8], ps2_key[7:0]})
		9'h045: kbd_idx = 5'd11; // 0
		9'h016: kbd_idx = 5'd8;  // 1
		9'h01E: kbd_idx = 5'd9;  // 2
		9'h026: kbd_idx = 5'd10; // 3
		9'h025: kbd_idx = 5'd12; // 4
		9'h02E: kbd_idx = 5'd13; // 5
		9'h036: kbd_idx = 5'd14; // 6
		9'h03D: kbd_idx = 5'd16; // 7
		9'h03E: kbd_idx = 5'd17; // 8
		9'h046: kbd_idx = 5'd18; // 9
		9'h04E: kbd_idx = 5'd5;  // '-' -> star
		9'h055: kbd_idx = 5'd6;  // '=' -> hash
		9'h05A: kbd_idx = 5'd24; // Enter -> OK
		9'h15A: kbd_idx = 5'd24; // numpad Enter -> OK
		9'h005: kbd_idx = 5'd0;  // F1 -> left softkey
		9'h006: kbd_idx = 5'd4;  // F2 -> right softkey
		9'h066: kbd_idx = 5'd2;  // Backspace -> C
		9'h004: kbd_idx = 5'd15; // F3 -> green
		9'h00C: kbd_idx = 5'd19; // F4 -> red
		9'h175: kbd_idx = 5'd21; // E0 75 -> up
		9'h172: kbd_idx = 5'd23; // E0 72 -> down
		9'h16B: kbd_idx = 5'd20; // E0 6B -> left
		9'h174: kbd_idx = 5'd22; // E0 74 -> right
		9'h070: kbd_idx = 5'd11; // KP0
		9'h069: kbd_idx = 5'd8;  // KP1
		9'h072: kbd_idx = 5'd9;  // KP2
		9'h07A: kbd_idx = 5'd10; // KP3
		9'h06B: kbd_idx = 5'd12; // KP4
		9'h073: kbd_idx = 5'd13; // KP5
		9'h074: kbd_idx = 5'd14; // KP6
		9'h06C: kbd_idx = 5'd16; // KP7
		9'h075: kbd_idx = 5'd17; // KP8
		9'h07D: kbd_idx = 5'd18; // KP9
		default: begin kbd_idx = 5'd0; kbd_hit = 1'b0; end
	endcase
end

reg        ps2_toggle_d = 1'b0;
reg [25:0] kbd_keys = 26'd0;
wire       ps2_new = ps2_key[10] ^ ps2_toggle_d;

always @(posedge clk) begin
	ps2_toggle_d <= ps2_key[10];
	if (reset) kbd_keys <= 26'd0; // a held key must not stick across an OSD open or ROM load
	else if (ps2_new && kbd_hit) kbd_keys[kbd_idx] <= ps2_key[9];
end

always @(posedge clk) keys <= joy_map | kbd_keys;

endmodule
