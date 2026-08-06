# PipeTune

PipeTune applies an [EffeTune](https://github.com/Frieve-A/effetune) DSP preset to all audio in one Linux desktop session.
It runs EffeTune's C++ DSP engine as native host code and publishes internal,
target-specific PipeWire smart filters for the physical outputs managed by
WirePlumber.

This repository currently provides an MVP for a native Linux host.
It uses the formal `.effetune_preset` format.

## Audio path

```text
desktop applications
        |
        v
WirePlumber routing to an ordinary physical output
        |
        v
internal PipeTune filter main node and PipeWire mix
        |
        v
native EffeTune C++ DSP pipeline for that output
        or pass-through bypass
        |
        v
internal PipeTune playback stream
        |
        v
physical PipeWire sink and its normal volume control
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
- Leaves output selection, default routing, mute, and volume ownership with
  PipeWire and the desktop sound controls.
- Creates one internal DSP runtime for every eligible local physical output and
  updates only that runtime after hotplug or format changes.
- Installs WirePlumber 0.4 and 0.5 policy integrations together. WirePlumber
  loads the matching integration at runtime, so the PipeTune binary and Debian
  package do not change across a WirePlumber upgrade.
- Selects the highest supported DSP rate or a fixed 44.1, 48, 96, 192, or
  384 kHz rate, with a suggested or forced PipeWire graph-rate request.
- Enumerates each physical output's sample-rate capabilities and independently
  reevaluates that output after capability or policy changes.
- Loads validated scalar, architecture-baseline SIMD, and applicable
  higher-ISA EffeTune DSP shared backends, with scalar as the compatibility
  default and startup fallback.
- Replaces the preset in a running process through a same-user Unix socket.
- Rebuilds and atomically switches an active preset between DSP backends.
- Switches live and future startup processing to explicit DSP bypass.
- Publishes initial and changed runtime state to same-user local subscribers.
- Starts the managed daemon without a preset and passes audio through unchanged.
- Automates per-user service, GTK, and autostart setup and removal.
- Hides PipeTune's filter nodes from ordinary desktop clients on WirePlumber
  0.5. WirePlumber 0.4 may also expose its internal nodes in client listings.
- Fails open per output: WirePlumber retains or restores the direct route until
  a filter is ready, and again if its nodes disappear.

The default policy is Max-and-suggest: each PipeTune output runtime uses the
highest selectable rate supported by its physical target. Eligible outputs
use their advertised one-through-eight-channel layout. PipeWire converts
application streams and resamples between a filter's DSP and physical-output
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

Verify a preset, the WirePlumber policy handshake, and PipeWire negotiation:

```sh
./build/release/pipetune --preset /absolute/path/to/foo.effetune_preset --check
```

`--check` creates target-specific filters briefly and exits after their initial
states settle. It never changes the configured or effective default output.

## Run directly

```sh
./build/release/pipetune --preset /absolute/path/to/foo.effetune_preset
```

The process runs until `SIGINT` or `SIGTERM`, publishes
one internal filter for each eligible output, and lets WirePlumber insert those
filters without changing the user's selected devices. Use
`--dsp-backend scalar` or `--dsp-backend simd` to select the native backend for
this direct run. `--dsp-variant auto|baseline|x86-64-v3|x86-64-v4|sve` selects
the SIMD dispatch preference. Those choices are not persisted.

Inspect or replace the running pipeline:

```sh
./build/release/pipetune --status
./build/release/pipetune \
  --load-preset /absolute/path/to/bar.effetune_preset
```

The status response includes the processing mode, active preset when
applicable, native DSP count, configured and effective DSP backends, backend
availability and fallback diagnostics, the active WirePlumber policy backend,
each physical output and internal filter state, per-output rate capabilities,
configured and resolved PCM rates, active physical rates, resampling fallback,
configuration diagnostics, and audio bridge error counters. A live replacement
made directly with `--load-preset` is not persisted.

Switch live processing to bypass and save that selection for future daemon
starts with:

```sh
./build/release/pipetune bypass
```

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
every available output. `rate get` displays the configured policy and each
filter's state, DSP rate, PipeWire request, active physical rate, and fallback.
Both queries accept `--json` and require a reachable daemon.

`rate set` sends a connected daemon the new policy first and persists it only
after the daemon completes the transition. A rejection preserves the previous
startup policy. If the daemon is unavailable, the command instead saves the
policy for the next start. **Suggest** supplies `node.rate` as a PipeWire
preference. **Force** also supplies `node.force-rate=0`, which asks PipeWire to
use the denominator of `node.rate` while each PipeTune playback node is active.
Neither operation rewrites PipeWire's global clock configuration.

In Max mode, each filter selects the highest of the five user-selectable rates
accepted by its physical output. While that output's capabilities are unknown,
it retains its current rates, using 48 kHz only as the initial fallback. In
fixed mode, the requested rate is always used for that filter's capture,
playback media format, and DSP. An unsupported output uses the greatest
advertised rate not above the fixed rate, or the device minimum, and PipeWire
resamples between them.

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
PIPETUNE_RATE=max
PIPETUNE_RATE_ENFORCEMENT=suggest
PIPETUNE_DSP_BACKEND=scalar
PIPETUNE_DSP_SIMD_VARIANT=auto
```

The absent preset assignment selects DSP bypass. Physical output routing is
not part of the PipeTune configuration. Scalar is the reset backend. After persistence, the command
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
The same package installs the WirePlumber 0.4 policy loader, the WirePlumber
0.5 component configuration, and both Lua implementations below
`/usr/share/wireplumber`; no installation-time version probe selects or copies
a different PipeTune binary. WirePlumber 0.4 also reads the 0.5 component
fragment from its main configuration, so `policy-0.5.lua` detects the absence
of the 0.5 profile feature API and exits before publishing or routing anything.

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
combined configure, build, and install workflow. WirePlumber 0.4 does not
search `/usr/local/share`, so policy files use `/usr/share/wireplumber`
independently of `PREFIX` by default. A fully user-writable dual-version
install must pass WirePlumber's separate per-user configuration and data
directories explicitly, for example:

```sh
make build-install PREFIX="$HOME/.local" \
  WIREPLUMBER_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/wireplumber" \
  WIREPLUMBER_DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/wireplumber"
```

Do not run `build-install` with `sudo`.

When upgrading a source installation that placed these policies below
`/usr/local/share/wireplumber`, run `sudo make uninstall` before reconfiguring
the release build. This uses the previous install manifest to remove the old
fragments before `make` and `sudo make install` write the corrected layout.
Run `pipetune unsetup` first when the user service is still configured.

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
- reloads the newly installed policy by try-restarting WirePlumber when it is
  already active;
- requires the version-1 WirePlumber 0.4 or 0.5 policy handshake before
  enabling PipeTune, and removes only obsolete default metadata that still
  selects the retired `pipetune_sink` virtual device;
- reloads, enables, and restarts `pipetune.service`, then verifies it is
  active;
- removes a PipeTune-managed GTK autostart mask and safely restores any custom
  override that was backed up by `unsetup`; and
- stops any resident GTK primary instance left from an older installation,
  then launches the installed `pipetune-gtk --hidden`.

If policy discovery, a required service operation, or GTK launch fails, setup
reports the failure and attempts to restore the previous startup configuration
and service state. A missing handshake therefore cannot leave a seemingly
successful installation whose audio still follows the direct route.
Unmanaged or orphaned autostart files are preserved and reported as warnings.

The daemon and GTK application share one optional startup configuration:

```text
$XDG_CONFIG_HOME/pipetune/environment
```

When `XDG_CONFIG_HOME` is unset, it resolves to
`~/.config/pipetune/environment`. It can store an absolute preset path, the
PCM rate policy, and the native DSP backend selection:

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
selections in this same file. Output routing is deliberately absent: PipeTune
tracks all eligible physical outputs while PipeWire retains the user's default
and per-application choices.

An absent `PIPETUNE_RATE` or `PIPETUNE_RATE_ENFORCEMENT` uses the
Max-and-suggest default. `PIPETUNE_RATE` accepts `max`, `44100`, `48000`,
`96000`, `192000`, or `384000`; the enforcement value accepts `suggest` or
`force`.

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
quit, and disables and stops the service. Existing PipeTune configuration is retained so a later `pipetune setup`
resumes the same selection. Use `pipetune unsetup --purge` to additionally
delete the shared startup configuration and the obsolete
`environment.gtk` file from older installations. The autostart mask and any
custom override backup are deliberately retained by `--purge`.

If a custom user file already occupies
`$XDG_CONFIG_HOME/autostart/net.kekyo.pipetune-gtk.desktop`, unsetup moves it
to a non-desktop PipeTune backup before writing the mask. It refuses to
overwrite an existing backup. Setup restores that backup exactly. Repeated
setup and unsetup calls are safe for PipeTune-managed state.

If the daemon or a filter runtime disappears, WirePlumber removes its internal
route and resumes the original direct physical-output route. The fail-open
transition can contain a short audio interruption.
See [the architecture notes](docs/architecture.md) for the process, real-time,
and recovery design, and
[the DSP backend notes](docs/dsp-backends.md) for optimization and benchmark
details.

---

## License

Under MIT.
