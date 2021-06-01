fxc /T vs_4_0 /E main /O3 /Fh display_vs.hlsl.h /Vn "static s_display_vs_bytecode" display_vs.hlsl
fxc /T ps_4_0 /E main /O3 /Fh display_ps.hlsl.h /Vn "static s_display_ps_bytecode" display_ps.hlsl
fxc /T ps_4_0 /E main /O3 /D ALPHA=1 /Fh display_ps_alpha.hlsl.h /Vn "static s_display_ps_alpha_bytecode" display_ps.hlsl