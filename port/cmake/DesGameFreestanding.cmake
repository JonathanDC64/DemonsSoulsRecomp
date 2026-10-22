# A freestanding des_game.dll (DES_SPLIT_FREESTANDING=ON, docs/84): no C runtime, Windows SDK or C++ library.
# Included by port/CMakeLists.txt at the end of its DES_SPLIT section, after des_game and des_host exist. It
# changes des_game in place, so scripts/game_build_desc.py reads its commands as it reads milestone 1's, and
# setup's compile (tools/setup/des_setup/compile.py) runs them with no change of its own.
#
#   Compile  clang-cl with /X: clang's own headers and ps3recomp's include/ps3emu/game_libc/ only. A hosted
#            compile, not -ffreestanding, which turns the inline memcpy and sqrt into calls. The forced include
#            ps3emu/game_freestanding.h sets the fences and the thread-local keyword that the lifted sources
#            take from the MSVC headers otherwise, and leaves out ps3types.h's C++ part (the workload table's
#            only use of the C++ library). /Zl: no default-library names in the objects. /GS-: no stack
#            cookies, which are the C runtime's. /GR-: no RTTI, which needs the C++ library's type_info.
#   Sources  one more: ps3recomp's runtime/ppu/game_libc.c (memcpy and friends, _fltused, the TLS directory).
#   Link     lld-link /nodefaultlib /noentry. CMake's default Windows libraries are cleared for this folder
#            and given back to des_boot and des_host by name. The one library is an import library for
#            kernel32's GetCurrentThreadId, made from ps3recomp's game_libc_kernel32.def with lld-link /lib:
#            Windows sets up a DLL's TLS only when the image has an import table (game_libc.c).
#
# The C library functions the sources still call are jump thunks into the host's C runtime
# (PS3_GAME_HOST_CRT_FUNCS, ps3emu/game_module.h). Build it in its own build directory: the same targets
# are milestone 1's in another.

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "DES_SPLIT_FREESTANDING needs clang-cl for C and C++")
endif()
get_filename_component(_des_fs_ld "${CMAKE_LINKER}" NAME_WE)
string(TOLOWER "${_des_fs_ld}" _des_fs_ld)
if(NOT _des_fs_ld STREQUAL "lld-link")
    message(FATAL_ERROR "DES_SPLIT_FREESTANDING needs lld-link (CMAKE_LINKER is ${CMAKE_LINKER})")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DES_SPLIT_FREESTANDING is for 64-bit Windows only")
endif()
# floor, trunc, ceil and nearbyint are one SSE4.1 instruction; without it (/Od, or no -msse4.1) they are
# C runtime calls, which the module does not have.
if(NOT DES_PPU_OPT OR NOT DES_SPU_OPT)
    message(FATAL_ERROR "DES_SPLIT_FREESTANDING needs DES_PPU_OPT=ON and DES_SPU_OPT=ON (SSE4.1 rounding)")
endif()
set(DES_GAME_LIBC "${PS3RECOMP_DIR}/include/ps3emu/game_libc")
foreach(_f "${DES_GAME_LIBC}/stdio.h" "${PS3RECOMP_DIR}/include/ps3emu/game_freestanding.h"
           "${PS3RECOMP_DIR}/runtime/ppu/game_libc.c" "${PS3RECOMP_DIR}/runtime/ppu/game_libc_kernel32.def")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "DES_SPLIT_FREESTANDING: ${_f} is missing (ps3recomp too old for docs/84)")
    endif()
endforeach()

target_sources(des_game PRIVATE "${PS3RECOMP_DIR}/runtime/ppu/game_libc.c")
target_include_directories(des_game BEFORE PRIVATE "${DES_GAME_LIBC}")
target_compile_definitions(des_game PRIVATE PS3_GAME_FREESTANDING)
target_compile_options(des_game PRIVATE /X /Zl /GS- $<$<COMPILE_LANGUAGE:CXX>:/GR->
    "/FI${PS3RECOMP_DIR}/include/ps3emu/game_freestanding.h")
target_link_options(des_game PRIVATE /NODEFAULTLIB /NOENTRY /MANIFEST:NO)

# The import library, in the split's generated folder (setup's {gen}).
set(DES_GAME_K32 "${DES_SPLIT_GEN}/game_libc_kernel32.lib")
add_custom_command(OUTPUT "${DES_GAME_K32}"
    COMMAND "${CMAKE_LINKER}" /lib /nologo /machine:x64
            "/def:${PS3RECOMP_DIR}/runtime/ppu/game_libc_kernel32.def" "/out:${DES_GAME_K32}"
    DEPENDS "${PS3RECOMP_DIR}/runtime/ppu/game_libc_kernel32.def"
    COMMENT "des_game: kernel32 import library (no Windows SDK)" VERBATIM)
add_custom_target(des_game_k32 DEPENDS "${DES_GAME_K32}")
add_dependencies(des_game des_game_k32)
target_link_libraries(des_game PRIVATE "${DES_GAME_K32}")

# CMake links every target of a folder with CMAKE_<LANG>_STANDARD_LIBRARIES (kernel32.lib and nine more),
# read when it generates. The DLL must have none (setup links it with no Windows SDK), so they are cleared
# here for the whole folder and given to the two exes by name.
separate_arguments(_des_fs_std WINDOWS_COMMAND "${CMAKE_CXX_STANDARD_LIBRARIES}")
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
target_link_libraries(des_boot PRIVATE ${_des_fs_std})
target_link_libraries(des_host PRIVATE ${_des_fs_std})

message(STATUS "des_boot: des_game.dll freestanding (DES_SPLIT_FREESTANDING): no C runtime, SDK or C++ library; "
               "C library calls go to the host (docs/84)")
