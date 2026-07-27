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
- Publishes initial and changed runtime state to same-user local subscribers.
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

The status response includes the active preset, native DSP count, physical
target, whether PipeTune owns the effective default, and audio bridge error
counters. A live replacement made directly with the CLI is not persisted to a
systemd environment file.

Run the graphical control application with:

```sh
./build/release/pipetune-gtk
```

It subscribes to daemon status changes, applies a selected preset live, and
persists a successful selection for later service starts. See the
[PipeTune GTK documentation](../pipetune-gtk/README.md) for the exact failure
and persistence behavior.

Use `pipetune --help` for the complete CLI.

## Install as a user service

Install under `/usr`:

```sh
sudo make install PREFIX=/usr
```

Create the per-user service configuration:

```sh
install -d -m 700 "$HOME/.config/pipetune"
install -m 600 \
  /usr/share/doc/pipetune/environment.example \
  "$HOME/.config/pipetune/environment"
```

Edit `~/.config/pipetune/environment` and assign an absolute preset path:

```text
PIPETUNE_PRESET=/home/user/presets/foo.effetune_preset
```

Quote the value when it contains spaces:

```text
PIPETUNE_PRESET="/home/user/My Presets/foo.effetune_preset"
```

Enable it for the current user:

```sh
systemctl --user daemon-reload
systemctl --user enable --now pipetune.service
systemctl --user status pipetune.service
```

This applies to every application's final output in that user's PipeWire
session. It is not a machine-wide service shared by multiple logged-in users.

The installed unit reads configuration in this order:

1. `~/.config/pipetune/environment`, which remains the required base
   configuration;
2. `$XDG_CONFIG_HOME/pipetune/environment.gtk` (normally
   `~/.config/pipetune/environment.gtk`), when that GUI-managed file exists.

The second file is optional and its `PIPETUNE_PRESET` value overrides the base
file. Do not edit it while `pipetune-gtk` is running. To return to the base
selection, quit the GUI and move the override aside:

```sh
mv "$HOME/.config/pipetune/environment.gtk" \
  "$HOME/.config/pipetune/environment.gtk.disabled"
systemctl --user restart pipetune.service
```

After changing the configured preset:

```sh
systemctl --user restart pipetune.service
```

Logs and shutdown:

```sh
journalctl --user -u pipetune.service
systemctl --user disable --now pipetune.service
```

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
