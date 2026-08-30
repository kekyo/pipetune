# PipeTune architecture

## Scope and integration choice

PipeTune is a normal per-user PipeWire client process. It is not a PipeWire
daemon plugin or a PulseAudio loadable module. The process publishes two
linked stream nodes that WirePlumber inserts as a transparent playback filter.
Native PipeWire applications and PulseAudio applications served by
`pipewire-pulse` therefore use the same path.

PipeTune owns only its filter nodes and EffeTune processing state. It does not:

- enumerate or select physical outputs;
- write PipeWire default-device metadata;
- read or write device, route, mute, or volume properties;
- store an output-device preference; or
- restore a default sink when it exits.

WirePlumber remains the sole owner of default-output selection and hotplug
policy. The desktop sound control remains the owner of the overall output
volume. PipeTune supports both WirePlumber 0.4 and 0.5 without changing this
division of responsibility.

Each package builds EffeTune, yyjson, and PFFFT from source for one target
architecture. The PipeTune executable explicitly loads one validated private
shared DSP backend. Scalar is the compatibility default. SIMD CPU support is
checked before `dlopen`, and every loaded library must match PipeTune's ABI and
complete DSP catalog. See [the DSP backend notes](dsp-backends.md) for the
backend variants, fallback rules, and benchmark procedure.

## Playback graph

The intended steady-state graph is:

```text
application 1 --+
application 2 --+--> PipeWire mix --> PipeTune filter input
application 3 --+                          |
                                           v
                                  EffeTune DSP or bypass
                                           |
                                           v
                                  PipeTune filter output
                                           |
                                           v
                             PipeWire master output volume
                                           |
                                           v
                               WirePlumber default device
```

Per-application volumes are applied to application streams before PipeWire
mixes them. The mixed PCM enters PipeTune once. In preset mode it passes
through the selected native EffeTune pipeline; in bypass mode it is copied
unchanged. The ordinary sink volume and mute are applied after the filter.

PipeTune publishes this node contract:

| Property | Filter input | Filter output |
| --- | --- | --- |
| `media.class` | `Audio/Sink` | `Stream/Output/Audio` |
| `node.name` | `pipetune_sink` | `pipetune_sink.output` |
| `node.link-group` | shared stable value | shared stable value |
| `filter.smart` | `true` | omitted |
| `filter.smart.name` | `net.kekyo.pipetune` | omitted |
| `media.role` | `DSP` | `PipeTune-Filter-Output` |
| `node.pipetune.internal` | `true` | omitted |
| `target.endpoint` | `endpoint.pipetune.playback` | omitted |
| `node.pipetune.target-endpoint` | `endpoint.pipetune.playback` | omitted |
| `node.passive` | omitted | `true` |
| `stream.dont-remix` | omitted | `true` |

Both nodes also publish planar float format, channel count, a shared node
group, `state.restore-props=false`, and channel-mixer bounds fixed at unity.
The output has no `target.object`; WirePlumber policy chooses and reconnects
the downstream sink. Only the output stream requests autoconnection. The input
exists as the policy-visible sink side of the filter pair.

### WirePlumber 0.5

WirePlumber 0.5 recognizes the input node's smart-filter properties and the
shared link group. Its smart-filter policy diverts matching playback streams
to the filter input and connects the filter output to the ordinary default
sink. A change to the desktop default output therefore changes the downstream
link without a PipeTune control request.

`pipetune setup` also installs the visibility component at the standard 0.5
configuration and data locations:

```text
$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d/60-pipetune-node-visibility.conf
$XDG_DATA_HOME/wireplumber/scripts/pipetune-node-visibility.lua
```

When `XDG_DATA_HOME` is unset, its standard `$HOME/.local/share` fallback is
used.

### WirePlumber 0.4

WirePlumber 0.4 does not implement the 0.5 smart-filter policy. `pipetune
setup` installs one configuration fragment and three runtime policy scripts:

```text
$XDG_CONFIG_HOME/wireplumber/policy.lua.d/60-pipetune-filter.lua
$XDG_CONFIG_HOME/wireplumber/scripts/pipetune-endpoint-client.lua
$XDG_CONFIG_HOME/wireplumber/scripts/pipetune-endpoint-device.lua
$XDG_CONFIG_HOME/wireplumber/scripts/pipetune-node-visibility.lua
```

The `60-` prefix makes the fragment run after WirePlumber's endpoint defaults
and before its `90-enable-all.lua`. It declares playback and capture roles,
paired endpoints, and replaces the stock 0.4 endpoint-client and
endpoint-device components with PipeTune's 0.4.17-derived compatibility
scripts. This also supplies the role and filter exclusions missing from early
0.4 releases.

Application playback streams are mixed at the playback endpoint and linked to
the filter input. Releases before 0.4.16 do not copy `target.endpoint` into the
session item, so the input also publishes
`node.pipetune.target-endpoint`; the compatibility device policy accepts
either property. The filter output uses a dedicated role and is excluded from
client endpoint routing, then the device policy links it to the current
default sink. The capture endpoint preserves ordinary source routing while
endpoint policy is active.

Both WirePlumber versions run the same visibility policy. It removes all
permissions for the marked filter input and the 0.4 endpoint backing nodes from
every client except WirePlumber, the PipeTune daemon, and the client that owns
the node. PipeTune retains access to the WirePlumber-owned endpoint backing
nodes. When a client owns an active audio playback or capture stream, the policy
temporarily restores that client's permissions so the stream can link through
the hidden endpoint. It removes the permissions again after the client's last
audio stream disappears. Desktop control panels and other clients without an
audio stream therefore do not receive the internal nodes.

Setup snapshots and atomically updates all six managed files, then restarts
`pipewire.service`, `wireplumber.service`, and `pipewire-pulse.service` in one
user-systemd transaction if any changed. WirePlumber 0.5 ignores the 0.4 Lua
configuration path and uses the smart-filter properties instead. `pipetune
unsetup` removes all six files and restarts the same audio stack when
necessary.

## Real-time data flow

The two PipeWire streams use F32P PCM with one plane per channel:

1. The input callback dequeues a capture buffer and validates its planes,
   strides, chunk bounds, and frame count.
2. It copies one bounded block into preallocated planar scratch storage.
3. A preallocated streaming converter changes negotiated PipeWire PCM to the
   configured DSP rate when those rates differ.
4. Preset mode processes that block in place with EffeTune unless silent-input
   suspension is already active; bypass mode skips the engine.
5. A second streaming converter returns fixed-rate DSP output to the
   negotiated PipeWire PCM rate.
6. The block is written to a preallocated single-producer/single-consumer
   planar ring.
7. The output callback reads available frames into its PipeWire buffer and
   smooths transitions into and out of underrun silence.

If the ring is full, the newest input tail that does not fit is discarded.
Overrun, underrun, and processing-error counts are exposed in runtime status.
Fully consumed input chunks are returned with zero valid size so a
property-only graph wake cannot republish retained PCM.

When an active PipeWire stream returns from `STREAMING` to `PAUSED`, PipeTune
flushes that stream's PipeWire queue and discards its internal output queue.
The next activation therefore starts behind the silence guard instead of
replaying PCM retained from the previous application stream.

No mutex, allocation, filesystem access, JSON parsing, socket operation, or
DSP destruction occurs in a PipeWire process callback. Preset construction
happens on the control thread. A hazard-protected pipeline slot swaps a
prepared pipeline atomically and defers reclamation until no real-time callback
can still reference it.

## Silent-input DSP suspension

The optional idle policy is `ignore` or 100 through 5000 ms in 100 ms steps.
Detection runs after input sample-rate conversion, at the DSP rate. A block is
silent only when every channel sample is positive or negative floating-point
zero; any other bit pattern, including a subnormal value, wakes the pipeline.
A mixed block is treated as active so its first nonzero sample is never lost.

After the first fully silent block, the active preset continues processing for
the configured number of DSP frames. This tail-draining interval intentionally
retains delay, reverb, and source-generator output. At the deadline, the
current output receives a 5 ms fade to exact zero and the EffeTune engine is
reset. Later silent blocks are zero-filled without entering the native DSP,
and the cumulative DSP frame/time counters stop advancing. The first later
nonzero block is processed immediately.

Pipeline replacement, rate changes, backend changes, and idle-policy changes
wake and reset the detector. Bypass has its own activity state and preserves
PCM without using the idle controller. Policy, activity, and generation are
published from one lock-free atomic word so a stale real-time callback cannot
overwrite a newer control-thread policy or bypass transition.

## Sample-rate negotiation and transitions

PipeTune supports two rate modes:

- Automatic advertises an F32P rate range from 8 kHz through 768 kHz on both
  filter nodes. PipeTune rebuilds the EffeTune pipeline at the negotiated PCM
  rate.
- Fixed requests one of 44.1, 48, 96, 192, or 384 kHz for both filter nodes.
  EffeTune remains at that selected rate. If PipeWire negotiates another PCM
  rate, PipeTune converts immediately before and after the DSP boundary.

Both filter nodes must negotiate the same PCM rate. A disagreement is an error.
The graph's actual time-domain rate is reported independently from
`pw_time.rate`. Under a fixed Suggest policy, PipeWire may select another PCM
or graph rate without changing the DSP rate. A Force request that the graph
does not apply is exposed as a rate diagnostic, while the internal rate bridge
keeps the selected DSP rate usable.

For a fixed rate, Suggest publishes `node.rate` as a preference. Force also
publishes `node.force-rate=0` on the output stream while it is active. Neither
mode edits PipeWire's global clock configuration. Enforcement has no effect in
Automatic mode because no fixed denominator is requested.

A live rate change runs outside the real-time callbacks:

1. serialize the request and mark the filter as transitioning;
2. disconnect both streams and discard queued PCM;
3. recreate their properties and format parameters for the new policy;
4. reconnect and wait for both formats to resolve;
5. rebuild the active preset or bypass pipeline at the negotiated rate; and
6. publish the final status or the transition diagnostic.

A short silent interval is intentional. The output uses a 5 ms fade-out,
suppresses at least the first 20 ms of queued PCM after a discontinuity, and
uses a 5 ms fade-in. Frames emitted while the input remains empty do not
consume that guard, so a delayed tail from a stopped stream cannot resume after
the initial silence. The same boundary smoothing covers PipeWire format loss
and ordinary ring underruns. Preset, bypass, and backend mutations are
serialized with the transition so no partially rebuilt state becomes visible.

## Preset to native DSP mapping

At build time, `tools/generate-dsp-catalog.mjs` combines:

- EffeTune's `dsp/registry.inc`;
- display names and native type names in `plugins/plugins.txt`; and
- each native plugin's `params.json`.

The generated catalog packs JSON parameters into the exact native ABI expected
by EffeTune. Tests compare those packed parameters with EffeTune's JavaScript
packer and compare native PCM output with EffeTune's parity corpus.

The pinned EffeTune 2.6.0 registry contains 94 native kernels. PipeTune can load
MD Simulator and all nine effects added in 2.5.0. It regenerates and stages the
convolution assets for FIR Crossover, 5Band FIR PEQ, Group Delay EQ, and Group
Delay PEQ from their serialized design parameters. The updated Tube Simulator
includes output-transformer magnetics, and Phase Select EQ accepts left/right
balance selection alongside frequency and phase.

At load time, `src/dsp_pipeline.cpp`:

1. bounds the input to 8 MiB and parses it with vendored yyjson;
2. accepts the canonical `pipeline` array and EffeTune's legacy forms;
3. applies enabled section, bus, and channel routing;
4. omits disabled nodes;
5. regenerates supported FIR assets and warns for unknown or unresolved
   stored-asset nodes;
6. creates up to 96 native DSP instances;
7. copies generated assets through PipeTune's pointer-safe backend extension;
8. configures one EffeTune engine for the complete routed pipeline; and
9. reads the configured pipeline latency from EffeTune's native ABI.

EffeTune 2.6 computes latency per channel while configuring the pipeline. It
delays shorter additive-merge and output paths so channels remain aligned, and
reports the resulting aggregate through `et_pipeline_latency`. PipeTune uses
that value directly instead of estimating latency from individual nodes.

PipeTune continues to consume EffeTune app pipeline documents rather than the
experimental Graph v1 document format. EffeTune's desktop Pipeline Analyzer
and OpenHome playback control are also outside PipeTune's filter and control
surfaces.

Invalid JSON, invalid routing, invalid known-DSP parameters, or native engine
errors reject the complete load. A failed live load leaves the previous
pipeline active.

## Local control

The default endpoint is `$XDG_RUNTIME_DIR/pipetune/control.sock`. Its directory
and socket are user-only, peers are checked with `SO_PEERCRED`, messages are
bounded newline-framed JSON, and only the owning effective user is accepted.
There is no network listener.

Supported commands are:

- status;
- load and atomically activate another `.effetune_preset`;
- bypass DSP and activate transparent pass-through;
- set an Automatic or fixed sample-rate policy;
- set the scalar or SIMD native DSP backend;
- set or ignore the silent-input DSP timeout; and
- subscribe to an initial status event and later publications.

Status contains processing mode, independent DSP activity, active preset,
input telemetry, the idle policy, DSP and graph rates, transition state, DSP
performance counters, configured and effective backend variants,
availability, fallback, and diagnostics. It contains no physical output,
default-sink, or volume state.

The subscriber server uses an `eventfd` wakeup and bounded, coalescing output
per client. Preset activation and relevant PipeWire state transitions request a
fresh publication outside the real-time callbacks. Slow subscribers cannot
accumulate an unbounded history.

The control service poll loop also observes an inotify descriptor for the
active preset. It watches both the file and its parent so in-place close-write,
atomic replacement, deletion, and recreation are detected. Reloading happens
on the control thread under the same pipeline-mutation boundary as an explicit
load. The candidate pipeline is parsed and fully constructed before the active
slot is replaced. Failure therefore preserves the previous pipeline, active
path, plugin count, and configuration revision while publishing a
configuration diagnostic. A later filesystem event retries the load. A
sample-rate transition defers the attempt until the transition completes, and
bypass removes the active watch.

## Managed startup and preferences

The systemd user unit starts `pipetune daemon` with the optional configuration
path `$XDG_CONFIG_HOME/pipetune/environment`. Supported assignments are:

- `PIPETUNE_PRESET` with an absolute preset path;
- `PIPETUNE_RATE` with `automatic` or a supported fixed rate;
- `PIPETUNE_RATE_ENFORCEMENT` with `suggest` or `force`;
- `PIPETUNE_DSP_BACKEND` with `scalar` or `simd`;
- `PIPETUNE_DSP_SIMD_VARIANT` with `auto` or an applicable fixed tier; and
- `PIPETUNE_DSP_IDLE_TIMEOUT` with `ignore` or 100 through 5000 ms in
  100 ms steps.

An absent preset selects bypass. Missing rate assignments select
Automatic-and-suggest. Missing backend assignments select scalar with
automatic SIMD dispatch preference. A missing idle assignment selects
`ignore`. Preset, rate, backend, and idle updates use one atomic writer and
preserve the other selections. Obsolete or unknown
assignments are rejected; `pipetune config reset` replaces such a file without
first parsing it.

Malformed configuration, an unavailable preset, or a preset that fails
validation degrades to bypass instead of terminating the audio service. The
diagnostic is retained in runtime status. Configured SIMD falls back through
usable lower variants or to scalar according to the backend rules.

The installed service is ordered after PipeWire and WirePlumber and uses
`Restart=on-failure`. Start limiting allows at most three starts in 30 seconds,
so a permanent startup error cannot repeatedly remove and recreate the audio
graph. It has no default-sink restoration command. Removing or stopping
PipeTune removes the filter nodes; WirePlumber continues to own the normal
device and volume policy.

The setup and unsetup coordinators invoke `systemctl` and `pipetune-gtk` with
direct argument vectors and no shell. Setup serializes per-user management
with an advisory lock and checks a versioned record below `XDG_STATE_HOME`,
the exact contents of all six managed WirePlumber files, service enablement
and activity, and the GTK autostart mask. Plain setup returns once these are
current; `--force` repeats the workflow. Setup snapshots the managed
configuration, WirePlumber 0.4/0.5 policy files, and service state before
mutation. A later failure triggers best-effort rollback. Unsetup removes the
completion record and only PipeTune-managed files, preserving a custom GTK
autostart override through its reserved backup.

## GTK control plane

`pipetune-gtk` is a single-instance `GtkApplication`. Its GIO client keeps one
asynchronous subscription connection and uses separate asynchronous requests
for preset, bypass, rate-policy, DSP-backend, and DSP-idle changes. There is no
output device UI or output control request.

During primary application startup, a separate GIO subprocess client invokes
the installed CLI as `pipetune setup --no-launch-gtk`. Control subscription
initialization waits for this asynchronous check. The UI remains responsive
and read-only while it is pending. A setup failure is retained in the action
log and sent through `GNotification` for a hidden launch, after which GTK
still starts its control subscription. Secondary application activations do
not run setup again.

EffeTune desktop presets are stored together in one JSON object, so a saved
choice is materialized as a deterministic private standalone snapshot before
the daemon loads it. When a valid catalog refresh changes the entry whose
snapshot path matches the daemon's active preset, GTK compares serialized
contents and atomically replaces only that snapshot. This gives the daemon's
ordinary active-file monitor one consistent source of reload events. Invalid
catalog refreshes, unchanged contents, and non-active entries do not touch the
active snapshot. The same reconciliation runs when daemon status first reports
a different active path, covering updates made while GTK was stopped.

Runtime counters and cumulative native EffeTune processing time are published
once per second. The GUI derives a per-frame interval average and divides it by
the frame duration implied by the negotiated input rate to show DSP Load. The
graph is capped at 100%, while overload text retains the actual value. A retry
timer reconnects a lost subscription; status itself is not polled. Sleeping
DSP activity replaces the Load percentage with `Suspended`.

The settings transaction applies live changes in rate, backend, idle policy,
then processing order. Apply persists the complete confirmed snapshot. Cancel
restores the captured live baseline before hiding the window. The Advanced
default is bypass, Automatic-and-suggest, scalar, automatic SIMD preference,
and ignored silent-input suspension.

The tray backend discovers a StatusNotifierItem host first. If none is
available on X11, it uses the `GtkStatusIcon` and XEmbed compatibility path. A
hidden start remains unmapped even when neither tray transport has a host; a
later explicit activation presents the existing singleton window.

## Known limits

- Stereo is the managed-service layout. Direct runs accept one through eight
  channels, but no live channel-layout control is provided.
- FIR Crossover requires a 4, 6, or 8 channel output bus. PipeTune omits it
  with a warning when the active layout is incompatible.
- Room EQ and IR Reverb remain unresolved stored-asset DSPs. Room EQ opens
  EffeTune's browser-backed
  [measurement store](https://github.com/Frieve-A/effetune/blob/7d8db4dbe44f63fa004c993490976699dd621839/plugins/eq/room_eq.js#L1572-L1605),
  while IR Reverb looks up its serialized identifier in the
  [IR library](https://github.com/Frieve-A/effetune/blob/7d8db4dbe44f63fa004c993490976699dd621839/plugins/reverb/ir_reverb.js#L766-L794).
  Their source PCM is not carried by `.effetune_preset`, so PipeTune omits the
  nodes with warnings.
- PipeTune does not provide a machine-wide service shared by multiple logged-in
  users.
