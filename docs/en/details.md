# Details

## Crosstalk Cancellation

PipeTune can apply EffeTune's Crosstalk Cancellation to stereo speaker playback.
In EffeTune, measure both speakers at each ear position, assign all four paths,
and save the preset. The two paths for each ear must belong to the same
single-point measurement. See the
[EffeTune instructions](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/plugins/spatial.md#crosstalk-cancellation).

PipeTune automatically reads `measurement-backups/<measurement-ID>.json`
beside EffeTune's saved preset collection. The directory is
`$XDG_CONFIG_HOME/effetune`, or `~/.config/effetune` when unset.
EffeTune 2.13.0 desktop automatically
[backs up measurements as JSON](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/measurement-backup-ipc.cjs)
there. When moving a preset to another computer, copy its referenced measurement
JSON files too. For browser measurements, export JSON including the impulse
responses and place it there using the original measurement ID as the filename.

The service watches referenced measurements for updates, deletion, and recovery,
even while GTK is closed, and rebuilds filters when the processing sample rate
changes. Missing or invalid measurements, or a non-stereo channel selection,
omit only this effect with a warning. Check that the backup contains impulse
responses and that the preset references the correct measurement IDs.
Use this effect at the measured listening position with speakers, not headphones.
Added latency is the selected Latency plus half the tap count, in samples.

## Spatial Mapper and TV Audio Simulator

[Spatial Mapper](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/spatial-mapper/index.md)
presets run with their saved Direct, Diffuse, and Residual routing matrices.
Transparent preserves the input with added delay: 2,560 frames at 48 kHz
(about 53 ms). The reported delay follows the processing sample rate.

For multichannel upmixing, select Ch = All in EffeTune and start PipeTune with
enough channels using `--channels`. The default is two channels, and loading
a preset does not change the stream width. A pair selection limits processing
to that pair. Check the output connections: PipeTune's six-channel order is
FL, FR, FC, LFE, RL, RR, while nine or more channels use AUX positions.
The 12-channel 7.1.4 preset therefore requires explicit output connections;
PipeTune does not automatically assign height speakers.

[TV Audio Simulator](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/tv-audio-simulator/index.md)
supports 44.1, 48, 88.2, 96, 176.4, 192, 352.8, and 384 kHz.
If Automatic negotiates an unsupported rate, select a supported fixed rate.
Select a mono channel or stereo pair to process; the
[upstream kernel](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/lofi/tv_audio_simulator/kernel.cpp)
processes only the first two channels of an All selection and leaves later
channels unchanged, without matching their delay. Use separate pair-selected
nodes when processing multiple pairs.

Broadcast Off leaves receiver noise enabled, and Mix = 0 retains the dry
path's delay. The default DSP silence policy, Ignore, keeps receiver noise
running on silent input. An explicit silence timeout fades and suspends it
until input resumes.

## Attack Tonal Balance and Bass Extender

[Attack Tonal Balance](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/attack-tonal-balance/index.md)
adjusts attack and tonal components. At 48 kHz it adds 5,120 frames (about
106.67 ms) of delay, including when both gains are 0 dB. Delay changes with
the processing rate. Turning both components off can leave residual sound;
use PipeTune bypass to restore the original audio path. High rates and many
channels increase processing cost.

[Bass Extender](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/saturation/bass_extender/kernel.cpp)
supports 44.1, 48, 88.2, 96, 176.4, and 192 kHz, with one or two selected
channels and no added processing latency. Stereo processing generates a
shared low-frequency component from the average of the two inputs. On a
wider stream, choose a mono channel or stereo pair; an All selection wider
than two channels is a load error. If Automatic selects an unsupported
rate, choose a supported fixed rate, such as 48 kHz.

## Tonal Balance EQ

[Tonal Balance EQ](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/tonal-balance-eq/index.md)
measures the music's spectrum and gradually corrects its tonal balance. Save
All, a style target, or Tilt and any Target Adjust bands in EffeTune, then
load the preset in PipeTune. Correction develops over time; compare after
about half a minute with the default averaging time. All selected channels
receive the same correction, with no added processing latency. Unselected
channels are unchanged.

Averaging Time = 100 (`at: 100`) means cumulative measurement since creation
or reset, rather than a 100-second window. Measurement Paused (`mp`) freezes
measurement while continuing to process audio with the established correction.
Amount = 0 or Range = 0 leaves the audio unchanged. Target Adjust reshapes
the desired balance; it is not an additional fixed EQ. The loudness make-up
can raise peaks. Reduce the following stage's gain or add a limiter if needed.
These behaviors follow the [upstream contract](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/eq/tonal_balance_eq/kernel.cpp).

## Rhythm Analyzer and silence suspension

[Rhythm Analyzer](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/rhythm-analyzer/index.md)
runs its analysis in PipeTune. Metronome Click (`ck`) defaults to off and
leaves PCM unchanged, but the analyzer still consumes processing time and
counts as an active DSP. With the click enabled, a steady beat produces
clicks after detection settles. Set Min BPM / Max BPM in EffeTune when the
detected beat is at half or twice the intended tempo.

The same click is added to the first two selected channels, or one channel
for mono. With Ch = All on a wider stream, channels 3 onward stay unchanged;
choose a different pair to put the click there. Processing adds no reported
audio latency; beat detection itself takes time. Later effects also process
the click. PipeTune shows the preset entry and its state, without the upstream
analysis graphs. See the [channel and click contract](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp).

The supported rates are 8, 11.025, 16, 22.05, 24, 32, 44.1, 48, 88.2, 96,
176.4, 192, 352.8, and 384 kHz. At other rates, input passes through without
analysis or clicks. If Automatic selects another rate, choose a supported
fixed rate. These rates follow the [upstream rate filters](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/rhythm_d_tables.h).

When Suspend DSP on silence is enabled, sustained silent input causes a fade,
reset, and suspension for these effects too. This clears Tonal's cumulative
measurement and Rhythm's beat detection, including when clicks are enabled.
They start measuring again when input returns. Turn silence suspension off
(Ignore in the CLI) to continue DSP calls through silent input without this
host reset. The effects retain their own rules for quiet input. Preset reloads,
sample-rate changes, and backend changes still start a new measurement even
when silence suspension is off.

Independently of PipeTune's suspension setting, EffeTune 2.13.0 clears Rhythm's
internal detection state after about one second of digital silence. The next
beat requires detection again, including with Ignore selected. See the
[upstream silence handling](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/rd6_engine.h).

## Bass Management

[Bass Management](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/bass-management/index.md)
supports IIR and Linear presets. Select Ch = All and start PipeTune with
enough channels using `--channels` (1–16). Configure the input roles and
subwoofer destinations in EffeTune before saving the preset. A subwoofer
output cannot also be a Full Range or Managed main output. With subwoofer outputs enabled, each Managed or
LFE input needs a valid destination among the configured subwoofer outputs.
Invalid routes reject the load and preserve the previous live pipeline.

Check the physical output connections. Channel numbers in a preset do not
assign speakers automatically, and nine or more channels use AUX positions.
Multiple subwoofer destinations share an input equally (`1 / destination
count`). IIR adds no processing latency. Linear generates the required
filters from the preset without a separate IR file:

| Linear taps | Added delay | At 48 kHz |
| --- | --- | --- |
| 8,192 | 4,224 frames | 88 ms |
| 16,384 | 8,320 frames | about 173.33 ms |
| 32,768 | 16,512 frames | 344 ms |

Linear delay remains even when no subwoofer is configured. Filters are
regenerated after a sample-rate or backend change, and subwoofer output can
remain silent while the new filters prepare. Configurations exceeding the
32 MiB asset and processing-memory limit are rejected; PipeTune does not
reduce the tap count automatically. These conditions follow the
[upstream processing and asset contract](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/basics/bass_management/kernel.cpp).

## Oversampling and delay controls

Saturation, Dynamic Saturation, Exciter, Hard Clipping, Harmonic Distortion,
and Multiband Saturation support 1x, 2x, 4x, and 8x oversampling; Hard
Clipping also supports 16x. The default is 1x. Supported factors of 2x and
above add 64 frames, including the dry mix. Reloading a preset updates the
reported delay. See the
[upstream oversampling contract](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/include/effetune/dsp/oversampled_shaper.h).

Brickwall Limiter adds lookahead plus 64 frames with oversampling enabled.
Its 1x mode uses an approximate reciprocal calculation and may slightly
exceed the configured ceiling. PipeTune preserves that upstream audio;
select 2x or higher when the output samples must stay within the ceiling.
See the [Limiter implementation](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/dynamics/brickwall_limiter/kernel.cpp).

[Time Alignment](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/delay/time_alignment/kernel.cpp)
accepts up to 500 ms. This is an intentional speaker-alignment delay and is
not included in automatic host latency compensation.

## Adaptive Prediction and Cassette Artifacts

[Adaptive Prediction](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/adaptive-prediction-effect/index.md)
learns from the input and mixes the original, prediction, and residual. Select
one channel or a stereo pair; All with more than two channels is rejected.
Learning is independent for left and right and is not saved in the preset.
Reloading, changing rate/backend, or silence suspension resets the learned state.
With suspension disabled, processing continues according to the effect's Hold,
Freeze, and silence rules. Hold needs an already learned signal. The prediction
gap is not a reported audio latency.

[Cassette Artifacts](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/cassette-artifacts/index.md)
supports All, Encode Only, Encode + Artifacts, Artifacts + Decode, and Decode Only.
PipeTune uses the mode saved in EffeTune. Noise Reduction Off bypasses the Dolby
stages; a mode containing only those stages can therefore sound unchanged.

## External assets

SFZ Note Player, IR Reverb, and Room EQ read the data registered by EffeTune
desktop under `$XDG_CONFIG_HOME/effetune`, or `~/.config/effetune` when the XDG
variable is unset. A preset contains references, so copying the preset alone
does not copy its instrument, IR, or measurement. Register the original data
in EffeTune desktop on the same account before loading the preset in PipeTune.
Browser-only libraries are not available to PipeTune.

| Effect | Required saved data |
| --- | --- |
| SFZ Note Player | `sfz-references.json`, its registered SFZ root, included definitions, and referenced sample files |
| IR Reverb | `ir-library/index.json` and the single original or L/R original pair named in that index |
| Room EQ | `measurement-backups/<id>.json`, including the selected channel response and, for Correction, every measurement point's IR |

The layouts follow the upstream [SFZ registration](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/sfz-library-ipc.js),
[IR library](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/ir-library-ipc.js),
and [measurement backups](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/measurement-backup-ipc.cjs).
PipeTune reads these originals without modifying EffeTune's library. Initial
preparation, playback, and cache regeneration use native code; neither Node.js,
the ffmpeg command, nor a running EffeTune/Electron process is required.

SFZ follows the [subset supported by EffeTune 2.13.0](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/sfz-note-player/index.md),
including includes, inherited regions, loops, and envelopes. WAV, AIFF, and FLAC
mono/stereo samples are supported. The bank budget defaults to 256 MiB; select
64, 128, 256, 512, or 1024 MiB in settings, with `--sfz-max-size MIB` for direct
runs or `PIPETUNE_SFZ_MAX_SIZE_MIB` in the daemon configuration. When necessary,
the upstream selection rule reduces layers while preserving the playable keys.
Skipped regions, missing samples in a usable partial instrument, and reduced
banks are reported on the active DSP. An instrument that cannot retain all
playable keys within the budget fails loading. Sample paths must stay within
the registered root.

IR Reverb supports WAV/IRS, AIFF, FLAC, MP3, Ogg, and M4A originals decoded by the
installed FFmpeg libraries. It preserves channel order and supports independent
channels, true stereo, L/R pairs, Direct Cut, Decay, Trim, and the saved convolution
mode. Original files and decoded PCM are each limited to 64 MiB, with at most
16 channels. The convolver has a 32 MiB working-size limit; truncation required
by that limit is reported. See the [upstream IR settings](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/ir-reverb/index.md).

Room EQ designs correction filters from measurements; it does not play the
measured room IR as a reverb. Minimum and Linear can use frequency-response-only
backups. Correction requires a complete IR set and supports direct phase,
low-frequency extension, reverb correction, Consensus, and Reference Point.
Shared and per-channel measurements follow the selected output range: for a
3/4 pair, the first two measurement slots apply to channels 3/4. A single-channel
selection uses the shared measurement. Empty per-channel slots fall back to the
shared measurement; an intentionally unassigned channel stays aligned with the
other channels. All above eight channels limits the effective filter to 65,536
taps. Reduced or unavailable phase/reverb correction is shown as an active
diagnostic, with its reason and effective amount. Gain and manual Delay are
retained; manual Delay is intentionally outside automatic latency compensation.
See the [upstream Room EQ settings](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/room-eq/index.md).

PipeTune caches prepared assets under `$XDG_CACHE_HOME/pipetune/assets-v1`, or
`~/.cache/pipetune/assets-v1`. The cache is rebuildable, capped at 2 GiB, and never
replaces the originals. Original changes and missing-file restoration trigger
automatic reload. Damaged cache entries are regenerated; a cache write failure
does not prevent playback of a successfully prepared asset. A failed SFZ/IR/Room
reload keeps the previous audio pipeline and reports the error. An unassigned
effect remains active with a diagnostic. Disabled nodes and disabled sections
do not load external files. Crosstalk Cancellation retains its existing behavior:
invalid measurements omit that node with a warning.

Large preparations and a replacement running alongside the old pipeline share
a native memory budget: 4 GiB on 64-bit systems and 1 GiB on 32-bit systems.
This is separate from the SFZ bank budget and is not a limit on total process
RSS. If preparation cannot fit, reduce the instrument budget, filter size, or
number of asset effects before retrying.

## Limitations

FIR Crossover, 5Band FIR PEQ, Group Delay EQ, and Group Delay PEQ are
supported. PipeTune regenerates their convolution coefficients from the preset
parameters and the active sample rate. FIR Crossover follows the preset's Ch
selection: two selected channels pass through unchanged with zero added latency;
an even selection from 4 through 16 channels splits the input stereo pair into
frequency bands. Select All to use a multichannel bus; the default stereo pair
and other stereo-pair selections pass through even on a wider bus. Mono and
odd channel selections are omitted with a warning.

Level Meter, Note Spectrogram, Pitch Meter, Oscilloscope, Spectrogram, Spectrum Analyzer,
Stereo Meter, Chroma Spiral, and Analog Meter are ignored without warnings. These nine
analyzers add no processing nodes,
latency, or transfers between buses. Effects that also change the sound remain
active even when they include a visualizer.
