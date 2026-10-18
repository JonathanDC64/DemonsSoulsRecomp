# Compares the ps3recomp checkout (PS3RECOMP_DIR) with ps3recomp.pin, the one commit of the fork this
# revision of the port builds with (docs/54). tools/release/pin_ps3recomp.py makes the same checks by hand
# and changes the pin.
#
#   Dev builds (the default) warn when the checkout does not contain the pinned commit: the port needs
#   ps3recomp changes the checkout lacks. A newer checkout, or one with local changes, is fine.
#   DES_REQUIRE_PIN=ON (release and player builds) stops unless the checkout is exactly the pinned commit
#   with no changes to tracked files. A ps3recomp folder without git (exported by the release packaging)
#   must carry a copy of the pin file whose commit matches.
#
# Sets DES_PIN_REPO, DES_PIN_TAG and DES_PIN_COMMIT (empty when there is no valid pin).
option(DES_REQUIRE_PIN "Stop unless PS3RECOMP_DIR is exactly the commit ps3recomp.pin names" OFF)

get_filename_component(DES_PIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../../ps3recomp.pin" ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DES_PIN_FILE}")
if(DES_REQUIRE_PIN)
  set(_pin_level FATAL_ERROR)
else()
  set(_pin_level WARNING)
endif()

# KEY=VALUE lines; "#" lines are comments.
function(des_read_pin file prefix)
  set(${prefix}_REPO "" PARENT_SCOPE)
  set(${prefix}_TAG "" PARENT_SCOPE)
  set(${prefix}_COMMIT "" PARENT_SCOPE)
  if(NOT EXISTS "${file}")
    return()
  endif()
  file(STRINGS "${file}" _lines REGEX "^[ \t]*(repo|tag|commit)[ \t]*=")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "^[ \t]*(repo|tag|commit)[ \t]*=[ \t]*([^ \t\r]*)")
      string(TOUPPER "${CMAKE_MATCH_1}" _key)
      set(${prefix}_${_key} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    endif()
  endforeach()
endfunction()

des_read_pin("${DES_PIN_FILE}" DES_PIN)
if(NOT EXISTS "${DES_PIN_FILE}")
  message(${_pin_level} "ps3recomp pin: ${DES_PIN_FILE} is missing")
  return()
elseif(NOT DES_PIN_COMMIT MATCHES "^[0-9a-f]+$")
  message(${_pin_level} "ps3recomp pin: no valid commit= line in ${DES_PIN_FILE}")
  set(DES_PIN_COMMIT "")
  return()
endif()
string(SUBSTRING "${DES_PIN_COMMIT}" 0 12 _pin_short)
set(_pin_name "${_pin_short}")
set(_pin_full "${DES_PIN_COMMIT}")
if(DES_PIN_TAG AND NOT DES_PIN_TAG STREQUAL "none")
  set(_pin_name "${DES_PIN_TAG} (${_pin_short})")
  set(_pin_full "${DES_PIN_TAG} (${DES_PIN_COMMIT})")
endif()

find_package(Git QUIET)
if(NOT EXISTS "${PS3RECOMP_DIR}/.git" OR NOT GIT_EXECUTABLE)
  des_read_pin("${PS3RECOMP_DIR}/ps3recomp.pin" _have)
  if(_have_COMMIT STREQUAL DES_PIN_COMMIT)
    message(STATUS "ps3recomp pin: ${_pin_name}, the exported folder ${PS3RECOMP_DIR}")
  elseif(DES_REQUIRE_PIN)
    message(FATAL_ERROR "ps3recomp pin: ${PS3RECOMP_DIR} is not a git checkout and carries no pin file for "
      "${_pin_full}; it is not the ps3recomp this release needs.")
  else()
    message(STATUS "ps3recomp pin: ${_pin_name}; cannot compare ${PS3RECOMP_DIR} (no git checkout)")
  endif()
  return()
endif()

execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PS3RECOMP_DIR}" rev-parse HEAD
  OUTPUT_VARIABLE _pin_head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _pin_rc ERROR_QUIET)
if(NOT _pin_rc EQUAL 0)
  message(${_pin_level} "ps3recomp pin: cannot read the commit of ${PS3RECOMP_DIR}")
  return()
endif()
string(SUBSTRING "${_pin_head}" 0 12 _pin_head_short)

if(DES_REQUIRE_PIN)
  if(NOT _pin_head STREQUAL DES_PIN_COMMIT)
    message(FATAL_ERROR "ps3recomp pin: ${PS3RECOMP_DIR} is at ${_pin_head}, but this build needs exactly "
      "${_pin_full}.")
  endif()
  # (--no-optional-locks: reading only; git status would otherwise rewrite the checkout's index.)
  execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks -C "${PS3RECOMP_DIR}" status --porcelain
    --untracked-files=no
    OUTPUT_VARIABLE _pin_dirty OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _pin_rc ERROR_QUIET)
  if(NOT _pin_rc EQUAL 0 OR _pin_dirty)
    message(FATAL_ERROR "ps3recomp pin: ${PS3RECOMP_DIR} has changes to tracked files:\n${_pin_dirty}")
  endif()
  message(STATUS "ps3recomp pin: ${_pin_name}, exactly")
  return()
endif()

execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PS3RECOMP_DIR}" merge-base --is-ancestor "${DES_PIN_COMMIT}" HEAD
  RESULT_VARIABLE _pin_rc OUTPUT_QUIET ERROR_QUIET)
if(_pin_rc EQUAL 0)
  message(STATUS "ps3recomp pin: ${_pin_name}; ${PS3RECOMP_DIR} is at ${_pin_head_short}, which contains it")
elseif(_pin_rc EQUAL 1)
  message(WARNING "ps3recomp pin: ${PS3RECOMP_DIR} is at ${_pin_head_short}, which does not contain the "
    "commit this port needs (${_pin_name}, ps3recomp.pin). Update it to the fork's master.")
else()
  message(WARNING "ps3recomp pin: ${PS3RECOMP_DIR} does not have the commit this port needs "
    "(${_pin_name}, ps3recomp.pin). Fetch the fork, then update the checkout to its master.")
endif()
