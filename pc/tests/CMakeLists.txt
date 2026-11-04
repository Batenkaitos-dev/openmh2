# Unit tests of the build itself (run by tools/pc/build.py --check, or ctest).
# The calling-convention test: the game's flags (MH2_FLAGS), once as the game is built and once with the
# i386 default as a control.
add_library(cc_game OBJECT cc_callee.c cc_caller.c)
target_compile_options(cc_game PRIVATE ${MH2_FLAGS} -Wall -Wextra -Werror)
add_library(cc_ctl OBJECT cc_callee.c cc_caller.c)
target_compile_options(cc_ctl PRIVATE ${MH2_HOST_FLAGS} -Wall -Wextra -Werror)

add_executable(mh2_cc_test cc_main.c $<TARGET_OBJECTS:cc_game>)
target_compile_options(mh2_cc_test PRIVATE ${MH2_HOST_FLAGS})
add_executable(mh2_cc_control cc_main.c $<TARGET_OBJECTS:cc_ctl>)
target_compile_options(mh2_cc_control PRIVATE ${MH2_HOST_FLAGS} -DCC_CONTROL)

mh2_add_exe_test(cc_game_nan mh2_cc_test)
mh2_add_exe_test(cc_control_x87_quiets_snan mh2_cc_control)
add_test(NAME cc_game_no_x87 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/no_x87.py" $<TARGET_OBJECTS:cc_game>)

# The bit-field and 8-byte layout rules of this build's compiler (the game's flags, but the host's calling
# convention: the test prints), against MWCC's layout of pc/tests/layout_probe.h (tools/pc/layout.py).
set(MH2_LAYOUT_FLAGS "")
foreach(_f IN LISTS MH2_FLAGS)
  if(NOT _f MATCHES "fdefault-calling-conv")
    list(APPEND MH2_LAYOUT_FLAGS "${_f}")
  endif()
endforeach()
add_executable(mh2_layout_check layout_check.c)
target_include_directories(mh2_layout_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_options(mh2_layout_check PRIVATE ${MH2_LAYOUT_FLAGS} -Wall -Wextra -Werror)
mh2_add_exe_test(layout_probe mh2_layout_check)

# The tests of the IPC (pc/core/ipc/tests)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../core/ipc/tests" lane_ipc)

# Local arrays of 16 bytes or more are 16-aligned in code built with the game's flags (tools/pc/stackalign.py):
# the test is built with them, through the same launcher.
add_executable(mh2_stack_align stack_align.c)
target_compile_options(mh2_stack_align PRIVATE ${MH2_FLAGS} -Wall -Wextra -Werror)
mh2_add_exe_test(stack_align mh2_stack_align)

# The compiler's runtime helpers the game calls by name (pc/core/api/libgcc.c): against the
# PS2's results, through the game's calling convention.
add_executable(mh2_libgcc_helpers libgcc_helpers.c "${CMAKE_CURRENT_LIST_DIR}/../core/api/libgcc.c")
target_include_directories(mh2_libgcc_helpers PRIVATE "${MH2_ROOT}/include" "${CMAKE_CURRENT_LIST_DIR}/../core")
target_compile_options(mh2_libgcc_helpers PRIVATE ${MH2_FLAGS} -Wall -Wextra -Werror)
mh2_add_exe_test(libgcc_helpers mh2_libgcc_helpers)

# The game's string literals are never merged across units (tools/pc/literals.py, in the game's compile
# pipeline): two files compiled as the game's are, read by a test program.
if(MH2_PS2FP)
  add_library(literals_game OBJECT literals_a.c literals_b.c)
  set_target_properties(literals_game PROPERTIES C_COMPILER_LAUNCHER "${Python3_EXECUTABLE};${MH2_ROOT}/tools/pc/ps2fp/ps2fpcc.py")
  target_compile_options(literals_game PRIVATE ${MH2_FLAGS} ${MH2_GAME_FLAGS} -w)
  add_executable(mh2_literals literals_main.c $<TARGET_OBJECTS:literals_game>)
  target_compile_options(mh2_literals PRIVATE ${MH2_FLAGS} -Wall -Wextra -Werror)
  mh2_add_exe_test(literals mh2_literals)
endif()
