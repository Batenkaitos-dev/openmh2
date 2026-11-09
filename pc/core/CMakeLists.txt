# mh2-core: the game's C (every file of the matching build, whole) and the
# deterministic part of the platform layer, 32-bit x86..
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "mh2-core is 32-bit (3.1): use pc/cmake/linux-i686.cmake or mingw-i686.cmake as the toolchain file")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include("${MH2_ROOT}/pc/cmake/stackalign.cmake")   # local arrays get 16-byte alignment
if(NOT MH2_PLATFORM)
  set(MH2_PLATFORM linux)
endif()
set(MH2_GEN "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${MH2_GEN}")

# ---- the flags: pc/game_flags.txt, which tools/pc/census.py reads too ----
file(STRINGS "${MH2_ROOT}/pc/game_flags.txt" _lines)
set(_sect "")
foreach(_l IN LISTS _lines)
  string(REGEX REPLACE "#.*$" "" _l "${_l}")
  string(STRIP "${_l}" _l)
  if(_l STREQUAL "")
    continue()
  endif()
  if(_l MATCHES "^\\[([a-z0-9_]+)\\]$")
    set(_sect "${CMAKE_MATCH_1}")
  elseif(_sect STREQUAL "common" OR _sect STREQUAL "i686")
    list(APPEND MH2_FLAGS "SHELL:${_l}")
  elseif(_sect STREQUAL "game")
    list(APPEND MH2_GAME_FLAGS "SHELL:${_l}")
  elseif(_sect STREQUAL "mingw" AND MH2_PLATFORM STREQUAL "windows")
    list(APPEND MH2_FLAGS "SHELL:${_l}")           # not the host-facing file: see MH2_HOST_FLAGS
  endif()
endforeach()
if(MH2_PLATFORM STREQUAL "linux")
  list(APPEND MH2_FLAGS "-malign-double")      # 8-byte types align to 8 inside structs, as on the EE
endif()

# MH2_FPTRACE (build.py, MH2_FPTRACE=1 in the environment): the product trace, a build of its own (the float model notes)
option(MH2_FPTRACE "the product trace" OFF)
if(MH2_FPTRACE)
  list(APPEND MH2_FLAGS -DMH2_FPTRACE=1)
endif()


option(MH2_PS2FP "compile the game's C through tools/pc/ps2fp/ps2fpcc.py (the EE's float, division and shift semantics)" ON)

# ---- the game's files, by module (tools/pc/sources.py) ----
execute_process(COMMAND "${Python3_EXECUTABLE}" "${MH2_ROOT}/tools/pc/sources.py" --cmake "${MH2_GEN}/sources.cmake"
                RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "tools/pc/sources.py failed")
endif()
include("${MH2_GEN}/sources.cmake")
list(LENGTH MH2_BROKEN _nbroken)
if(_nbroken GREATER 0)
  message(STATUS "mh2-core: ${_nbroken} files are left out until fixes arrive (pc/known_broken.txt); their functions are aborting stubs")
endif()

# ---- the data objects with no C definition (tools/pc/data_c.py) ----
# Their definitions, written from the matching build into build/, are one more file of each module;
# the aliases of objects that a C file defines are compiled into that file (-include of a header).
file(GLOB _data_deps "${MH2_ROOT}/config/symbols/*.txt" "${MH2_ROOT}/config/ld/*.ld" "${MH2_ROOT}/build/*.elf"
     "${MH2_ROOT}/build/*.map" "${MH2_ROOT}/config/pc/layout/*.txt" "${MH2_ROOT}/config/pc/sources.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_data_deps} "${MH2_ROOT}/tools/pc/data_c.py"
             "${MH2_ROOT}/pc/core/lib_data.txt" "${MH2_ROOT}/config/pc/data_names.txt")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${MH2_ROOT}/tools/pc/data_c.py" --out "${MH2_GEN}/data" --platform ${MH2_PLATFORM}
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "tools/pc/data_c.py failed")
endif()
string(STRIP "${_out}" _out)
message(STATUS "${_out}")
include("${MH2_GEN}/data/inject.cmake")
# The .bss blocks' layout test: the data files alone, linked with data_c.py's check (every name of a block at
# its PS2 offset); registered once mh2_add_exe_test exists, at the end of this directory
set(_data_files "")
foreach(mod IN LISTS MH2_MODULES)
  list(APPEND _data_files "${MH2_GEN}/data/data_${mod}.c")
endforeach()
add_executable(data_layout_test "${MH2_GEN}/data/layout_check.c" ${_data_files})
target_compile_options(data_layout_test PRIVATE -w)
cmake_language(DEFER CALL mh2_add_exe_test data_layout data_layout_test)
# The alignment check (ctest data_align, both systems): a name 16-aligned on the PS2 is 16-aligned in the core
# (gen/data/align.json, read against the core's symbols)
add_test(NAME data_align COMMAND "${Python3_EXECUTABLE}" "${MH2_ROOT}/tools/pc/data_c.py" --out "${MH2_GEN}/data"
         --platform ${MH2_PLATFORM} --align "$<TARGET_FILE:mh2-core>" --nm "${CMAKE_NM}")
# The standing check of the data (ctest data_cases): every name the C sizes fits the PC object that holds it, as
# one memory with the PS2's, and so does every name's extent in the matching build (an `extern T X[];`). A probe
# per game TU (clang -fsyntax-only), cached in gen/data/probe by its text and its dependencies. Linux only: the
# game's types have the same layout on mingw (tools/pc/layout.py --arch mingw).
if(MH2_PLATFORM STREQUAL "linux")
  add_test(NAME data_cases COMMAND "${Python3_EXECUTABLE}" "${MH2_ROOT}/tools/pc/data_c.py" --out "${MH2_GEN}/data"
           --cases "${CMAKE_BINARY_DIR}" --clang "${CMAKE_C_COMPILER}" -j 10)
  # the generator stops, naming the file, when one of its inputs is missing (a module's ELF, map, linker script,
  # a splitter's data object): never a build with names silently dropped
  add_test(NAME data_inputs COMMAND "${Python3_EXECUTABLE}" "${MH2_ROOT}/tools/pc/data_c.py" --out "${MH2_GEN}/data"
           --check-inputs)
endif()
function(mh2_inject src obj)
  file(RELATIVE_PATH _rel "${MH2_ROOT}" "${src}")
  string(REGEX REPLACE "[^A-Za-z0-9]" "_" _k "${_rel}")
  if(DEFINED MH2_INJECT_${_k})
    set_source_files_properties("${obj}" PROPERTIES COMPILE_OPTIONS "-include;${MH2_INJECT_${_k}}")
  endif()
endfunction()

set(MH2_INCLUDES "${MH2_ROOT}/include" "${MH2_ROOT}/build/gen" "${MH2_ROOT}/src")
set(MH2_OBJECTS "")
foreach(mod IN LISTS MH2_MODULES)
  set(_srcs "")
  foreach(_s IN LISTS MH2_SRC_${mod})
    file(RELATIVE_PATH _rel "${MH2_ROOT}" "${_s}")
    if(_rel IN_LIST MH2_TEXT_SOURCES)
      # UTF-8 text in the file: the game's Shift-JIS bytes as escapes (sjis_c.py)
      get_filename_component(_n "${_s}" NAME)
      set(_o "${MH2_GEN}/sjis/${mod}_${_n}")
      add_custom_command(OUTPUT "${_o}"
        COMMAND "${Python3_EXECUTABLE}" "${MH2_ROOT}/tools/pc/sjis_c.py" "${_s}" "${_o}"
        DEPENDS "${_s}" "${MH2_ROOT}/tools/pc/sjis_c.py" VERBATIM)
      list(APPEND _srcs "${_o}")
      mh2_inject("${_s}" "${_o}")
    else()
      list(APPEND _srcs "${_s}")
      mh2_inject("${_s}" "${_s}")
    endif()
  endforeach()
  list(APPEND _srcs "${MH2_GEN}/data/data_${mod}.c")      # the module's objects with no C definition
  add_library(game_${mod} OBJECT ${_srcs})
  if(MH2_PS2FP)
    # The game's C only, through the EE's float semantics: never the platform layer or ps2crt
    set_target_properties(game_${mod} PROPERTIES C_COMPILER_LAUNCHER "${Python3_EXECUTABLE};${MH2_ROOT}/tools/pc/ps2fp/ps2fpcc.py")
  endif()
  target_include_directories(game_${mod} PRIVATE ${MH2_INCLUDES})
  # The census (tools/pc/census.py) counts the game's warnings against a baseline; the build shows errors.
  target_compile_options(game_${mod} PRIVATE ${MH2_FLAGS} ${MH2_GAME_FLAGS} -w)
  list(APPEND MH2_OBJECTS "$<TARGET_OBJECTS:game_${mod}>")
endforeach()

# ---- the platform layer ----
set(MH2_PAL_SOURCES main.c pal_mem.c pal_overlay.c pal_camera.c pal_fpmodel.c pal_hud.c pal_freecam.c pal_text.c pal_hub.c)
if(MH2_FPTRACE)   # the product trace (pal_fptrace.c; PS2FP_SITES names the site list the pass writes)
  list(APPEND MH2_PAL_SOURCES pal_fptrace.c)
endif()
# Each module owns a directory here, and the library
# functions go in api/, one file per library (3.8). Every .c file in them is built, so none of them edits this
# file: host_*.c files use the system headers and go into pal_host (host_*_linux.c and host_*_windows.c only
# on their platform); every other one goes into pal. A module's own build steps (generated sources, per-file
# properties) go in its directory's sources.cmake, include()d here; its tests in NAME/tests (pc/tests).
# cse: the native sound engine.
set(MH2_LANES api kernel io gfx snd ipc cse)
set(MH2_LANE_HOST_SOURCES "")
foreach(_lane IN LISTS MH2_LANES)
  set(_dir "${CMAKE_CURRENT_LIST_DIR}/${_lane}")
  file(GLOB _lane_c CONFIGURE_DEPENDS "${_dir}/*.c")
  foreach(_f IN LISTS _lane_c)
    get_filename_component(_n "${_f}" NAME)
    if(NOT _n MATCHES "^host_")
      list(APPEND MH2_PAL_SOURCES "${_f}")
    elseif(NOT _n MATCHES "_(linux|windows)\\.c$" OR _n MATCHES "_${MH2_PLATFORM}\\.c$")
      list(APPEND MH2_LANE_HOST_SOURCES "${_f}")
    endif()
  endforeach()
  if(EXISTS "${_dir}/sources.cmake")
    include("${_dir}/sources.cmake")
  endif()
endforeach()
# ps2crt and the VU0 operations : crt/CMakeLists.txt is include()d here, in this scope, and
# sets MH2_CRT_SOURCES (absolute paths). They are compiled with the game's flags (regcall, so a game call
# can only bind to what they define) but never through the ps2fp pass, and their definitions replace the
# generated stubs of the same names.
set(MH2_CRT_SOURCES "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/crt/CMakeLists.txt")
  include("${CMAKE_CURRENT_LIST_DIR}/crt/CMakeLists.txt")
endif()
if(MH2_CRT_SOURCES)
  add_library(crt OBJECT ${MH2_CRT_SOURCES})
  target_include_directories(crt PRIVATE "${MH2_ROOT}/include" "${CMAKE_CURRENT_LIST_DIR}/crt" "${MH2_ROOT}/pc")   # pc/: the headers of the core's other directories
  target_compile_options(crt PRIVATE ${MH2_FLAGS} -Wall -Wextra -Werror -Wno-unused-parameter)
  set(MH2_CRT_OBJECTS "$<TARGET_OBJECTS:crt>")
endif()
add_library(pal OBJECT ${MH2_PAL_SOURCES})
target_include_directories(pal PRIVATE "${MH2_ROOT}/include" "${CMAKE_CURRENT_LIST_DIR}" "${MH2_ROOT}/pc")
mh2_inject("${CMAKE_CURRENT_LIST_DIR}/pal_mem.c" "${CMAKE_CURRENT_LIST_DIR}/pal_mem.c")      # the aliases of the RAM block (D_ADDR names)
target_compile_options(pal PRIVATE ${MH2_FLAGS} -Wall -Wextra -Werror -Wno-unused-parameter)

# The host seam (host.h): the files that include the system headers use the host's own calling
# convention, and the Windows SDK's bit-field layout (-mno-ms-bitfields would break its structs).
# The flags are the same without them.
set(MH2_HOST_FLAGS "")
foreach(_f IN LISTS MH2_FLAGS)
  if(NOT _f MATCHES "fdefault-calling-conv|mno-ms-bitfields")
    list(APPEND MH2_HOST_FLAGS "${_f}")
  endif()
endforeach()
add_library(pal_host OBJECT host_${MH2_PLATFORM}.c ${MH2_LANE_HOST_SOURCES})
if(MH2_FPTRACE)
  target_sources(pal_host PRIVATE host_fptrace.c)
endif()
target_include_directories(pal_host PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${MH2_ROOT}/pc")
target_compile_options(pal_host PRIVATE ${MH2_HOST_FLAGS} -Wall -Wextra -Werror -Wno-unused-parameter)

include("${CMAKE_CURRENT_LIST_DIR}/kernel/state.cmake")   # save states: zlib and the host-owned sections

# ---- the aborting stubs for every library function not written yet ----
file(GENERATE OUTPUT "${MH2_GEN}/objects.rsp" CONTENT "$<JOIN:${MH2_OBJECTS},
>
")
file(GENERATE OUTPUT "${MH2_GEN}/host_objects.rsp" CONTENT "$<JOIN:$<TARGET_OBJECTS:pal>;$<TARGET_OBJECTS:pal_host>;${MH2_CRT_OBJECTS},
>
")
set(_stubs "${MH2_GEN}/stubs.c")
# gen_stubs.py also reads each module's YAML and the symbol files it lists (the calls by PS2 address) and
# pal_overlay.c's OV lines: a change there alone regenerates the stubs (CONFIGURE_DEPENDS: a new file too)
file(GLOB _stubs_cfg CONFIGURE_DEPENDS "${MH2_ROOT}/config/main.yaml" "${MH2_ROOT}/config/overlays/*.yaml"
     "${MH2_ROOT}/config/symbols/*.txt" "${MH2_ROOT}/config/functions/*.txt")
add_custom_command(OUTPUT "${_stubs}"
  COMMAND "${Python3_EXECUTABLE}" "${MH2_ROOT}/tools/pc/gen_stubs.py"
          --objects "@${MH2_GEN}/objects.rsp" --host-objects "@${MH2_GEN}/host_objects.rsp" --names "${CMAKE_CURRENT_LIST_DIR}/api_names.txt"
          --headers "${MH2_ROOT}/include" --out "${_stubs}" --platform ${MH2_PLATFORM}
  DEPENDS ${MH2_OBJECTS} $<TARGET_OBJECTS:pal> $<TARGET_OBJECTS:pal_host> ${MH2_CRT_OBJECTS} "${CMAKE_CURRENT_LIST_DIR}/api_names.txt" "${MH2_ROOT}/tools/pc/gen_stubs.py"
          ${_stubs_cfg} "${CMAKE_CURRENT_LIST_DIR}/pal_overlay.c"
  COMMENT "Generating the stubs of the library functions not written yet"
  VERBATIM)
add_library(stubs OBJECT "${_stubs}")
target_include_directories(stubs PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_options(stubs PRIVATE ${MH2_FLAGS} -w)

add_executable(mh2-core ${MH2_OBJECTS} $<TARGET_OBJECTS:pal> $<TARGET_OBJECTS:pal_host> ${MH2_CRT_OBJECTS} ${MH2_STATE_OBJECTS} $<TARGET_OBJECTS:stubs>)
set_target_properties(mh2-core PROPERTIES LINKER_LANGUAGE C)
if(MH2_PLATFORM STREQUAL "linux")
  target_link_libraries(mh2-core PRIVATE -Wl,-Map=${CMAKE_BINARY_DIR}/mh2-core.map)
endif()
# Tests of the executables. MH2_TEST_COPY (set by the Windows test runner: the build tree is on WSL's file share)
# runs a copy from the Windows temp directory: an executable run in place on that share stays locked,
# cannot be deleted or relinked (Permission denied) even from Windows, and its name is lost for good.
option(MH2_TEST_COPY "run the tests' executables from a copy in the temp directory" OFF)
# The disc archive the tests that need game data read: the checkout's build/port (python tools/port/make_archive.py), or
# $MH2_DISC, which the Windows test runner sets for a build in its copy on the Windows disk (the archive stays on the share).
if(DEFINED ENV{MH2_DISC})
  file(TO_CMAKE_PATH "$ENV{MH2_DISC}" MH2_DISC_ARC)
else()
  set(MH2_DISC_ARC "${MH2_ROOT}/build/port/mh2-disc.arc")
endif()
function(mh2_add_exe_test name exe)
  if(MH2_TEST_COPY)
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:${exe}>" "-DARGS=${ARGN}"
             -P "${MH2_ROOT}/pc/tests/run_copy.cmake")
  else()
    add_test(NAME ${name} COMMAND ${exe} ${ARGN})
  endif()
endfunction()
mh2_add_exe_test(core_startup mh2-core --check)
add_subdirectory(../tests tests)      # after the flags: the unit tests use them
