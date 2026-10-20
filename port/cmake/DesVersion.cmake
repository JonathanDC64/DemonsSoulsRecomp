# The port's version, the ps3recomp it is built with and the game dump it is built from (docs/54), written into
# des_version.h (for the UI, the reports and the log) and des_version.rc (the exe's version resource) on every build,
# so a commit or a local change shows without configuring again. The files are rewritten only when their text
# changes, so an unchanged version recompiles nothing.
#
#   Port version: VERSION holds the release being worked toward (or just made). The commit tagged v<VERSION> shows
#   exactly that ("0.1.0"). Before the tag exists, a build shows "0.1.0-dev.<commits>+g<commit>"; after it, until
#   VERSION moves on, "0.1.0+<commits since the tag>.g<commit>". Changes to tracked files add ".dirty". Without git
#   (an exported release), release-info.txt next to VERSION says it.
#   ps3recomp: git describe of PS3RECOMP_DIR against the fork's release tags (des-*), else the commit; without git,
#   the copy of ps3recomp.pin the release packaging puts in the folder.
#   Game dump: the SHA-256 of DES_DUMP_ELF (work/game/EBOOT.ELF, the decrypted executable the lift is made from),
#   hashed at configure time; changing the file configures again.
#
# Included from port/CMakeLists.txt (and from port/ui/CMakeLists.txt when the UI is built alone): sets
# DES_VERSION_DIR (the generated files' folder) and DES_VERSION_RC, and adds the target des_version that rewrites them.
# Run with cmake -P (the build step), it writes the files from the -D values below.

function(_des_git dir out)
  set(${out} "" PARENT_SCOPE)
  if(NOT GIT_EXECUTABLE OR NOT EXISTS "${dir}/.git")
    return()
  endif()
  # --no-optional-locks: reading only; git status and describe would otherwise rewrite the index.
  execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks -C "${dir}" ${ARGN}
    OUTPUT_VARIABLE _o RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_rc EQUAL 0)
    set(${out} "${_o}" PARENT_SCOPE)
  endif()
endfunction()

# KEY=VALUE lines of a small text file (release-info.txt, ps3recomp.pin) into <prefix>_<KEY>.
function(_des_read_keys file prefix)
  foreach(_k VERSION COMMIT TAG)
    set(${prefix}_${_k} "" PARENT_SCOPE)
  endforeach()
  if(EXISTS "${file}")
    file(STRINGS "${file}" _lines REGEX "^[ \t]*(version|commit|tag)[ \t]*=")
    foreach(_line IN LISTS _lines)
      if(_line MATCHES "^[ \t]*(version|commit|tag)[ \t]*=[ \t]*([^ \t\r]*)")
        string(TOUPPER "${CMAKE_MATCH_1}" _k)
        set(${prefix}_${_k} "${CMAKE_MATCH_2}" PARENT_SCOPE)
      endif()
    endforeach()
  endif()
endfunction()

function(_des_write_if_changed file text)
  if(EXISTS "${file}")
    file(READ "${file}" _old)
    if(_old STREQUAL text)
      return()
    endif()
  endif()
  file(WRITE "${file}" "${text}")
endfunction()

# Needs DES_SOURCE_ROOT, PS3RECOMP_DIR, DES_VERSION_DIR, DES_DUMP_SHA256 and GIT_EXECUTABLE (may be empty).
function(des_version_write)
  set(root "${DES_SOURCE_ROOT}")

  # ---- The port ----
  set(base "0.0.0")
  if(EXISTS "${root}/VERSION")
    file(STRINGS "${root}/VERSION" _v LIMIT_COUNT 1)
    string(STRIP "${_v}" _v)
    if(_v MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$")
      set(base "${_v}")
    else()
      message(WARNING "VERSION: \"${_v}\" is not a version (x.y.z or x.y.z-pre); using ${base}")
    endif()
  endif()
  set(n "")
  set(commit "")
  set(release OFF)
  _des_git("${root}" commit rev-parse HEAD)
  if(commit)
    _des_git("${root}" short rev-parse --short=9 HEAD)
    _des_git("${root}" dirty status --porcelain --untracked-files=no)
    _des_git("${root}" shallow rev-parse --is-shallow-repository)
    _des_git("${root}" tagged rev-parse -q --verify "refs/tags/v${base}^{commit}")
    if(tagged STREQUAL commit)
      set(version "${base}")
      set(release ON)
    elseif(tagged)
      _des_git("${root}" n rev-list --count "v${base}..HEAD")
      set(version "${base}+${n}.g${short}")
    else()
      _des_git("${root}" last describe --tags --abbrev=0 --match "v[0-9]*" HEAD)
      if(shallow STREQUAL "true")
        set(n "")
      elseif(last)
        _des_git("${root}" n rev-list --count "${last}..HEAD")
      else()
        _des_git("${root}" n rev-list --count HEAD)
      endif()
      if(base MATCHES "-")
        set(version "${base}.dev")
      else()
        set(version "${base}-dev")
      endif()
      if(NOT n STREQUAL "")
        string(APPEND version ".${n}")
      endif()
      string(APPEND version "+g${short}")
    endif()
    if(dirty)
      if(version MATCHES "\\+")
        string(APPEND version ".dirty")
      else()
        string(APPEND version "+dirty")
      endif()
      set(release OFF)
    endif()
  else()
    _des_read_keys("${root}/release-info.txt" _rel)
    if(_rel_VERSION)
      set(version "${_rel_VERSION}")
      set(commit "${_rel_COMMIT}")
      set(release ON)
    else()
      set(version "${base}-dev+unknown")
    endif()
  endif()

  # ---- ps3recomp ----
  set(ps3r "unknown")
  set(ps3r_commit "")
  _des_git("${PS3RECOMP_DIR}" ps3r_commit rev-parse HEAD)
  if(ps3r_commit)
    _des_git("${PS3RECOMP_DIR}" ps3r describe --tags --match "des-*" --always --abbrev=12 --dirty=-dirty)
  else()
    _des_read_keys("${PS3RECOMP_DIR}/ps3recomp.pin" _pin)
    if(_pin_COMMIT MATCHES "^[0-9a-f]+$")
      set(ps3r_commit "${_pin_COMMIT}")
      if(_pin_TAG AND NOT _pin_TAG STREQUAL "none")
        set(ps3r "${_pin_TAG}")
      else()
        string(SUBSTRING "${_pin_COMMIT}" 0 12 ps3r)
      endif()
    endif()
  endif()

  # ---- The exe's version resource: x.y.z.<commits> (0 for a release) ----
  string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${base}")
  set(fv "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3}")
  if(release OR NOT n MATCHES "^[0-9]+$")
    set(n 0)
  elseif(n GREATER 65535)
    set(n 65535)
  endif()
  if(release)
    set(flags 0x0)
  else()
    set(flags 0x2)  # VS_FF_PRERELEASE
  endif()
  set(dump "${DES_DUMP_SHA256}")
  set(dump_text "${dump}")
  if(NOT dump)
    set(dump_text "unknown")
  endif()

  _des_write_if_changed("${DES_VERSION_DIR}/des_version.h"
"/* Generated by port/cmake/DesVersion.cmake on every build. Do not edit. */
#ifndef DES_VERSION_H
#define DES_VERSION_H
#define DES_VERSION \"${version}\"
#define DES_VERSION_COMMIT \"${commit}\"
#define DES_PS3RECOMP_VERSION \"${ps3r}\"
#define DES_PS3RECOMP_COMMIT \"${ps3r_commit}\"
#define DES_DUMP_SHA256 \"${dump}\"
#endif
")
  _des_write_if_changed("${DES_VERSION_DIR}/des_version.rc"
"// Generated by port/cmake/DesVersion.cmake on every build. Do not edit.
1 VERSIONINFO
FILEVERSION ${fv},${n}
PRODUCTVERSION ${fv},${n}
FILEFLAGSMASK 0x3F
FILEFLAGS ${flags}
FILEOS 0x40004
FILETYPE 0x1
FILESUBTYPE 0x0
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904B0\"
    BEGIN
      VALUE \"FileDescription\", \"DemonsSoulsRecomp\"
      VALUE \"FileVersion\", \"${version}\"
      VALUE \"InternalName\", \"des_boot\"
      VALUE \"OriginalFilename\", \"des_boot.exe\"
      VALUE \"ProductName\", \"DemonsSoulsRecomp\"
      VALUE \"ProductVersion\", \"${version}\"
      VALUE \"Comments\", \"ps3recomp ${ps3r}; game dump SHA-256 ${dump_text}\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  des_version_write()
  return()
endif()

# ---- Configure time ----
if(TARGET des_version)
  return()  # (included again, by port/ui under port/CMakeLists.txt: the variables are already set)
endif()
get_filename_component(DES_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT PS3RECOMP_DIR)
  set(PS3RECOMP_DIR "${DES_SOURCE_ROOT}/ps3recomp")
endif()
set(DES_DUMP_ELF "${DES_SOURCE_ROOT}/work/game/EBOOT.ELF" CACHE FILEPATH
  "the decrypted game executable the lift is made from; its SHA-256 is shown as the game dump")
set(DES_DUMP_SHA256 "")
if(EXISTS "${DES_DUMP_ELF}")
  file(SHA256 "${DES_DUMP_ELF}" DES_DUMP_SHA256)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DES_DUMP_ELF}")
endif()
find_package(Git QUIET)
set(DES_VERSION_DIR "${CMAKE_BINARY_DIR}/des_version")
set(DES_VERSION_RC "${DES_VERSION_DIR}/des_version.rc")
file(MAKE_DIRECTORY "${DES_VERSION_DIR}")
des_version_write()  # (so the files exist before the first build)
add_custom_target(des_version
  COMMAND "${CMAKE_COMMAND}" "-DDES_SOURCE_ROOT=${DES_SOURCE_ROOT}" "-DPS3RECOMP_DIR=${PS3RECOMP_DIR}"
          "-DDES_VERSION_DIR=${DES_VERSION_DIR}" "-DDES_DUMP_SHA256=${DES_DUMP_SHA256}"
          "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" -P "${CMAKE_CURRENT_LIST_FILE}"
  BYPRODUCTS "${DES_VERSION_DIR}/des_version.h" "${DES_VERSION_RC}"
  COMMENT "Version: port, ps3recomp and game dump"
  VERBATIM)
file(STRINGS "${DES_VERSION_DIR}/des_version.h" _des_version_line REGEX "define DES_VERSION ")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" _des_version_line "${_des_version_line}")
message(STATUS "des_boot: version ${_des_version_line}")
