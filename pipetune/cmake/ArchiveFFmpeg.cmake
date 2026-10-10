# Ship the exact upstream source tree alongside the replaceable LGPL libraries.
foreach(required GIT_EXECUTABLE XZ_EXECUTABLE FFMPEG_SOURCE FFMPEG_COMMIT FFMPEG_ARCHIVE)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()
set(temporary "${FFMPEG_ARCHIVE}.tar")
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${FFMPEG_SOURCE}"
    -C "${FFMPEG_SOURCE}" archive --format=tar --prefix=ffmpeg/ "${FFMPEG_COMMIT}"
  OUTPUT_FILE "${temporary}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${XZ_EXECUTABLE}" -T1 -c "${temporary}"
  OUTPUT_FILE "${FFMPEG_ARCHIVE}.tmp" COMMAND_ERROR_IS_FATAL ANY)
file(RENAME "${FFMPEG_ARCHIVE}.tmp" "${FFMPEG_ARCHIVE}")
file(REMOVE "${temporary}")
