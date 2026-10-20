cmake_minimum_required(VERSION 3.20)
project(des_boot LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Locations -------------------------------------------------------------
# The ps3recomp checkout and the (gitignored) generated/derived tree.
set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" CACHE PATH "ps3recomp checkout")
set(RECOMP_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/../work/recomp" CACHE PATH "lifted output dir")
# The ps3recomp commit this revision needs (ps3recomp.pin, docs/54): warns when the checkout lacks it;
# -DDES_REQUIRE_PIN=ON (releases) requires exactly that commit.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Ps3recompPin.cmake")

# --- Prebuilt runtime library ----------------------------------------------
# Where ps3recomp_runtime.lib is built. A second runtime build (tools/pgo/pgo.sh's) links from its own folder.
set(PS3RECOMP_BUILD_DIR "${PS3RECOMP_DIR}/build" CACHE PATH "ps3recomp runtime build dir")
add_library(ps3recomp_runtime STATIC IMPORTED GLOBAL)
set_target_properties(ps3recomp_runtime PROPERTIES
    IMPORTED_LOCATION "${PS3RECOMP_BUILD_DIR}/ps3recomp_runtime.lib")

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
    "${CMAKE_CURRENT_SOURCE_DIR}/des_aspect.cpp"            # DES_ASPECT / DES_HUD: wider pictures, the HUD kept 16:9
    "${CMAKE_CURRENT_SOURCE_DIR}/des_menu_sfx.cpp"          # UI sounds via the game's menu SE (0x724E40 hook)
    "${CMAKE_CURRENT_SOURCE_DIR}/des_chrins.cpp"            # ChrIns worker: done flag before the done event
    "${CMAKE_CURRENT_SOURCE_DIR}/des_prework.cpp"           # startup work: the shader cache, behind a progress bar
)

target_include_directories(des_boot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"   # Win32 <dirent.h> shim for ppu_fs.cpp
    "${RECOMP_DIR}"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/libs/video"          # rsx_shader_precompile.h
)

# A lift made with --shadow-stack keeps a guest call stack in every lifted
# function for ds_dump_shadow() backtraces: a thread-local push and pop and an
# unwind cleanup per call, ~2.4% of the frame rate in Boletarian Palace. Off
# by default; DES_SHADOW_STACK=ON keeps it for debugging. (A lift made without
# --shadow-stack has no stack either way.)
option(DES_SHADOW_STACK "Keep the guest shadow call stack of a --shadow-stack lift" OFF)
if(NOT DES_SHADOW_STACK)
    target_compile_definitions(des_boot PRIVATE DS_NO_SHADOW_STACK)
endif()

# Developer builds (docs/55): the dev cheats and the settings window's Developer tab.
# Never for a release. Build them in a folder of their own, such as port/build_dev,
# not in port/build: the option is cached, so a folder configured once with
# -DDES_DEV=ON makes a developer build at every later `cmake --build`, and
# tools/des/build.sh and play.sh use port/build. Nothing checks a release for it
# yet (there is no release workflow); run tools/ci/check_release_no_dev.py by hand
# on anything before sharing it. DES_DEV is always defined, as 0 or 1, but only
# for the port's sources that read it, so switching it recompiles those few files
# and not the lifted chunks. The dev hook lines are in insn_hooks.txt either way
# (one lift for both builds).
option(DES_DEV "Developer build: dev cheats and the Developer settings tab (never in a release)" OFF)
if(DES_DEV)
    target_sources(des_boot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/des_dev.cpp")
    set(DES_DEV_VALUE 1)
    message(WARNING "DES_DEV=ON: ${CMAKE_BINARY_DIR} makes DEVELOPER builds, with the cheats compiled "
        "in, at every build until it is configured with -DDES_DEV=OFF (the option is cached). Never share "
        "one; see docs/55.")
    file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" _des_bin)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/build" _des_std)
    string(TOLOWER "${_des_bin}" _des_bin)
    string(TOLOWER "${_des_std}" _des_std)
    if(_des_bin STREQUAL _des_std)
        message(WARNING "DES_DEV=ON in port/build, the folder tools/des/build.sh and play.sh use: every "
            "des_boot.exe they build and run will have the cheats. Configure port/build with -DDES_DEV=OFF "
            "and make developer builds in port/build_dev.")
    endif()
    # Each link says so too: a later `cmake --build` does not configure again, so it never shows the warning.
    add_custom_command(TARGET des_boot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E echo "des_boot: DEVELOPER BUILD (DES_DEV=ON), the cheats are compiled in. Never share it."
        VERBATIM)
else()
    set(DES_DEV_VALUE 0)
endif()
set_property(SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/des_options.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/des_menu_sfx.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/des_dev.cpp"
    APPEND PROPERTY COMPILE_DEFINITIONS DES_DEV=${DES_DEV_VALUE})
message(STATUS "DES_DEV (developer build, dev cheats): ${DES_DEV}")

# Huge generated TUs need /bigobj (COFF >65k sections). NOTE: configure with an
# empty CMAKE_BUILD_TYPE so the Debug config's /RTC1 (runtime checks: big
# slowdown, and incompatible with /O2) is NOT added.
#
# No debug information by default. The link has no /DEBUG, so there is no PDB:
# what /Zi put in the objects was dropped at the link, and host backtraces go
# through des_boot.map. /Zi cost build time and memory: chunk 009 took 228 s
# of CPU and 6.9 GB at its peak with it, 161 s and 2.4 GB without. The code is
# nearly the same without it: /Zi nudges clang's code generation in a few
# places (3 of chunk 004's 8,162 functions differ by a byte). DES_DEBUG_INFO=ON
# compiles with /Zi and links des_boot.pdb, with /OPT:REF,ICF kept on (/DEBUG
# turns them off) so that it links like the default build.
option(DES_DEBUG_INFO "Compile des_boot (and the UI) with /Zi and link a PDB" OFF)
set(DES_DEBUG_FLAGS "")
if(DES_DEBUG_INFO)
    set(DES_DEBUG_FLAGS /Zi)
    target_link_options(des_boot PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
#
# Everything here builds /O2: the lifted PPU chunks, the lifted libsre, and the
# runtime sources compiled into this target (ppu_loader.cpp holds the guest
# memory accessors every lifted load and store calls). It was /Od apart from
# chunk 005 (the boot's hot init loop), and in Boletarian Palace the game's
# own threads were then the limit: main and ChrInsManThread hand each frame
# back and forth, ~70 ms a frame between them, and HighGraphics spent most of
# its running time in the (uninlined) accessor checks. One chunk at /O2 takes
# ~2 minutes and ~3 GB with clang-cl. -fwrapv: signed overflow in lifted
# arithmetic (mulld, add on int64_t casts) wraps as the hardware does instead
# of being undefined to the optimizer. SSE4.1, as for the SPU images below:
# the lifted VMX code keeps vectors in guest byte order and swaps every lane on
# each load and store, ~8 SSE2 shuffles per 16 bytes and one pshufb with SSSE3
# (HighGraphics' VMX culling function: 1,798 -> 1,105 instructions). +1.9% in
# Boletarian Palace at ~117 fps, where HighGraphics is on the frame's path; at
# ~67 fps, with HighGraphics waiting 40% of the time, it measured nothing.
# DES_PPU_OPT=OFF goes back to /Od (with chunk 005 /O2).
option(DES_PPU_OPT "Build des_boot (lifted PPU code and runtime sources) /O2" ON)
if(MSVC)   # also true for clang-cl
    if(DES_PPU_OPT)
        set(DES_OPT_FLAGS /O2 /clang:-fwrapv /clang:-msse4.1)
    else()
        set(DES_OPT_FLAGS /Od)
        set(DES_CHUNK005_FLAGS /O2 ${DES_DEBUG_FLAGS})
        set_source_files_properties("${RECOMP_DIR}/ppu_recomp_005.cpp"
            PROPERTIES COMPILE_OPTIONS "${DES_CHUNK005_FLAGS}")
    endif()
    target_compile_options(des_boot PRIVATE
        /bigobj ${DES_OPT_FLAGS} ${DES_DEBUG_FLAGS} /EHsc /Zc:__cplusplus
        /wd4100 /wd4244 /wd4267 /wd4146 /wd4334
    )
    target_compile_definitions(des_boot PRIVATE _CRT_SECURE_NO_WARNINGS)
    # NOTE: lifted SPU images host guest loops that branch to another lifted
    # function's entry. The lifter emits those as forced tail calls
    # (SPU_TAILCALL = clang __attribute__((musttail))), which iterate in O(1) host
    # stack at ANY optimization level -- so the SPU TUs do NOT need /O2 for
    # correctness (without the forced tail call, /Od nesting overflowed the stack
    # and silently killed the process).
    # In the first level one SPU thread runs the lifted images flat out (98% of
    # a core, a third of it in spu_shufb), so the SPU TUs build /O2 with
    # SSE4.1: the helpers in spu_helpers.h inline, and spu_shufb takes its
    # pshufb path instead of the byte loop. SSE4.1 is safe on any PC that runs
    # Windows 11 (24H2 requires SSE4.2). DES_SPU_OPT=OFF builds them with the
    # target's flags above, without SSE4.1.
    option(DES_SPU_OPT "Build the lifted SPU images /O2 with SSE4.1" ON)
    if(DES_SPU_OPT)
        set(DES_SPU_FLAGS /O2 ${DES_DEBUG_FLAGS} /clang:-msse4.1)
        set_source_files_properties(${SPU_CHUNKS} PROPERTIES
            COMPILE_OPTIONS "${DES_SPU_FLAGS}")
    endif()
endif()

target_link_libraries(des_boot PRIVATE
    ps3recomp_runtime
    ws2_32 xinput ole32 bcrypt          # winsock / gamepad / COM(WASAPI) / RNG
    d3d12 dxgi d3dcompiler uuid dxguid   # RSX D3D12 backend (dxguid: IID_IDXGIFactory4/SwapChain3)
)

# Emit a linker map so we can resolve host RVAs back to func_<guestaddr> even for
# /O2 chunks that llvm-symbolizer can't (used to locate busy-loop spin addresses).
target_link_options(des_boot PRIVATE "/MAP:des_boot.map")

# --- The exe's version ---------------------------------------------------------
# The port version (VERSION and git), the ps3recomp commit or tag and the game
# dump's SHA-256, rewritten on every build into des_version.h (the UI, reports,
# log) and a version resource (Explorer: Properties > Details). docs/54.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DesVersion.cmake")
enable_language(RC)
target_sources(des_boot PRIVATE "${DES_VERSION_RC}")
add_dependencies(des_boot des_version)

# --- The exe's icon ------------------------------------------------------------
# Made at configure time from the game's own PS3_GAME/ICON0.PNG (copy it to
# work/game/ next to EBOOT.ELF, or point DES_ICON0 at it) by scripts/make_icon.py,
# which needs only the Python standard library. GAME-DERIVED: written into the
# build directory, never committed. Without the file the exe has no icon. The
# window uses it too (resource 1, loaded by the runtime's window class).
set(DES_ICON0 "${CMAKE_CURRENT_SOURCE_DIR}/../work/game/ICON0.PNG" CACHE FILEPATH "the game's PS3_GAME/ICON0.PNG")
if(EXISTS "${DES_ICON0}")
    enable_language(RC)
    set(DES_ICO "${CMAKE_CURRENT_BINARY_DIR}/des_boot.ico")
    set(DES_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/des_boot_icon.rc")
    add_custom_command(OUTPUT "${DES_ICO}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/make_icon.py" "${DES_ICON0}" "${DES_ICO}"
        DEPENDS "${DES_ICON0}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/make_icon.py"
        VERBATIM)
    file(CONFIGURE OUTPUT "${DES_ICON_RC}" CONTENT "1 ICON \"${DES_ICO}\"\n")
    set_source_files_properties("${DES_ICON_RC}" PROPERTIES OBJECT_DEPENDS "${DES_ICO}")
    target_sources(des_boot PRIVATE "${DES_ICON_RC}")
    message(STATUS "des_boot: icon from ${DES_ICON0}")
else()
    message(STATUS "des_boot: no icon (${DES_ICON0} not found; copy PS3_GAME/ICON0.PNG there)")
endif()

# --- The wrapper UI (port/ui, docs/32) ---------------------------------------
# Start screen, in-game menu (F1 / Esc, or L3+R3 on a pad) and settings,
# drawn over the game frame. Its own static library, linked only: des_boot's
# compile flags do not change. DES_UI=0 at run time leaves it uninstalled.
add_subdirectory(ui)
target_link_libraries(des_boot PRIVATE des_ui)
