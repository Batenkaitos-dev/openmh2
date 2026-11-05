# The launcher's headless test (development builds): mh2 runs each case's script of key presses and mouse clicks
# (--launcher-script) on SDL's dummy video driver and software renderer; every `shot` line is the hash of the picture read back
# at that point, and the mh2.ini the launcher wrote (or did not) is compared with the one the case expects.
#
#   cmake -DEXE=mh2 -DSRC=pc/host/tests/launcher -DWORK=DIR "-DCASES=a b" [-DUPDATE=1] -P launcher_test.cmake
#
# A case NAME is SRC/NAME.script, SRC/NAME.ini.in (the file before; none: no file), SRC/NAME.args (extra options, one a
# line), SRC/NAME.out (the lines "launcher ..." mh2 prints) and SRC/NAME.ini.out (the file after, or the word "(no file)").
# UPDATE=1 writes the two expectations from the run (the author's, to be read before they are committed).
#
# Every case runs with its own data folder, cards folder (one card in it, unless SRC/NAME.nocards exists) and the fake disc
# archive in SRC/ready (unless its args name --set archive=...). @SRC@, @FIX@ and @WORKDIR@ in the args and the script are the test's
# folder, its fixtures' and the case's. SRC/NAME.check lists the files the case must leave: "exists REL", "absent REL" or "text REL EXPECTED" (REL under the case's
# folder; the file holds exactly EXPECTED). The cases that make an archive run the fake tools in SRC/tree with the Python named by -DPYTHON= (SRC/NAME.python
# marks them; without a Python they are skipped). SRC/NAME.holder (with .python): a live process holds the card Mcd001.ps2's lock while the case runs.
# SRC/NAME.orig: MH2_ORIG_DIR is the fixtures' disc folder for it.
foreach(var EXE SRC WORK CASES FIXTURES_EXE)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "launcher_test: -D${var}= is missing")
  endif()
endforeach()

# The folder that looks like a disc has a name outside the ANSI code page (three katakana, DI SU KU), above the folder that holds the files, as MH2_ORIG_DIR's value
# must arrive as it was set (the host reads its environment and command line as UTF-8: hostfs.h).
set(DISC "ディスク/disc")

# The fixtures that a repository cannot hold (the ignore rules keep disc and card files out): a folder that looks like a disc (a
# SYSTEM.CNF) and three cards, made fresh in WORK.
set(FIX "${WORK}/fixtures")
file(REMOVE_RECURSE "${FIX}")
file(MAKE_DIRECTORY "${FIX}/${DISC}" "${FIX}/cardsfx")
file(WRITE "${FIX}/${DISC}/SYSTEM.CNF" "BOOT2 = fake
")
foreach(n 1 2 3)
  file(WRITE "${FIX}/cardsfx/Mcd00${n}.ps2" "fake card ${n}")
endforeach()

# The fake discs of the archive maker's cases (nothing of the game in them): the folder that looks like a disc holds the disc's files
# under their names too; @FIX@/arc holds the others; the Japanese-named source folder is above the leaf the picture shows.
function(make_fixture)
  execute_process(COMMAND "${FIXTURES_EXE}" ${ARGN} RESULT_VARIABLE frc ERROR_VARIABLE ferr)
  if(NOT frc EQUAL 0)
    message(FATAL_ERROR "launcher_test: the fake disc fixtures (${ARGN}): ${frc} ${ferr}")
  endif()
endfunction()
make_fixture(--folder "${FIX}/${DISC}")
make_fixture(--fixtures "${FIX}/arc")
make_fixture(--fixtures "${FIX}/ソース")

separate_arguments(CASES)
set(failed 0)
foreach(case IN LISTS CASES)
  set(dir "${WORK}/${case}")
  file(REMOVE_RECURSE "${dir}")
  file(MAKE_DIRECTORY "${dir}")
  if(EXISTS "${SRC}/${case}.ini.in")
    configure_file("${SRC}/${case}.ini.in" "${dir}/mh2.ini" COPYONLY)
  endif()
  if(EXISTS "${SRC}/${case}.python" AND NOT PYTHON)
    message(STATUS "launcher_test ${case}: skipped (no Python)")
    continue()
  endif()
  set(args)
  if(EXISTS "${SRC}/${case}.args")
    file(STRINGS "${SRC}/${case}.args" args)
  endif()
  set(argtext "${args}")
  if(NOT argtext MATCHES "cards=")
    list(APPEND args --set "cards=@WORKDIR@/cards")
    file(MAKE_DIRECTORY "${dir}/cards")
    if(NOT EXISTS "${SRC}/${case}.nocards")
      file(WRITE "${dir}/cards/Mcd001.ps2" "a card")
    endif()
  endif()
  if(NOT argtext MATCHES "archive=")
    list(APPEND args --set "archive=@SRC@/ready/fake-archive")
  endif()
  if(NOT argtext MATCHES "archive-table")
    list(APPEND args --archive-table "@FIX@/arc/table.txt")
  endif()
  list(APPEND args --data-dir "@WORKDIR@/data")
  if(SHOTS)   # (the author's look at the pictures: -DSHOTS=DIR keeps them as PNGs)
    list(APPEND args --launcher-shots "${SHOTS}/${case}")
    file(MAKE_DIRECTORY "${SHOTS}/${case}")
  endif()
  string(REPLACE "@SRC@" "${SRC}" args "${args}")
  string(REPLACE "@FIX@" "${FIX}" args "${args}")
  string(REPLACE "@WORKDIR@" "${dir}" args "${args}")
  set(WORKDIR "${dir}")
  configure_file("${SRC}/${case}.script" "${dir}/script.txt" @ONLY)
  set(envs)
  if(EXISTS "${SRC}/${case}.orig")
    list(APPEND envs "MH2_ORIG_DIR=${FIX}/${DISC}")
  endif()
  if(EXISTS "${SRC}/${case}.holder")   # a live process that holds the card's lock (holder.py), until the case is over
    execute_process(COMMAND "${PYTHON}" "${SRC}/holder.py" "${dir}/cards/Mcd001.ps2.lock" "${dir}/holder.done"
                    RESULT_VARIABLE hrc OUTPUT_QUIET ERROR_QUIET TIMEOUT 30)
    if(NOT hrc EQUAL 0)
      message(FATAL_ERROR "launcher_test ${case}: the lock's holder did not start (${hrc})")
    endif()
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=dummy MH2_LAUNCHER_SOFTWARE=1 ${envs}
            "${EXE}" --config "${dir}/mh2.ini" --launcher-script "${dir}/script.txt" ${args}
    WORKING_DIRECTORY "${FIX}/arc"   # (a source named by its leaf only: the failures that name it read the same in every folder)
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc TIMEOUT 120)
  if(EXISTS "${SRC}/${case}.holder")
    file(WRITE "${dir}/holder.done" "")
  endif()
  # the lines the test compares
  string(REPLACE "\r\n" "\n" out "${out}")
  string(REGEX MATCHALL "launcher [^\n]*" lines "${out}")
  list(JOIN lines "\n" got)
  set(got "${got}\n")
  if(EXISTS "${dir}/mh2.ini")
    file(READ "${dir}/mh2.ini" ini_got)
    string(REPLACE "\r\n" "\n" ini_got_cmp "${ini_got}")
  else()
    set(ini_got "(no file)\n")
    set(ini_got_cmp "(no file)\n")
  endif()
  if(UPDATE)
    file(WRITE "${SRC}/${case}.out" "${got}")
    file(WRITE "${SRC}/${case}.ini.out" "${ini_got}")
    message(STATUS "launcher_test ${case}: expectations written (${rc})")
    continue()
  endif()
  file(READ "${SRC}/${case}.out" want)
  file(READ "${SRC}/${case}.ini.out" ini_want)
  string(REPLACE "\r\n" "\n" want "${want}")
  string(REPLACE "\r\n" "\n" ini_want_cmp "${ini_want}")
  set(ok 1)
  if(NOT rc EQUAL 0)
    set(ok 0)
    message(STATUS "launcher_test ${case}: mh2 ended with ${rc}\n${err}")
  endif()
  if(NOT got STREQUAL want)
    set(ok 0)
    message(STATUS "launcher_test ${case}: the pictures or results differ\n--- expected\n${want}--- got\n${got}")
  endif()
  if(NOT ini_got_cmp STREQUAL ini_want_cmp)
    set(ok 0)
    message(STATUS "launcher_test ${case}: mh2.ini differs\n--- expected\n${ini_want}--- got\n${ini_got}")
  endif()
  if(EXISTS "${SRC}/${case}.check")
    file(STRINGS "${SRC}/${case}.check" checks ENCODING UTF-8)
    foreach(chk IN LISTS checks)
      string(REGEX MATCH "^(exists|absent|text) (.*)$" m "${chk}")
      if(CMAKE_MATCH_1 STREQUAL "text")   # "text REL EXPECTED": the file holds exactly EXPECTED (@FIX@ and @WORKDIR@ replaced)
        string(REGEX MATCH "^([^ ]*) (.*)$" m2 "${CMAKE_MATCH_2}")
        set(rel "${CMAKE_MATCH_1}")
        set(want_text "${CMAKE_MATCH_2}")
        string(REPLACE "@FIX@" "${FIX}" want_text "${want_text}")
        string(REPLACE "@WORKDIR@" "${dir}" want_text "${want_text}")
        set(have_text "(no file)")
        if(EXISTS "${dir}/${rel}")
          file(READ "${dir}/${rel}" have_text)
        endif()
        if(NOT have_text STREQUAL want_text)
          set(ok 0)
          message(STATUS "launcher_test ${case}: ${rel} holds
${have_text}
not
${want_text}")
        endif()
      elseif(CMAKE_MATCH_1 STREQUAL "exists" AND NOT EXISTS "${dir}/${CMAKE_MATCH_2}")
        set(ok 0)
        message(STATUS "launcher_test ${case}: ${CMAKE_MATCH_2} should be there")
      elseif(CMAKE_MATCH_1 STREQUAL "absent" AND EXISTS "${dir}/${CMAKE_MATCH_2}")
        set(ok 0)
        message(STATUS "launcher_test ${case}: ${CMAKE_MATCH_2} should not be there")
      endif()
    endforeach()
  endif()
  if(ok)
    message(STATUS "launcher_test ${case}: ok")
  else()
    math(EXPR failed "${failed} + 1")
  endif()
endforeach()
if(failed)
  message(FATAL_ERROR "launcher_test: ${failed} case(s) failed")
endif()
