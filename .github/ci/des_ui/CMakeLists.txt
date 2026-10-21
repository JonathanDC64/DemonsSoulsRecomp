# The wrapper UI (port/ui) built on its own, for CI (.github/workflows/ci.yml). des_boot needs the lift,
# which CI never has; the UI library does not. Same standards as port/CMakeLists.txt.
cmake_minimum_required(VERSION 3.20)
project(des_ui_ci LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Off Windows the UI draws through D3D12 on vkd3d in an SDL window (docs/76): found here the way
# port/CMakeLists.txt finds them for des_boot.
if(NOT WIN32)
  find_package(SDL2 REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DES_VKD3D_LIBS REQUIRED IMPORTED_TARGET libvkd3d libvkd3d-utils)
endif()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../port/ui" des_ui)
