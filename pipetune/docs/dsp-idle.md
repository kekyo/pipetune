# DSP and PipeWire idling

PipeTune reduces idle-session work without classifying ordinary quiet audio as
silence. The target case is a desktop application that keeps its stream
running and continuously sends exact-zero PCM after audible playback has
stopped. The implementation has three cooperating layers.

## PipeWire graph idling

Both PipeTune nodes set `node.pause-on-idle=true` and
`node.suspend-on-idle=false`. The virtual sink initially uses
`node.always-process=true` so both formats can negotiate and PipeTune can take
effective-default ownership without a startup race. After initial readiness,
and after each sample-rate transition, PipeTune removes that override and
waits for the corresponding PipeWire core sync.

PipeWire can then pause the capture and playback streams when the graph has no
work. Pausing stops the process callbacks themselves and is the cheapest idle
state. Avoiding suspension keeps the negotiated nodes available for prompt
reuse. Status reports `pipeWireIdle=true` only while both streams are in
PipeWire's `PAUSED` state.

## EMPTY and GAP preservation

PipeTune preserves neutral-media information across its internal planar ring:

- a capture buffer with `SPA_META_HEADER_FLAG_GAP` is treated as exact zero;
- each capture plane with `SPA_CHUNK_FLAG_EMPTY` is treated as exact zero;
- the ring records whether each frame is exact zero without storing PCM for
  an explicitly written gap; and
- a completely neutral playback buffer carries both
  `SPA_CHUNK_FLAG_EMPTY` and `SPA_META_HEADER_FLAG_GAP`.

Other chunk and header flags are left unchanged. A mixed playback buffer
remains ordinary PCM because PipeWire's EMPTY/GAP markers describe the whole
buffer, not individual frames. This prevents stale buffer contents from being
interpreted as audio and allows downstream PipeWire nodes to retain the graph's
neutral-media semantics.

## Exact-zero DSP sleep

When callbacks remain active, an allocation-free controller observes every
channel before volume and DSP processing. Input silence always means
mathematical zero (`sample == 0.0F`); there is no input amplitude threshold.
Consequently dither, noise, non-finite samples, and any real signal keep the
DSP awake.

The controller has three states:

- `active`: recent input contains a nonzero sample;
- `draining`: input is zero but the five-second input interval, the DSP tail,
  or the one-second output interval has not settled; and
- `sleeping`: the active pipeline has been reset and exact-zero input blocks
  bypass DSP processing.

Sleep requires at least five seconds of exact-zero input on every channel and
at least one second of qualifying final DSP output. The output rule is selected
by the idle policy:

| Policy | Output required for one second | Tradeoff |
| --- | --- | --- |
| `conservative` | finite samples no greater than -150 dBFS in magnitude | Default; sleeps for effects whose tail becomes inaudible but not mathematically zero |
| `exact` | exact zero on every channel | Never truncates a nonzero tail, but some feedback or noise-producing presets may never sleep |

Immediately before entering `sleeping`, PipeTune resets every EffeTune kernel,
its arena, and telemetry state through the real-time-safe engine reset API. If
reset fails, PipeTune increments the processing-error counter and remains in
`draining` for that silent interval.

While sleeping, PipeTune still scans input for exact zero, advances its volume
ramp, and forwards an intentional gap through the ring, but it does not invoke
the active DSP pipeline. Any nonzero input wakes processing in the same
callback block. A preset replacement, sample-rate change, backend change, or
idle-policy change also returns the controller to `active`. The first resumed
block is processed from reset DSP state; no leading audible block is discarded.

## Configuration and control

The managed startup file stores:

```text
PIPETUNE_DSP_IDLE_POLICY=conservative
```

Accepted values are `conservative` and `exact`. A missing assignment uses
`conservative`. Configuration reset also selects `conservative`. A direct,
non-managed run can use:

```sh
pipetune --preset /absolute/path/to/preset.effetune_preset \
  --dsp-idle-policy exact
```

Inspect or change the managed daemon with:

```sh
pipetune idle get
pipetune idle get --json
pipetune idle set conservative
pipetune idle set exact
```

`idle set` applies the policy live and saves it after daemon confirmation. If
the daemon is unavailable, it saves the policy for the next start. A daemon
rejection does not overwrite the startup choice. The GTK **DSP idle** section
uses the same live-first persistence rule.

Human-readable status includes the policy, controller state, skipped frames,
sleep transitions, and whether the PipeWire graph is paused. JSON status
exposes the same values as `dspIdlePolicy`, `dspIdleState`,
`dspIdleSkippedFrames`, `dspIdleSleepTransitions`, and `pipeWireIdle`.
Counters are cumulative for the daemon process and are preserved when the
controller restarts after a policy, pipeline, or rate change.

## Operational limits

- Exact-zero detection is deliberately not a general silence gate. An
  application that emits dither or low-level noise keeps the DSP active.
- PipeWire graph pausing and DSP sleeping are independent. Either one may be
  active without the other.
- DSP sleep removes native pipeline processing, but active callbacks still
  perform bounded buffer handling and exact-zero scanning.
- Actual CPU and battery savings depend on the preset, sample rate, quantum,
  PipeWire graph, and the behavior of source applications.
