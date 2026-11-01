cmake_minimum_required(VERSION 3.20)
project(des_boot LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- The C and C++ runtime libraries (docs/85) -------------------------------
# By default every target uses the DLL runtime (/MD): the exe then imports
# MSVCP140.dll, VCRUNTIME140.dll and VCRUNTIME140_1.dll, which only the Visual C++
# redistributable installs. DES_HOST_STATIC_CRT=ON links the Visual C++ runtime
# and the C++ library into each binary (/MT) and takes the C library from the
# UCRT that Windows 10 and 11 include (ucrt.lib in place of libucrt.lib): the
# "hybrid" CRT, as CI builds des_host.exe. It holds for the whole build
# directory, since /MT and /MD C++ objects do not link together (the STL's
# RuntimeLibrary check): the UI, FreeType and RmlUi follow it, and the ps3recomp
# runtime libraries linked here must be built with PS3RECOMP_STATIC_CRT=ON.
option(DES_HOST_STATIC_CRT "Link the Visual C++ runtime into each binary and use the UCRT of Windows (hybrid CRT)" OFF)
if(DES_HOST_STATIC_CRT)
    if(NOT MSVC)
        message(FATAL_ERROR "DES_HOST_STATIC_CRT is for clang-cl on Windows")
    endif()
    # cmake_minimum_required(3.20) sets CMP0091 to NEW: this is the runtime of every
    # target made after it, add_subdirectory(ui) and FetchContent's included.
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    # At each exe and DLL link: the UCRT's import library in place of its static
    # copy, and none of the DLL runtime's libraries, so a stray /MD object stops
    # the link instead of importing VCRUNTIME140.dll on a player's PC.
    set(DES_HYBRID_CRT_LINK
        /NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib
        /NODEFAULTLIB:msvcrt.lib /NODEFAULTLIB:msvcprt.lib /NODEFAULTLIB:vcruntime.lib)
    message(STATUS "des_boot: hybrid CRT (DES_HOST_STATIC_CRT): /MT, and the UCRT of Windows")
endif()

# --- Locations -------------------------------------------------------------
# The ps3recomp checkout and the (gitignored) generated/derived tree.
set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" CACHE PATH "ps3recomp checkout")
set(RECOMP_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/../work/recomp" CACHE PATH "lifted output dir")
# The ps3recomp commit this revision needs (ps3recomp.pin, docs/54): warns when the checkout lacks it;
# -DDES_REQUIRE_PIN=ON (releases) requires exactly that commit.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Ps3recompPin.cmake")

# --- Prebuilt runtime library ----------------------------------------------
# Where ps3recomp_runtime.lib (libps3recomp_runtime.a on Linux) is built. A second runtime build
# (tools/pgo/pgo.sh's) links from its own folder.
set(PS3RECOMP_BUILD_DIR "${PS3RECOMP_DIR}/build" CACHE PATH "ps3recomp runtime build dir")
add_library(ps3recomp_runtime STATIC IMPORTED GLOBAL)
if(WIN32)
    set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_BUILD_DIR}/ps3recomp_runtime.lib")
else()
    set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_BUILD_DIR}/libps3recomp_runtime.a")
endif()
set_target_properties(ps3recomp_runtime PROPERTIES IMPORTED_LOCATION "${PS3RECOMP_RUNTIME_LIB}")
# The static libraries built with a runtime that it links, glslang and SPIRV-Tools
# with PS3RECOMP_RSX_HAL_SPV: ps3recomp's configure lists them in
# ps3recomp_runtime_deps.cmake beside the library, and whatever links the
# runtime links them after it. A runtime from before that file has none.
function(des_runtime_deps target lib)
    get_filename_component(_dir "${lib}" DIRECTORY)
    set(_file "${_dir}/ps3recomp_runtime_deps.cmake")
    if(NOT EXISTS "${_file}")
        return()
    endif()
    set(PS3RECOMP_RUNTIME_DEPS "")
    # include() alone makes a change to the file re-run the configure. Naming it in
    # CMAKE_CONFIGURE_DEPENDS too fails under Ninja when the path has a "..":
    # the same file is then listed under two spellings.
    include("${_file}")
    set_property(TARGET ${target} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${PS3RECOMP_RUNTIME_DEPS})
    list(LENGTH PS3RECOMP_RUNTIME_DEPS _n)
    message(STATUS "des_boot: ${target} brings ${_n} static libraries (${_file})")
endfunction()
des_runtime_deps(ps3recomp_runtime "${PS3RECOMP_RUNTIME_LIB}")

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
    # The glue includes "prx_loader.h" by name, so it builds wherever ps3recomp
    # is (a copy, a link, setup's install).
    set_source_files_properties("${LIBSRE_GLUE}" PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/prx")
    message(STATUS "des_boot: libsre LLE available (run with LIBSRE_LLE=1)")
else()
    message(STATUS "des_boot: libsre LLE not built (HLE cellSpurs only)")
endif()

# --- Host only (docs/85) -----------------------------------------------------
# DES_HOST_ONLY=ON, with DES_SPLIT=ON, configures des_host.exe with no lift, as
# CI builds it. des_boot is still defined, because des_host takes its sources
# from it, but it is left out of the build and has no SPU workload table (a lift
# file); des_game is not defined. RECOMP_DIR still has to hold hle_nid_table.cpp
# (ps3recomp's tools/gen_hle_nids.py --all).
option(DES_HOST_ONLY "With DES_SPLIT: configure des_host alone, with no lift" OFF)
set(DES_SPU_WORKLOADS "${SPU_DIR}/_gen/spu_workloads.cpp")
if(DES_HOST_ONLY)
    set(DES_SPU_WORKLOADS "")
endif()

# --- The boot executable ---------------------------------------------------
add_executable(des_boot
    ${RECOMP_CHUNKS}
    ${SPU_CHUNKS}                                           # lifted SPU images
    ${LIBSRE_SOURCES}                                       # real lifted SPURS kernel (opt-in)
    ${TSP_SOURCES}                                          # real SPURS taskset policy (blob + lifted, optional)
    ${DES_SPU_WORKLOADS}                                    # SPU workload registration (auto at startup)
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
    "${CMAKE_CURRENT_SOURCE_DIR}/des_hud.cpp"               # the HUD's size, corners and hidden groups
    "${CMAKE_CURRENT_SOURCE_DIR}/des_menu_mouse.cpp"        # the mouse in the game's menus
    "${CMAKE_CURRENT_SOURCE_DIR}/des_menu_sfx.cpp"          # UI sounds via the game's menu SE (0x724E40 hook)
    "${CMAKE_CURRENT_SOURCE_DIR}/des_chrins.cpp"            # ChrIns worker: done flag before the done event
    "${CMAKE_CURRENT_SOURCE_DIR}/des_souls_use.cpp"         # DES_SOULS_AT_ONCE: Use several soul items at once
    "${CMAKE_CURRENT_SOURCE_DIR}/des_respec.cpp"            # DES_RESPEC: stat respec at the Nexus statue and the Maiden
    "${CMAKE_CURRENT_SOURCE_DIR}/des_prework.cpp"           # startup work: the shader cache, behind a progress bar
)

if(WIN32)
    target_include_directories(des_boot PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/compat")   # Win32 <dirent.h> shim for ppu_fs.cpp
endif()
target_include_directories(des_boot PRIVATE
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
#
# The lifted sources take DS_SHADOW_STACK from the lift's ppu_recomp.h, which
# defines it for a --shadow-stack lift unless DS_NO_SHADOW_STACK is defined.
# The runtime's boot scaffold and the port's own sources include only the fixed
# part of that header (ps3recomp's ps3emu/ppu_lift.h), which cannot say how the
# lift was made, so with DES_SHADOW_STACK=ON they get it here, on the same
# condition: a lift made with --shadow-stack, as scripts/lift_ppu.json (the
# flags of the README's lift and of setup) says. Their DS_FRAME (the ChrIns
# loop's, des_chrins.cpp) then records a frame exactly when the lifted
# functions' do, as when they included the lift's header. Only for these
# sources: every lifted function has a DS_FRAME, and the lifted libsre is made
# without --shadow-stack, so a definition for the whole target would turn on
# libsre's frames. A new source that includes ps3emu/ppu_lift.h goes in the
# list. (des_game_host.cpp is DES_SPLIT's, which has no shadow stack.)
option(DES_SHADOW_STACK "Keep the guest shadow call stack of a --shadow-stack lift" OFF)
if(NOT DES_SHADOW_STACK)
    target_compile_definitions(des_boot PRIVATE DS_NO_SHADOW_STACK)
else()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/lift_ppu.json" _des_lift_json)
    string(JSON _des_lift_ss ERROR_VARIABLE _des_lift_err GET "${_des_lift_json}" shadow_stack)
    if(_des_lift_ss)
        set_property(SOURCE
            "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
            "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
            "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
            "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
            "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/missing_stubs.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_heap.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_options.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_aspect.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_hud.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_menu_mouse.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_menu_sfx.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_chrins.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_souls_use.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_respec.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_dev.cpp"
            APPEND PROPERTY COMPILE_DEFINITIONS DS_SHADOW_STACK=1)
    else()
        message(WARNING "DES_SHADOW_STACK=ON, but scripts/lift_ppu.json lifts without --shadow-stack: "
            "there is no shadow stack to keep")
    endif()
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
# DES_PPU_OPT=OFF goes back to /Od, with the chunk of the boot's hot init loop
# still /O2: the chunk that defines func_00AE1408 (docs/07). It is found by
# that name, since the chunk size sets its number (chunk 005 at 600,000 lines
# a chunk, chunk 022 at 150,000).
option(DES_PPU_OPT "Build des_boot (lifted PPU code and runtime sources) /O2" ON)
set(DES_HOT_CHUNK "")
if(NOT DES_PPU_OPT)
    foreach(_c IN LISTS RECOMP_CHUNKS)
        file(STRINGS "${_c}" _hit REGEX "^void func_00AE1408\\(" LIMIT_COUNT 1)
        if(_hit)
            set(DES_HOT_CHUNK "${_c}")
            break()
        endif()
    endforeach()
    if(NOT DES_HOT_CHUNK)
        message(WARNING "DES_PPU_OPT=OFF: no chunk defines func_00AE1408; every chunk builds /Od")
    endif()
endif()
if(MSVC)   # also true for clang-cl
    if(DES_PPU_OPT)
        set(DES_OPT_FLAGS /O2 /clang:-fwrapv /clang:-msse4.1)
    else()
        set(DES_OPT_FLAGS /Od)
        if(DES_HOT_CHUNK)
            set(DES_HOT_CHUNK_FLAGS /O2 ${DES_DEBUG_FLAGS})
            set_source_files_properties("${DES_HOT_CHUNK}"
                PROPERTIES COMPILE_OPTIONS "${DES_HOT_CHUNK_FLAGS}")
        endif()
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
else()
    # Clang or GCC (Linux, docs/76): the same code generation as clang-cl above. clang-cl turns strict
    # aliasing off by default; here it has to be asked for, and the lifted code depends on it (guest
    # memory is read through casts of every width). No fused multiply-add, as the runtime builds
    # (its CMakeLists.txt says why).
    if(DES_PPU_OPT)
        set(DES_OPT_FLAGS -O2 -fwrapv -msse4.1)
    else()
        set(DES_OPT_FLAGS -O0)
        if(DES_HOT_CHUNK)
            set_source_files_properties("${DES_HOT_CHUNK}" PROPERTIES COMPILE_OPTIONS "-O2")
        endif()
    endif()
    if(DES_DEBUG_INFO)
        set(DES_DEBUG_FLAGS -g)
    endif()
    target_compile_options(des_boot PRIVATE
        ${DES_OPT_FLAGS} ${DES_DEBUG_FLAGS} -fno-strict-aliasing -ffp-contract=off
        -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-label
        -Wno-unused-but-set-variable)
    target_compile_definitions(des_boot PRIVATE _GNU_SOURCE)
    option(DES_SPU_OPT "Build the lifted SPU images -O2 with SSE4.1" ON)
    if(DES_SPU_OPT)
        set_source_files_properties(${SPU_CHUNKS} PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1")
    endif()
endif()

target_link_libraries(des_boot PRIVATE ps3recomp_runtime)
if(DES_HOST_STATIC_CRT)
    target_link_options(des_boot PRIVATE ${DES_HYBRID_CRT_LINK})
endif()
if(DES_HOST_ONLY)
    set_target_properties(des_boot PROPERTIES EXCLUDE_FROM_ALL ON)   # only des_host's source list
endif()
if(WIN32)
    target_link_libraries(des_boot PRIVATE
        ws2_32 xinput ole32 bcrypt          # winsock / gamepad / COM(WASAPI) / RNG
        d3d12 dxgi d3dcompiler uuid dxguid   # RSX D3D12 backend (dxguid: IID_IDXGIFactory4/SwapChain3)
    )
    # Emit a linker map so we can resolve host RVAs back to func_<guestaddr> even for
    # /O2 chunks that llvm-symbolizer can't (used to locate busy-loop spin addresses).
    target_link_options(des_boot PRIVATE "/MAP:des_boot.map")
else()
    # The runtime's own dependencies (a static library carries none): SDL2 for the pad and audio,
    # threads, dlopen.
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    if(TARGET SDL2::SDL2)
        target_link_libraries(des_boot PRIVATE SDL2::SDL2)
    else()
        target_link_libraries(des_boot PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_link_libraries(des_boot PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    target_link_options(des_boot PRIVATE "-Wl,-Map=des_boot.map")
    # The window and the rendering: ps3recomp's D3D12 backend on vkd3d (Wine's D3D12 on Vulkan), which the
    # runtime must be built with (-DPS3RECOMP_RSX_D3D12_VKD3D=ON; docs/76). Off, the game runs headless on
    # the null backend.
    option(DES_VKD3D "Render through the D3D12 backend on vkd3d" ON)
    if(DES_VKD3D)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(DES_VKD3D_LIBS REQUIRED IMPORTED_TARGET libvkd3d libvkd3d-utils)
        target_compile_definitions(des_boot PRIVATE PS3RECOMP_RSX_D3D12_VKD3D)
        target_link_libraries(des_boot PRIVATE PkgConfig::DES_VKD3D_LIBS)
    endif()
    message(STATUS "des_boot: rendering ${DES_VKD3D} (DES_VKD3D: the D3D12 backend on vkd3d)")
endif()

# --- The exe's version ---------------------------------------------------------
# The port version (VERSION and git), the ps3recomp commit or tag and the game
# dump's SHA-256, rewritten on every build into des_version.h (the UI, reports,
# log) and a version resource (Explorer: Properties > Details). docs/54.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DesVersion.cmake")
if(WIN32)
    enable_language(RC)
    target_sources(des_boot PRIVATE "${DES_VERSION_RC}")
endif()
add_dependencies(des_boot des_version)

# --- FFmpeg beside the exe (Windows) -------------------------------------------
# A Windows runtime build downloads FFmpeg's MPEG-2 video decoder into <its build
# folder>/ffmpeg-win64/bin (ps3recomp's cmake/ffmpeg_download.cmake). The runtime
# opens the DLLs by name, so each link copies them beside the exe; without them
# the movies use Windows' own decoder, which needs the Microsoft Store's MPEG-2
# Video Extension (docs/110). CI's runtime has its own FFmpeg and no such folder,
# so nothing is copied there.
function(des_copy_ffmpeg_dlls target runtime_dir)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" "-DFROM=${runtime_dir}/ffmpeg-win64/bin" "-DTO=$<TARGET_FILE_DIR:${target}>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
            VERBATIM)
    endif()
endfunction()
des_copy_ffmpeg_dlls(des_boot "${PS3RECOMP_BUILD_DIR}")

# --- The exe's icon ------------------------------------------------------------
# Made at configure time from the game's own PS3_GAME/ICON0.PNG (copy it to
# work/game/ next to EBOOT.ELF, or point DES_ICON0 at it) by scripts/make_icon.py,
# which needs only the Python standard library. GAME-DERIVED: written into the
# build directory, never committed. The window uses it too (resource 1, loaded
# by the runtime's window class). Without the file the exe has no icon, and the
# window takes the game's ICON0.PNG at run time instead (ps3recomp's
# rsx_window_icon.h), as CI's des_host.exe does.
set(DES_ICON0 "${CMAKE_CURRENT_SOURCE_DIR}/../work/game/ICON0.PNG" CACHE FILEPATH "the game's PS3_GAME/ICON0.PNG")
if(EXISTS "${DES_ICON0}" AND WIN32)
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
# compile flags do not change. DES_UI=0 at run time leaves it uninstalled. -DDES_UI=OFF builds
# without it (des_ui_stub.cpp). Off Windows it draws through D3D12 on vkd3d, so it needs DES_VKD3D (docs/76).
if(WIN32 OR DES_VKD3D)
    option(DES_UI "Build the wrapper UI (start screen, menu, settings)" ON)
else()
    option(DES_UI "Build the wrapper UI (start screen, menu, settings)" OFF)
endif()
if(DES_UI)
    add_subdirectory(ui)
    target_link_libraries(des_boot PRIVATE des_ui)
else()
    target_sources(des_boot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/des_ui_stub.cpp")
    target_include_directories(des_boot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui/include")
    message(STATUS "des_boot: no wrapper UI (DES_UI=OFF)")
endif()

# --- Split build: des_host.exe and des_game.dll (docs/57) ----------------------
# DES_SPLIT=ON adds two targets beside des_boot, which does not change:
#   des_game  a DLL of the lifted code (PPU chunks, SPU images, the lifted libsre
#             and its glue), compiled with des_boot's flags, so its objects are
#             des_boot's; ps3recomp's runtime/ppu/game_module.c and
#             des_game_module.cpp supply what the lifted code calls outside it.
#   des_host  the exe: des_boot's other sources and libraries, with the runtime
#             built with PS3RECOMP_GAME_SPLIT=ON in its own build directory
#             (PS3RECOMP_SPLIT_LIB). It loads des_game.dll from its own folder
#             (or PS3_GAME_MODULE).
# Both take their sources, include folders, definitions and options from
# des_boot itself, so they follow it; this section stays last. Windows
# (clang-cl) and x86-64 Linux (clang), not with DES_SHADOW_STACK.
# DES_SPLIT_FREESTANDING=ON builds des_game.dll with no C runtime, SDK or C++
# library of its own (cmake/DesGameFreestanding.cmake, docs/84); use its own
# build directory. On Linux the module is des_game.so, always freestanding
# (docs/87): the player's PC builds it with no C library headers.
option(DES_SPLIT "Also build des_host.exe and des_game.dll (the lifted code as a DLL)" OFF)
if(DES_HOST_ONLY AND NOT DES_SPLIT)
    message(FATAL_ERROR "DES_HOST_ONLY needs -DDES_SPLIT=ON")
endif()
if(DES_SPLIT)
    if(WIN32 AND NOT MSVC)
        message(FATAL_ERROR "DES_SPLIT needs clang-cl")
    endif()
    if(NOT WIN32 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID STREQUAL "Clang"
                          AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_SIZEOF_VOID_P EQUAL 8))
        message(FATAL_ERROR "DES_SPLIT off Windows needs x86-64 Linux and clang for C and C++")
    endif()
    if(NOT WIN32 AND NOT DES_SPLIT_FREESTANDING)
        message(FATAL_ERROR "DES_SPLIT on Linux builds des_game.so freestanding only: add -DDES_SPLIT_FREESTANDING=ON")
    endif()
    if(DES_SHADOW_STACK)
        message(FATAL_ERROR "DES_SPLIT does not support DES_SHADOW_STACK")
    endif()
    # What differs: the module's name, the runtime library's, the forced include's flag (clang takes -include<file>
    # as one word, which CMake does not split or merge) and the linker map's.
    if(WIN32)
        set(DES_GAME_MODULE "des_game.dll")
        set(_des_split_lib "ps3recomp_runtime.lib")
        set(DES_FORCE_INCLUDE "/FI")
        set(_des_map "/MAP:")
    else()
        set(DES_GAME_MODULE "des_game.so")
        set(_des_split_lib "libps3recomp_runtime.a")
        set(DES_FORCE_INCLUDE "-include")
        set(_des_map "-Wl,-Map=")
    endif()
    set(PS3RECOMP_SPLIT_LIB "${PS3RECOMP_DIR}/build_split/${_des_split_lib}" CACHE FILEPATH
        "the ps3recomp runtime library built with PS3RECOMP_GAME_SPLIT=ON")
    add_library(ps3recomp_runtime_split STATIC IMPORTED GLOBAL)
    set_target_properties(ps3recomp_runtime_split PROPERTIES IMPORTED_LOCATION "${PS3RECOMP_SPLIT_LIB}")
    des_runtime_deps(ps3recomp_runtime_split "${PS3RECOMP_SPLIT_LIB}")
    set(DES_SPLIT_GEN "${CMAKE_CURRENT_BINARY_DIR}/split_gen")

    # A generated file, written only when its text changes (no rebuild otherwise).
    function(_des_split_write path text)
        set(old "")
        if(EXISTS "${path}")
            file(READ "${path}" old)
        endif()
        if(NOT old STREQUAL text)
            file(WRITE "${path}" "${text}")
        endif()
    endfunction()

    # The SPU workload table without its static constructor: des_game_init hands
    # des_spu_register_all to the exe, which calls it when the DLL loads. A copy
    # in the build directory, so the include of the runtime header is absolute.
    set(DES_SPLIT_WL_IN "${SPU_DIR}/_gen/spu_workloads.cpp")
    set(DES_SPLIT_WL "${DES_SPLIT_GEN}/spu_workloads.cpp")
    # (Not with DES_HOST_ONLY: there is no lift, and des_host does not compile it.)
    if(NOT DES_HOST_ONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DES_SPLIT_WL_IN}")
        file(READ "${DES_SPLIT_WL_IN}" _des_wl)
        string(REPLACE "__attribute__((constructor)) static void des_spu_autoreg(void)"
                       "[[maybe_unused]] static void des_spu_autoreg(void)" _des_wl "${_des_wl}")
        string(REPLACE "#include \"../../runtime/spu/" "#include \"${PS3RECOMP_DIR}/runtime/spu/" _des_wl "${_des_wl}")
        string(FIND "${_des_wl}" "constructor" _des_wl_ctor)
        if(NOT _des_wl_ctor EQUAL -1)
            message(FATAL_ERROR "DES_SPLIT: ${DES_SPLIT_WL_IN} still has a static constructor")
        endif()
        _des_split_write("${DES_SPLIT_WL}" "${_des_wl}")
    endif()

    # The port functions the lifted code calls by name: the function overrides
    # (scripts/lift_ppu.json) and the instruction hooks (insn_hooks.txt), read
    # from the lift's own inputs into split_gen/des_game_title_funcs.h. The DLL
    # has a jump thunk for each and asks the exe for them by name at the load.
    set(DES_LIFT_JSON  "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/lift_ppu.json")
    set(DES_INSN_HOOKS "${CMAKE_CURRENT_SOURCE_DIR}/insn_hooks.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DES_LIFT_JSON}" "${DES_INSN_HOOKS}")
    set(_des_fns "")
    file(READ "${DES_LIFT_JSON}" _des_json)
    string(JSON _des_n LENGTH "${_des_json}" override_funcs)
    if(_des_n GREATER 0)
        math(EXPR _des_last "${_des_n} - 1")
        foreach(_i RANGE ${_des_last})
            string(JSON _des_ov GET "${_des_json}" override_funcs ${_i})
            if(_des_ov MATCHES "=[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*$")
                list(APPEND _des_fns "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endif()
    file(STRINGS "${DES_INSN_HOOKS}" _des_hooks REGEX "^[ \t]*0x[0-9A-Fa-f]+[ \t]*=")
    foreach(_l IN LISTS _des_hooks)
        if(_l MATCHES "^[ \t]*0x[0-9A-Fa-f]+[ \t]*=[ \t]*([A-Za-z_][A-Za-z0-9_]*)")
            list(APPEND _des_fns "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _des_fns)
    list(LENGTH _des_fns _des_nfns)
    set(_des_tf "/* Generated by port/CMakeLists.txt (DES_SPLIT) from scripts/lift_ppu.json and port/insn_hooks.txt. */\n")
    string(APPEND _des_tf "#pragma once\n#define DES_GAME_TITLE_FUNCS(X)")
    foreach(_f IN LISTS _des_fns)
        string(APPEND _des_tf " \\\n    X(${_f})")
    endforeach()
    string(APPEND _des_tf "\n")
    _des_split_write("${DES_SPLIT_GEN}/des_game_title_funcs.h" "${_des_tf}")

    # The SPU native hooks and blocks (scripts/build_spu.py) are runtime
    # functions the lifted SPU code calls by name, so each has to be one the exe
    # hands the DLL (PS3_GAME_HOST_FUNCS in ps3recomp's ps3emu/game_module.h).
    # A name missing there fails only the DLL's link, which needs the lift (on
    # the player's PC, at setup), so it is checked here, where CI configures.
    set(DES_BUILD_SPU "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build_spu.py")
    set(DES_GAME_MODULE_H "${PS3RECOMP_DIR}/include/ps3emu/game_module.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DES_BUILD_SPU}" "${DES_GAME_MODULE_H}")
    file(READ "${DES_BUILD_SPU}" _des_bspu)
    file(READ "${DES_GAME_MODULE_H}" _des_gm_h)
    # "ADDR=SYMBOL" and "ADDR-END=SYMBOL" (a BI_SWITCH entry's value is an address).
    string(REGEX MATCHALL "\"0x[0-9A-Fa-f]+(-0x[0-9A-Fa-f]+)?=[A-Za-z_][A-Za-z0-9_]*\"" _des_hs "${_des_bspu}")
    set(_des_missing "")
    foreach(_h IN LISTS _des_hs)
        string(REGEX REPLACE "^.*=([A-Za-z0-9_]+)\"$" "\\1" _h "${_h}")
        if(NOT _des_gm_h MATCHES "X\\(${_h}\\)")
            list(APPEND _des_missing "${_h}")
        endif()
    endforeach()
    if(_des_missing)
        list(REMOVE_DUPLICATES _des_missing)
        message(FATAL_ERROR "DES_SPLIT: scripts/build_spu.py's native hooks call ${_des_missing}, which "
            "PS3_GAME_HOST_FUNCS (${DES_GAME_MODULE_H}) does not list: des_game's link would fail. Add each at "
            "the end of that list in ps3recomp, with PS3_GAME_ABI raised, and pin it.")
    endif()

    # des_boot's own settings, for both targets.
    foreach(_p SOURCES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES)
        get_target_property(_des_boot_${_p} des_boot ${_p})
        if(NOT _des_boot_${_p})
            set(_des_boot_${_p} "")
        endif()
    endforeach()

    if(NOT DES_HOST_ONLY)
        # The game DLL. Its lifted sources get exactly des_boot's definitions and
        # options; the glue gets PS3_GAME_MODULE on its own.
        set(DES_GAME_GLUE
            "${PS3RECOMP_DIR}/runtime/ppu/game_module.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/des_game_module.cpp"
        )
        set_source_files_properties(${DES_GAME_GLUE} PROPERTIES COMPILE_DEFINITIONS PS3_GAME_MODULE)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/des_game_module.cpp"
            APPEND PROPERTY INCLUDE_DIRECTORIES "${DES_SPLIT_GEN}")
        set(DES_GAME_LIBSRE "")
        if(LIBSRE_SOURCES)
            set(DES_GAME_LIBSRE "${LIBSRE_LIFTED}" "${LIBSRE_GLUE}")
            set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/des_game_module.cpp"
                APPEND PROPERTY COMPILE_DEFINITIONS DES_GAME_HAS_LIBSRE)
        endif()
        add_library(des_game SHARED
            ${RECOMP_CHUNKS}
            ${SPU_CHUNKS}
            ${DES_GAME_LIBSRE}
            "${DES_SPLIT_WL}"
            ${DES_GAME_GLUE}
        )
        # No des_game_EXPORTS definition: the lifted sources then compile with
        # des_boot's command line exactly (the export is declared in its source).
        set_target_properties(des_game PROPERTIES DEFINE_SYMBOL "")
        target_include_directories(des_game PRIVATE ${_des_boot_INCLUDE_DIRECTORIES})
        target_compile_definitions(des_game PRIVATE ${_des_boot_COMPILE_DEFINITIONS})
        target_compile_options(des_game PRIVATE ${_des_boot_COMPILE_OPTIONS})
        target_link_options(des_game PRIVATE "${_des_map}des_game.map")
        if(DES_DEBUG_INFO AND WIN32)
            target_link_options(des_game PRIVATE /DEBUG /OPT:REF /OPT:ICF)
        endif()
        if(NOT WIN32)   # des_game.so, as the host looks for it (CMake would name it libdes_game.so)
            set_target_properties(des_game PROPERTIES PREFIX "" SUFFIX ".so")
        endif()
        if(DES_HOST_STATIC_CRT)
            target_link_options(des_game PRIVATE ${DES_HYBRID_CRT_LINK})
        endif()
    endif()

    # The exe: des_boot's sources minus the lifted ones, plus the loader and the
    # port's two halves of the boundary.
    set(_des_host_src ${_des_boot_SOURCES})
    list(REMOVE_ITEM _des_host_src ${RECOMP_CHUNKS} ${SPU_CHUNKS} "${DES_SPLIT_WL_IN}")
    if(LIBSRE_SOURCES)
        list(REMOVE_ITEM _des_host_src "${LIBSRE_LIFTED}" "${LIBSRE_GLUE}")
    endif()
    foreach(_s IN LISTS _des_host_src)
        if(_s MATCHES "ppu_recomp_[0-9]+\\.cpp$|work_spu/.*spu_recomp\\.c$|spu_workloads\\.cpp$|libsre/lifted/|libsre_glue\\.c$")
            message(FATAL_ERROR "DES_SPLIT: ${_s} is lifted code but would go into des_host")
        endif()
    endforeach()
    list(REMOVE_ITEM _des_boot_LINK_LIBRARIES ps3recomp_runtime)
    add_executable(des_host
        ${_des_host_src}
        "${PS3RECOMP_DIR}/runtime/ppu/game_host.c"              # loads des_game.dll (des_game.so)
        "${CMAKE_CURRENT_SOURCE_DIR}/des_game_host.cpp"         # the lifted functions the port calls
        "${CMAKE_CURRENT_SOURCE_DIR}/des_game_title.c"          # the port functions the lift calls
    )
    target_include_directories(des_host PRIVATE ${_des_boot_INCLUDE_DIRECTORIES} "${DES_SPLIT_GEN}")
    target_compile_definitions(des_host PRIVATE ${_des_boot_COMPILE_DEFINITIONS} PS3_GAME_SPLIT
        PS3_GAME_MODULE_NAME="${DES_GAME_MODULE}" PS3_GAME_MODULE_INIT="des_game_init")
    target_compile_options(des_host PRIVATE ${_des_boot_COMPILE_OPTIONS}
        "${DES_FORCE_INCLUDE}${PS3RECOMP_DIR}/include/ps3emu/game_split.h")
    target_link_libraries(des_host PRIVATE ps3recomp_runtime_split ${_des_boot_LINK_LIBRARIES})
    target_link_options(des_host PRIVATE "${_des_map}des_host.map")
    if(DES_DEBUG_INFO AND WIN32)
        target_link_options(des_host PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()
    add_dependencies(des_host des_version)
    if(NOT DES_HOST_ONLY)
        add_dependencies(des_host des_game)   # built together, so a run finds a matching DLL beside the exe
    endif()
    if(DES_HOST_STATIC_CRT)
        target_link_options(des_host PRIVATE ${DES_HYBRID_CRT_LINK})
    endif()
    if(DES_DEV)
        add_custom_command(TARGET des_host POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E echo "des_host: DEVELOPER BUILD (DES_DEV=ON), the cheats are compiled in. Never share it."
            VERBATIM)
    endif()
    get_filename_component(_des_split_dir "${PS3RECOMP_SPLIT_LIB}" DIRECTORY)
    des_copy_ffmpeg_dlls(des_host "${_des_split_dir}")
    # A freestanding des_game.dll: the same targets, changed in place (not with DES_HOST_ONLY: there is no des_game).
    option(DES_SPLIT_FREESTANDING "Build des_game.dll freestanding: no C runtime, SDK or C++ library (docs/84)" OFF)
    if(DES_SPLIT_FREESTANDING AND NOT DES_HOST_ONLY)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DesGameFreestanding.cmake")
    endif()
    if(DES_HOST_ONLY)
        set(_des_split_what "des_host only (DES_HOST_ONLY)")
    else()
        set(_des_split_what "des_host + ${DES_GAME_MODULE}")
    endif()
    message(STATUS "des_boot: split build ON: ${_des_split_what} (runtime ${PS3RECOMP_SPLIT_LIB}, ${_des_nfns} port functions for the lift)")
endif()

# --- The renderer's route switch (PS3RECOMP_RSX_HAL, docs/120) -----------------
# A runtime built with ps3recomp's PS3RECOMP_RSX_HAL has a route switch, rsx_out
# (libs/video/rsx_out.h): it runs today's D3D12 renderer or the portable
# renderer's new path, as RSX_BACKEND says. With such a runtime the port calls
# the renderer through it: boot_main.cpp's branch, and des_rsx_route.h in the
# files listed below (a new file that includes it goes in the list). The runtime
# is an imported library, so its PUBLIC definition does not reach these sources.
# DES_RSX_HAL=AUTO (the default) reads the option from the CMakeCache.txt of each
# runtime build linked here (PS3RECOMP_BUILD_DIR, and PS3RECOMP_SPLIT_LIB's
# folder with DES_SPLIT); ON or OFF overrides it. Off, no compile line changes.
# This comes after the split build: des_host compiles the same source files, so
# it gets these source properties too.
set(DES_RSX_HAL "AUTO" CACHE STRING "The runtime has the renderer's route switch (PS3RECOMP_RSX_HAL): AUTO, ON or OFF")
set_property(CACHE DES_RSX_HAL PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${DES_RSX_HAL}" _des_hal)
set(_des_hal_dirs "")
if(NOT DES_HOST_ONLY)
    list(APPEND _des_hal_dirs "${PS3RECOMP_BUILD_DIR}")
endif()
if(DES_SPLIT)
    get_filename_component(_d "${PS3RECOMP_SPLIT_LIB}" DIRECTORY)
    list(APPEND _des_hal_dirs "${_d}")
endif()
if(_des_hal STREQUAL "AUTO")
    set(_des_hal_has "")
    foreach(_d IN LISTS _des_hal_dirs)
        set(_v OFF)
        if(EXISTS "${_d}/CMakeCache.txt")
            file(STRINGS "${_d}/CMakeCache.txt" _l REGEX "^PS3RECOMP_RSX_HAL:[A-Z]+=")
            string(REGEX REPLACE "^[^=]*=" "" _l "${_l}")
            if(_l)
                set(_v ON)
            endif()
        endif()
        list(APPEND _des_hal_has ${_v})
    endforeach()
    list(REMOVE_DUPLICATES _des_hal_has)
    list(LENGTH _des_hal_has _n)
    if(_n GREATER 1)
        string(REPLACE ";" " and " _des_hal_dirs "${_des_hal_dirs}")
        message(FATAL_ERROR "DES_RSX_HAL=AUTO: of the runtimes in ${_des_hal_dirs}, one is built with "
            "PS3RECOMP_RSX_HAL and one without. Build them the same way, or set DES_RSX_HAL.")
    endif()
    set(_des_hal ${_des_hal_has})
elseif(_des_hal)
    set(_des_hal ON)
else()
    set(_des_hal OFF)
endif()
# Off Windows the switch is built only with the D3D12 backend on vkd3d (rsx_out.c).
if(_des_hal AND NOT WIN32 AND NOT DES_VKD3D)
    message(STATUS "des_boot: the runtime's route switch needs vkd3d (DES_VKD3D); the renderer calls stay direct")
    set(_des_hal OFF)
endif()
if(_des_hal)
    set_property(SOURCE
        "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/des_options.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/des_prework.cpp"
        APPEND PROPERTY COMPILE_DEFINITIONS PS3RECOMP_RSX_HAL=1)
    if(TARGET des_ui)
        # The UI's files, which also need rsx_out.h's folder (des_boot's sources have it).
        set(_des_ui_route
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/src/des_ui.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/src/report.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/src/mouse_sdl.cpp")
        set_property(SOURCE ${_des_ui_route} TARGET_DIRECTORY des_ui
            APPEND PROPERTY COMPILE_DEFINITIONS PS3RECOMP_RSX_HAL=1)
        set_property(SOURCE ${_des_ui_route} TARGET_DIRECTORY des_ui
            APPEND PROPERTY INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/libs/video")
        # The menus' renderer on the runtime's GPU layer (render_gpu.cpp, which includes gpu.h) and host.cpp's choice
        # of it (DES_UI_RENDER=gpu). Without the switch render_gpu.cpp is an empty file.
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ui/src/host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/ui/src/render_gpu.cpp"
            TARGET_DIRECTORY des_ui APPEND PROPERTY COMPILE_DEFINITIONS PS3RECOMP_RSX_HAL=1)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ui/src/render_gpu.cpp" TARGET_DIRECTORY des_ui
            APPEND PROPERTY INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/libs/video/gpu")
    endif()
endif()
# prework_check (port/tests): des_prework.cpp's shaders pass as setup runs it, in a program that runs no game, for
# the pass's targets and their stamps. With the route switch; built only when asked for. des_prework.cpp keeps the
# source properties above (PS3RECOMP_RSX_HAL); the rest of des_boot's compile line it does not need.
if(_des_hal)
    add_executable(prework_check EXCLUDE_FROM_ALL
        tests/prework_check.cpp
        des_prework.cpp
        "${PS3RECOMP_DIR}/runtime/host/host_stubs.c")   # (what the runtime's modules take from the PPU loader)
    target_include_directories(prework_check PRIVATE
        "${PS3RECOMP_DIR}/include" "${PS3RECOMP_DIR}/runtime/ppu" "${PS3RECOMP_DIR}/libs/video")
    if(MSVC)
        target_compile_options(prework_check PRIVATE /EHsc /Zc:__cplusplus)
        target_compile_definitions(prework_check PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(prework_check PRIVATE ps3recomp_runtime)
    if(WIN32)
        target_link_libraries(prework_check PRIVATE ws2_32 xinput ole32 bcrypt d3d12 dxgi d3dcompiler uuid dxguid)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(prework_check PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    endif()
endif()

message(STATUS "des_boot: renderer calls through the route switch (DES_RSX_HAL=${DES_RSX_HAL}): ${_des_hal}")

# ui_render_compare (port/ui/tests): render_gpu.cpp's proof, the menus drawn by it and by render_d3d12.cpp on one
# D3D12 device and their pictures compared byte for byte; with a runtime built with the Vulkan backend
# (PS3RECOMP_RSX_HAL_VULKAN in its CMakeCache.txt), drawn on a Vulkan device too (URC_VULKAN). With the route switch, on
# Windows; built only when asked for.
if(_des_hal AND WIN32 AND TARGET des_ui)
    add_executable(ui_render_compare EXCLUDE_FROM_ALL
        ui/tests/ui_render_compare.cpp
        ui/tests/ui_render_compare_hooks.c
        ui/tests/ui_render_compare_stubs.cpp
        "${PS3RECOMP_DIR}/runtime/host/host_stubs.c")   # (what the runtime's modules take from the PPU loader)
    target_include_directories(ui_render_compare PRIVATE ui/src ui/include "${PS3RECOMP_DIR}/libs/video/gpu")
    target_compile_definitions(ui_render_compare PRIVATE PS3RECOMP_RSX_HAL=1 NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set(_urc_vk "")
    if(EXISTS "${PS3RECOMP_BUILD_DIR}/CMakeCache.txt")
        file(STRINGS "${PS3RECOMP_BUILD_DIR}/CMakeCache.txt" _urc_vk REGEX "^PS3RECOMP_RSX_HAL_VULKAN:[A-Z]+=(ON|TRUE|1)$")
    endif()
    if(_urc_vk)
        target_compile_definitions(ui_render_compare PRIVATE URC_VULKAN=1)
    endif()
    if(MSVC)
        target_compile_options(ui_render_compare PRIVATE /O2 /EHsc /W3)
    endif()
    target_link_libraries(ui_render_compare PRIVATE des_ui RmlUi::RmlUi ps3recomp_runtime
        d3d12 dxgi dxguid d3dcompiler)
endif()

# Settings > Display > Renderer (D3D12 / Vulkan; docs/120): only with a route switch that has the Vulkan route, on
# Windows: every runtime linked here built with PS3RECOMP_RSX_HAL_VULKAN and the spv target it needs
# (PS3RECOMP_RSX_HAL_SPV), as their CMakeCache.txt files say. The UI's settings files then get DES_RSX_VULKAN=1;
# otherwise their compile lines do not change.
set(_des_hal_vk OFF)
if(_des_hal AND WIN32 AND TARGET des_ui AND _des_hal_dirs)
    set(_des_hal_vk ON)
    foreach(_d IN LISTS _des_hal_dirs)
        set(_vk OFF)
        set(_spv OFF)
        if(EXISTS "${_d}/CMakeCache.txt")
            file(STRINGS "${_d}/CMakeCache.txt" _l REGEX "^PS3RECOMP_RSX_HAL_(VULKAN|SPV):[A-Z]+=")
            foreach(_e IN LISTS _l)
                if(_e MATCHES "^PS3RECOMP_RSX_HAL_VULKAN:[A-Z]+=(ON|1|TRUE|YES)$")
                    set(_vk ON)
                elseif(_e MATCHES "^PS3RECOMP_RSX_HAL_SPV:[A-Z]+=(ON|1|TRUE|YES)$")
                    set(_spv ON)
                endif()
            endforeach()
        endif()
        if(NOT _vk OR NOT _spv)
            set(_des_hal_vk OFF)
        endif()
    endforeach()
endif()
if(_des_hal_vk)
    set_property(SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/src/settings.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/src/settings_window.cpp"
        TARGET_DIRECTORY des_ui APPEND PROPERTY COMPILE_DEFINITIONS DES_RSX_VULKAN=1)
endif()
message(STATUS "des_boot: Settings > Display > Renderer (the runtime's Vulkan route): ${_des_hal_vk}")
