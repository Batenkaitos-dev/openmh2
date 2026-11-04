# The graphics front's build steps, include()d by pc/core/CMakeLists.txt in its scope.
#
# VU1's microprograms as C: vu1gen.py assembles the
# decomp's src/sub_main/vu1_microcode.vsm (tools/vu1as.py) and compiles it with
# tools/pc/vu1rec.py. The output is derived from the game: build/ only.
set(_gfx_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_gfx_vu1_gen "${MH2_GEN}/gfx/vu1_gen.c")
add_custom_command(OUTPUT "${_gfx_vu1_gen}"
  COMMAND "${Python3_EXECUTABLE}" "${_gfx_dir}/vu1gen.py" "${MH2_ROOT}/src/sub_main/vu1_microcode.vsm" "${_gfx_vu1_gen}"
  DEPENDS "${MH2_ROOT}/src/sub_main/vu1_microcode.vsm" "${_gfx_dir}/vu1gen.py" "${MH2_ROOT}/tools/vu1as.py"
          "${MH2_ROOT}/tools/pc/vu1rec.py"
  COMMENT "Compiling VU1's microprograms to C (vu1rec, from the source)"
  VERBATIM)
list(APPEND MH2_PAL_SOURCES "${_gfx_vu1_gen}")
set_source_files_properties("${_gfx_vu1_gen}" PROPERTIES INCLUDE_DIRECTORIES "${_gfx_dir}")
# The unit tests build the graphics front's files on their own (pc/core/gfx/tests), without the compiled
# microprograms (VU1 runs on its interpreter there).
set(MH2_GFX_SOURCES "${_gfx_dir}/gfx_dma.c" "${_gfx_dir}/gfx_gs.c" "${_gfx_dir}/gfx_gif.c" "${_gfx_dir}/gfx_vif.c"
    "${_gfx_dir}/gfx_vu1.c" "${CMAKE_CURRENT_LIST_DIR}/../api/libdma.c" "${CMAKE_CURRENT_LIST_DIR}/../api/libgraph.c"
    "${CMAKE_CURRENT_LIST_DIR}/../api/libvu0.c")
