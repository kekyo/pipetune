# PipeTune MVP architecture

## Scope and integration choice

PipeTune is a normal per-user PipeWire client process, not a PipeWire daemon
plugin or a PulseAudio loadable module. It publishes an `Audio/Sink` node and a
linked output stream. This keeps the implementation on PipeWire's public client
API and covers native PipeWire applications as well as PulseAudio applications
served by `pipewire-pulse`.

The MVP builds only for the host architecture. EffeTune, yyjson, and PFFFT are
compiled from source in the same build; no WebAssembly or prebuilt DSP object is
loaded at runtime. EffeTune's `dsp/vendor/pffft` supplies PFFFT, and the current
native build uses its portable scalar configuration (`ET_SIMD=OFF`). This
configuration works with the host compiler and ABI without assuming WebAssembly
SIMD or another target's instruction set. Per-architecture optimized builds are
future work.

## Data flow

```text
                      PipeWire graph

application streams ──> PipeTune Audio/Sink
                              |
                       F32P capture callback
                              |
                  native EffeTune pipeline
                     or explicit bypass
                              |
                    preallocated planar ring
                              |
                       F32P output callback
                              |
                       physical Audio/Sink
```

Both streams use a fixed configured sample rate and channel layout. The
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

The registry tracker accepts only non-virtual `Audio/Sink` nodes and always
excludes PipeTune's own node. Without `--target`, initial enumeration chooses
the effective physical default or the highest session priority. Once selected,
the device remains stable until the session default changes or the device
disappears. With `--target`, PipeTune waits for the requested `node.name` or
`object.serial` and resumes it after hotplug.

Changing the target destroys and recreates only the non-real-time output
stream. The virtual sink and prepared DSP pipeline remain present.

## Default-sink ownership and fail-open behavior

PipeTune writes only the effective `default.audio.sink` metadata key. It never
writes WirePlumber's persistent `default.configured.audio.sink` selection.
Startup is reported ready only after:

1. the virtual sink negotiated its format;
2. a physical output stream negotiated its format; and
3. making the virtual sink effective completed a PipeWire core round trip.

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
- subscribe to an initial status event and later status publications.

The subscriber server uses an `eventfd` wakeup and bounded, coalescing output
per client. Preset activation and relevant PipeWire state transitions request
a fresh publication outside the real-time callbacks. Slow subscribers cannot
accumulate an unbounded history. The server runs outside the PipeWire process
callbacks. There is no network listener.

## Managed startup and bypass

The systemd user unit starts `pipetune daemon` with the optional configuration
path `$XDG_CONFIG_HOME/pipetune/environment`. A valid absolute
`PIPETUNE_PRESET` assignment constructs the native DSP graph. A missing file,
an empty configuration without that assignment, or an explicit saved bypass
selection creates no EffeTune engine and copies captured samples into the
output ring unchanged.

Malformed configuration, an unavailable preset, or a preset that fails
validation also degrades to bypass instead of terminating the audio service.
The diagnostic is retained in runtime status so the CLI and GTK application
can distinguish an intentional bypass from a configuration failure. Live load
and bypass commands atomically replace the pipeline slot and publish the
resulting state.

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
for explicit refreshes and preset loads. A retry timer reconnects a lost
subscription; status itself is not polled.

The tray backend discovers a StatusNotifierItem host first. If none is
available on X11, it creates the same `GtkStatusIcon`/XEmbed compatibility
path used by elder-terms. A hidden start falls back to presenting the main
window when neither tray transport has a host, so the process never becomes
inaccessible.

Startup persistence is separate from daemon control. The GUI and CLI
atomically write the shared optional
`$XDG_CONFIG_HOME/pipetune/environment` file with user-only permissions. A
connected preset or bypass change is persisted only after the daemon accepts
it; a disconnected selection is saved for the next daemon start.

## Known MVP limits

- Linux PipeWire 0.3 and systemd user sessions only.
- Each build contains native DSP code for one target architecture; no portable
  runtime DSP object is distributed.
- Standalone PulseAudio without `pipewire-pulse` is unsupported.
- DSPs requiring external assets are skipped.
- The configured stream rate and channel count are fixed for one process run.
- Effective DSP latency is not yet published to the wider PipeWire graph for
  audio/video latency compensation.
- Fail-open recovery permits a short dropout.
