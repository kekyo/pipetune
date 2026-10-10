# Dependencies

The submodules under this directory are upstream-owned dependencies. PipeTune
does not modify or independently maintain their source code. PipeTune-specific
integration and adaptations must be implemented outside the submodule
directories. Submodule pointers must refer to commits published by the
corresponding official upstream repository.

## FFmpeg audio libraries

PipeTune builds the pinned FFmpeg `n6.1.6` sources as LGPL-2.1-or-later shared
libraries. The configure recipe disables GPL, version3, nonfree components,
external library autodetection, network access, programs, encoders, and video
decoders. The `-pipetune` library name suffix separates these libraries from
distribution-provided FFmpeg builds. PipeTune itself remains MIT licensed.
See [FFmpeg's licensing guidance](https://ffmpeg.org/legal.html) and the upstream
`LICENSE.md` for the distinction between the configured libraries and optional
components present in the complete source archive.

Installed packages include the exact upstream source archive, upstream license
notices, `build-info.txt`, and `ConfigureFFmpeg.cmake` under
`share/doc/pipetune/ffmpeg`. No upstream source modifications are made. The source
archive retains the individual notices and licenses of all upstream files,
including optional components that are excluded from PipeTune's build.

To rebuild or modify the libraries, install a C compiler, GNU make, CMake and
the usual C development headers (plus NASM on x86). In a writable directory,
extract `ffmpeg-source.tar.xz`, copy `ConfigureFFmpeg.cmake` there, and run:

```sh
mkdir ffmpeg-build
cd ffmpeg-build
cmake -DPIPETUNE_FFMPEG_SOURCE="$PWD/../ffmpeg" \
  -DPIPETUNE_FFMPEG_PREFIX="$PWD/../ffmpeg-install" \
  -DPIPETUNE_FFMPEG_CC=/usr/bin/cc \
  -P ../ConfigureFFmpeg.cmake
make -j4
make install-libs install-headers
```

Use the architecture and extra C flags recorded in `build-info.txt` when needed,
via `PIPETUNE_FFMPEG_ARCH` and `PIPETUNE_FFMPEG_CFLAGS`. Keep the library name
suffix and compatible public ABI when replacing the libraries. The application
uses ordinary dynamic linking and does not verify or restrict user changes.
For a per-process replacement, set `LD_LIBRARY_PATH` to the rebuilt installation's
`lib` directory when launching PipeTune. Alternatively, replace the libraries in
PipeTune's installation under `lib/pipetune` (or the configured library directory).
Retain the matching versioned files and symbolic links. No `ffmpeg` executable
is required to load assets at runtime.

The library configuration can be independently checked with the public
`avformat_license`, `avcodec_license`, `avutil_license`, and
`swresample_license` functions; PipeTune's build tests also verify the required
audio decoders and the absence of file/network protocols.
