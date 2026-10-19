# The wrapper UI: start screen, in-game menu bar and settings (docs/32).
# FFCCEOT-Recomp's UI, itself adapted from Dusklight's (CC0), drawn with
# RmlUi (MIT) and FreeType (FTL) over the game frame through D3D11On12.
#
# RmlUi and FreeType are downloaded at configure time into the build
# directory (FetchContent, not vendored). Everything here is its own static
# library: des_boot only links it, so des_boot's compile flags (and with them
# the lifted chunks' build) do not change.
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

# RmlUi's D3D11 renderer and Win32 platform helpers are sources in its tree.
set(RMLUI_BACKENDS ${rmlui_SOURCE_DIR}/Backends)
add_library(des_rmlui_backend STATIC
  ${RMLUI_BACKENDS}/RmlUi_Renderer_DX11.cpp
  ${RMLUI_BACKENDS}/RmlUi_Platform_Win32.cpp)
target_include_directories(des_rmlui_backend PUBLIC ${RMLUI_BACKENDS})
target_link_libraries(des_rmlui_backend PUBLIC RmlUi::RmlUi PRIVATE d3d11 d3dcompiler dxgi imm32)

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
file(GLOB DES_UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
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
endif()
# PRIVATE: nothing of RmlUi's (include paths, definitions) reaches des_boot's
# compile lines, only its link line.
target_link_libraries(des_ui PRIVATE des_rmlui_backend RmlUi::RmlUi d3d11 d3d12 dxgi dxguid xinput shell32 ole32 user32
  dbghelp windowscodecs uuid)   # crash reports (minidump, stack), report screenshots (PNG through WIC)

# The version shown on the start screen and in About (the git commit at configure time).
find_package(Git QUIET)
set(DES_VERSION "dev")
if(GIT_EXECUTABLE)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DES_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE DES_GIT_RC)
  if(DES_GIT_RC EQUAL 0 AND DES_GIT_REV)
    set(DES_VERSION "${DES_GIT_REV}")
  endif()
endif()
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/prelaunch.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/settings_window.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/report.cpp
  PROPERTIES COMPILE_DEFINITIONS "DES_VERSION=\"${DES_VERSION}\"")
