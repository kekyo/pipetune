# PipeTune native DSP backends

## Purpose and default

PipeTune packages the EffeTune DSP engine as two private shared libraries:

- `libeffetune-dsp-scalar.so` is the compatibility backend. PFFFT is built
  with `PFFFT_SIMD_DISABLE=1`, and the remaining DSP code uses the normal
  CMake Release compiler policy.
- `libeffetune-dsp-simd.so` enables PFFFT's architecture implementation where
  PFFFT provides one and explicitly enables GCC tree vectorization for the
  complete EffeTune DSP translation set in Release builds.

`scalar` is the startup, missing-configuration, and configuration-reset
default. The SIMD backend is opt-in because a compiler is not required to
produce numerically identical instruction sequences after vectorization.
Neither backend uses `-Ofast`, `-ffast-math`, `-march=native`, or link-time
optimization.

This separation preserves one known compatibility path while allowing the
same PipeTune process to replace an active preset pipeline with an optimized
implementation. It does not load two implementations into one pipeline.

## Architecture policy

The SIMD library has one baseline per Debian package architecture:

| Package architecture | PFFFT in SIMD library | Additional compiler target | Runtime requirement |
| --- | --- | --- | --- |
| amd64 | SSE1 float implementation | x86-64 compiler baseline; no extra ISA flag | x86-64 SSE2 architectural baseline |
| i386 | SSE1 float implementation | `-msse2 -mfpmath=sse` | CPUID SSE2 |
| arm64 | NEON implementation | `PFFFT_ENABLE_NEON=1` | AArch64 Advanced SIMD architectural baseline |
| armhf | NEON implementation | `PFFFT_ENABLE_NEON=1 -mfpu=neon` | Linux `HWCAP_NEON` |
| riscv64 | Portable PFFFT fallback | `-march=rv64gcv -mabi=lp64d` | Linux RISC-V V HWCAP |

The vendored PFFFT has x86, Arm NEON, and PowerPC AltiVec float
implementations, but no RISC-V V implementation. The riscv64 SIMD library
therefore relies on GCC vectorization of PFFFT's portable code and the other
EffeTune loops; it does not claim a native PFFFT RVV kernel.

Both Release libraries are normally compiled with GCC's `-O3`. GCC already
enables loop and SLP vectorization at that optimization level, so
`-ftree-vectorize` on the SIMD target is primarily an explicit policy marker.
The material differences are PFFFT's x86/NEON code and the additional i386,
armhf, and riscv64 target options. A loop-carried dependency, aliasing,
control flow, or a scalar math-library call can still prevent GCC from
vectorizing an individual loop.

There are deliberately no separate SSE4, AVX2, or AVX-512 libraries. Adding
one would increase package size, loader and status states, test combinations,
and numerical variants. It should be considered only after repeatable
per-preset measurements show a useful gain beyond the current architecture
baseline. `-march=native` must not be used for distributed packages.

## Discovery and validation

PipeTune resolves each library only from:

1. the directory containing the running executable, for workspace builds; or
2. the executable-relative private installation directory, normally
   `/usr/lib/pipetune`.

It does not pass a bare soname to the dynamic loader and does not search a
generic plugin path. Before opening the SIMD library, PipeTune checks the
architecture requirement listed above. It then uses `RTLD_NOW | RTLD_LOCAL`
and validates:

- the complete required C ABI symbol set;
- the EffeTune DSP ABI version;
- the scalar/SIMD build flag;
- every kernel name and parameter-layout hash;
- each kernel's parameter-byte capacity; and
- every external-asset capacity.

The scalar backend is mandatory. If it cannot be validated, a managed daemon
starts in pass-through mode and reports the error; a direct preset run fails.
If configured SIMD is unavailable but scalar is valid, daemon startup uses
scalar and reports configured `simd`, effective `scalar`, fallback state, and
the exact diagnostic. A new live `dsp set simd` request instead rejects the
unavailable choice and does not persist it.

## Selection and live switching

The shared startup configuration accepts:

```text
PIPETUNE_DSP_BACKEND=scalar
```

The value is exactly `scalar` or `simd`. An absent assignment selects scalar.
The CLI operations are:

```sh
pipetune dsp list
pipetune dsp list --json
pipetune dsp get
pipetune dsp get --json
pipetune dsp set scalar
pipetune dsp set simd
```

`list` and `get` require the daemon and report both library availability,
CPU requirements, configured and effective variants, fallback, and loader
diagnostics. A connected `set` rebuilds the active preset with the requested
library on the control thread, then atomically replaces the real-time pipeline.
Only a daemon-confirmed change is saved. A construction or daemon error keeps
the old pipeline and startup choice.

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
  --dsp-backend simd
```

The GTK window exposes the same state and selection in **DSP backend** /
**Native engine**. It displays the effective variant, CPU requirement,
fallback, and validation error.

## Where SIMD can help

PFFFT acceleration and compiler auto-vectorization affect different DSP work:

| DSP work | Expected SIMD opportunity | Limiting factors |
| --- | --- | --- |
| Spectrum Analyzer and Spectrogram FFTs | High, direct PFFFT consumer | FFT runs at the analyzer cadence, so whole-pipeline gain is diluted between frames |
| IR Reverb and Room EQ convolution | High, direct PFFFT consumer | PipeTune currently omits nodes that require external assets |
| Large sample-independent transforms, buffer mixing, matrixing, and analyzer preparation | Medium to high auto-vectorization potential | Often memory-bandwidth limited; inexpensive nodes have little absolute cost |
| Pitch shifting and long window/ring-buffer operations | Medium, sometimes high aggregate potential | Wrap branches, square roots, interpolation, and state handling inhibit parts of each loop |
| Multiband split/combine, saturation, and dynamics | Medium aggregate potential | Filters and envelopes have sample-to-sample dependencies |
| Biquads, recursive delays, algorithmic reverbs, resonators, and modulation | Low to medium | Feedback state usually prevents vectorization across time; channels or modes may still be parallel |
| Volume, mute, polarity, simple routing, and meters | Technically vectorizable but usually low impact | Their scalar cost is already small |

Only Spectrum Analyzer, Spectrogram, the partitioned convolver used by IR
Reverb and Room EQ, and the design-time FFT API call PFFFT directly in the
current native code. Other gains must come from compiler vectorization or
secondary effects such as better instruction scheduling.

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
validates the same two libraries as PipeTune, constructs an independent
pipeline for each, warms both, and alternates their timing order per block.
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

The report gives scalar and SIMD nanoseconds per frame and
`scalar / SIMD` speedup for each preset. It also exposes both output checksums
as a coarse diagnostic, but does not require them to be bit-identical.

For decisions about another backend variant:

1. use a performance CPU governor and avoid unrelated load;
2. run the same Release binary several times, alternating test order;
3. compare medians across complete runs rather than one result;
4. test the real channel count, sample rate, and PipeWire block size; and
5. require a repeatable whole-preset improvement, not just a faster isolated
   loop.

Debug builds, virtual machines, emulation, and thermally throttled systems are
useful for correctness but not for selecting optimization policy.

## Numerical verification

The complete test suite loads both shared libraries, checks their ABI and
catalogs, compares a scalar and SIMD PFFFT impulse transform within tolerance,
and processes equivalent preset pipelines through both variants. It also runs
the existing EffeTune native DSP tests and JavaScript/native parity corpus.

These tests establish compatibility for covered inputs, not universal
floating-point identity. Scalar remains the recovery backend when a user or
platform encounters a SIMD-specific numerical or performance regression.
