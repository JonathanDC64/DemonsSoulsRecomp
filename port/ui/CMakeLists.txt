# The wrapper UI: start screen, in-game menu bar and settings (docs/32).
# FFCCEOT-Recomp's UI, itself adapted from Dusklight's (CC0), drawn with
# RmlUi (MIT) and FreeType (FTL) over the game frame: through D3D11On12 on
# Windows, through its own D3D12 renderer elsewhere (docs/76).
#
# RmlUi and FreeType (and off Windows stb) are downloaded at configure time
# into the build directory (FetchContent, not vendored). Everything here is its
# own static library: des_boot only links it, so des_boot's compile flags (and
# with them the lifted chunks' build) do not change.
include(FetchContent)

# ---- FreeType (FTL) ----
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
  set(FT_DISABLE_${dep} ON CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG VER-2-14-3
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(freetype)
if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS freetype)   # what RmlUi looks for
endif()

# ---- RmlUi (MIT) ----
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(RmlUi
  GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
  GIT_TAG 6.3
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(RmlUi)

if(WIN32)
  # RmlUi's D3D11 renderer and Win32 platform helpers are sources in its tree.
  set(RMLUI_BACKENDS ${rmlui_SOURCE_DIR}/Backends)
  add_library(des_rmlui_backend STATIC
    ${RMLUI_BACKENDS}/RmlUi_Renderer_DX11.cpp
    ${RMLUI_BACKENDS}/RmlUi_Platform_Win32.cpp)
  target_include_directories(des_rmlui_backend PUBLIC ${RMLUI_BACKENDS})
  target_link_libraries(des_rmlui_backend PUBLIC RmlUi::RmlUi PRIVATE d3d11 d3dcompiler dxgi imm32)
else()
  # ---- stb (public domain / MIT): the images and the reports' PNGs ----
  # Headers only (stb_impl.cpp has the code); a commit, as stb has no releases.
  FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d)
  FetchContent_MakeAvailable(stb)
endif()

# Third-party code: no warnings, optimised (the port configures with an empty
# build type, which has no optimisation flags of its own).
foreach(t freetype rmlui_core rmlui_debugger des_rmlui_backend)
  if(TARGET ${t} AND MSVC)
    target_compile_options(${t} PRIVATE /w /O2)
  elseif(TARGET ${t})
    target_compile_options(${t} PRIVATE -w -O2)
  endif()
endforeach()

# ---- Embedded styles and fonts (cmake/EmbedFiles.cmake) ----
set(DES_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB DES_UI_STYLES CONFIGURE_DEPENDS ${DES_UI_DIR}/res/rml/*.rcss)
set(DES_UI_FILES
  ${DES_UI_STYLES}
  ${DES_UI_DIR}/res/fonts/Cinzel.ttf
  ${DES_UI_DIR}/res/fonts/AlegreyaSans-Regular.ttf
  ${DES_UI_DIR}/res/fonts/AlegreyaSans-Bold.ttf
  ${DES_UI_DIR}/res/fonts/MaterialSymbolsRounded-Regular.ttf)
set(DES_UI_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/ui_assets.cpp)
add_custom_command(
  OUTPUT ${DES_UI_ASSETS}
  COMMAND ${CMAKE_COMMAND} -D BASE_DIR=${DES_UI_DIR} -D OUTPUT=${DES_UI_ASSETS} "-DFILES=${DES_UI_FILES}"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EmbedFiles.cmake
  DEPENDS ${DES_UI_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EmbedFiles.cmake
  COMMENT "Embedding the UI's styles and fonts"
  VERBATIM)

# ---- The UI ----
# Each platform's host, mouse and crash reports (docs/76): D3D11On12 and Win32 on Windows, D3D12 and SDL elsewhere.
file(GLOB DES_UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
if(WIN32)
  set(DES_UI_OTHER host_d3d12 render_d3d12 stb_impl mouse_sdl crash_report_posix)
else()
  set(DES_UI_OTHER host mouse crash_report crash_dialog)
endif()
foreach(f ${DES_UI_OTHER})
  list(REMOVE_ITEM DES_UI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/${f}.cpp)
endforeach()
add_library(des_ui STATIC ${DES_UI_SOURCES} ${DES_UI_ASSETS})
target_include_directories(des_ui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(des_ui PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
# Developer builds (docs/55; the option is in port/CMakeLists.txt): the settings window's Developer tab. Off when the
# UI is configured on its own without the option.
if(DES_DEV)
  target_compile_definitions(des_ui PRIVATE DES_DEV=1)
else()
  target_compile_definitions(des_ui PRIVATE DES_DEV=0)
endif()
if(MSVC)
  target_compile_options(des_ui PRIVATE /O2 /EHsc ${DES_DEBUG_FLAGS} /W3 /wd4100)   # DES_DEBUG_FLAGS: port/CMakeLists.txt
else()
  target_compile_options(des_ui PRIVATE -O2)
endif()
# PRIVATE: nothing of RmlUi's (include paths, definitions) reaches des_boot's
# compile lines, only its link line.
if(WIN32)
  target_link_libraries(des_ui PRIVATE des_rmlui_backend RmlUi::RmlUi d3d11 d3d12 dxgi dxguid xinput shell32 ole32 user32
    dbghelp windowscodecs uuid)   # crash reports (minidump, stack), report screenshots (PNG through WIC)
else()
  # SDL2 and vkd3d as port/CMakeLists.txt found them (the window, the pads, D3D12 and D3DCompile).
  if(NOT TARGET PkgConfig::DES_VKD3D_LIBS)
    message(FATAL_ERROR "The wrapper UI off Windows draws through vkd3d: configure with DES_VKD3D=ON (or DES_UI=OFF)")
  endif()
  target_include_directories(des_ui PRIVATE ${stb_SOURCE_DIR})
  if(TARGET SDL2::SDL2)
    target_link_libraries(des_ui PRIVATE SDL2::SDL2)
  else()
    target_include_directories(des_ui PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(des_ui PRIVATE ${SDL2_LIBRARIES})
  endif()
  target_link_libraries(des_ui PRIVATE RmlUi::RmlUi PkgConfig::DES_VKD3D_LIBS)
endif()

# The version shown on the start screen, in About, in reports and in the log: des_version.h, written on every build
# by cmake/DesVersion.cmake (port/CMakeLists.txt includes it; a build of the UI alone includes it here).
if(NOT TARGET des_version)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/DesVersion.cmake)
endif()
target_include_directories(des_ui PRIVATE ${DES_VERSION_DIR})
add_dependencies(des_ui des_version)
