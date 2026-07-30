# PipeTune

PipeTune applies an [EffeTune](https://github.com/Frieve-A/effetune) DSP preset to all audio in one Linux desktop session.
It runs EffeTune's C++ DSP engine as native host code and inserts a virtual PipeWire sink in front of the selected physical output.

This repository currently provides an MVP for a native Linux host.
It uses the formal `.effetune_preset` format.

## Audio path

```text
desktop applications
        |
        v
PipeTune virtual default sink
        |
        v
native EffeTune C++ DSP pipeline
        or pass-through bypass
        |
        v
selected physical PipeWire sink
```

Ubuntu 24.04 uses PipeWire with `pipewire-pulse` by default. PulseAudio
applications therefore enter the same PipeWire graph and do not require a
separate PulseAudio module. A system running a standalone PulseAudio daemon is
not supported by this MVP.

## Supported behavior

- Loads canonical and legacy EffeTune preset JSON from files whose extension is
  exactly `.effetune_preset`.
- Builds every enabled DSP found in the checked-out EffeTune native registry,
  including bus and channel routing.
- Skips unknown DSPs and DSPs that require external assets, with a warning for
  each omitted node.
- Follows the physical system default when no preference exists, or persists a
  preferred `node.name` with automatic fallback and hotplug restoration.
- Selects the highest supported DSP rate or a fixed 44.1, 48, 96, 192, or
  384 kHz rate, with a suggested or forced PipeWire graph-rate request.
- Enumerates each physical output's sample-rate capabilities and immediately
  reevaluates the rates after capability, target, or policy changes.
- Loads validated scalar and architecture-SIMD EffeTune DSP shared backends,
  with scalar as the compatibility default and startup fallback.
- Replaces the preset in a running process through a same-user Unix socket.
- Rebuilds and atomically switches an active preset between DSP backends.
- Switches live and future startup processing to explicit DSP bypass.
- Publishes initial and changed runtime state to same-user local subscribers.
- Starts the managed daemon without a preset and passes audio through unchanged.
- Automates per-user service, GTK, and autostart setup and removal.
- Temporarily makes PipeTune the effective PipeWire default without changing
  WirePlumber's persistent configured default.
- Restores a physical default on orderly shutdown. The installed systemd unit
  also invokes an independent restoration command after a crash and restarts
  PipeTune.

The default policy is Max-and-suggest: PipeTune uses the highest selectable
rate supported by the selected output. Stereo remains the default channel
layout, and direct runs accept one through eight channels. PipeWire converts
application streams and resamples between PipeTune's DSP and physical-output
rates when those differ.

## Requirements

On Ubuntu 24.04:

```sh
sudo apt install \
  build-essential cmake dbus-x11 desktop-file-utils git \
  libgdk-pixbuf2.0-bin libgtk-3-dev libpipewire-0.3-dev \
  nodejs pkg-config x11-utils xvfb
```

PipeTune requires CMake 3.24 or newer, a C++20 GCC toolchain, Node.js, PipeWire
0.3 development files, and GTK 3 development files. The complete test suite
also uses `systemd-analyze`, an isolated D-Bus session, Xvfb, X11 utilities,
`desktop-file-validate`, and the GdkPixbuf thumbnailer.

Clone with both pinned dependencies:

```sh
git clone --recurse-submodules <PipeTune repository URL>
cd PipeTune
git submodule update --init --recursive
```

`deps/effetune` is pinned to the EffeTune main-branch revision used by this
checkout. `deps/yyjson` is pinned to yyjson 0.12.0.

## Workspace layout

This repository is organized as a multi-component workspace:

- [`pipetune/`](../pipetune/) contains the native DSP daemon, command-line
  control operations, tests, packaging, and daemon documentation.
- [`pipetune-gtk/`](../pipetune-gtk/) contains the GTK control window,
  system-tray backends, XDG autostart integration, and desktop packaging.
- `deps/` contains dependencies shared at the workspace level.

The root CMake project discovers and builds both components.

## Build and test

```sh
make
make test
```

`make` produces `build/release/pipetune`,
`build/release/pipetune-gtk`, the private scalar and SIMD DSP libraries, and
the developer-only `build/release/pipetune-dsp-benchmark`. `make test` always
runs PipeTune's complete CTest suite, EffeTune's native DSP tests,
JavaScript/native parameter packing parity, native DSP output parity, GTK
lifecycle tests, and staged install validation. Tests that require a live
PipeWire user session report a skip only when its session socket is
unavailable.

Node.js and `npx` are required. Unless `PIPETUNE_BUILD_VERSION` is supplied to
CMake, the version embedded in both executables is resolved from the repository
Git metadata with `npx screw-up format -e '{version}' -f`.

Before changing the session default, verify a preset and PipeWire negotiation:

```sh
./build/release/pipetune --preset /absolute/path/to/foo.effetune_preset --check
```

`--check` creates the streams briefly but does not make PipeTune the default
sink.

## Run directly

```sh
./build/release/pipetune --preset /absolute/path/to/foo.effetune_preset
```

The process runs until `SIGINT` or `SIGTERM`, publishes
`pipetune_sink`, and makes that sink the effective default. To select a
particular physical output for this direct process run:

```sh
./build/release/pipetune \
  --preset /absolute/path/to/foo.effetune_preset \
  --target alsa_output.example
```

The direct `--target` value is a PipeWire `node.name` and is not persisted.
Managed daemon output preferences use the `pipetune output` commands below.
Use `--dsp-backend scalar` or `--dsp-backend simd` to select the native
backend for this direct run; that choice is also not persisted.

Inspect or replace the running pipeline:

```sh
./build/release/pipetune --status
./build/release/pipetune \
  --load-preset /absolute/path/to/bar.effetune_preset
```

The status response includes the processing mode, active preset when
applicable, native DSP count, configured and effective DSP backends, backend
availability and fallback diagnostics, preferred and effective physical
outputs, output selection reason, selectable output list and rate
capabilities, configured and resolved PCM rates, active physical rate,
transition and fallback state, whether PipeTune owns the effective default,
configuration diagnostics, and audio bridge error counters. A live
replacement made directly with `--load-preset` is not persisted.

Switch live processing to bypass and save that selection for future daemon
starts with:

```sh
./build/release/pipetune bypass
```

List, inspect, choose, or clear the managed daemon's output preference with:

```sh
./build/release/pipetune output list
./build/release/pipetune output get
./build/release/pipetune output select
./build/release/pipetune output set alsa_output.example
./build/release/pipetune output clear
```

`list` and `get` also accept `--json`. `select` requires an interactive
terminal and displays a numbered list. All five operations require a reachable
daemon. A set or clear request changes the daemon first and updates the shared
startup configuration only after the daemon confirms it. If persistence then
fails, the command exits nonzero and reports that the live change remains
active.

Inspect output support and manage the daemon's PCM rate policy with:

```sh
./build/release/pipetune rate list
./build/release/pipetune rate get
./build/release/pipetune rate set max suggest
./build/release/pipetune rate set 44100 suggest
./build/release/pipetune rate set 192000 force
```

Fixed `RATE` values are `44100`, `48000`, `96000`, `192000`, and `384000`
hertz. `rate list` marks each value supported, unsupported, or unknown for
every available output. `rate get` displays the configured policy, input/DSP
rate, selected output rate, active physical rate, fallback state, and
transition state. Both queries accept `--json` and require a reachable daemon.

`rate set` sends a connected daemon the new policy first and persists it only
after the daemon completes the transition. A rejection preserves the previous
startup policy. If the daemon is unavailable, the command instead saves the
policy for the next start. **Suggest** supplies `node.rate` as a PipeWire
preference. **Force** also supplies `node.force-rate=0`, which asks PipeWire to
use the denominator of `node.rate` while PipeTune's playback node is active.
Neither operation rewrites PipeWire's global clock configuration.

In Max mode, PipeTune selects the highest of its five user-selectable rates
accepted by the selected output. While device capabilities are unknown, it
retains the current rates, using 48 kHz only as the initial fallback. In fixed
mode, the requested rate is always used for capture, playback media format,
and DSP. An unsupported output uses the greatest advertised rate not above the
fixed rate, or the device minimum, and PipeWire resamples between them.

Inspect and select the native DSP backend with:

```sh
./build/release/pipetune dsp list
./build/release/pipetune dsp get
./build/release/pipetune dsp set scalar
./build/release/pipetune dsp set simd
```

`dsp list` reports availability, CPU requirements, and validation errors for
the packaged scalar and SIMD libraries. `dsp get` reports the configured and
effective backend plus startup fallback state. Both queries accept `--json`
and require a reachable daemon.

A connected `dsp set` rebuilds an active preset on the control thread,
atomically replaces it, and persists the choice only after daemon
confirmation. The new EffeTune instances have reset DSP state. A failure keeps
the previous live pipeline and startup choice. Backend changes are rejected
during a sample-rate transition. If the daemon is unavailable, the command
validates the local CPU, library, ABI, and catalog before saving the choice for
the next start.

Scalar is the compatibility default. Configured SIMD falls back to scalar
during startup if SIMD validation fails, with both variants and the diagnostic
retained in status. See
[the DSP backend notes](docs/dsp-backends.md) for architecture flags, expected
per-DSP and standard-preset effects, and the benchmark procedure.

Reset every saved PipeTune selection with:

```sh
./build/release/pipetune config reset
./build/release/pipetune config reset --yes
```

Without `-y` or `--yes`, the command always reads one confirmation line,
including when standard input is not a terminal. Leading and trailing
whitespace is ignored, and only case-insensitive `y` or `yes` confirms the
operation. Any other response or end-of-file cancels it successfully.

The reset does not parse, migrate, or back up the previous `environment`
file. It atomically replaces it with:

```text
# Managed by PipeTune.
PIPETUNE_RATE=max
PIPETUNE_RATE_ENFORCEMENT=suggest
PIPETUNE_DSP_BACKEND=scalar
```

The absent preset and target assignments select DSP bypass and the physical
system default. Scalar is the reset backend. After persistence, the command
waits for `systemctl --user try-restart pipetune.service`. A running service
therefore restarts immediately with the defaults, while an inactive service
remains inactive. If `systemctl` fails, the command exits nonzero and explains
that the configuration was reset; the reset file remains in place.

Run the graphical control application with:

```sh
./build/release/pipetune-gtk
```

It subscribes to daemon status changes, applies a selected preset, bypass, or
native DSP backend live, and persists a successful selection in the shared
startup configuration. See the
[PipeTune GTK documentation](../pipetune-gtk/README.md) for the exact failure
and persistence behavior.

Use `pipetune --help` for the complete CLI.

## Debian package builds

The packaging entry points follow the same prerequisite-image and build-script
structure as `scheme-cd-ripper`. They create one `pipetune` package containing
the daemon, GTK application, systemd user unit, desktop and XDG autostart
entries, icon, configuration example, documentation, and license notices.

The supported package matrix is:

| Distribution | Release | Architectures |
| --- | --- | --- |
| Debian | bookworm | amd64, i386, arm64, armhf |
| Debian | trixie | amd64, i386, arm64, armhf, riscv64 |
| Ubuntu | 24.04 | amd64, arm64 |
| Ubuntu | 26.04 | amd64, arm64 |

The host requires Podman, binfmt/QEMU support for non-native targets,
`dpkg-deb`, `readelf`, Node.js, and `npx`. Build all prerequisite images once,
then build the complete matrix from the workspace root:

```sh
./prereq.sh
./build_package_all.sh
```

Prerequisite images are reused. Rebuild them after changing their dependency
set with:

```sh
./prereq.sh --force
```

Both commands accept `--distro`, `--release`, `--arch`, and `--jobs` filters.
For example, a native Ubuntu 24.04 build is:

```sh
./prereq.sh \
  --distro ubuntu --release 24.04 --arch amd64
./build_package.sh \
  --target deb --distro ubuntu --release 24.04 --arch amd64
```

Architecture aliases such as `amd64`, `i386`, `armhf`, and `aarch64` are
accepted. Release aliases `noble` and `resolute` resolve to Ubuntu 24.04 and
26.04 respectively. Use `--debug` to retain debug information.

Unless `--version` is supplied, the package version is resolved in the
repository root with:

```sh
npx screw-up format -e '{version}' -f
```

The resolved package version is also compiled into both executables. Generated
packages are written to `artifacts/deb/`, with names such as:

```text
pipetune-VERSION-ubuntu-24.04-amd64.deb
```

Each package is checked for its control metadata, runtime dependencies,
installed file layout, and ELF architecture. It is then installed into a fresh
container for its target distribution and architecture, where both
`pipetune --version` and `pipetune-gtk --version` are executed.

## Install as a user service

Build as the desktop user, then install under `/usr`:

```sh
make PREFIX=/usr
sudo make install PREFIX=/usr
```

For a user-writable prefix, `make build-install PREFIX=...` retains the
combined configure, build, and install workflow. Do not run `build-install`
with `sudo`.

Files recorded by the most recent installation can be removed with:

```sh
sudo make uninstall
```

This removes installed files but does not remove user configuration or empty
installation directories.

For end-user installation from a prebuilt Debian package, see the
[workspace installation guide](../README.md#download-and-install).

Configure and start PipeTune as the desktop user, without `sudo`:

```sh
pipetune setup
```

The preset is optional. With no existing selection, setup starts the daemon in
bypass mode: audio passes through PipeTune without DSP processing. When a
selection already exists, omitting `--preset` preserves it.

To validate and select a preset before starting the service:

```sh
pipetune setup --preset /absolute/path/to/foo.effetune_preset
```

Setup performs the following operations:

- rejects effective user ID 0 so that state is never written to root's home;
- validates an explicitly supplied preset before making external changes;
- saves that preset atomically with user-only permissions, or preserves the
  existing startup selection when omitted;
- reloads, enables, and restarts `pipetune.service`, then verifies it is
  active;
- removes a PipeTune-managed GTK autostart mask and safely restores any custom
  override that was backed up by `unsetup`; and
- launches `pipetune-gtk --hidden`.

If a required service operation or GTK launch fails, setup reports the failure
and attempts to restore the previous startup configuration and service state.
Unmanaged or orphaned autostart files are preserved and reported as warnings.

The daemon and GTK application share one optional startup configuration:

```text
$XDG_CONFIG_HOME/pipetune/environment
```

When `XDG_CONFIG_HOME` is unset, it resolves to
`~/.config/pipetune/environment`. It can store an absolute preset path, a
stable PipeWire output `node.name`, the PCM rate policy, and the native DSP
backend:

```text
PIPETUNE_PRESET="/home/user/My Presets/foo.effetune_preset"
PIPETUNE_TARGET="alsa_output.usb-example"
PIPETUNE_RATE=192000
PIPETUNE_RATE_ENFORCEMENT=force
PIPETUNE_DSP_BACKEND=simd
```

An absent file or absent `PIPETUNE_PRESET` means bypass. An invalid
configuration or unusable startup preset is reported in daemon status, but the
daemon still starts in bypass so the audio path remains available. The GUI and
CLI atomically preserve and update the four independent selections in this
same file.

An absent `PIPETUNE_TARGET` means to follow the physical system default. When
the configured target is unavailable, PipeTune retains the preference and
uses the current physical system default as a fallback. It returns to the
preferred target automatically after hotplug. With no physical output at all,
the daemon remains alive, releases PipeTune's effective-default claim, and
waits for a device before resuming playback.

An absent `PIPETUNE_RATE` or `PIPETUNE_RATE_ENFORCEMENT` uses the
Max-and-suggest default. `PIPETUNE_RATE` accepts `max`, `44100`, `48000`,
`96000`, `192000`, or `384000`; the enforcement value accepts `suggest` or
`force`.

An absent `PIPETUNE_DSP_BACKEND` selects `scalar`. The only accepted values
are `scalar` and `simd`. A configured SIMD backend that fails CPU, file, ABI,
or catalog validation falls back to scalar during managed startup and retains
the diagnostic in status. Failure of the mandatory scalar backend keeps the
daemon available in bypass mode.

`pipetune config reset` is also the recovery path for an `environment` file
containing unsupported or obsolete assignments because it replaces the file
without first loading it.

This service affects every application's final output in that user's PipeWire
session. It is not a machine-wide service shared by multiple logged-in users.

Logs:

```sh
journalctl --user -u pipetune.service
```

Undo PipeTune's per-user integration as the same non-root user:

```sh
pipetune unsetup
```

This installs a managed user XDG autostart mask, asks the GTK singleton to
quit, disables and stops the service, and restores a physical default sink.
Existing PipeTune configuration is retained so a later `pipetune setup`
resumes the same selection. Use `pipetune unsetup --purge` to additionally
delete the shared startup configuration and the obsolete
`environment.gtk` file from older installations. The autostart mask and any
custom override backup are deliberately retained by `--purge`.

If a custom user file already occupies
`$XDG_CONFIG_HOME/autostart/net.kekyo.pipetune-gtk.desktop`, unsetup moves it
to a non-desktop PipeTune backup before writing the mask. It refuses to
overwrite an existing backup. Setup restores that backup exactly. Repeated
setup and unsetup calls are safe for PipeTune-managed state.

If a manually launched process is killed without restoration, recover
immediately with:

```sh
pipetune --restore-default
```

The fail-open path can contain a short audio interruption.
See [the architecture notes](docs/architecture.md) for the process, real-time,
and recovery design, and
[the DSP backend notes](docs/dsp-backends.md) for optimization and benchmark
details.

---

## License

Under MIT.
