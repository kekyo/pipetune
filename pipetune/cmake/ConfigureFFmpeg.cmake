# PipeTune's reproducible LGPL audio-only FFmpeg configuration.
# Upstream configure and source files remain unmodified.
foreach(required PIPETUNE_FFMPEG_SOURCE PIPETUNE_FFMPEG_PREFIX PIPETUNE_FFMPEG_CC)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()

set(arguments
  "--prefix=${PIPETUNE_FFMPEG_PREFIX}"
  "--cc=${PIPETUNE_FFMPEG_CC}"
  --disable-gpl --disable-version3 --disable-nonfree --disable-autodetect
  --disable-static --enable-shared --enable-pic --build-suffix=-pipetune
  --disable-programs --disable-doc --disable-debug --disable-network
  --disable-avdevice --disable-avfilter --disable-postproc --disable-swscale
  --disable-everything
  --enable-avformat --enable-avcodec --enable-avutil --enable-swresample
  --enable-demuxer=wav,aiff,flac,mp3,ogg,mov
  "--enable-decoder=pcm_*,adpcm_*,aac,aac_fixed,alac,flac,mp3,mp3float,vorbis,opus"
  --enable-parser=aac,flac,mpegaudio,vorbis,opus
  # configure evaluates the flags once, then Make expands the shared-library
  # rule twice. Preserve the final dollar sign for the ELF dynamic loader.
  [=[--extra-ldsoflags=-Wl,-rpath,'\$\$\$\$ORIGIN']=])
if(DEFINED PIPETUNE_FFMPEG_ARCH AND NOT PIPETUNE_FFMPEG_ARCH STREQUAL "")
  list(APPEND arguments "--arch=${PIPETUNE_FFMPEG_ARCH}")
endif()
if(DEFINED PIPETUNE_FFMPEG_CFLAGS AND NOT PIPETUNE_FFMPEG_CFLAGS STREQUAL "")
  list(APPEND arguments "--extra-cflags=${PIPETUNE_FFMPEG_CFLAGS}")
endif()
execute_process(
  COMMAND "${PIPETUNE_FFMPEG_SOURCE}/configure" ${arguments}
  COMMAND_ERROR_IS_FATAL ANY)
