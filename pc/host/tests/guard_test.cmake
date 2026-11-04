# cmake -DEXE=<mh2> -DDISC=<file> -DDATA=<dir> [-DFAKE_HOME=1] -P guard_test.cmake: the guard (main.c).
# Every automated form of an mh2 run (a test's options, a pad script, the core's own arguments)
# that names no cards folder or other folder of its own must stop with its reason
# (exit code 2) before it starts a core, opens a window or touches the user data folder.
#
# FAKE_HOME (Linux): the user data folder is SDL's, $XDG_DATA_HOME/mh2-decomp/mh2; the test points
# XDG_DATA_HOME and HOME into DATA, checks that the stopped runs leave them empty, and then that a run
# of the player's kind (--import-card, which makes the cards folder) does write there: the fake folder
# is the one mh2 would have used. On Windows SDL asks the shell for %APPDATA%, which no variable moves,
# so the test checks the stops only and never runs the player's kind there (the owner's folder).
file(REMOVE_RECURSE "${DATA}")
file(MAKE_DIRECTORY "${DATA}/xdg" "${DATA}/home")
if(FAKE_HOME)
  set(ENV{XDG_DATA_HOME} "${DATA}/xdg")
  set(ENV{HOME} "${DATA}/home")
endif()
file(WRITE "${DATA}/route.txt" "CROSS@100\n")

# each case: the option the guard must name, ">", then the arguments, separated by |
if(RELEASE)   # the release host: a cards folder is all a test names
  set(_cases
    "--no-window>--no-window|--gs-off|--quit-after|2"
    "--memcard1>--memcard1|${DATA}/c.ps2"
    "--archive>--archive|${DISC}"
    "the core's own arguments>--|--frames|10")
endif()
set(_n 0)
foreach(_case IN LISTS _cases)
  string(FIND "${_case}" ">" _gt)
  string(SUBSTRING "${_case}" 0 ${_gt} _trig)
  math(EXPR _gt "${_gt} + 1")
  string(SUBSTRING "${_case}" ${_gt} -1 _case)
  string(REPLACE "|" " " _show "${_case}")
  string(REPLACE "|" ";" _args "${_case}")
  execute_process(COMMAND "${EXE}" ${_args}
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out TIMEOUT 30)
  string(FIND "${_out}" "this run is automated (${_trig}" _named)
  if(_named LESS 0)
    message(FATAL_ERROR "mh2 ${_show}: the guard's stop doesn't name ${_trig}:\n${_out}")
  endif()
  if(NOT _rc EQUAL 2 OR NOT _out MATCHES "this run is automated [(][^)]+[)] and names no [a-z ]+ folder of its own")
    message(FATAL_ERROR "mh2 ${_show}: expected the guard's stop (exit code 2), got ${_rc}:\n${_out}")
  endif()
  if(_out MATCHES "mh2 started|mh2-core|cards: |log: ")
    message(FATAL_ERROR "mh2 ${_show}: went on past the guard:\n${_out}")
  endif()
  file(GLOB_RECURSE _made LIST_DIRECTORIES true "${DATA}/xdg/*" "${DATA}/home/*")
  if(_made)
    message(FATAL_ERROR "mh2 ${_show}: made ${_made}")
  endif()
  if(EXISTS "${DATA}/rec" OR EXISTS "${DATA}/cards")
    message(FATAL_ERROR "mh2 ${_show}: made a named folder before stopping")
  endif()
  math(EXPR _n "${_n} + 1")
endforeach()
message("${_n} automated runs without folders of their own: each stopped, nothing made")

if(FAKE_HOME)
  execute_process(COMMAND "${EXE}" --import-card "${DATA}/missing.ps2"
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out TIMEOUT 30)
  if(NOT IS_DIRECTORY "${DATA}/xdg/mh2-decomp/mh2/cards")
    message(FATAL_ERROR "the control (--import-card, the player's kind) made no cards folder in the fake "
                        "user data folder (${_rc}):\n${_out}")
  endif()
  message("control: --import-card made ${DATA}/xdg/mh2-decomp/mh2/cards (the fake user data folder is mh2's)")
endif()
file(REMOVE_RECURSE "${DATA}")
