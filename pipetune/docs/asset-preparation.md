# Native asset preparation

## Boundary and replacement policy

PipeTune's native DSP engine follows the EffeTune application tag v2.17.0, commit
`d026b1664ec9444a66297efd36e251bc8aae3d05`. The C++20 libraries in
`asset-preparation/` adapt the JavaScript preparation needed before the upstream
native kernels can run. SFZ bank and generator preparation follow 2.17.0,
while the parser and source adapters retain the existing resident SFZ subset.
The source identity and preparation contract are part of the cache key.
The libraries accept values, source text, and decoded PCM in
memory, and return prepared data, source requests, and diagnostics. They do not
depend on PipeWire, GTK, JSON storage, XDG paths, a decoder, or the native engine.

The host in `src/` owns registration lookup, asynchronous I/O, decoding,
snapshot verification, cache storage, memory admission, and native activation.
In particular, `sfz_asset_loader.cpp` does not implement SFZ opcodes, region
selection rules, or numeric bank offsets. This keeps an eventual upstream C++
replacement local to the preparation library and its value adapter. Such an
upstream port is a possible future integration, not an announced upstream plan.

Do not merge the preparation algorithms into the host loaders when changing
storage or control behavior. Conversely, do not introduce filesystem or engine
types into the pure library to accommodate a host feature. The behavioral tests
at the boundary must remain useful when the implementation is replaced.

## Upstream port map

Source links below identify the application tag from which each module's
behavior was adapted. SFZ parsing and source handling originated in v2.14.0;
the parser now rejects the unsupported feature families introduced by 2.17.0.
Bank and generator preparation and whole-instrument capacity validation follow
v2.17.0. The unchanged SFZ helpers, IR,
and Room EQ adaptations retain their v2.13.0 origin. The adapted modules retain the upstream MIT attribution and
`LICENSE.effetune` files.

| PipeTune module | Upstream contract | Responsibility |
| --- | --- | --- |
| `sfz/parser.cpp`, `sfz/parser.h` | [parser.js](https://github.com/Frieve-A/effetune/blob/v2.14.0/js/sfz/parser.js) | Path normalization, include/define expansion, inheritance, opcode normalization, conditional regions, envelopes, and diagnostics |
| `sfz/region.h` | [parser.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/parser.js) | Prepared region values and source PCM layout |
| `sfz/source.cpp` | [source.js](https://github.com/Frieve-A/effetune/blob/v2.14.0/js/sfz/source.js) | UTF-8-first/Windows-1252 decoding, ARIA bank selection, initial definitions, and consulted bank dependencies |
| `sfz/generators.cpp` | [generators.js](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/generators.js) | Periodic source PCM and two-frame descriptors for native continuous noise and silence |
| `sfz/text.cpp` | [parser.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/parser.js), [asset.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/asset.js) | UTF-8/UTF-16 conversion and JavaScript string ordering/length semantics |
| `sfz/audio_header.cpp` | [bank.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/bank.js), [audio-header-metadata.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/audio-header-metadata.js) | Bounded WAV/AIFF/FLAC header estimates for capacity validation; unknown lengths stay unknown |
| `sfz/selection.cpp` | [service.js](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/service.js) | Complete-instrument capacity validation without layer reduction, and warning aggregation |
| `sfz/bank.cpp` | [asset.js](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/asset.js), [modulation.js](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/modulation.js), [oscillator-source.js](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/oscillator-source.js) | Resident v6 with 113 region fields; v7 fixed-value envelopes and built-in generators with prepared cycle pyramids; integer lanes, PCM, payload extent, and warmup |
| `sfz/preparation.cpp` | [service.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/service.js) | Combining normalized regions and supplied PCM, sample-position validation, final warnings, and bank preparation |
| `ir/preparation.cpp` | [ir-preparation.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-preparation.js), [ir-plugin-contract.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-plugin-contract.js), [ir-true-stereo-pair.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-true-stereo-pair.js) | Original channel layout, true-stereo pairing, trim/direct-cut/decay, and capacity-aware IR preparation |
| `common/convolution.cpp` | [ir-asset-payload.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-asset-payload.js) | Convolution payload layout and native convolver footprint |
| `common/resample.cpp` | [resample.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/utils/measurement-dsp/resample.js) | Measurement IR resampling with the upstream Kaiser-window contract |
| `room-eq/design.cpp`, `room-eq/numeric.cpp` | [design-core.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/room-eq/design-core.js), [smoothing.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/utils/measurement-dsp/smoothing.js) | Settings, log-grid amplitude correction, Gaussian smoothing, additional EQ, minimum/linear synthesis, and diagnostics |
| `room-eq/fft.cpp` | [fft.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/utils/measurement-dsp/fft.js) | Mixed-radix transforms, real packing, and float32 twiddles with double working values |
| `room-eq/analysis.cpp` | [design-core.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/room-eq/design-core.js), [onset.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/utils/measurement-dsp/onset.js) | Complete measurement IR analysis, onset/reference scale, and multi-point power mean |
| `room-eq/group_delay.cpp` | [group-delay-analysis.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/room-eq/group-delay-analysis.js) | Excess/group-delay observations and reliability |
| `room-eq/phase.cpp`, `phase_spectrum.cpp`, `phase_timing.cpp` | [design-core.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/room-eq/design-core.js) | Direct-sound phase, reference-point/consensus alignment, timing, and energy guards |
| `room-eq/low_phase.cpp`, `room-eq/reverb.cpp` | [design-core.js](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/room-eq/design-core.js) | Low-frequency extension, reverberant phase correction, and effective/reduced/omitted diagnostics |

SFZ bank-path ordering and Unicode case conversion use ICU's
[collation API](https://unicode-org.github.io/icu/userguide/collation/api.html)
to match the application's
[`localeCompare` selection](https://github.com/Frieve-A/effetune/blob/v2.14.0/js/sfz/source.js).
The build needs `icu-i18n`, and packaged binaries link the matching system ICU
library. Node.js is not needed at runtime. Generated SFZ samples need no source
audio file; file-backed samples still use the decoder described below.

File-only banks use the resident v6 format. Banks containing built-in generators
use v7 to carry their oscillator kinds and the existing gain, tuning, pan, and
amplitude envelope. This does not enable authored modulation, scripts, effects,
embedded samples, streaming, or SF2/SF3. Those recognized declarations are
rejected explicitly. Selecting SF2/SF3 fails preparation, while unselected valid
entries remain acceptable in the shared instrument registry.

Browser audio decoding is deliberately outside this map. The host's
`asset_audio_decoder.cpp` wraps public FFmpeg APIs for bounded memory input and
planar float PCM, keeping source rate and channel order. IR playback resampling
uses libsamplerate in that adapter. Measurement analysis uses the separate pure
Kaiser resampler above; it must not silently inherit the playback algorithm.

## Host contracts

`sfz_asset_loader.cpp`, `ir_asset_loader.cpp`, and `room_asset_loader.cpp` resolve
desktop registrations and supply the pure modules. `measurement_store.cpp`
shares measurement parsing and validation with Crosstalk Cancellation, while
Crosstalk-specific topology validation remains separate.

EffeTune 2.16.0 backups may include optional `harmonicAnalysis` beside the linear
IR, following the [upstream storage format](https://github.com/Frieve-A/effetune/blob/v2.16.0/features/measurement/dataStorage.js).
Room EQ and Crosstalk use the same linear data with or without that metadata.
The shared reader retains its 64 MiB limit for the complete backup. An oversized
replacement fails preparation and leaves the current Room EQ or Crosstalk pipeline active;
repairing the watched file allows automatic reload to recover.

`asset_file.cpp` uses cardio/GIO asynchronous reads, bounded prefix reads, and
streaming SHA-256 snapshots. SFZ includes and samples stay within the registered
root. Before publishing a candidate, the host verifies the snapshots and the
registration mapping again. Missing dependencies remain watchable so their
creation can retry the load. Source paths include the full registered root;
equal content in different roots must not reuse stale dependency identities.
All examined ARIA bank candidates remain snapshot dependencies; only the selected
bank's bytes count toward the source-byte admission limit. A change to an
unselected candidate can therefore trigger another selection without charging
that candidate as a retained bank.

Preparation uses the existing control dispatcher, with no additional worker or
audio-thread I/O. Requests wait on a cancellable asynchronous mutex. Completed
immutable results may be shared by concurrent candidates, including when disk
caching is disabled. Cancelling one request does not cancel another owner's
result. The control server drains pending operations on shutdown.

The `assets-v1` cache uses validated PTAC0001 records and atomic replacement.
Its key includes the preparation-source commit, preparation contract, original content,
and every setting affecting generated data, plus decoder identity where used.
Room/IR keys include design rate and effective layout. SFZ retains source-rate
samples and is reusable across output rate, backend, and `Classic`/`Event`
detection-mode changes. Runtime-only
gain/delay controls do not invalidate Room's generated FIR. A cache hit still
requires valid originals; corrupt entries regenerate, and an unwritable cache
does not discard a successfully prepared payload.

The 2.17.0 SFZ identity is `sfz-resident-v4/ETA1-table-v6-generator-v7` under the
pinned upstream commit. Earlier SFZ entries regenerate from originals.
IR and Room EQ keep their existing identities. Cached SFZ footprint metadata
contains only the portable payload length; native preparation storage is queried
again on the actual backend at activation.

Memory reservations include bounded preparation work, retained payloads, and
the native copy until each owner releases it. Old and candidate pipelines share
the 4 GiB budget on 64-bit hosts or 1 GiB on 32-bit hosts. These are admission
limits for asset work, not process RSS limits. The disk cache has a separate
default 2 GiB limit with unused entries reclaimed first.

For SFZ, the host supplies the complete control prefix, full payload size,
processing rate, maximum frame count, and actual output width to
`et_sample_player_preparation_bytes`. The instrument budget covers the payload
plus the returned additional bytes, without adding a second index estimate.
The query and upload use the same zero minimum physical BPM (the upstream
standalone fallback). The process-wide budget also reserves fixed instance
preparation before instance creation, including an unassigned player: 64 MiB
plus block and rate/channel-dependent storage. The reservation remains live
through native destruction and includes concurrent old and candidate pipelines.
See the [native memory contract](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/README.md#native-midi-sample-player-hosts)
and [instance preparation](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz/engine.h).

The native copy ABI uses the public pointer-returning asset begin function,
copies the complete payload, and commits it, aborting on failure. No private
Engine access or ABI patch remains. SFZ, IR, and Room kernels complete their
bounded activation and reset before the candidate replaces the old pipeline.
A configured-source failure rejects that replacement. Unassigned nodes and
partly supported data retain distinct warning states; disabled nodes and
sections perform no source I/O. Invalid Crosstalk measurements retain that
effect's established warning-and-omission behavior.

## Verification and replacement criteria

`make -C pipetune/asset-preparation test` builds the pure libraries and executes
three in-memory drivers without PipeWire, GTK, FFmpeg, or Node.js. The root
test build additionally runs TypeScript/Vitest contract tests against the
unmodified pinned JavaScript. Node.js is a development dependency only.

Tests compare observable normalized regions, byte-level bank data, numeric
FIR/phase results, diagnostics, and rendered PCM. Separate integration tests
cover actual files/formats, registrations, cache invalidation, cancellation,
source mutation during preparation, memory rejection, and preservation of the
old pipeline. All 13 official SFZ and 13 IR golden cases run through installed-style
shared backends with the original tolerances. The mixed preset activates all
nine asset-using effects, checking each effect's contribution, multichannel
routing, rate/backend rebuilds, and reset.

An upstream C++ replacement must satisfy these same contracts. Keep host storage
and cancellation behavior, adapt public upstream values at the preparation
boundary, retire the replaced algorithms, and rerun the independent and product
tests. Do not substitute private upstream APIs or weaken PCM comparisons to
accommodate the replacement.

## Linked libraries and notices

Prepared-source algorithms retain EffeTune's MIT attribution. Packaged DSP
libraries also carry upstream PFFFT and fdlibm notices in
`/usr/share/doc/pipetune/copyright`. FFmpeg is built from the pinned `n6.1.6`
submodule as LGPL-2.1-or-later shared libraries, with GPL, version3, nonfree,
and external library autodetection disabled. The `-pipetune` SONAME suffix
and relative RUNPATH keep its four audio libraries separate from system builds.
`/usr/share/doc/pipetune/ffmpeg` includes the exact source archive, license,
configuration, and instructions for rebuilding and replacing the libraries.
PipeTune's own source remains MIT licensed. See the
[FFmpeg licensing information](https://ffmpeg.org/legal.html) and
[rebuild instructions](../../deps/README.md).
