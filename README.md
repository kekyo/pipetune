# PipeTune workspace

PipeTune applies an [EffeTune](https://github.com/Frieve-A/effetune) DSP preset to all audio in one Linux desktop session.
It runs EffeTune's C++ DSP engine as native host code and inserts a virtual PipeWire sink in front of the selected physical output.
The workspace also includes `pipetune-gtk`, a GTK 3 control application that
stays available through the desktop system tray.

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
- Publishes daemon status changes to local subscribers without status polling.
- Displays runtime state and applies or persists presets from a single-instance
  GTK 3 application.
- Temporarily makes PipeTune the effective PipeWire default without changing
  WirePlumber's persistent configured default.
- Restores a physical default on orderly shutdown. The installed systemd unit
  also invokes an independent restoration command after a crash and restarts
  PipeTune.

The default stream format is 48 kHz stereo. The CLI accepts 32–192 kHz and one
through eight channels. PipeWire performs conversion for clients that use
another format.

---

This repository is organized as a multi-component workspace:

- [`pipetune/`](pipetune/) contains the native DSP daemon, its command-line
  control operations, tests, packaging, and detailed documentation.
- [`pipetune-gtk/`](pipetune-gtk/) contains the GTK control window, system-tray
  backends, XDG autostart integration, and desktop packaging.
- `deps/` contains dependencies shared at the workspace level.

The root CMake project discovers each component. Existing build, test, and
installation commands remain available from the repository root:

```sh
make
make test
sudo make install PREFIX=/usr
```

See the [PipeTune component documentation](pipetune/README.md) for daemon
requirements, operation, service installation, and recovery behavior. See the
[PipeTune GTK documentation](pipetune-gtk/README.md) for its controls, tray
compatibility, persistence rules, and autostart behavior.
