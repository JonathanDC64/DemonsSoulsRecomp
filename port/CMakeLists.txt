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

# --- Real SPURS taskset policy (A/B oracle, LBP_REAL_POLICY=1) --------------
# ps3recomp/runtime/spu/spurs_policy.c can run the AUTHENTIC Sony taskset policy
# module and diff the SpursTasksetContext it builds against our C reimplementation
# (spurs_pm_build_context). It needs the module's raw bytes plus the lifted policy;
# without them a zero-size weak stub links and the probe is inert.
#
# The blob is FIRMWARE-DERIVED, so it is generated into port/gen/ (gitignored) from
# the user's own fw_spu/libsre.prx at configure time, never committed. If libsre.prx
# is absent the generator no-ops and the weak stub still links.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_EXECUTABLE)
    set(Python3_EXECUTABLE "python")
endif()
set(TSP_LIFTED "${PS3RECOMP_DIR}/fw_spu/spu_libsre/taskset_policy_lifted/spu_recomp.c")
set(TSP_BLOB   "${CMAKE_CURRENT_SOURCE_DIR}/gen/taskset_policy_blob.c")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/gen_taskset_policy_blob.py"
    RESULT_VARIABLE TSP_GEN_RC OUTPUT_VARIABLE TSP_GEN_OUT ERROR_VARIABLE TSP_GEN_OUT)
message(STATUS "des_boot: taskset-policy blob: ${TSP_GEN_OUT}")
set(TSP_SOURCES "")
if(EXISTS "${TSP_BLOB}" AND EXISTS "${TSP_LIFTED}")
    set(TSP_SOURCES "${TSP_BLOB}" "${TSP_LIFTED}")
    message(STATUS "des_boot: real taskset policy WIRED (LBP_REAL_POLICY=1 to run the A/B oracle)")
else()
    message(STATUS "des_boot: real taskset policy NOT wired (weak stub; probe inert)")
endif()

# --- Real lifted Sony SPURS kernel (libsre firmware LLE) --------------------
# ps3recomp's dispatcher ALREADY prefers a loaded PRX over its own HLE stub
# (ps3_hle_call -> prx_resolve_export); supplying the module is what makes that
# bridge live. FIRMWARE-DERIVED and gitignored: work/libsre/ is produced locally
# by scripts/build_libsre.sh from the user's own libsre.prx, port/gen/ from it.
# Entirely GATED at runtime by LIBSRE_LLE=1 -- unset, nothing is registered and
# every SPURS call takes the existing HLE path, so the default build is
# behaviourally identical and reverting costs nothing.
set(LIBSRE_LIFTED "${CMAKE_CURRENT_SOURCE_DIR}/../work/libsre/lifted/ppu_recomp.c")
set(LIBSRE_GLUE   "${CMAKE_CURRENT_SOURCE_DIR}/gen/libsre_glue.c")
set(LIBSRE_SOURCES "")
if(EXISTS "${LIBSRE_LIFTED}" AND EXISTS "${LIBSRE_GLUE}")
    set(LIBSRE_SOURCES "${LIBSRE_LIFTED}" "${LIBSRE_GLUE}"
        "${PS3RECOMP_DIR}/runtime/prx/prx_loader.c")
    # The lifter emits C++ (extern "C" blocks) regardless of the .c suffix, and
    # the game's own chunks are .cpp for the same reason -- compile it as C++.
    set_source_files_properties("${LIBSRE_LIFTED}" PROPERTIES LANGUAGE CXX)
    message(STATUS "des_boot: libsre LLE available (run with LIBSRE_LLE=1)")
else()
    message(STATUS "des_boot: libsre LLE not built (HLE cellSpurs only)")
endif()

# --- The boot executable ---------------------------------------------------
add_executable(des_boot
    ${RECOMP_CHUNKS}
    ${SPU_CHUNKS}                                           # lifted SPU images
    ${LIBSRE_SOURCES}                                       # real lifted SPURS kernel (opt-in)
    ${TSP_SOURCES}                                          # real SPURS taskset policy (blob + lifted, optional)
    "${SPU_DIR}/_gen/spu_workloads.cpp"                     # SPU workload registration (auto at startup)
    "${RECOMP_DIR}/hle_nid_table.cpp"                       # generated NID->handler table
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"           # image load, vm, registry, dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"              # NID -> HLE bridge
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"               # cellFs VFS over game dir
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"           # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"      # entry/boot harness
    "${CMAKE_CURRENT_SOURCE_DIR}/missing_stubs.cpp"         # degenerate external targets
    "${CMAKE_CURRENT_SOURCE_DIR}/des_heap.cpp"              # clean guest allocator (malloc override)
    "${CMAKE_CURRENT_SOURCE_DIR}/des_options.cpp"           # user options (DES_SKIP_INTRO, ...)
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
    # NOTE: lifted SPU images host guest loops that branch to another lifted
    # function's entry. The lifter emits those as forced tail calls
    # (SPU_TAILCALL = clang __attribute__((musttail))), which iterate in O(1) host
    # stack at ANY optimization level -- so the SPU TUs do NOT need /O2 for
    # correctness (without the forced tail call, /Od nesting overflowed the stack
    # and silently killed the process). They stay at the /Od baseline for fast
    # builds; opt them later only if SPU compute throughput matters.
endif()

target_link_libraries(des_boot PRIVATE
    ps3recomp_runtime
    ws2_32 xinput ole32 bcrypt          # winsock / gamepad / COM(WASAPI) / RNG
    d3d12 dxgi d3dcompiler uuid dxguid   # RSX D3D12 backend (dxguid: IID_IDXGIFactory4/SwapChain3)
)

# Emit a linker map so we can resolve host RVAs back to func_<guestaddr> even for
# /O2 chunks that llvm-symbolizer can't (used to locate busy-loop spin addresses).
target_link_options(des_boot PRIVATE "/MAP:des_boot.map")
