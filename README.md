# PipeTune workspace

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
