# The host's IPC in mh2-core (pc/core/ipc): host_ipc.c is picked up by pc/core/CMakeLists.txt;
# the operating system's part of the protocol (pc/ipc, shared with the host, mh2) goes into the host seam.
list(APPEND MH2_LANE_HOST_SOURCES "${MH2_ROOT}/pc/ipc/mh2ipc_os_${MH2_PLATFORM}.c")

# The movies' timing table beside the disc archive (tools/pc/movie_table.py):
# written by every Linux build once `python tools/port/make_archive.py` has made the archive and
# the movie library, so that a fresh clone's core finds it; up to date, the step returns at once.
if(MH2_PLATFORM STREQUAL "linux")
  add_custom_target(mh2_movie_table ALL
    COMMAND "${Python3_EXECUTABLE}" "${MH2_ROOT}/tools/pc/movie_table.py" --if-able
            --disc "${MH2_ROOT}/build/port/mh2-disc.arc" --lib "${MH2_ROOT}/build/port"
    COMMENT "The movies' timing table (tools/pc/movie_table.py --if-able)"
    VERBATIM)
endif()
