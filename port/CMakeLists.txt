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

# --- Lifted game code (generated PPU chunks) -------------------------------
file(GLOB RECOMP_CHUNKS CONFIGURE_DEPENDS "${RECOMP_DIR}/ppu_recomp_*.cpp")
list(LENGTH RECOMP_CHUNKS N_CHUNKS)
message(STATUS "des_boot: ${N_CHUNKS} recompiled chunks from ${RECOMP_DIR}")

# --- Lifted SPU images + workload registration -----------------------------
# Each DeS SPU image is lifted (spu_lifter.py --symbol-prefix) into
# ps3recomp/work_spu/<img>/spu_recomp.c; _gen/spu_workloads.cpp registers them
# with the workload-dispatch registry (by FNV-1a fingerprint) at startup so
# cellSpurs AddWorkload/CreateTask dispatch runs the recompiled SPU code.
set(SPU_DIR "${PS3RECOMP_DIR}/work_spu")
file(GLOB SPU_CHUNKS CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
list(LENGTH SPU_CHUNKS N_SPU)
message(STATUS "des_boot: ${N_SPU} lifted SPU images from ${SPU_DIR}")

# --- The boot executable ---------------------------------------------------
add_executable(des_boot
    ${RECOMP_CHUNKS}
    ${SPU_CHUNKS}                                           # lifted SPU images
    "${SPU_DIR}/_gen/spu_workloads.cpp"                     # SPU workload registration (auto at startup)
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

# Huge generated TUs need /bigobj (COFF >65k sections). Baseline is /Od (fast
# compile) + /Zi (symbols for host backtraces). NOTE: configure with an empty
# CMAKE_BUILD_TYPE so the Debug config's /RTC1 (runtime checks: big slowdown,
# and incompatible with /O2) is NOT added.
if(MSVC)   # also true for clang-cl
    target_compile_options(des_boot PRIVATE
        /bigobj /Od /Zi /EHsc /Zc:__cplusplus
        /wd4100 /wd4244 /wd4267 /wd4146 /wd4334
    )
    target_compile_definitions(des_boot PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Per-chunk optimization: the hot main-init data-structure loop lives in
    # chunk 005 (func_00AE1408 + func_00ADCB28). Build it /O2 so the per-16-byte
    # copy inlines — tests whether blocker #3 is just unoptimized-build slowness.
    # (Full /O2 over all 36 chunks is impractical; opt the hot one.)
    set(DES_O2_CHUNKS
        "${RECOMP_DIR}/ppu_recomp_005.cpp"
    )
    foreach(_c ${DES_O2_CHUNKS})
        set_source_files_properties("${_c}" PROPERTIES COMPILE_OPTIONS "/O2;/Zi")
    endforeach()
endif()

target_link_libraries(des_boot PRIVATE
    ps3recomp_runtime
    ws2_32 xinput ole32 bcrypt          # winsock / gamepad / COM(WASAPI) / RNG
    d3d12 dxgi d3dcompiler uuid dxguid   # RSX D3D12 backend (dxguid: IID_IDXGIFactory4/SwapChain3)
)

# Emit a linker map so we can resolve host RVAs back to func_<guestaddr> even for
# /O2 chunks that llvm-symbolizer can't (used to locate busy-loop spin addresses).
target_link_options(des_boot PRIVATE "/MAP:des_boot.map")
