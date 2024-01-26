set $run_bomb_continue = 1
tui enable
layout asm
layout regs
b explode_bomb
b phase7
b secret_phase
b *0x401b10