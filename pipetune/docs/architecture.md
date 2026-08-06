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

### WirePlumber 0.4

WirePlumber 0.4 does not implement the 0.5 smart-filter policy. `pipetune
setup` installs one configuration fragment and two runtime policy scripts:

```text
$XDG_CONFIG_HOME/wireplumber/policy.lua.d/60-pipetune-filter.lua
$XDG_CONFIG_HOME/wireplumber/scripts/pipetune-endpoint-client.lua
$XDG_CONFIG_HOME/wireplumber/scripts/pipetune-endpoint-device.lua
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

Setup snapshots and atomically updates all three managed files, then restarts
`wireplumber.service` once if any changed. WirePlumber 0.5 ignores the 0.4 Lua
configuration path and uses the smart-filter properties instead. `pipetune
unsetup` removes all three files and restarts WirePlumber when necessary.

## Real-time data flow

The two PipeWire streams use F32P PCM with one plane per channel:

1. The input callback dequeues a capture buffer and validates its planes,
   strides, chunk bounds, and frame count.
2. It copies one bounded block into preallocated planar scratch storage.
3. Preset mode processes that block in place with EffeTune; bypass mode skips
   the engine.
4. The block is written to a preallocated single-producer/single-consumer
   planar ring.
5. The output callback reads the available frames into its PipeWire buffer and
   supplies silence for any underrun.

If the ring is full, the oldest unread frames are discarded. Overrun,
underrun, and processing-error counts are exposed in runtime status. Fully
consumed input chunks are returned with zero valid size so a property-only
graph wake cannot republish retained PCM.

No mutex, allocation, filesystem access, JSON parsing, socket operation, or
DSP destruction occurs in a PipeWire process callback. Preset construction
happens on the control thread. A hazard-protected pipeline slot swaps a
prepared pipeline atomically and defers reclamation until no real-time callback
can still reference it.

## Sample-rate negotiation and transitions

PipeTune supports two rate modes:

- Automatic advertises an F32P rate range from 8 kHz through 768 kHz on both
  filter nodes. PipeWire negotiates one graph rate, and PipeTune rebuilds the
  EffeTune pipeline at that resolved rate.
- Fixed constrains both filter nodes and EffeTune to one of 44.1, 48, 96, 192,
  or 384 kHz.

Both nodes must negotiate the same rate. A disagreement is a graph error
rather than an implicit resampling boundary inside PipeTune. In fixed mode,
failure to negotiate the requested value is also an error. Conversion needed
by an application or physical device remains PipeWire's responsibility outside
the filter.

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

A short silent interval is intentional. Preset, bypass, and backend mutations
are serialized with the transition so no partially rebuilt state becomes
visible.

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
3. applies enabled section, bus, and channel routing;
4. omits disabled nodes;
5. warns and omits unknown or external-asset-dependent nodes;
6. creates up to 96 native DSP instances; and
7. configures one EffeTune engine for the complete graph.

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
- set the scalar or SIMD native DSP backend; and
- subscribe to an initial status event and later publications.

Status contains processing mode, active preset, input telemetry, DSP and graph
rates, transition state, DSP performance counters, configured and effective
backend variants, availability, fallback, and diagnostics. It contains no
physical output, default-sink, or volume state.

The subscriber server uses an `eventfd` wakeup and bounded, coalescing output
per client. Preset activation and relevant PipeWire state transitions request a
fresh publication outside the real-time callbacks. Slow subscribers cannot
accumulate an unbounded history.

## Managed startup and preferences

The systemd user unit starts `pipetune daemon` with the optional configuration
path `$XDG_CONFIG_HOME/pipetune/environment`. Supported assignments are:

- `PIPETUNE_PRESET` with an absolute preset path;
- `PIPETUNE_RATE` with `automatic` or a supported fixed rate;
- `PIPETUNE_RATE_ENFORCEMENT` with `suggest` or `force`;
- `PIPETUNE_DSP_BACKEND` with `scalar` or `simd`; and
- `PIPETUNE_DSP_SIMD_VARIANT` with `auto` or an applicable fixed tier.

An absent preset selects bypass. Missing rate assignments select
Automatic-and-suggest. Missing backend assignments select scalar with
automatic SIMD dispatch preference. Preset, rate, and backend updates use one
atomic writer and preserve the other selections. Obsolete or unknown
assignments are rejected; `pipetune config reset` replaces such a file without
first parsing it.

Malformed configuration, an unavailable preset, or a preset that fails
validation degrades to bypass instead of terminating the audio service. The
diagnostic is retained in runtime status. Configured SIMD falls back through
usable lower variants or to scalar according to the backend rules.

The installed service is ordered after PipeWire and WirePlumber and uses
`Restart=on-failure`. It has no default-sink restoration command. Removing or
stopping PipeTune removes the filter nodes; WirePlumber continues to own the
normal device and volume policy.

The setup and unsetup coordinators invoke `systemctl` and `pipetune-gtk` with
direct argument vectors and no shell. Setup snapshots the managed
configuration, WirePlumber 0.4 policy files, service state, and GTK autostart
state before mutation. A later failure triggers best-effort rollback. Unsetup
removes only PipeTune-managed files, preserving a custom GTK autostart override
through its reserved backup.

## GTK control plane

`pipetune-gtk` is a single-instance `GtkApplication`. Its GIO client keeps one
asynchronous subscription connection and uses separate asynchronous requests
for preset, bypass, rate-policy, and DSP-backend changes. There is no output
device UI or output control request.

Runtime counters and cumulative native EffeTune processing time are published
once per second. The GUI derives a per-frame interval average and divides it by
the frame duration implied by the negotiated input rate to show DSP Load. The
graph is capped at 100%, while overload text retains the actual value. A retry
timer reconnects a lost subscription; status itself is not polled.

The settings transaction applies live changes in rate, backend, then processing
order. Apply persists the complete confirmed snapshot. Cancel restores the
captured live baseline before hiding the window. The Advanced default is
bypass, Automatic-and-suggest, scalar, and automatic SIMD preference.

The tray backend discovers a StatusNotifierItem host first. If none is
available on X11, it uses the `GtkStatusIcon` and XEmbed compatibility path. A
hidden start remains unmapped even when neither tray transport has a host; a
later explicit activation presents the existing singleton window.

## Known limits

- Stereo is the managed-service layout. Direct runs accept one through eight
  channels, but no live channel-layout control is provided.
- Room EQ and IR Reverb assets stored in EffeTune's IndexedDB are not carried
  by `.effetune_preset` files and are omitted with warnings.
- PipeTune does not provide a machine-wide service shared by multiple logged-in
  users.
