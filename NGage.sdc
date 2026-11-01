derive_pll_clocks
derive_clock_uncertainty

# core specific constraints
# soc samples dout0 one clk_cpu after its 1-flop ack0 sync; dout0 settles 1 clk_sys after ack0, so >= 20 ns
set_multicycle_path -start -setup 2 -from [get_registers {emu:emu|sdram:sdram|dout[*] emu:emu|sdram:sdram|dout_lo[*]}] -to [get_registers {emu:emu|ngage_hyb_top:hyb|ngage_soc:soc|mem_din[*]}]
# capture is READY+3 or +4 clk_sys, the next dout write READY+7 at the earliest (next slot), so >= 3 clk_sys of hold
set_multicycle_path -start -hold 2 -from [get_registers {emu:emu|sdram:sdram|dout[*] emu:emu|sdram:sdram|dout_lo[*]}] -to [get_registers {emu:emu|ngage_hyb_top:hyb|ngage_soc:soc|mem_din[*]}]
