# cmake -DEXE=<path> [-DARGS=<list>] -P run_copy.cmake: runs a copy of EXE from the temp directory.
# Used by ctest when the build tree is on WSL's file share (MH2_TEST_COPY, pc/core/CMakeLists.txt).
# EXE's exit code is this script's, so a test's SKIP_RETURN_CODE (77: no game data) works through it.
if(NOT EXISTS "${EXE}")
  message(FATAL_ERROR "run_copy: ${EXE} does not exist")
endif()
if(DEFINED ENV{TEMP})
  set(_t "$ENV{TEMP}")
else()
  set(_t "/tmp")
endif()
string(RANDOM LENGTH 10 ALPHABET "abcdefghijklmnopqrstuvwxyz" _r)
set(_d "${_t}/mh2-ctest-${_r}")
file(MAKE_DIRECTORY "${_d}")
get_filename_component(_n "${EXE}" NAME)
file(COPY "${EXE}" DESTINATION "${_d}")
execute_process(COMMAND "${_d}/${_n}" ${ARGS} RESULT_VARIABLE _rc)
file(REMOVE_RECURSE "${_d}")
if(NOT _rc EQUAL 0)
  # A number is the program's exit code; anything else (a crash, a program that didn't start) is
  # execute_process's description of it.
  if(_rc MATCHES "^[0-9]+$" AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    message("${_n} exited with ${_rc}")
    cmake_language(EXIT ${_rc})
  endif()
  message(FATAL_ERROR "${_n} exited with ${_rc}")
endif()
