# PipeTune transparent-filter architecture

## Scope and integration choice

PipeTune is one per-user PipeWire client process. It is not a PipeWire daemon
plugin, a PulseAudio module, or a selectable desktop output device. For every
eligible physical `Audio/Sink`, the process publishes one target-specific
smart-filter main node and one playback stream. WirePlumber inserts that hidden
pair when an application targets the corresponding physical output.

The desktop therefore continues to own all user-facing routing:

- the default output remains the real DAC, speakers, or headphones;
- per-application output choices continue to name real devices;
- PipeTune nodes are hidden from ordinary clients; and
- the physical sink remains the final mute and volume control.

One `pipetune` executable implements the audio and control behavior for both
supported WirePlumber generations. The package installs both policy variants;
there is no install-time OS probe and no alternative daemon binary.

Each package builds EffeTune, yyjson, and PFFFT from source for one target
architecture. The executable loads one validated private native backend.
`libeffetune-dsp-scalar.so` is the compatibility implementation, while the SIMD
libraries use the package architecture's supported ISA tiers. See
[the DSP backend notes](dsp-backends.md) for validation and fallback details.

## Audio and volume flow

```text
application streams targeting physical output A
                    |
       PipeWire per-application volume and mix
                    |
        hidden PipeTune filter for output A
                    |
          F32P capture process callback
                    |
       EffeTune DSP or pass-through pipeline
                    |
         preallocated planar audio ring
                    |
          F32P playback process callback
                    |
             physical output A
                    |
        physical mute and volume control
```

There is one such runtime for each eligible output. Streams targeting output A
never pass through output B's runtime. PipeWire mixes application streams at
the filter main node before PipeTune receives the block; the corresponding
playback stream feeds the originally selected physical sink.

PipeTune clamps its stream channel-mixer range to unity, disables restored
stream properties, and never mirrors or writes physical volume. The old
virtual-sink gain stage no longer exists. Per-application gain is applied
before DSP, gain deliberately produced by a preset is part of DSP, and the
physical sink gain is applied once after DSP.

## Runtime WirePlumber selection

The same package installs these four files:

```text
/usr/share/wireplumber/main.lua.d/85-pipetune.lua
/usr/share/wireplumber/wireplumber.conf.d/90-pipetune.conf
/usr/share/wireplumber/scripts/pipetune/policy-0.4.lua
/usr/share/wireplumber/scripts/pipetune/policy-0.5.lua
```

WirePlumber 0.4 loads `main.lua.d` and starts `policy-0.4.lua` alongside its
device-monitor components. This publishes the setup handshake without waiting
for every ALSA device to activate. Its main configuration also merges
`wireplumber.conf.d`, so the 0.5 component script guards itself with the
0.5-only profile feature API and exits without publishing policy state on 0.4.
WirePlumber 0.5 no longer loads the 0.4 Lua configuration fragments and starts
`policy-0.5.lua` from its component configuration. An OS upgrade naturally
selects the new policy the next time WirePlumber starts; the PipeTune
executable and environment file remain the same.

Both policies publish a `pipetune-policy` metadata object with:

- `protocol.version=1`;
- `policy.backend=wireplumber-0.4` or `wireplumber-0.5`; and
- `policy.state=ready`.

The daemon accepts only this complete handshake. Each new filter is statically
disabled and is published as `filter.enabled=true` only after both PipeWire
streams and their exact formats are ready. Missing or incompatible policy
metadata therefore leaves the original direct route intact.

### WirePlumber 0.5 policy

The 0.5 implementation integrates with WirePlumber Smart Filters. It writes
the target filter's `filter.smart.disabled` and `filter.smart.after` values to
the standard `filters` metadata. Existing target-compatible smart filters are
ordered before PipeTune; declared before/after constraints are represented as
a graph and a cycle publishes a conflict instead of installing an ambiguous
chain.

### WirePlumber 0.4 compatibility policy

WirePlumber 0.4 has no equivalent Smart Filters policy. The compatibility
script observes output streams, explicit `target.object`/`target.node`
metadata, and the current default output. For each target with a ready PipeTune
filter, it builds the same ordered chain by temporarily changing stream target
metadata. It remembers the original target and restores it whenever the filter
is disabled or removed.

Both variants deny ordinary clients access to PipeTune's main and playback
nodes while retaining access for WirePlumber and the owning PipeTune client.
Consequently Ubuntu's sound panel and equivalent applications list only the
physical outputs.

## Physical-output discovery

PipeTune binds every registry `Audio/Sink` and enumerates its format
capabilities. A node receives a DSP runtime only when it:

- has a stable PipeWire identity and is backed by a device;
- uses the local ALSA or BlueZ device API;
- is not virtual, network, encoded-only, or another smart filter; and
- declares one through eight unique, supported channel positions.

Rejected nodes remain visible to the desktop and use their direct route.
Unsupported layouts and invalid rate policy are reported as errors; other
ineligible classes are reported as intentional bypasses. Additions, removals,
format changes, and active-rate changes reconcile only the affected output.

Each accepted output gets stable runtime properties derived from its PipeWire
global ID:

- main node `pipetune.filter.ID` with media class `Audio/Sink`;
- playback node `pipetune.filter.ID.output`;
- shared `node.link-group` and `filter.smart.name` values; and
- `pipetune.target.node` naming the physical `node.name`.

The main node is not targetable as a user output. The playback stream is
passive, targets the physical node directly, and is configured not to reconnect
silently to a different device.

## Per-output DSP and real-time path

The startup preset is parsed once as a reusable recipe. A fresh native
EffeTune pipeline is then built for each output's exact sample rate, channel
count, and channel positions. This avoids forcing all hardware through the
least-capable output's format.

The capture callback processes a bounded F32P block in place and writes it to
that output's single-producer/single-consumer planar ring. The playback callback
reads available frames and supplies silence on underrun. If the ring is full,
the oldest unread frames are discarded. Overrun, underrun, processed-frame,
DSP-time, and processing-error counters are retained per output and aggregated
for status.

No mutex, allocation, filesystem access, JSON parsing, socket operation, or DSP
destruction occurs in a PipeWire process callback. Preset and backend
construction happens on the control thread. Hazard-protected pipeline slots
atomically exchange prepared pipelines and defer reclamation until no callback
can still reference the old object.

A DSP processing exception increments the error counter and passes the block
through. A stream construction or negotiation failure disables only that
output's filter so WirePlumber can preserve the direct route.

## Preset mapping and live replacement

At build time, `tools/generate-dsp-catalog.mjs` combines EffeTune's native
registry, plugin names, and parameter definitions. At load time,
`src/dsp_pipeline.cpp` bounds and parses the preset, applies section/bus/channel
routing, omits disabled and unsupported external-asset nodes, and constructs up
to 96 native DSP instances.

Invalid JSON, invalid known-DSP parameters, invalid routing, and engine errors
reject the complete replacement. A live preset, bypass, or backend change first
prepares replacement pipelines for every running output. All slots are changed
only after every preparation succeeds, so one incompatible output cannot leave
the session with a partially updated preset.

## Per-output sample-rate policy

One saved policy is resolved independently for every physical output:

- **Max** chooses the highest of 44.1, 48, 96, 192, and 384 kHz accepted by
  that output.
- **Fixed** keeps the selected value as that output filter's DSP rate.

When a fixed DSP rate is unsupported, the output request uses the greatest
supported rate not above it, or the device minimum if none is below it.
PipeWire performs the conversion. Unknown capabilities retain the current
choice, with 48 kHz as the initial fallback.

The main node requests the DSP rate with `node.rate`. The playback node requests
the resolved physical rate. **Suggest** leaves this as a preference; **Force**
also supplies `node.force-rate=0` while that playback node is active. PipeTune
never writes PipeWire's global clock configuration.

A live policy change builds the required new pipelines and streams for all
affected outputs. A filter is enabled again only after negotiation succeeds;
until then, WirePlumber's direct route is the fail-open state. A short
transition interruption is allowed.

## Failure and shutdown behavior

Fail-open behavior is expressed as routing state, not default-sink mutation:

1. a filter starts disabled;
2. PipeTune enables it only after the policy handshake and stream readiness;
3. WirePlumber inserts it and publishes `filter.state=active`; and
4. disabling, failure, process exit, or node removal restores the direct route.

PipeTune never writes `default.audio.sink` or
`default.configured.audio.sink`. It therefore needs no shutdown helper, no
`ExecStopPost` default restoration, and no crash-time selection recovery.
The systemd user service may restart the daemon after failure without changing
the user's selected physical output or volume.

As a one-time migration, `pipetune setup` removes either default metadata key
only when its JSON `name` is exactly the retired `pipetune_sink` virtual
device. Physical and otherwise unknown selections are never changed.

## Local control protocol

The default endpoint is `$XDG_RUNTIME_DIR/pipetune/control.sock`. Its directory
and socket are user-only, peers are checked with `SO_PEERCRED`, messages are
bounded newline-framed JSON, and only the owning effective user is accepted.

Protocol version 2 supports:

- status and subscription;
- load preset;
- switch to bypass;
- set the global Max/fixed and suggest/force policy; and
- set the scalar or SIMD backend and SIMD preference.

There are no output-selection or volume commands. Status contains the detected
WirePlumber backend and a `filterOutputs` array. Every entry reports its
physical node and description, hidden filter name, waiting/active/direct/error
state, diagnostic, channel count, capabilities, DSP/requested/active rates,
resampling fallback, latency, and counters.

The subscriber server uses an `eventfd` wakeup and bounded, coalescing output
per client. Slow subscribers cannot accumulate an unbounded history. Socket
work and once-per-second telemetry publication remain outside real-time
callbacks.

## Managed configuration and GTK

The systemd user unit runs:

```text
pipetune daemon --config %E/pipetune/environment
```

The optional environment file stores only:

- `PIPETUNE_PRESET`;
- `PIPETUNE_RATE` and `PIPETUNE_RATE_ENFORCEMENT`; and
- `PIPETUNE_DSP_BACKEND` and `PIPETUNE_DSP_SIMD_VARIANT`.

Output routing is intentionally absent. Malformed configuration or an
unusable preset degrades processing to bypass while retaining a diagnostic.
The mandatory scalar-backend failure also leaves the service available in
bypass.

`pipetune-gtk` presents Processing, Rate, DSP, and Advanced pages. Its status
pane reports every physical output filter and its rate path. Live setting
operations are serialized as rate, backend, then processing; Apply atomically
persists the complete confirmed snapshot, while Cancel rolls the live state
back before hiding.

The GUI computes aggregate load from the change in cumulative DSP CPU
nanoseconds divided by the wall-clock publication interval. Thus 100% means
that all output filters together occupied one logical CPU for the complete
interval; the text can exceed 100% while the meter itself remains capped.

`setup` first asks systemd to `try-restart` WirePlumber so a running desktop
session loads newly installed policy files. It then waits for the compatible
version-1 policy handshake, performs the narrowly scoped legacy-default
migration, and only then starts PipeTune. `setup` and `unsetup` manage the user
unit and GTK autostart with direct argument vectors and no shell. Unsetup only
stops PipeTune; it cannot and need not restore an output because PipeTune never
owns that selection.
