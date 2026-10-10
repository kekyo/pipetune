#!/usr/bin/env bash
set -euo pipefail

probe=$1
libraries=$2
staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT

for component in avformat avcodec avutil swresample; do
  dynamic=$(readelf -d "$libraries/lib${component}-pipetune.so")
  if [[ "$dynamic" != *'Library runpath: [$ORIGIN]'* ]]; then
    printf 'FFmpeg %s must resolve its dependencies beside itself\n%s\n' "$component" "$dynamic" >&2
    exit 1
  fi
  if [[ "$dynamic" =~ Shared\ library:\ \[lib(avformat|avcodec|avutil|swresample)\.so ]]; then
    printf 'FFmpeg %s references a system FFmpeg library\n' "$component" >&2
    exit 1
  fi
done

# The probe uses a relative build RUNPATH. Moving both it and the libraries
# verifies actual loading without access to the original build prefix.
mkdir -p "$staging/ffmpeg/install/lib"
cp "$probe" "$staging/probe"
cp -a "$libraries/"*.so* "$staging/ffmpeg/install/lib/"
env -u LD_LIBRARY_PATH "$staging/probe"
