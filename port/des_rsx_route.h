/* The port's calls to the renderer (docs/120). ps3recomp built with PS3RECOMP_RSX_HAL has a route switch, rsx_out
 * (libs/video/rsx_out.h), that runs today's D3D12 renderer or the portable renderer's new path, as RSX_BACKEND says.
 * In that build port/CMakeLists.txt defines PS3RECOMP_RSX_HAL for each file that includes this one, and this file
 * renames each of today's renderer functions, rsx_d3d12_<name>, to rsx_out_<name>. The calls and their declarations
 * stay as they are, and the overlay's type keeps its name (rsx_d3d12_overlay). Without the option this file is empty,
 * and the port calls today's renderer directly. Include it before those declarations. A new file that includes it
 * goes in port/CMakeLists.txt's list. */
#pragma once
#if defined(PS3RECOMP_RSX_HAL)
#include "rsx_out.h"   /* its declarations: a port declaration that differs from one does not compile */
#define rsx_d3d12_backend_init             rsx_out_backend_init
#define rsx_d3d12_backend_shutdown         rsx_out_backend_shutdown
#define rsx_d3d12_backend_pump_messages    rsx_out_backend_pump_messages
#define rsx_d3d12_backend_present          rsx_out_backend_present
#define rsx_d3d12_set_overlay              rsx_out_set_overlay
#define rsx_d3d12_overlay_repaint          rsx_out_overlay_repaint
#define rsx_d3d12_overlay_wake             rsx_out_overlay_wake
#define rsx_d3d12_overlay_tick             rsx_out_overlay_tick
#define rsx_d3d12_device                   rsx_out_device
#define rsx_d3d12_device_lost              rsx_out_device_lost
#define rsx_d3d12_queue                    rsx_out_queue
#define rsx_d3d12_hwnd                     rsx_out_hwnd
#define rsx_d3d12_post_message             rsx_out_post_message
#define rsx_d3d12_back_buffer              rsx_out_back_buffer
#define rsx_d3d12_back_buffer_count        rsx_out_back_buffer_count
#define rsx_d3d12_back_buffer_format       rsx_out_back_buffer_format
#define rsx_d3d12_hdr                      rsx_out_hdr
#define rsx_d3d12_set_borderless           rsx_out_set_borderless
#define rsx_d3d12_borderless               rsx_out_borderless
#define rsx_d3d12_display_timing           rsx_out_display_timing
#define rsx_d3d12_display_refresh_mhz      rsx_out_display_refresh_mhz
#define rsx_d3d12_display_mode_mhz         rsx_out_display_mode_mhz
#define rsx_d3d12_display_shown_mhz        rsx_out_display_shown_mhz
#define rsx_d3d12_shader_hlsl              rsx_out_shader_hlsl
#define rsx_d3d12_shader_hdr               rsx_out_shader_hdr
#define rsx_d3d12_shader_cache_put         rsx_out_shader_cache_put
#define rsx_d3d12_shader_cache_path        rsx_out_shader_cache_path
#define rsx_d3d12_shader_fp_variants       rsx_out_shader_fp_variants
#define rsx_d3d12_pso_index_game           rsx_out_pso_index_game
#define rsx_d3d12_program_hash64           rsx_out_program_hash64
#define rsx_d3d12_shader_key               rsx_out_shader_key
#define rsx_d3d12_pso_index_add            rsx_out_pso_index_add
#define rsx_d3d12_pso_index_ready          rsx_out_pso_index_ready
#define rsx_d3d12_composite_from_offscreen rsx_out_composite_from_offscreen
#endif
