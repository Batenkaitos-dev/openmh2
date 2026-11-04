# The native port's build. tools/pc/build.py runs it.
#
# One source tree, two builds with different toolchains:
#   -DMH2_TARGET=core   mh2-core, the game and the deterministic platform layer, 32-bit x86
#                       (Linux: clang -m32; Windows: llvm-mingw's i686 target)
#   -DMH2_TARGET=host   mh2, the 64-bit host (window, input, sound device, renderer, movies)
cmake_minimum_required(VERSION 3.20)
get_filename_component(MH2_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MH2_TARGET "core" CACHE STRING "core (mh2-core, i686) or host (mh2, x86-64)")
project(mh2 LANGUAGES C)
enable_testing()


if(MH2_TARGET STREQUAL "core")
  add_subdirectory(core)
elseif(MH2_TARGET STREQUAL "host")
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/host/CMakeLists.txt")
    add_subdirectory(host)
  else()
    message(FATAL_ERROR "the host (pc/host) is not written yet")
  endif()
else()
  message(FATAL_ERROR "MH2_TARGET must be core or host")
endif()
