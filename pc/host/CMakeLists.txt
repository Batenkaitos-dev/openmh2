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

# The disc archive maker's built-in tables (arcmake.c), from config/disc_layout.txt and config/disc.sha1.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/arc_tables.cmake")
mh2_arc_tables("${CMAKE_CURRENT_BINARY_DIR}/arc_tables.inc" "${MH2_ROOT}")

add_executable(mh2 main.c hostfs.c core_args.c process.c gs.c present.c input.c keyboard.c states.c freecam.c audio.c movie.c settings.c launcher.c launcher_make.c arcmake.c sha1.c movietable.c movielib.c launcher_ui.c launcher_stb.c launcher_font.c launcher_text.c play.c cards.c crash.c
                   "${MH2_ROOT}/pc/ipc/mh2ipc_os_${MH2_PLATFORM}.c")
  # the release host: without the development-only features
target_include_directories(mh2 PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "${MH2_ROOT}/pc")
# stb_truetype's code is the library's (public domain, unmodified): compiled apart from the host's own warnings. The font is
# embedded with C23's #embed.
set_source_files_properties(launcher_stb.c PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(launcher_font.c PROPERTIES COMPILE_OPTIONS "-std=c23")
set_property(SOURCE launcher_font.c APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fonts/NotoSans-Regular.ttf")
target_compile_options(mh2 PRIVATE -Wall -Wextra -Werror)
# The guard (hostfs.h): every file of mh2 is compiled with hostfs.h first, which makes the C library's ANSI names (fopen, remove,
# rename, getenv) an error; a file opens through hfs_fopen and the others. Only hostfs.c, which implements them, is let alone.
target_compile_options(mh2 PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/hostfs.h")
set_source_files_properties(hostfs.c PROPERTIES COMPILE_DEFINITIONS MH2_HOSTFS_IMPL)
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
    "line 4: no button .bogus.*line 5: .nonsense. is not a gamepad button name.*line 9: ssaa is 1 [(]off[)], 2, 4, 8 or 16: ignored.*line 11: expected key = value.*line 12: .orig. .the original files. is the original.s.*line [0-9]+: .Nonsense. is not a key name.*unknown setting .usb_key_bogus.*line [0-9]+: .Nonsensestate. is not a key name.*enhancements: ssaa 4, hires on.*textures: dump on, packs on, folder /tmp/mh2-settings-test-textures.*USB keyboard: connected [(]F9 switches between typing.*widescreen: on [(]the camera and the 2D picture are 16:9.*save states: folder /tmp/mh2-settings-test-states; F5 saves, End loads, none and PageUp choose the slot.*mh2-core ended [(]0[)] after 2 VSyncs")
  # the enhancements' options: --ssaa N alone, --hires (implies ssaa 4), --ssaa 1 over hires = on; off says nothing
  host_add_exe_test(host_ssaa mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" --ssaa 8
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_ssaa PROPERTIES PASS_REGULAR_EXPRESSION "enhancements: ssaa 8, hires off")
  host_add_exe_test(host_hires mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" --hires --ssaa 2
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_hires PROPERTIES PASS_REGULAR_EXPRESSION "enhancements: ssaa 4, hires on")
  # the textures' options: each alone, and what the usage names
  host_add_exe_test(host_texture_dump mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" --texture-dump --textures /tmp/mh2-test-textures
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_texture_dump PROPERTIES PASS_REGULAR_EXPRESSION
    "textures: dump on, packs off, folder /tmp/mh2-test-textures.*mh2-core ended [(]0[)]")
  host_add_exe_test(host_texture_packs mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" --texture-packs
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_texture_packs PROPERTIES PASS_REGULAR_EXPRESSION "textures: dump off, packs on")
  host_add_exe_test(host_help mh2 --help)
  set_tests_properties(host_help PROPERTIES PASS_REGULAR_EXPRESSION
    "--texture-dump.*--texture-packs.*--textures FOLDER.*--usb-keyboard")
  # widescreen: the setting reaches the core as --widescreen
  host_add_exe_test(host_widescreen mh2 --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
                    --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc" --widescreen
                    --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_widescreen PROPERTIES PASS_REGULAR_EXPRESSION "widescreen: on [(]the camera and the 2D picture are 16:9")
  # an automated run never shows the launcher, whatever else it is told: on SDL's dummy video driver a launcher that
  # opened would wait for a key for ever (the test's timeout), and its window would be a line in the log
  add_test(NAME host_launcher_guard COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=dummy MH2_LAUNCHER_SOFTWARE=1
           "$<TARGET_FILE:mh2>" --launcher --config "${CMAKE_CURRENT_BINARY_DIR}/test-empty.ini"
           --archive "${CMAKE_CURRENT_BINARY_DIR}/test-disc.arc"
           --no-window --gs-off --mute --no-card --quit-after 2 --core "${_fake}" ${_dirs})
  set_tests_properties(host_launcher_guard PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "mh2-core ended [(]0[)] after 2 VSyncs")
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

# The PC keyboard as the game's USB keyboard (keyboard.c): the mapping, the typing toggle, the bindings, the ring.
add_executable(host_keyboard_test tests/keyboard_test.c keyboard.c)
target_include_directories(host_keyboard_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${MH2_ROOT}/pc")
target_compile_options(host_keyboard_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_keyboard_test PRIVATE SDL3::SDL3)
host_add_exe_test(host_keyboard host_keyboard_test)

# When mh2 adds --local-date to the core's arguments (core_args.c): play only.
add_executable(host_core_args_test tests/core_args_test.c core_args.c)
target_include_directories(host_core_args_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
host_add_exe_test(host_core_args host_core_args_test)

# mh2's files and environment with names outside the ANSI code page (hostfs.c): the word on the command line is three katakana,
# a folder, files, a variable and a renamed file carry it (on Windows through the wide calls).
add_executable(host_hostfs_test tests/hostfs_test.c hostfs.c)
target_include_directories(host_hostfs_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${MH2_ROOT}/pc")
target_compile_options(host_hostfs_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_hostfs_test PRIVATE SDL3::SDL3)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/hostfs-work")
host_add_exe_test(host_hostfs host_hostfs_test "${CMAKE_CURRENT_BINARY_DIR}/hostfs-work" "テスト")

# The disc archive maker (arcmake.c, sha1.c, movietable.c) on fake discs made by the test itself, with nothing of the game in them; the
# same program makes the launcher test's fake discs (tests/launcher_test.cmake runs it with --fixtures).

# The settings table's writer (settings.c, the launcher's save): the file's other lines stay, a CRLF file stays CRLF, a value
# the command line gave is not saved over. settings.c is compiled into the test with stubs for the rest of the host.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/settings-writer")
add_executable(host_settings_writer_test tests/settings_writer_test.c settings.c hostfs.c launcher_text.c)
target_compile_definitions(host_settings_writer_test PRIVATE MH2_SETTINGS_TEST=1)
  target_compile_definitions(host_settings_writer_test PRIVATE MH2_RELEASE=1)
target_include_directories(host_settings_writer_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${MH2_ROOT}/pc")
target_compile_options(host_settings_writer_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_settings_writer_test PRIVATE SDL3::SDL3)
host_add_exe_test(host_settings_writer host_settings_writer_test "${CMAKE_CURRENT_BINARY_DIR}/settings-writer")

# The launcher's headless test (development builds): each case is a script of key presses and mouse clicks that mh2 runs on SDL's
# dummy video driver and software renderer; the pictures are read back to hashes, and the mh2.ini it wrote is compared.

# When the launcher appears (launcher.c): never in an automated run, never with --no-launcher, only with an archive. launcher.c
# and settings.c are compiled into the test with stubs for the window.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher-wanted")
add_executable(host_launcher_wanted_test tests/launcher_wanted_test.c launcher.c launcher_make.c arcmake.c sha1.c movietable.c movielib.c settings.c hostfs.c launcher_text.c)
target_compile_definitions(host_launcher_wanted_test PRIVATE MH2_SETTINGS_TEST=1)
  target_compile_definitions(host_launcher_wanted_test PRIVATE MH2_RELEASE=1)
target_include_directories(host_launcher_wanted_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "${MH2_ROOT}/pc")
target_compile_options(host_launcher_wanted_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_launcher_wanted_test PRIVATE SDL3::SDL3)
host_add_exe_test(host_launcher_wanted host_launcher_wanted_test "${CMAKE_CURRENT_BINARY_DIR}/launcher-wanted")
