# PipeTune workspace

PipeTune applies an [EffeTune](https://github.com/Frieve-A/effetune) DSP preset to all audio in one Linux desktop session.
It runs EffeTune's C++ DSP engine as native host code and inserts a virtual PipeWire sink in front of the selected physical output.

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
- [`pipetune-gtk/`](pipetune-gtk/) is reserved for the GTK control
  application.
- `deps/` contains dependencies shared at the workspace level.

The root CMake project discovers each component. Existing build, test, and
installation commands remain available from the repository root:

```sh
make
make test
sudo make install PREFIX=/usr
```

See the [PipeTune component documentation](pipetune/README.md) for
requirements, operation, service installation, and recovery behavior.
