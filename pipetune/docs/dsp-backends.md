# PipeTune native DSP backends

## Purpose and default

PipeTune packages the EffeTune DSP engine as a private shared-library family:

- `libeffetune-dsp-scalar.so` is the compatibility backend. PFFFT is built
  with `PFFFT_SIMD_DISABLE=1`, and the remaining DSP code uses the normal
  CMake Release compiler policy.
- `libeffetune-dsp-simd.so` enables PFFFT's architecture implementation where
  PFFFT provides one and explicitly enables GCC tree vectorization for the
  complete EffeTune DSP translation set at the package architecture's
  baseline.
- `libeffetune-dsp-simd-x86-64-v3.so`,
  `libeffetune-dsp-simd-x86-64-v4.so`, and
  `libeffetune-dsp-simd-arm64-sve.so` are installed only on applicable
  architectures. They compile the same DSP source set for a higher,
  runtime-checked ISA level.

`scalar` is the startup, missing-configuration, and configuration-reset
default. The SIMD backend is opt-in because a compiler is not required to
produce numerically identical instruction sequences after vectorization.
Neither backend uses `-Ofast`, `-ffast-math`, `-march=native`, or link-time
optimization.

This separation preserves one known compatibility path while allowing
PipeTune to select the highest usable optimized implementation or a
user-pinned tier. One pipeline always uses exactly one implementation.

The private backend source set follows EffeTune 2.11.0, including the graph core
translation unit and generated graph-capacity contract required by its engine.
PipeTune's loader surface remains the routed pipeline API and now includes
`et_pipeline_latency`; PipeTune does not expose Graph v1 as a preset format or
control API.

Generated convolution assets cross a PipeTune-owned copy ABI instead of
EffeTune's
[asset-begin implementation](https://github.com/Frieve-A/effetune/blob/bedc6c662a6edc88c9644b7e00cec9122a250cfb/dsp/core/abi.cpp#L170-L185),
which narrows its staging pointer to a 32-bit return value. At build time CMake
copies EffeTune's `core/abi.cpp` into the generated build tree and applies
`patches/effetune/abi-engine-access.patch` with zero fuzz. The patch only
includes a PipeTune engine-access definition; coefficient generation and the
copy-and-commit bridge remain separate PipeTune source files. No source below
`deps/` is modified, no native pointer is narrowed into an EffeTune integer
handle, and an incompatible future `abi.cpp` causes the build to fail while
applying the patch.

## Embedded analysis models

Every private backend links the upstream `effetune_note_models` target. Its
three models total 5,779,072 bytes (about 5.51 MiB) per shared library. The
[upstream model build](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/analyzer/note_spectrogram/models.cmake)
requires Python 3.10 or later to validate the data and generate the embedding.
Python and separate model files are not runtime dependencies.

For 32-bit Arm, the
[upstream generator](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/analyzer/note_spectrogram/embed_models.py)
emits ELF directives with `@progbits` and `@object`, while GNU Arm assembly uses
`@` for comments. PipeTune compiles a build-tree copy with those directive
suffixes changed to `%progbits` and `%object`. The conversion uses npm/npx,
`funcity-cli@1.5.0`, `funcity@1.5.0`, and `commander@12.1.0`; the last pin
keeps the CLI usable with bookworm's Node 18. The upstream sources, binary
models, validation, and original generated files are preserved.

Level Meter, Note Spectrogram, Pitch Meter, Oscilloscope, Spectrogram, Spectrum Analyzer,
and Stereo Meter remain in the validated backend catalog but are excluded
from preset execution. They consume no active-node slots, add no latency,
and perform no bus transfers. Their exclusion is silent and does not apply
to effects that also modify audio. The catalog count is not a count of
user-executable effects; unresolved asset effects can also be omitted.

## Architecture policy

The package contents and dispatch tiers are:

| Package architecture | Variant | Compiler/PFFFT policy | Runtime requirement |
| --- | --- | --- | --- |
| amd64 | `baseline` | x86 PFFFT; x86-64 compiler baseline | x86-64 SSE2 architectural baseline |
| amd64 | `x86-64-v3` | `-march=x86-64-v3` | GCC CPUID x86-64-v3 feature level |
| amd64 | `x86-64-v4` | `-march=x86-64-v4` | GCC CPUID x86-64-v4 feature level |
| i386 | `baseline` | x86 PFFFT; `-msse2 -mfpmath=sse` | CPUID SSE2 |
| i386 | `x86-64-v3` | x86-64-v3 feature-targeted 32-bit code | GCC CPUID x86-64-v3 feature level |
| arm64 | `baseline` | NEON PFFFT | AArch64 Advanced SIMD architectural baseline |
| arm64 | `sve` | NEON PFFFT; `-march=armv8-a+sve -msve-vector-bits=scalable` for auto-vectorized code | Linux `HWCAP_SVE` |
| armhf | `baseline` | NEON PFFFT; `-mfpu=neon` | Linux `HWCAP_NEON` |
| riscv64 | `baseline` | portable PFFFT; `-march=rv64gcv -mabi=lp64d` for auto-vectorized code | Linux RISC-V V HWCAP |

The vendored PFFFT has x86, Arm NEON, and PowerPC AltiVec float
implementations, but no RISC-V V implementation. The riscv64 SIMD library
therefore relies on GCC vectorization of PFFFT's portable code and the other
EffeTune loops; it does not claim a native PFFFT RVV kernel.

Release libraries are normally compiled with GCC's `-O3`. GCC already
enables loop and SLP vectorization at that optimization level, so
`-ftree-vectorize` on the SIMD target is primarily an explicit policy marker.
The material differences are PFFFT's x86/NEON code and each tier's compiler
target. x86-64-v3 groups AVX, AVX2, FMA, BMI, and related features rather than
creating a library for every individual instruction extension. x86-64-v4
adds the AVX-512 feature-level group. Arm64 similarly has one architectural
NEON baseline and one scalable-vector SVE tier. armhf and riscv64 retain one
SIMD tier because the current implementation and package matrix do not
justify another independently dispatched library.

A loop-carried dependency, aliasing, control flow, or a scalar math-library
call can still prevent GCC from vectorizing an individual loop.
`-march=native` must not be used for distributed packages.

## Discovery and validation

PipeTune resolves each library only from:

1. the directory containing the running executable, for workspace builds; or
2. the executable-relative private installation directory, normally
   `/usr/lib/pipetune`.

It does not pass a bare soname to the dynamic loader and does not search a
generic plugin path. Before opening any SIMD library, PipeTune checks its
architecture requirement listed above. It then uses `RTLD_NOW | RTLD_LOCAL`
and validates:

- the complete required C ABI symbol set;
- the EffeTune DSP ABI version;
- the scalar/SIMD build flag;
- every kernel name and parameter-layout hash;
- each kernel's parameter-byte capacity; and
- every external-asset capacity.

The pinned EffeTune 2.11.0 contract contains 107 kernels. A backend from an
earlier release is rejected even though the numeric ABI version remains one,
because its required symbols and complete catalog do not match.

This count includes eight visualization-only analyzers excluded from preset
execution and does not mean every registered effect is available without
assets. Attack Tonal Balance, Bass Extender, Spatial Mapper, and TV Audio
Simulator need no external assets. Bass Management IIR also needs no asset;
PipeTune generates the necessary FIR assets for its Linear mode.
Spectrogram and Spectrum Analyzer include the upstream `hq` parameter in
their validated layouts even though PipeTune omits their execution.

TV Audio Simulator and Attack Tonal Balance use the same source-specific `-ffp-contract=off` policy
as the [upstream native build](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/CMakeLists.txt).
Artifact tests load every CPU-applicable private backend and compare all seven
Spatial Mapper and six TV Audio Simulator golden cases, including their input
stimuli and parameter events. Spatial Mapper's goldens originate from the
[promoted native implementation](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/spatial/spatial_mapper/golden/case-001.json);
independent preset tests also check routing, polarity, silence, and actual delay.
The same shared-library tests cover all nine Attack Tonal Balance, ten Bass
Extender, three Bass Management, and 104 changed saturation, Limiter, and MP3
goldens. Bass Management's Linear golden uses a synthetic IR; separate tests
compare generated coefficients and PCM with the official JavaScript design.
Official tolerances are retained for every case.

The scalar backend is mandatory. If it cannot be validated, a managed daemon
starts in pass-through mode and reports the error; a direct preset run fails.
For automatic SIMD selection, CPU-inapplicable upper tiers are skipped
silently. If a CPU-supported upper library is missing or fails validation,
PipeTune uses the next lower SIMD tier and reports the failed upper tier as a
fallback diagnostic. If no SIMD tier is usable, daemon startup uses scalar.
A pinned tier that is unavailable also falls back to scalar at startup. A
new live request for an unavailable pinned tier is rejected and is not
persisted.

## Selection and live switching

The shared startup configuration accepts:

```text
PIPETUNE_DSP_BACKEND=scalar
PIPETUNE_DSP_SIMD_VARIANT=auto
```

The backend value is exactly `scalar` or `simd`. The SIMD variant is exactly
`auto`, `baseline`, `x86-64-v3`, `x86-64-v4`, or `sve`; only
architecture-applicable values can become effective. An absent backend
assignment selects scalar, and an absent variant assignment selects `auto`.
Configuration reset restores those defaults. The CLI operations are:

```sh
pipetune dsp list
pipetune dsp list --json
pipetune dsp get
pipetune dsp get --json
pipetune dsp set scalar
pipetune dsp set simd
pipetune dsp set simd --variant baseline
pipetune dsp set simd --variant x86-64-v3
```

`list` and `get` require the daemon and report both library availability,
CPU requirements, configured and effective variants, fallback, and loader
diagnostics. A connected `set` rebuilds the active preset with the requested
library on the control thread, then atomically replaces the real-time pipeline.
Only a daemon-confirmed change is saved. A construction or daemon error keeps
the old pipeline and startup choice. Changing only the saved preference when
it resolves to the already-active concrete library does not rebuild the
pipeline.

The replacement creates fresh EffeTune instances, so filter histories, delay
lines, analyzer windows, and other DSP state reset. PipeTune retains its
external audio error and timing counters. It does not deliberately reconnect
PipeWire or insert silence for a backend-only change, but a discontinuity or
brief silence is allowed. Backend requests are rejected during a sample-rate
transition.

While bypass is active, changing the backend updates the configured/effective
choice without constructing an EffeTune pipeline. The choice is used by the
next preset load. If the daemon is unavailable, CLI and GTK first perform the
same local CPU, file, ABI, and catalog validation and then save a valid choice
for the next start.

A direct, non-persistent run can select a backend separately:

```sh
pipetune --preset /absolute/path/to/example.effetune_preset \
  --dsp-backend simd \
  --dsp-variant x86-64-v3
```

The GTK window exposes scalar, automatic SIMD, and applicable pinned tiers in
one **DSP backend** / **Native engine** drop-down. It displays the effective
variant, CPU requirement, fallback, and validation error.

Machine-readable status includes `configuredDspSimdVariant`,
`effectiveDspVariant`, and `availableDspVariants` in addition to the logical
configured and effective backend fields.

## Where SIMD can help

PFFFT acceleration and compiler auto-vectorization affect different DSP work:

| DSP work | Expected SIMD opportunity | Limiting factors |
| --- | --- | --- |
| Attack Tonal Balance, Spatial Mapper, Noise Reduction, Phase Select EQ, and spectral pitch/bandwidth processing | High, direct PFFFT consumers | Transform size, channel count, and non-FFT processing determine total cost |
| Spectrum Analyzer, Spectrogram, Note Spectrogram, and Pitch Meter FFTs | High within their kernels | PipeTune omits these analyzers from preset execution |
| Bass Management Linear, FIR Crossover, 5Band FIR PEQ, Group Delay EQ, and Group Delay PEQ design and convolution | High, direct PFFFT consumer | Design runs while loading or rebuilding the preset; steady-state convolution cost depends on taps and channels |
| IR Reverb and Room EQ convolution | High when an asset is available | PipeTune omits these nodes because the preset does not contain their stored PCM source |
| Large sample-independent transforms, buffer mixing, matrixing, and analyzer preparation | Medium to high auto-vectorization potential | Often memory-bandwidth limited; inexpensive nodes have little absolute cost |
| Pitch shifting and long window/ring-buffer operations | Medium, sometimes high aggregate potential | Wrap branches, square roots, interpolation, and state handling inhibit parts of each loop |
| Multiband split/combine, saturation, and dynamics | Medium aggregate potential | Filters and envelopes have sample-to-sample dependencies |
| Biquads, recursive delays, algorithmic reverbs, resonators, and modulation | Low to medium | Feedback state usually prevents vectorization across time; channels or modes may still be parallel |
| Volume, mute, polarity, simple routing, and meters | Technically vectorizable but usually low impact | Their scalar cost is already small |

Direct PFFFT consumers include the effects above, Pitch Shifter HQ, Bandwidth
Extender, the partitioned convolver, and the design-time FFT API. Attack Tonal
Balance uses [incremental PFFFT transforms](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/dynamics/attack_tonal_balance/kernel.cpp).
MP3 Codec Simulator's 2.11 [codec processing](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/lofi/mp3_codec_simulator/kernel.cpp)
has no direct PFFFT dependency; its performance depends on codec transforms,
resampling, and compiler optimization. Gains elsewhere can come from compiler
vectorization or instruction scheduling.

Telemetry-rate behavior is unchanged by this work. In particular, selecting
telemetry rate zero does not newly stop analyzer processing.

## Standard preset expectations

The following ranking is based on the enabled native nodes in the pinned
EffeTune standard presets. It predicts where measurement is most useful; it
is not a promised speedup.

| Expected opportunity | Standard presets | Reason |
| --- | --- | --- |
| Most direct PFFFT effect | `visualize/all_analyzers` | Contains both Spectrogram and Spectrum Analyzer |
| Strong compiler-vectorization candidate | `others/dsd_noise_listening` | Contains eight Pitch Shifter instances and long window operations |
| Moderate | `lofi/authentic_vinyl`, `lofi/vinyl`, `processor/fm_radio`, `spatial/live` | Multiband processing plus several per-sample stages |
| Moderate | `spkr_sim/vintage_full_range`, `4ch/rear_reverb` | Resonator/reverb work and, for the latter, four-channel processing |
| Low to moderate | `processor/bbe`, `amp_sim/tube_amp`, `others/karaoke`, `lofi/needle_drop`, `lofi/old_radio` | Stateful EQ, dynamics, saturation, or one pitch shifter |
| Usually low | `4ch/matrix`, `lofi/dsd_noise`, `lofi/old_r2r_dac`, `utils/bgm` | Mostly simple routing, gain, noise, or inexpensive stateful processing |

Preset parameters, sample rate, block size, channel count, CPU, compiler, and
thermal state can change the order. In particular, a high-cost recursive DSP
can dominate total time without being easy to vectorize, while a directly
accelerated FFT may run only periodically.

## Benchmarking

The developer build includes `pipetune-dsp-benchmark`. It discovers and
validates the same library family as PipeTune, then constructs an independent
pipeline for scalar and every SIMD variant usable on the current CPU. It
warms each pipeline and reverses their timing order on alternating blocks.
Input generation and buffer copies are outside the measured `process` call.
Pipeline construction and backend switching are not timed.

Build and run it in Release mode:

```sh
cmake -S . -B build/benchmark \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build/benchmark --target pipetune-dsp-benchmark --parallel

./build/benchmark/pipetune-dsp-benchmark \
  --warmup-blocks 500 \
  --measure-blocks 5000 \
  --frames 256 \
  deps/effetune/presets/processor

./build/benchmark/pipetune-dsp-benchmark \
  --channels 4 \
  --warmup-blocks 500 \
  --measure-blocks 5000 \
  deps/effetune/presets/4ch
```

One preset file or any number of files and recursively searched directories
may be supplied. `--json` emits machine-readable results. `--sample-rate`,
`--channels`, and `--frames` should match the intended workload. Use
`--help` for all bounds and defaults.

The report gives each concrete variant's nanoseconds per frame and
`scalar / variant` speedup for every preset. JSON reports a top-level
`variants` array and a corresponding per-preset `variants` measurement array.
Each measurement also exposes an output checksum as a coarse diagnostic, but
checksums are not required to be bit-identical.

For decisions about another backend variant:

1. use a performance CPU governor and avoid unrelated load;
2. run the same Release binary several times, alternating test order;
3. compare medians across complete runs rather than one result;
4. test the real channel count, sample rate, and PipeWire block size; and
5. require a repeatable whole-preset improvement, not just a faster isolated
   loop.

Debug builds, virtual machines, emulation, and thermally throttled systems are
useful for correctness but not for selecting optimization policy.

## Denormal handling

Immediately before an active native pipeline is processed on x86, PipeTune
enables flush-to-zero and denormals-are-zero in the current audio thread's
MXCSR register while preserving its other control and status bits. Recursive
DSP can otherwise retain inaudible subnormal tails after input stops and spend
substantially more CPU time processing them. Bypass processing and non-x86
platforms are unchanged.

## Numerical verification

The complete test suite loads every architecture-applicable shared library,
checks its ABI and catalog, compares each CPU-runnable SIMD PFFFT impulse
transform with scalar within tolerance, and processes equivalent preset
pipelines through the runnable variants. It compares the actual packaged
libraries, including standalone Release builds, with EffeTune's official Auto
Leveler, Bluetooth SBC, Cassette Artifacts, Tape Artifacts, Vinyl Artifacts,
Click Remover, Clip Restorer, and Noise Reduction golden cases to preserve source-specific floating-point policy. It also runs
the existing EffeTune native DSP tests and JavaScript/native parity corpus.

These tests establish compatibility for covered inputs, not universal
floating-point identity. Scalar remains the recovery backend when a user or
platform encounters a SIMD-specific numerical or performance regression.
