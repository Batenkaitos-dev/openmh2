# Save states (kernel/state.c), include()d by pc/core/CMakeLists.txt once pal and pal_host exist.
#  - zlib, vendored (third_party/zlib), for the compressed sections: its names carry the prefix z_, and it is
#    built with the host's convention like the other C library code;
#  - the host-owned sections: the files of the host seam, and the development layer's own files, keep their
#    writable data in the sections hostbss and hostdat (kernel/hostsec.h), which a state does not hold.
set(_zdir "${CMAKE_CURRENT_LIST_DIR}/../third_party/zlib")
add_library(zlib_state OBJECT
  "${_zdir}/adler32.c" "${_zdir}/crc32.c" "${_zdir}/deflate.c" "${_zdir}/inffast.c" "${_zdir}/inflate.c"
  "${_zdir}/inftrees.c" "${_zdir}/trees.c" "${_zdir}/zutil.c")
target_include_directories(zlib_state PRIVATE "${_zdir}")
target_compile_definitions(zlib_state PRIVATE Z_PREFIX=1)
target_compile_options(zlib_state PRIVATE ${MH2_HOST_FLAGS} -w)
set(MH2_STATE_OBJECTS "$<TARGET_OBJECTS:zlib_state>")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/host_state.c" PROPERTIES
  INCLUDE_DIRECTORIES "${_zdir}" COMPILE_DEFINITIONS "Z_PREFIX=1")
target_compile_options(pal_host PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/hostsec.h")
foreach(_f IN LISTS MH2_PAL_SOURCES)
  if(_f MATCHES "/core/verify/")
    set_property(SOURCE "${_f}" APPEND PROPERTY COMPILE_OPTIONS -include "${CMAKE_CURRENT_LIST_DIR}/hostsec.h")
  endif()
endforeach()
