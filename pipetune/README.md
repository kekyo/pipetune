# PipeTune

PipeTune applies an [EffeTune](https://github.com/Frieve-A/effetune) DSP preset to all audio in one Linux desktop session.
It runs EffeTune's C++ DSP engine as native host code and publishes a
WirePlumber-managed transparent PipeWire filter.

This repository currently provides an MVP for a native Linux host.
It uses the formal `.effetune_preset` format.

## Audio path

```text
desktop applications
        |
        v
PipeWire playback mix
        |
        v
PipeTune filter input
        |
        v
native EffeTune C++ DSP pipeline
        or pass-through bypass
        |
        v
PipeTune filter output
        |
        v
WirePlumber default-output policy and system volume
        |
        v
selected PipeWire sink
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
- Publishes a WirePlumber 0.5 smart-filter pair and a WirePlumber 0.4 endpoint
  contract with the same transparent playback behavior.
- Keeps PipeTune's internal processing nodes out of desktop input and output
  device selectors while retaining transparent WirePlumber routing.
- Follows the negotiated graph rate or requests a fixed 44.1, 48, 96, 192, or
  384 kHz rate, with suggested or forced PipeWire enforcement.
- Loads validated scalar, architecture-baseline SIMD, and applicable
  higher-ISA EffeTune DSP shared backends, with scalar as the compatibility
  default and startup fallback.
- Replaces the preset in a running process through a same-user Unix socket.
- Monitors the active preset for in-place writes, atomic replacement,
  deletion, and recreation, and reloads valid updates automatically.
- Keeps the previous DSP pipeline active and publishes a diagnostic when an
  automatic preset reload fails.
- Rebuilds and atomically switches an active preset between DSP backends.
- Switches live and future startup processing to explicit DSP bypass.
- Publishes initial and changed runtime state to same-user local subscribers.
- Starts the managed daemon without a preset and passes audio through unchanged.
- Automates per-user service, GTK, and autostart setup and removal.
- Leaves default-device selection, hotplug routing, and master volume entirely
  under WirePlumber and the desktop sound controls.

The default rate policy is Automatic: PipeTune follows the graph rate
negotiated for its two filter nodes. Stereo remains the default channel
layout, and direct runs accept one through eight channels. PipeWire performs
any conversion required by applications or the selected device.

## Requirements

On Ubuntu 24.04:

```sh
sudo apt install \
  build-essential cmake dbus-x11 desktop-file-utils git \
  libgdk-pixbuf2.0-bin libgtk-3-dev libpipewire-0.3-dev \
  libsamplerate0-dev nodejs pkg-config x11-utils xvfb
```

PipeTune requires CMake 3.24 or newer, a C++20 GCC toolchain, Node.js, PipeWire
0.3 and libsamplerate development files, GTK 3 development files, and a
WirePlumber 0.4 or 0.5 desktop session. The complete test suite
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
`build/release/pipetune-gtk`, the private scalar and ISA-tiered DSP libraries,
and the developer-only `build/release/pipetune-dsp-benchmark`. `make test` always
runs PipeTune's complete CTest suite, EffeTune's native DSP tests,
JavaScript/native parameter packing parity, native DSP output parity, GTK
lifecycle tests, and staged install validation. Tests that require a live
PipeWire user session report a skip only when its session socket is
unavailable.

Node.js and `npx` are required. Unless `PIPETUNE_BUILD_VERSION` is supplied to
CMake, the version embedded in both executables is resolved from the repository
Git metadata with `npx screw-up format -e '{version}' -f`.

Before running the filter continuously, verify a preset and PipeWire
negotiation:

```sh
./build/release/pipetune --preset /absolute/path/to/foo.effetune_preset --check
```

`--check` creates and negotiates the filter streams briefly, then exits.

## Run directly

```sh
./build/release/pipetune --preset /absolute/path/to/foo.effetune_preset
```

The process runs until `SIGINT` or `SIGTERM` and publishes the linked
`pipetune_sink` input and `pipetune_sink.output` output nodes. WirePlumber
inserts them before the ordinary default output; PipeTune does not select a
physical device.

Use `--dsp-backend scalar` or `--dsp-backend simd` to select the native
backend for this direct run. `--dsp-variant auto|baseline|x86-64-v3|x86-64-v4|sve`
selects the SIMD dispatch preference. Those choices are not persisted.

Inspect or replace the running pipeline:

```sh
./build/release/pipetune --status
./build/release/pipetune \
  --load-preset /absolute/path/to/bar.effetune_preset
```

The status response includes the processing mode, active preset when
applicable, native DSP count, configured and effective DSP backends, backend
availability and fallback diagnostics, configured and negotiated PCM rates,
transition state, configuration diagnostics, input telemetry, and audio bridge
error counters. A live replacement made directly with `--load-preset` is not
persisted.

The active preset path is monitored after it is loaded. A valid in-place edit,
atomic replacement, or recreation builds a complete pipeline and atomically
activates it. A failed automatic reload leaves the previous pipeline and
configuration revision intact, publishes the failure in the configuration
diagnostic, and retries after the next file update. Bypass mode stops active
preset monitoring.

Switch live processing to bypass and save that selection for future daemon
starts with:

```sh
./build/release/pipetune bypass
```

Inspect and manage the daemon's PCM rate policy with:

```sh
./build/release/pipetune rate list
./build/release/pipetune rate get
./build/release/pipetune rate set automatic
./build/release/pipetune rate set 44100 suggest
./build/release/pipetune rate set 192000 force
```

Fixed `RATE` values are `44100`, `48000`, `96000`, `192000`, and `384000`
hertz. `rate list` displays Automatic and the five fixed choices. `rate get`
displays the configured policy, DSP rate, negotiated graph rate, and
transition state. Both queries accept `--json` and require a reachable daemon.

`rate set` sends a connected daemon the new policy first and persists it only
after the daemon completes the transition. A rejection preserves the previous
startup policy. If the daemon is unavailable, the command instead saves the
policy for the next start. Suggest supplies `node.rate` as a PipeWire
preference. Force also supplies `node.force-rate=0`, which asks PipeWire to use
the denominator of `node.rate` while PipeTune's playback node is active. A
fixed selection keeps PipeTune's PCM streams and DSP at that rate even when
Suggest allows PipeWire to run its graph at another rate. PipeWire performs
that conversion outside the filter. An unapplied Force request is shown as a
rate diagnostic.

Neither operation rewrites PipeWire's global clock configuration. Automatic
leaves both filter nodes negotiable and rebuilds EffeTune at the resolved PCM
rate.

Inspect and select the native DSP backend with:

```sh
./build/release/pipetune dsp list
./build/release/pipetune dsp get
./build/release/pipetune dsp set scalar
./build/release/pipetune dsp set simd
./build/release/pipetune dsp set simd --variant x86-64-v3
```

`dsp list` reports availability, CPU requirements, and validation errors for
every packaged scalar and architecture-applicable SIMD library. `dsp get`
reports the configured SIMD preference, concrete effective variant, and
startup fallback state. Both queries accept `--json` and require a reachable
daemon.

A connected `dsp set` rebuilds an active preset on the control thread,
atomically replaces it, and persists the choice only after daemon
confirmation. The new EffeTune instances have reset DSP state. A failure keeps
the previous live pipeline and startup choice. Backend changes are rejected
during a sample-rate transition. If the daemon is unavailable, the command
validates the local CPU, library, ABI, and catalog before saving the choice for
the next start.

Scalar is the compatibility default. Automatic SIMD selects the highest
usable tier. A CPU-supported broken upper tier falls back to a lower SIMD
tier, while an unusable pinned tier falls back to scalar during startup.
The concrete variants and diagnostic are retained in status. See
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
PIPETUNE_DSP_BACKEND=scalar
PIPETUNE_DSP_SIMD_VARIANT=auto
PIPETUNE_RATE=automatic
PIPETUNE_RATE_ENFORCEMENT=suggest
```

The absent preset assignment selects DSP bypass. Scalar is the reset backend.
After persistence, the command
waits for `systemctl --user try-restart pipetune.service`. A running service
therefore restarts immediately with the defaults, while an inactive service
remains inactive. If `systemctl` fails, the command exits nonzero and explains
that the configuration was reset; the reset file remains in place.

Run the graphical control application with:

```sh
./build/release/pipetune-gtk
```

It subscribes to daemon status changes, applies a selected preset, bypass,
rate policy, or native DSP backend live, and persists a successful selection
in the shared startup configuration. See the
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
sudo make uninstall PREFIX=/usr
```

This removes installed files but does not remove user configuration or empty
installation directories. Use the same `PREFIX` and `DESTDIR` as the install.
When CMake's manifest is missing, the target reconstructs it from the current
`BUILD_DIR` in a temporary staging directory before removing files.

For end-user installation from a prebuilt Debian package, see the
[workspace installation guide](../README.md#download-and-install).

Configure and start PipeTune as the desktop user, without `sudo`:

```sh
pipetune setup
```

Plain `setup` first checks whether the current user's integration is already
current. It returns successfully without repeating setup when the installed
PipeTune version, all six managed WirePlumber files, the GTK autostart state,
and the enabled and active service are ready. Run the existing setup workflow
unconditionally with either form:

```sh
pipetune setup --force
pipetune setup -f
```

The preset is optional. With no existing selection, setup starts the daemon in
bypass mode: audio passes through PipeTune without DSP processing. When a
selection already exists, omitting `--preset` preserves it.

To validate and select a preset before starting the service:

```sh
pipetune setup --preset /absolute/path/to/foo.effetune_preset
```

An explicit `--preset` always validates, saves, and applies that selection,
even when the remaining setup state is current.

Setup performs the following operations:

- rejects effective user ID 0 so that state is never written to root's home;
- validates an explicitly supplied preset before making external changes;
- saves that preset atomically with user-only permissions, or preserves the
  existing startup selection when omitted;
- installs the WirePlumber 0.4 compatibility policy and the 0.4/0.5 internal
  node visibility policy, then restarts PipeWire, WirePlumber, and PipeWire's
  PulseAudio server in one user-systemd transaction when any managed file
  changes; WirePlumber 0.5 uses PipeTune's smart-filter node properties for
  routing;
- reloads, enables, and restarts `pipetune.service`, then verifies it is
  active;
- removes a PipeTune-managed GTK autostart mask and safely restores any custom
  override that was backed up by `unsetup`; and
- launches `pipetune-gtk --hidden`.

After successful setup, a versioned completion record is written to
`$XDG_STATE_HOME/pipetune/setup-state`, or
`~/.local/state/pipetune/setup-state` when `XDG_STATE_HOME` is unset. Setup
and unsetup serialize their changes through an advisory lock in the same
state directory. `unsetup` removes the completion record.

PipeTune GTK runs this conditional check asynchronously during primary
application startup as `pipetune setup --no-launch-gtk`. The launch
suppression prevents setup from recursively starting another GTK instance.
Consequently, starting `pipetune-gtk` directly or from its desktop launcher
also performs missing per-user setup without requiring a terminal.

If a required service operation or GTK launch fails, setup reports the failure
and attempts to restore the previous startup configuration and service state.
Unmanaged or orphaned autostart files are preserved and reported as warnings.
GTK records automatic-setup failures in its Action Log, sends a desktop
notification when hidden, and then still attempts the control connection.

The daemon and GTK application share one optional startup configuration:

```text
$XDG_CONFIG_HOME/pipetune/environment
```

When `XDG_CONFIG_HOME` is unset, it resolves to
`~/.config/pipetune/environment`. It can store an absolute preset path, the PCM
rate policy, and the native DSP backend selection:

```text
PIPETUNE_PRESET="/home/user/My Presets/foo.effetune_preset"
PIPETUNE_RATE=192000
PIPETUNE_RATE_ENFORCEMENT=force
PIPETUNE_DSP_BACKEND=simd
PIPETUNE_DSP_SIMD_VARIANT=auto
```

An absent file or absent `PIPETUNE_PRESET` means bypass. An invalid
configuration or unusable startup preset is reported in daemon status, but the
daemon still starts in bypass so the audio path remains available. The GUI and
CLI atomically preserve and update the preset, rate, backend, and SIMD variant
selections in this same file. Output-device selection and master volume remain
ordinary WirePlumber state and are never stored here.

An absent `PIPETUNE_RATE` or `PIPETUNE_RATE_ENFORCEMENT` uses the
Automatic-and-suggest default. `PIPETUNE_RATE` accepts `automatic`, `44100`,
`48000`, `96000`, `192000`, or `384000`; the enforcement value accepts
`suggest` or `force`.

An absent `PIPETUNE_DSP_BACKEND` selects `scalar`. The only accepted values
are `scalar` and `simd`. An absent `PIPETUNE_DSP_SIMD_VARIANT` selects `auto`;
the other accepted values are `baseline`, `x86-64-v3`, `x86-64-v4`, and
`sve`. Automatic dispatch skips CPU-inapplicable upper tiers and falls back
through usable lower tiers. An unusable pinned tier falls back to scalar
during managed startup and retains the diagnostic in status. Failure of the
mandatory scalar backend keeps the daemon available in bypass mode.

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
quit, disables and stops the service, removes all PipeTune-managed WirePlumber
files, and restarts PipeWire, WirePlumber, and PipeWire's PulseAudio server in
one user-systemd transaction when necessary. Existing PipeTune
configuration is retained so a later `pipetune setup` resumes the same
selection. Use `pipetune unsetup --purge` to additionally delete the shared
startup configuration and the obsolete
`environment.gtk` file from older installations. The autostart mask and any
custom override backup are deliberately retained by `--purge`.

If a custom user file already occupies
`$XDG_CONFIG_HOME/autostart/net.kekyo.pipetune_gtk.desktop`, unsetup moves it
to a non-desktop PipeTune backup before writing the mask. It refuses to
overwrite an existing backup. Setup restores that backup exactly. Repeated
setup and unsetup calls are safe for PipeTune-managed state.

PipeTune never owns the default sink. Stopping or crashing the daemon removes
its filter nodes, while WirePlumber continues to own ordinary output routing
and volume; no restoration command is required. See
[the architecture notes](docs/architecture.md) for the process and real-time
design, and
[the DSP backend notes](docs/dsp-backends.md) for optimization and benchmark
details.

---

## License

Under MIT.
