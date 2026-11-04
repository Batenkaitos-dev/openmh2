# cmake -DEXE=<path> -DARGS=<list> -DEXPECT=<regex> -P expect_error.cmake: runs EXE, which must end
# with an error code (not 0) and print a line matching EXPECT. The host's test of a fatal error in an
# unattended run (host_fatal): logged, an error code, no modal box to wait for.
execute_process(COMMAND "${EXE}" ${ARGS} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out
                TIMEOUT 60)
message("${_out}")
if(_rc EQUAL 0 OR NOT _rc MATCHES "^[0-9]+$")
  message(FATAL_ERROR "expected an error code, got: ${_rc}")
endif()
if(NOT _out MATCHES "${EXPECT}")
  message(FATAL_ERROR "no line matches: ${EXPECT}")
endif()
message("exit code ${_rc}, as expected")
