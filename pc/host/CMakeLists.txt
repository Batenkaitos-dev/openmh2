# mh2, the 64-bit host: the window and the presentation,
# the input, the sound device (SDL3), and the GS renderer mh2gs (paraLLEl-GS on Vulkan, built from
# pc/gs/ into its own shared library, LGPL-3.0-or-later, which mh2 loads at run time).
# tools/pc/build.py --target host builds it into build/pc/<platform>-x86_64: mh2 and mh2gs side by side.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "mh2 is 64-bit (3.1): use pc/host/cmake/linux-x86_64.cmake or mingw-x86_64.cmake")
endif()
if(NOT MH2_PLATFORM)
  set(MH2_PLATFORM linux)
endif()
set(_unused "${Python3_EXECUTABLE}")   # build.py passes it for the core
# The tests' executables, as the core's mh2_add_exe_test runs them (pc/core/CMakeLists.txt): under
# MH2_TEST_COPY (winrun: the build tree is on WSL's share) a copy in the temp directory, since one
# run in place there stays locked and its next link fails (Permission denied).
option(MH2_TEST_COPY "run the tests' executables from a copy in the temp directory" OFF)
function(host_add_exe_test name exe)
  if(MH2_TEST_COPY)
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:${exe}>" "-DARGS=${ARGN}"
             -P "${MH2_ROOT}/pc/tests/run_copy.cmake")
  else()
    add_test(NAME ${name} COMMAND ${exe} ${ARGN})
  endif()
endfunction()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED CONFIG)

add_executable(mh2 main.c core_args.c process.c gs.c present.c input.c audio.c movie.c settings.c play.c cards.c crash.c
                   "${MH2_ROOT}/pc/ipc/mh2ipc_os_${MH2_PLATFORM}.c")
  # the release host: without the development-only features
target_include_directories(mh2 PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${MH2_ROOT}/pc")
target_compile_options(mh2 PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mh2 PRIVATE SDL3::SDL3)
if(MH2_PLATFORM STREQUAL "linux")
  target_link_libraries(mh2 PRIVATE m)   # pow, lrintf (movie.c)
endif()
set_target_properties(mh2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MH2_PLATFORM STREQUAL "windows")
  # SDL3.dll beside the executable
  add_custom_command(TARGET mh2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:mh2>)
endif()

# The movies: mh2movie (pc/movie, FFmpeg's LGPL shared libraries), which mh2 loads at run
# time; tools/pc/movie_table.py drives the same library at setup to measure the movies' timing.
# On Windows, FFmpeg is where winrun found the Windows toolchain (the checkout's
# tools/deps/windows-x64, then origin's), beside its llvm-mingw.
if(MH2_PLATFORM STREQUAL "windows")
  set(_ffdefault "${MH2_ROOT}/tools/deps/windows-x64/ffmpeg")
  set(_ffmingw "${MH2_LLVM_MINGW}")
  if(NOT _ffmingw AND DEFINED ENV{MH2_LLVM_MINGW})
    file(TO_CMAKE_PATH "$ENV{MH2_LLVM_MINGW}" _ffmingw)
  endif()
  if(NOT EXISTS "${_ffdefault}/include/libavformat/avformat.h" AND _ffmingw)
    get_filename_component(_ffdefault "${_ffmingw}/../ffmpeg" ABSOLUTE)
  endif()
else()
  set(_ffdefault "${MH2_ROOT}/tools/deps/linux-x64/ffmpeg")
endif()
set(FFMPEG_DIR "${_ffdefault}" CACHE PATH "FFmpeg with headers and shared libraries (tools/setup)")
if(NOT EXISTS "${FFMPEG_DIR}/include/libavformat/avformat.h" AND EXISTS "${_ffdefault}/include/libavformat/avformat.h")
  # a build directory configured before the lookup keeps an unusable path in its cache
  set(FFMPEG_DIR "${_ffdefault}" CACHE PATH "FFmpeg with headers and shared libraries (tools/setup)" FORCE)
endif()
if(EXISTS "${FFMPEG_DIR}/include/libavformat/avformat.h")
  add_subdirectory("${MH2_ROOT}/pc/movie" movie-ffmpeg)
  add_dependencies(mh2 mh2movie)
else()
  message(WARNING "FFmpeg not found in ${FFMPEG_DIR}: mh2 without movies (run tools/setup)")
endif()

# The renderer: paraLLEl-GS (tools/setup fetches it at the pinned commit, with the port's patch).
set(PARALLEL_GS_DIR "${MH2_ROOT}/tools/deps/src/parallel-gs" CACHE PATH "paraLLEl-GS checkout with its submodules")
option(MH2_HOST_GS "Build the paraLLEl-GS renderer (mh2gs)" ON)
if(MH2_HOST_GS)
  if(NOT EXISTS "${PARALLEL_GS_DIR}/gs/gs_interface.hpp")
    message(FATAL_ERROR "paraLLEl-GS not found in ${PARALLEL_GS_DIR}: run tools/setup, "
                        "or configure with -DMH2_HOST_GS=OFF for a host without graphics")
  endif()
  enable_language(CXX)
  add_subdirectory(mh2gs)
  add_dependencies(mh2 mh2gs)
endif()

# The host's check (Linux): mh2 runs mh2-fake-core (the core's build, pc/core/ipc/tests) without a
# window or renderer; the core ends after exactly 120 VSyncs (--quit-after), with status 0. An empty
# settings file (no user's mh2.ini read) and an empty file for the launcher's disc archive (the fake
# core reads none).
# A build with a tag (build.py --tag T) is in <platform>-x86_64-T: its fake core is in <platform>-i686-T (the
# same suffix: a variant or the tag), not in the untagged build, which can be an older one whose
# IPC version the host refuses (the fake core aborts: 0xc0000409 on Windows).
get_filename_component(_hostdir "${CMAKE_BINARY_DIR}" NAME)
string(REGEX REPLACE "^${MH2_PLATFORM}-x86_64" "" _suffix "${_hostdir}")
set(_fake "${CMAKE_BINARY_DIR}/../${MH2_PLATFORM}-i686${_suffix}/core/mh2-fake-core")
# These runs are automated (the guard, main.c): they name their cards folders, here in
# the build tree (made only if a run makes marks or has a card: these don't).
set(_dirs --set "cards=${CMAKE_CURRENT_BINARY_DIR}/test-data/cards")
if(MH2_PLATFORM STREQUAL "linux")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini" "")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" "")
  host_add_exe_test(host_fake_core mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc"
                    --no-window --gs-off --mute --no-card --quit-after 120 --core "${_fake}" ${_dirs})
  # mh2.ini's problems, each reported with its line (tests/settings_test.ini), and the run goes on
  host_add_exe_test(host_settings mh2 --config "${CMAKE_CURRENT_LIST_DIR}/tests/settings_test.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc"
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_settings PROPERTIES PASS_REGULAR_EXPRESSION
    "line 4: no button .bogus.*line 5: .nonsense. is not a gamepad button name.*line 9: ssaa is 1 [(]off[)], 2, 4, 8 or 16: ignored.*line 11: expected key = value.*line 12: .orig. .the original files. is the original.s.*enhancements: ssaa 4, hires on.*widescreen: on [(]the camera and the 2D picture are 16:9.*mh2-core ended [(]0[)] after 2 VSyncs")
  # the enhancements' options: --ssaa N alone, --hires (implies ssaa 4), --ssaa 1 over hires = on; off says nothing
  host_add_exe_test(host_ssaa mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" --ssaa 8
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_ssaa PROPERTIES PASS_REGULAR_EXPRESSION "enhancements: ssaa 8, hires off")
  host_add_exe_test(host_hires mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" --hires --ssaa 2
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_hires PROPERTIES PASS_REGULAR_EXPRESSION "enhancements: ssaa 4, hires on")
  # widescreen: the setting reaches the core as --widescreen
  host_add_exe_test(host_widescreen mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" --widescreen
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_widescreen PROPERTIES PASS_REGULAR_EXPRESSION "widescreen: on [(]the camera and the 2D picture are 16:9")
  add_test(NAME host_ssaa_bad COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:mh2>"
           "-DARGS=--config;${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini;--ssaa;3;--no-window;--gs-off;--quit-after;2;--core;${_fake};${_dirs}"
           "-DEXPECT=usage: mh2"
           -P "${CMAKE_CURRENT_LIST_DIR}/tests/expect_error.cmake")
  set_tests_properties(host_fake_core PROPERTIES PASS_REGULAR_EXPRESSION "mh2-core ended [(]0[)] after 120 VSyncs")
  # the core's fatal error in an unattended run (here --no-window and --quit-after): logged, mh2 exits
  # with an error code (the fake core's own status is 0), and no box waits for a click
  add_test(NAME host_fatal COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:mh2>"
           "-DARGS=--config;${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini;--archive;${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc;--no-window;--gs-off;--mute;--no-card;--quit-after;120;--core;${_fake};${_dirs}"
           "-DEXPECT=mh2-core stopped: the fake core's fatal error"
           -P "${CMAKE_CURRENT_LIST_DIR}/tests/expect_error.cmake")
  set_tests_properties(host_fatal PROPERTIES ENVIRONMENT "MH2_FAKE_FATAL=30" TIMEOUT 90)
  # a card another mh2 holds (cards.c): its lock names a live process (1, init), so mh2 refuses it
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test-locked.ps2" "")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test-locked.ps2.lock" "1\n")
  add_test(NAME host_card_lock COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:mh2>"
           "-DARGS=--config;${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini;--archive;${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc;--no-window;--gs-off;--mute;--no-card;--quit-after;2;--memcard1;${CMAKE_CURRENT_BINARY_DIR}/test-locked.ps2;--core;${_fake};${_dirs}"
           "-DEXPECT=is in use by another mh2 [(]process 1[)]"
           -P "${CMAKE_CURRENT_LIST_DIR}/tests/expect_error.cmake")
endif()


# The guard (main.c): every automated form of a run without folders of its own stops, and makes
# nothing in the user data folder (on Linux a fake one, which a run of the player's kind then uses).
if(MH2_PLATFORM STREQUAL "linux")
  add_test(NAME host_guard COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:mh2>"
           "-DDISC=${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" "-DDATA=${CMAKE_CURRENT_BINARY_DIR}/guard-data"
           -DFAKE_HOME=1 -DRELEASE=1 -P "${CMAKE_CURRENT_LIST_DIR}/tests/guard_test.cmake")
elseif(NOT MH2_TEST_COPY)   # Windows: the stops only; %APPDATA% can't be moved (guard_test.cmake)
  add_test(NAME host_guard COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:mh2>"
           "-DDISC=${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" "-DDATA=${CMAKE_CURRENT_BINARY_DIR}/guard-data"
           -DRELEASE=1 -P "${CMAKE_CURRENT_LIST_DIR}/tests/guard_test.cmake")
endif()

# When mh2 adds --local-date to the core's arguments (core_args.c): play only.
add_executable(host_core_args_test tests/core_args_test.c core_args.c)
target_include_directories(host_core_args_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
host_add_exe_test(host_core_args host_core_args_test)
