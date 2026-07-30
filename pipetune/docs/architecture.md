# PipeTune MVP architecture

## Scope and integration choice

PipeTune is a normal per-user PipeWire client process, not a PipeWire daemon
plugin or a PulseAudio loadable module. It publishes an `Audio/Sink` node and a
linked output stream. This keeps the implementation on PipeWire's public client
API and covers native PipeWire applications as well as PulseAudio applications
served by `pipewire-pulse`.

Each package builds EffeTune, yyjson, and PFFFT from source for one target
architecture. The PipeTune executable does not statically contain the
EffeTune engine. It explicitly loads one validated private shared backend:
`libeffetune-dsp-scalar.so` uses portable scalar PFFFT, while
`libeffetune-dsp-simd.so` uses the package architecture's SIMD policy and GCC
auto-vectorization. No WebAssembly or backend for another architecture is
loaded.

Scalar is the compatibility default. SIMD CPU support is checked before
`dlopen`, and both libraries must match PipeTune's ABI and complete DSP
catalog. See [the DSP backend notes](dsp-backends.md) for exact compiler flags,
architecture requirements, fallback, and benchmarking.

## Data flow

```text
                      PipeWire graph

application streams ──> PipeTune Audio/Sink
                              |
                       F32P capture callback
                              |
            selected native EffeTune backend pipeline
                     or explicit bypass
                              |
                    preallocated planar ring
                              |
                       F32P output callback
                              |
                       physical Audio/Sink
```

The capture media format, EffeTune DSP, and playback media format share the
resolved rate R. The playback node requests a potentially different PipeWire
graph/output rate H, allowing PipeWire to resample R to a rate accepted by the
physical output. The channel layout remains fixed for one process run. The
capture callback copies one bounded block into preallocated planar storage,
processes it in place, and writes it to the single-producer/single-consumer
ring. The output callback reads available frames and supplies silence on
underrun. If the ring is full, the oldest unread input is discarded. Both
conditions are counted and exposed by the status command.

No mutex, allocation, filesystem access, JSON parsing, socket operation, or
DSP destruction occurs in a PipeWire process callback. Preset construction
happens on the control thread. A hazard-protected pipeline slot swaps the
prepared object atomically and defers reclamation until no real-time callback
can still reference it.

## Idle processing

PipeTune uses three independent but cooperating idle mechanisms:

1. Capture and playback advertise `node.pause-on-idle=true` and
   `node.suspend-on-idle=false`. Capture starts with `node.always-process=true`
   only for negotiation and effective-default activation, then returns to
   passive scheduling after a PipeWire core sync.
2. Capture GAP headers and EMPTY planar chunks become intentional zero frames
   in the internal ring. A wholly neutral playback buffer carries EMPTY on
   every chunk and GAP on its header, while unrelated flags are preserved.
3. An audio-thread-owned controller scans every channel for exact-zero input.
   After five seconds it may reset and skip the DSP once final output has also
   qualified for one second.

The default `conservative` output rule accepts finite samples at or below
-150 dBFS. The `exact` rule requires mathematical zero. Input is exact-zero
only in both modes, so low-level signal and dither wake or retain processing.
States are `active`, `draining`, and `sleeping`.

The EffeTune engine reset retains instances, parameters, assets, and pipeline
configuration while clearing every kernel, arena, and telemetry state. It is
guarded against allocation and can run through the hazard-protected active
pipeline on the real-time thread. A reset failure prevents sleep for that
silent interval. While sleeping, zero blocks become ring gaps without invoking
the DSP. A nonzero sample resumes processing in the same block. Pipeline
revision, rate, backend, and policy changes also restart the controller.

The PipeWire paused state and DSP sleep state are deliberately separate. When
PipeWire pauses both streams, callbacks stop entirely. Entry into that state
discards the internal ring and flushes both PipeWire stream queues without
draining. Fully consumed capture chunks are recycled with zero valid size so a
property-only graph wake cannot publish retained sample storage as fresh PCM.
Playback emits GAP if it restarts before fresh capture data. The first resumed
capture callback resets the active DSP through its real-time owner before
processing that data, preventing the previous playback interval's queued PCM or
DSP state from leading the new interval. If an application keeps callbacks
active by continuously writing zero, the DSP controller removes the native
processing cost while retaining bounded input scanning and buffer handling. See
[the DSP idle notes](dsp-idle.md) for exact state transitions, control commands,
and operational limits.

## Preset to native DSP mapping

At build time, `tools/generate-dsp-catalog.mjs` combines:

- EffeTune's `dsp/registry.inc`;
- display names and native type names in `plugins/plugins.txt`; and
- each native plugin's `params.json`.

The generated catalog packs JSON parameters into the exact native ABI expected
by EffeTune. Tests compare those packed parameters with EffeTune's JavaScript
packer and compare native PCM output with EffeTune's parity corpus.

At load time, `src/dsp_pipeline.cpp`:

1. bounds the input to 8 MiB and parses it with vendored yyjson;
2. accepts the canonical `pipeline` array and EffeTune's legacy forms;
3. applies enabled Section, bus, and channel routing;
4. omits disabled nodes;
5. warns and omits unknown or external-asset-dependent nodes;
6. creates up to 96 native DSP instances; and
7. configures one EffeTune engine for the complete graph.

Invalid JSON, invalid routing, invalid known-DSP parameters, or native engine
errors reject the complete load. A failed live load leaves the previous
pipeline active.

## Output selection

The registry tracker accepts only non-virtual `Audio/Sink` nodes with readable
and writable effective-volume controls and always excludes PipeTune's own node.
The user's optional preference is a stable `node.name`; the CLI and GTK
application do not perform target selection themselves.

The tracker applies this order:

1. use the preferred output when it is available;
2. otherwise use the physical system default as a fallback;
3. if the default has not yet been observed, retain a usable fallback or use
   the highest-priority eligible physical output; and
4. report output as unavailable when no eligible node exists.

An unavailable preference remains configured. Registry hotplug therefore
restores it automatically when the matching `node.name` returns. Clearing the
preference switches back to system-default tracking.

Changing the target first destroys and recreates the non-real-time output
stream. Queued frames for the previous device are discarded so stale audio is
not replayed after a switch. If the new target's capabilities resolve to
different R or H values, the sample-rate transaction described below also
rebuilds the DSP and reconnects both audio streams.

When every physical output disappears, PipeTune destroys the playback stream,
reports a null effective target with reason `unavailable`, and releases its
effective-default claim. The daemon and registry monitoring remain alive. A
new device creates and negotiates a playback stream first; only then does
PipeTune reclaim the effective default and resume audio. A retained preference
is not cleared during this state.

## System-volume ownership

The selected physical sink is the only system-volume gain stage. PipeTune does
not multiply PCM by the virtual sink volume in either preset or bypass mode.
Both PipeTune stream nodes clamp their channel-mixer gain to unity and disable
session-state property restoration. Per-application stream volume still
belongs to PipeWire, while gain deliberately introduced by a DSP preset remains
part of DSP processing.

PipeTune chooses one physical control surface from the sink's registry
properties:

- A card-backed sink declaring `device.routes` is associated with its
  `device.id` and `card.profile.device`. PipeTune binds that `pw_device`,
  subscribes to its active `SPA_PARAM_Route`, and treats the route's
  `SPA_PROP_channelVolumes` and mute as the effective state. Writes use
  `pw_device_set_param` with the active route index, profile-device identifier,
  and `SPA_PARAM_ROUTE_save`.
- A route-less software sink is controlled through the node's
  `SPA_PARAM_Props`. Writes normalize `SPA_PROP_volume` to unity and set the
  effective channel volumes and mute state.

PipeTune never writes node volume properties for a route-backed sink. Such a
node can expose the software portion of a hardware/software volume split, so
writing it while retaining the device route's hardware volume would multiply
the two gains. On startup and output switches, PipeTune reapplies the current
active route before playback begins; this also clears a stale software gain
left by an earlier process.

In both control surfaces, `SPA_PROP_channelVolumes` is treated as the effective
linear gain and is never multiplied by `SPA_PROP_volume` or
`SPA_PROP_softVolumes`. The virtual sink uses the public
`pw_stream_get_control` and `pw_stream_set_control` API as a desktop control
surface; its controls are not an additional PCM gain stage.

Startup and output switches use the selected physical node as the source of
truth. Its effective channel volumes and mute state are published to the
virtual sink before readiness is reported, so switching devices does not carry
the previous device's level forward. After that initialization, revisions from
either side are synchronized bidirectionally. A PipeWire core sync separates
an internal virtual-control publication from a later user change, preventing a
stale callback from being mistaken for new input. On layouts with different
channel maps, matching positions are copied directly and unmatched physical
channels retain their relative balance as the virtual master changes.

Route-backed nodes require a readable and writable active route plus device
write and execute permissions. Route-less nodes require readable and writable
effective properties plus node read, write, and execute permissions. PipeTune
does not fall back to node writes when a declared device route is unavailable.
A malformed parameter, subscription loss, or physical write failure removes
that node from selection and invokes the normal fallback rules. This failover
preserves the single-stage-volume invariant instead of silently reintroducing
attenuation in PipeTune's PCM path.

## Sample-rate selection and transitions

PipeTune supports two user policies and no separate oversampling multiplier:

- **Max** selects the highest of 44.1, 48, 96, 192, and 384 kHz accepted by
  the selected physical output.
- **Fixed** uses one of those five values as R regardless of device support.

Each physical output is bound for PipeWire `EnumFormat` enumeration. Discrete,
inclusive range, and stepped-rate choices are normalized and retained in the
registry tracker. Incomplete enumeration remains explicitly unknown rather
than being interpreted as unsupported. The active physical-device rate is
observed independently and is reported as zero while the device is idle.

The daemon owns all resolution:

1. Max with known capabilities chooses the highest supported selectable value
   and sets H equal to R.
2. Max with unknown capabilities retains the current R and H; the initial
   fallback is 48 kHz.
3. If known capabilities contain none of the five selectable values, Max uses
   R = 48 kHz and selects a device-compatible H using the fixed fallback rule.
4. Fixed always sets R to the requested value. If supported, H equals R.
   Otherwise H is the greatest supported rate not above R, or the device
   minimum when no supported rate is below R.

The playback media format remains R when R and H differ, so PipeWire performs
the resampling. The capture virtual sink advertises `node.rate=1/R`; the
playback stream advertises `node.rate=1/H`. This property is a graph-rate
request, not a guaranteed global-clock change. **Suggest** leaves it as a
preference. **Force** adds `node.force-rate=0` to the playback node, asking
PipeWire to use the denominator of `node.rate` while that node is active.
PipeTune never writes a global PipeWire clock setting.

A policy, selected target, or capability change schedules an immediate
non-real-time transition. Automatic changes are coalesced and explicit control
requests are serialized. A transition:

1. rebuilds the active preset or bypass pipeline for the new R and stages it;
2. disconnects capture and playback, clears queued frames, and applies the new
   R, H, and enforcement properties;
3. reconnects both streams, re-synchronizes the selected physical volume, and
   waits for the required formats and volume controls to become ready;
4. commits the staged pipeline and publishes the final state; or
5. reconnects the previous pipeline, rates, and enforcement after any failure.

A short silent interval is intentional. Failure to apply a new policy is
nonfatal after successful rollback and is retained in status. Only failure to
restore the previous working state terminates the daemon. Preset load and
bypass requests are rejected while a rate transaction is active.

## Default-sink ownership and fail-open behavior

PipeTune writes only the effective `default.audio.sink` metadata key. It never
writes WirePlumber's persistent `default.configured.audio.sink` selection.
Startup is reported ready only after:

1. the virtual sink negotiated its format;
2. the selected physical output's effective volume was read and published to
   the virtual sink;
3. a physical output stream negotiated its format; and
4. making the virtual sink effective completed a PipeWire core round trip.

On `SIGINT` or `SIGTERM`, PipeTune writes the current physical sink back to the
effective-default metadata and waits for another core round trip before
exiting.

An uncatchable signal cannot run that path. The installed systemd user unit
therefore uses two independent recovery mechanisms:

- removal of the crashed virtual node lets WirePlumber rescan its configured
  default; and
- `ExecStopPost` starts a separate `pipetune --restore-default` process that
  enumerates the surviving graph and explicitly selects a physical sink.

The service uses `Restart=on-failure` with a one-second delay. The restoration
helper and crash integration test do not depend on state held by the crashed
process.

## Local control

The default endpoint is
`$XDG_RUNTIME_DIR/pipetune/control.sock`. Its directory and socket are
user-only, peers are checked with `SO_PEERCRED`, messages are bounded
newline-framed JSON, and only the owning effective user is accepted.

Supported commands are:

- status;
- load and atomically activate another `.effetune_preset`;
- bypass DSP and atomically activate pass-through processing;
- set a preferred physical output by `node.name`;
- clear the preferred output and follow the physical system default;
- set a Max/fixed and suggest/force sample-rate policy;
- set the scalar or SIMD native DSP backend;
- set the conservative or exact DSP idle policy; and
- subscribe to an initial status event and later status publications.

Successful status and mutation replies include the preference, effective
target, selection reason, sorted eligible output list, per-output rate
capabilities, configured policy, R, H, active physical rate, fallback,
transition state, configured and effective DSP backends, per-backend
availability and CPU requirements, DSP idle policy and controller state,
cumulative skipped frames and sleep transitions, PipeWire graph-idle state,
and the latest diagnostics. Registry, default device, capability, preference,
final rate, backend, idle-policy, and stream-state changes publish fresh state
to subscribers.

The subscriber server uses an `eventfd` wakeup and bounded, coalescing output
per client. Preset activation and relevant PipeWire state transitions request
a fresh publication outside the real-time callbacks. Slow subscribers cannot
accumulate an unbounded history. The server runs outside the PipeWire process
callbacks. There is no network listener.

## Managed startup and preferences

The systemd user unit starts `pipetune daemon` with the optional configuration
path `$XDG_CONFIG_HOME/pipetune/environment`. A valid absolute
`PIPETUNE_PRESET` assignment constructs the native DSP graph. A missing file,
an empty configuration without that assignment, or an explicit saved bypass
selection creates no EffeTune engine and copies captured samples into the
output ring unchanged.

An optional `PIPETUNE_TARGET` assignment stores the preferred physical
`node.name`. Its absence selects system-default mode.

`PIPETUNE_RATE` stores `max` or one of `44100`, `48000`, `96000`, `192000`,
and `384000`. `PIPETUNE_RATE_ENFORCEMENT` stores `suggest` or `force`.
Missing rate assignments select Max-and-suggest. `PIPETUNE_DSP_BACKEND` stores
`scalar` or `simd`, with missing assignment selecting scalar.
`PIPETUNE_DSP_IDLE_POLICY` stores `conservative` or `exact`, with a missing
assignment selecting conservative. Preset, output, rate, backend, and idle
updates use one atomic writer and preserve the other selections.

Malformed configuration, an unavailable preset, or a preset that fails
validation also degrades to bypass instead of terminating the audio service.
The diagnostic is retained in runtime status so the CLI and GTK application
can distinguish an intentional bypass from a configuration failure. Live load
and bypass commands atomically replace the pipeline slot and publish the
resulting state.

The daemon discovers both backend libraries at startup. Configured SIMD falls
back to scalar when its CPU, file, ABI, or catalog validation fails. Failure of
the mandatory scalar backend also degrades preset processing to bypass. A live
backend change constructs a fresh active preset pipeline on the control
thread, atomically replaces the slot, and resets EffeTune state without
resetting the surrounding audio counters. A failed build retains the old
pipeline, and backend changes are rejected during sample-rate transitions.

The `setup` and `unsetup` coordinators run external `systemctl` and
`pipetune-gtk` operations with direct argument vectors and no shell. Default
sink restoration runs inside the unsetup process. Setup validates an explicit
preset before external changes, snapshots the previous configuration and
service state, enables and restarts the unit, verifies it is active, restores
managed autostart state, and launches GTK detached. A later failure triggers
best-effort rollback.

Unsetup first installs a same-name user XDG autostart mask. A custom existing
override is moved to a reserved non-desktop backup and is never overwritten.
It then remotely quits GTK, disables and stops the service, and restores a
physical default sink. Configuration purge is deferred until those required
shutdown operations succeed. Both commands reject effective user ID zero.

## GTK control plane

`pipetune-gtk` is a single-instance `GtkApplication`. Its GIO client keeps one
asynchronous subscription connection and uses separate asynchronous requests
for preset changes, output preference changes, rate-policy changes, DSP
backend changes, and DSP idle-policy changes. Runtime counters and cumulative
native EffeTune processing time are published once per second; the GUI derives
a per-frame interval average. It divides that average by the input-frame
duration implied by the negotiated sample rate to show DSP load, where 100% is
the theoretical processing deadline. A retry timer reconnects a lost
subscription; status itself is not polled.

The tray backend discovers a StatusNotifierItem host first. If none is
available on X11, it creates the same `GtkStatusIcon`/XEmbed compatibility
path used by elder-terms. A hidden start remains unmapped even when neither
tray transport has a host, ensuring desktop-session autostart never opens a
GTK window. A later explicit application or tray activation presents the
existing single-instance window.

Startup persistence is separate from daemon control. The GUI and CLI
atomically write the shared optional
`$XDG_CONFIG_HOME/pipetune/environment` file with user-only permissions. A
connected preset or bypass change is persisted only after the daemon accepts
it; a disconnected DSP selection is saved for the next daemon start. Output
selection is disabled while disconnected. A connected output change is also
persisted only after daemon confirmation, and persistence failure leaves the
live engine choice active. The GUI presents the engine's candidates and reason
without implementing fallback or hotplug policy.

Backend selection follows the same live-first persistence rule. While
disconnected, the GUI locally validates CPU support, the exact private shared
object, ABI, and catalog before saving a next-start choice. It passively
displays daemon-reported availability, configured/effective variants, startup
fallback, and diagnostics.

The rate controls remain editable while disconnected so a policy can be saved
for the next start. While connected, the GUI waits for the daemon to finish and
confirm the live transaction before persistence. It marks each fixed row using
daemon-reported device capabilities and passively shows R, H, the active
physical rate, fallback/resampling, transition state, and errors. It does not
resolve rates or implement oversampling.

Idle-policy selection uses the same live-first persistence rule and remains
editable while disconnected. The GUI passively shows configured policy,
active/draining/sleeping state, skipped frames, sleep transitions, and whether
both PipeWire streams are paused. It does not infer silence or run an
independent timer.

## Known MVP limits

- Linux PipeWire 0.3 and systemd user sessions only.
- Each package contains scalar and SIMD DSP objects for one target
  architecture; no cross-architecture portable runtime object is distributed.
- Standalone PulseAudio without `pipewire-pulse` is unsupported.
- DSPs requiring external assets are skipped.
- The channel count is fixed for one process run.
- Effective DSP latency is not yet published to the wider PipeWire graph for
  audio/video latency compensation.
- Fail-open recovery permits a short dropout.
