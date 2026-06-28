cmake_minimum_required(VERSION 3.20)
project(des_boot LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Locations -------------------------------------------------------------
# The ps3recomp checkout and the (gitignored) generated/derived tree.
set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" CACHE PATH "ps3recomp checkout")
set(RECOMP_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/../work/recomp" CACHE PATH "lifted output dir")

# --- Prebuilt runtime library ----------------------------------------------
add_library(ps3recomp_runtime STATIC IMPORTED GLOBAL)
set_target_properties(ps3recomp_runtime PROPERTIES
    IMPORTED_LOCATION "${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib")

# --- Lifted game code (36 generated chunks) --------------------------------
file(GLOB RECOMP_CHUNKS CONFIGURE_DEPENDS "${RECOMP_DIR}/ppu_recomp_*.cpp")
list(LENGTH RECOMP_CHUNKS N_CHUNKS)
message(STATUS "des_boot: ${N_CHUNKS} recompiled chunks from ${RECOMP_DIR}")

# --- The boot executable ---------------------------------------------------
add_executable(des_boot
    ${RECOMP_CHUNKS}
    "${RECOMP_DIR}/hle_nid_table.cpp"                       # generated NID->handler table
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"           # image load, vm, registry, dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"              # NID -> HLE bridge
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"               # cellFs VFS over game dir
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"           # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"      # entry/boot harness
    "${CMAKE_CURRENT_SOURCE_DIR}/missing_stubs.cpp"         # degenerate external targets
    "${CMAKE_CURRENT_SOURCE_DIR}/des_heap.cpp"              # clean guest allocator (malloc override)
)

target_include_directories(des_boot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"   # Win32 <dirent.h> shim for ppu_fs.cpp
    "${RECOMP_DIR}"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
)

# Huge generated TUs need /bigobj (COFF >65k sections). First-boot build uses
# /Od for fast compile + debuggability; switch to /O2 once it runs.
if(MSVC)   # also true for clang-cl
    target_compile_options(des_boot PRIVATE
        /bigobj /Od /EHsc /Zc:__cplusplus
        /wd4100 /wd4244 /wd4267 /wd4146 /wd4334
    )
    target_compile_definitions(des_boot PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_link_libraries(des_boot PRIVATE
    ps3recomp_runtime
    ws2_32 xinput ole32 bcrypt          # winsock / gamepad / COM(WASAPI) / RNG
    d3d12 dxgi d3dcompiler uuid dxguid   # RSX D3D12 backend (dxguid: IID_IDXGIFactory4/SwapChain3)
)
