# mh2movie: the game's movies through FFmpeg (LGPL shared libraries from
# tools/deps/<platform>/ffmpeg, pinned in tools/versions.txt). The bridge
# is CC0; the executable loads it at run time and works without it.
add_library(mh2movie SHARED mh2movie.c)
target_include_directories(mh2movie PRIVATE "${FFMPEG_DIR}/include")
target_compile_options(mh2movie PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(mh2movie PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
foreach(lib avformat avcodec avutil swscale swresample)
    find_library(FFMPEG_${lib}_LIB NAMES ${lib} lib${lib}.dll.a PATHS "${FFMPEG_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(mh2movie PRIVATE ${FFMPEG_${lib}_LIB})
endforeach()

# Place the library and FFmpeg's shared libraries next to the executable.
set_target_properties(mh2movie PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
                                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
    foreach(dll ${FFMPEG_DLLS})
        add_custom_command(TARGET mh2movie POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${CMAKE_BINARY_DIR}")
    endforeach()
else()
        # The release folder carries FFmpeg's libraries (their sonames) beside the library, and the library finds them there.
        set_target_properties(mh2movie PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
        foreach(lib avformat avcodec avutil swscale swresample)
            file(GLOB _so "${FFMPEG_DIR}/lib/lib${lib}.so.*")
            foreach(f ${_so})
                if(f MATCHES "[.]so[.][0-9]+$")
                    get_filename_component(_n "${f}" NAME)
                    add_custom_command(TARGET mh2movie POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${f}" "${CMAKE_BINARY_DIR}/${_n}")
                endif()
            endforeach()
        endforeach()
endif()
