# Details

## Crosstalk Cancellation

PipeTune can apply EffeTune's Crosstalk Cancellation to stereo speaker playback.
In EffeTune, measure both speakers at each ear position, assign all four paths,
and save the preset. The two paths for each ear must belong to the same
single-point measurement. See the
[EffeTune instructions](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/plugins/spatial.md#crosstalk-cancellation).

PipeTune automatically reads `measurement-backups/<measurement-ID>.json`
beside EffeTune's saved preset collection. The directory is
`$XDG_CONFIG_HOME/effetune`, or `~/.config/effetune` when unset.
EffeTune 2.10.0 desktop automatically
[backs up measurements as JSON](https://github.com/Frieve-A/effetune/blob/v2.10.0/electron/measurement-backup-ipc.cjs)
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

[Spatial Mapper](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/spatial-mapper/index.md)
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

[TV Audio Simulator](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/tv-audio-simulator/index.md)
supports 44.1, 48, 88.2, 96, 176.4, 192, 352.8, and 384 kHz.
If Automatic negotiates an unsupported rate, select a supported fixed rate.
Select a mono channel or stereo pair to process; the
[upstream kernel](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/plugins/lofi/tv_audio_simulator/kernel.cpp)
processes only the first two channels of an All selection and leaves later
channels unchanged, without matching their delay. Use separate pair-selected
nodes when processing multiple pairs.

Broadcast Off leaves receiver noise enabled, and Mix = 0 retains the dry
path's delay. The default DSP silence policy, Ignore, keeps receiver noise
running on silent input. An explicit silence timeout fades and suspends it
until input resumes.

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
and Stereo Meter are ignored without warnings. They add no processing nodes,
latency, or transfers between buses. Effects that also change the sound remain
active even when they include a visualizer.

Room EQ and IR Reverb remain unsupported. A Room EQ preset references
measurement data that EffeTune resolves through its
[measurement store](https://github.com/Frieve-A/effetune/blob/bedc6c662a6edc88c9644b7e00cec9122a250cfb/js/measurement-store/client.js#L71),
and IR Reverb resolves a content identifier through its
[IR library](https://github.com/Frieve-A/effetune/blob/bedc6c662a6edc88c9644b7e00cec9122a250cfb/plugins/reverb/ir_reverb.js#L766-L802).
The required PCM is not contained in `.effetune_preset`, so PipeTune omits
these nodes with warnings.
