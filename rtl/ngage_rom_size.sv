// SPDX-License-Identifier: GPL-3.0-or-later
module ngage_rom_size
(
	input             clk,
	input             ioctl_download,
	input             ioctl_wr,
	input      [24:0] ioctl_addr,
	output     [24:0] rom_size
);

// Main's DDR path (F entry with an address) writes the size into ioctl_addr and sends no bytes, so
// until the first ioctl_wr of a download the size is ioctl_addr itself; the end strobe adds 1 to
// ioctl_addr in the cycle that drops ioctl_download, so that value is never sampled
reg [24:0] size_q = 25'd0;
reg        had_wr = 1'b0;
always @(posedge clk) begin
	if (!ioctl_download) had_wr <= 1'b0;
	else if (ioctl_wr) had_wr <= 1'b1;

	if (ioctl_wr) size_q <= ioctl_addr + 25'd1;
	else if (ioctl_download && !had_wr) size_q <= ioctl_addr;
end
assign rom_size = size_q;

endmodule
