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
- Tracks the physical default output, follows default-device and hotplug
  changes, and can instead target a specific `node.name` or `object.serial`.
- Replaces the preset in a running process through a same-user Unix socket.
- Switches live and future startup processing to explicit DSP bypass.
- Publishes initial and changed runtime state to same-user local subscribers.
- Starts the managed daemon without a preset and passes audio through unchanged.
- Automates per-user service, GTK, and autostart setup and removal.
- Temporarily makes PipeTune the effective PipeWire default without changing
  WirePlumber's persistent configured default.
- Restores a physical default on orderly shutdown. The installed systemd unit
  also invokes an independent restoration command after a crash and restarts
  PipeTune.

The default stream format is 48 kHz stereo. The CLI accepts 32–192 kHz and one
through eight channels. PipeWire performs conversion for clients that use
another format.

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

`make` produces `build/release/pipetune` and
`build/release/pipetune-gtk`. `make test` always runs PipeTune's complete
CTest suite, EffeTune's native DSP tests, JavaScript/native parameter packing
parity, native DSP output parity, GTK lifecycle tests, and staged install
validation. Tests that require a live PipeWire user session report a skip only
when its session socket is unavailable.

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
particular physical output:

```sh
./build/release/pipetune \
  --preset /absolute/path/to/foo.effetune_preset \
  --target alsa_output.example
```

Inspect or replace the running pipeline:

```sh
./build/release/pipetune --status
./build/release/pipetune \
  --load-preset /absolute/path/to/bar.effetune_preset
```

The status response includes the processing mode, active preset when
applicable, native DSP count, physical target, whether PipeTune owns the
effective default, configuration diagnostics, and audio bridge error counters.
A live replacement made directly with `--load-preset` is not persisted.

Switch live processing to bypass and save that selection for future daemon
starts with:

```sh
./build/release/pipetune bypass
```

Run the graphical control application with:

```sh
./build/release/pipetune-gtk
```

It subscribes to daemon status changes, applies a selected preset or bypass
live, and persists a successful selection in the shared startup configuration.
See the
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

Install under `/usr`:

```sh
sudo make install PREFIX=/usr
```

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
`~/.config/pipetune/environment`. The only setting is an absolute preset path:

```text
PIPETUNE_PRESET="/home/user/My Presets/foo.effetune_preset"
```

An absent file or absent `PIPETUNE_PRESET` means bypass. An invalid
configuration or unusable startup preset is reported in daemon status, but the
daemon still starts in bypass so the audio path remains available. The GUI and
`pipetune bypass` atomically update this same file.

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
See [the architecture notes](docs/architecture.md) for the process, real-time, and recovery design.

---

## License

Under MIT.
