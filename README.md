# PipeTune

Engine and User Interface for Applied EffeTune DSP on a Linux Desktop

![PipeTune](./images/pipetune-120.png)

[![Project Status: Active – The project has reached a stable, usable state and is being actively developed.](https://www.repostatus.org/badges/latest/active.svg)](https://www.repostatus.org/#active)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

---

[(Japanese language is here/日本語はこちら)](./README_ja.md)

> Please note that this English version of the document was machine-translated and then partially edited, so it may contain inaccuracies.
> We welcome pull requests to correct any errors in the text.

## What Is This?

PipeTune applies an [EffeTune](https://github.com/Frieve-A/effetune) DSP preset
to all audio in one Linux desktop session.

EffeTune DSP performs computations using fully natively compiled binaries.
On each platform, you can select native SIMD operations.

WirePlumber (PipeWire orchestrator) inserts PipeTune as a transparent filter between the mixed desktop
playback stream and the normal PipeWire output path. A GTK3 control
application remains available through the desktop system tray.

![PipeTune UI](./images/pipetune-ui.png)

### Features

- You can load EffeTune preset files to apply a DSP pipeline to the audio output of the entire Linux system.
- Loads standard and legacy EffeTune preset files with the `.effetune_preset` extension
  and applies the DSP pipeline to desktop audio.
- Automatically negotiates the sampling rate with the PipeWire graph, or computes the DSP at specified rates of 44.1, 48, 96, 192, or 384 kHz.
- The DSP performs computations entirely in native code. You can choose between Scalar (for compatibility), automatic SIMD selection, or CPU-verified implementations for specific instruction sets.
- A GTK application that resides in the system tray allows you to manage various DSP states and settings.
- Operation is also possible via CLI commands.

### Supported systems

PipeTune requires a PipeWire desktop session managed by WirePlumber and
systemd user services. WirePlumber 0.4 and 0.5 are supported. A standalone

> Note: This applies to the standard Debian and Ubuntu distributions.
> It may also work on other distributions if they meet the system requirements.

Prebuilt Debian packages are published for:

| Distribution | Release | Architectures |
| :--- | :--- | :--- |
| Debian | bookworm | amd64, i386, arm64, armhf |
| Debian | trixie | amd64, i386, arm64, armhf, riscv64 |
| Ubuntu | 24.04 | amd64, arm64 |
| Ubuntu | 26.04 | amd64, arm64 |

---

## Download and install

Download the `.deb` matching your distribution, release, and architecture
from the [PipeTune GitHub Releases page](https://github.com/kekyo/pipetune/releases/).

Use the following command if you are unsure which package architecture to
download:

```sh
dpkg --print-architecture
```

Open a terminal in the directory containing the downloaded package. Make sure
that directory contains only the intended PipeTune package, then install it
with:

```sh
sudo apt install ./pipetune-*.deb
```

`apt` installs the package and its required runtime dependencies.

To verify that the installation succeeded, run:

```sh
pipetune --version
```

## Initial setup

Since PipeTune runs as a user-level daemon, additional setup is required for each user after the package is installed.

Run setup as the regular desktop user, without `sudo`:

```sh
pipetune setup
```

`setup` reloads, enables, and restarts the systemd user service, verifies that
it is active, and then launches the PipeTune GTK application in the system tray.

![System tray](./images/system-tray.png)

Open the PipeTune settings window by double-clicking the system tray icon or
selecting `Open` from its menu.

## PipeTune settings window

The PipeTune settings window always displays PipeTune's status, divided into
sections, on the left, while the Processing, Rate, DSP, and Advanced settings
are shown on the right.

![PipeTune UI Window](./images/pipetune-ui-window.png)

Changing a setting previews it immediately in the running daemon. The `Apply`
button confirms and saves the current state. The `Cancel` button, Escape, or
the title-bar close button restores the previous live state before hiding the
window.

The status pane on the left displays DSP utilization as a bar. The `Action Log`
drawer at the bottom lets you review the history of connections, previews,
saves, and failures.

Use the `Enable DSP processing` switch under `Processing` to easily turn DSP
processing on or off (bypass mode).

If the PipeTune daemon cannot be reached, the UI controls become read-only and
normal operation resumes after reconnection.

## Audio streams and PipeTune

PipeTune uses PipeWire and WirePlumber on Linux to insert DSP processing into
the audio stream as a filter, as shown below:

```mermaid
flowchart LR
    apps1["Application (1)"]
    apps2["Application (2)"]
    apps3["Application (3)"]
    mix["PipeWire mix"]
    tune["PipeTune filter<br/>EffeTune DSP or bypass"]
    output["PipeWire output<br/>master volume and default route"]
    device["Selected audio device"]
    device2["Audio device (2)"]
    device3["Audio device (3)"]

    apps1 --> mix
    apps2 --> mix
    apps3 --> mix
    mix --> tune
    tune --> output
    output --> device2
    output --> device
    output --> device3
```

Per-application volume is applied before mixing. In bypass mode, the mixed PCM
is passed downstream without invoking EffeTune. After the DSP filter, desktop
master-volume and output-device selection work exactly as they do without
PipeTune.

## Selecting an EffeTune DSP preset

When loading an EffeTune DSP preset into PipeTune, you can select from:

- EffeTune's standard DSP presets
- User presets saved by EffeTune for Linux AppImage
- An individual `*.effetune_preset` file

The user preset file saved by the Linux AppImage is located at
`$XDG_CONFIG_HOME/effetune/effetune_presets.json` (or
`~/.config/effetune/effetune_presets.json`).

Select a preset from the list or specify a file to preview it immediately in
the DSP.

From the CLI, you can specify either a preset file path or bypass mode:

```sh
pipetune setup --preset /absolute/path/to/example.effetune_preset
pipetune bypass
```

## Choosing the PCM rate

Configure the rate using `DSP sampling frequency` and `PipeWire enforcement`
in the PipeTune settings window. `Automatic` follows the rate negotiated by the
PipeWire graph. A fixed setting requests `44.1`, `48`, `96`, `192`, or `384`
kHz for both filter nodes and the EffeTune engine. The status display shows the
actual DSP and graph rates.

- `Suggest` proposes the graph rate to PipeWire. PipeWire may accept it or
  choose a different graph rate to accommodate the audio streams.
- `Force` additionally asks PipeWire to maintain that rate.

Neither mode changes PipeWire's global clock configuration.

If the requested value cannot be applied, PipeTune remains connected and
continues DSP processing at the graph rate that was actually negotiated. An
unsuccessful `Force` request appears in the Rate diagnostics.

The same information and controls are available from the CLI:

```sh
pipetune rate list
pipetune rate get
pipetune rate set automatic
pipetune rate set 192000 force
```

`rate list` lists `Automatic` and the five fixed rates. `rate get` reports the
configured policy and negotiated rates. While connected to the daemon,
`rate set` applies the change immediately and saves it only after the daemon
confirms success. If the daemon is unavailable, the setting is saved for its
next start.

Changing the rate may cause a brief period of silence while PipeTune rebuilds
the DSP and PipeWire streams.

## Choosing the native DSP backend

PipeTune lets you choose which CPU instruction set is used to compute the
EffeTune DSP. Select it from `Native backend` on the `DSP` page of the PipeTune
settings window.

- `Scalar` is the compatibility-oriented default. It is intended to reproduce
  EffeTune's computations as faithfully as possible.
- `SIMD (Auto)` automatically selects the highest validated implementation
  supported by the CPU.
- The same drop-down can also pin `baseline`, `x86-64-v3`, `x86-64-v4`, or
  `Arm64 SVE` where available on the target architecture.

> Note: The original EffeTune performs its computations using WebAssembly and
> WebAssembly SIMD, whereas PipeTune uses native CPU instructions. Therefore,
> very small numerical differences from EffeTune may occur. The project owner
> could not hear any difference.

The same information and controls are available from the CLI:

```sh
pipetune dsp list
pipetune dsp get
pipetune dsp set scalar
pipetune dsp set simd
pipetune dsp set simd --variant x86-64-v3
```

While connected to the daemon, `dsp set` rebuilds the current preset pipeline
with the new backend and saves the setting only after the daemon confirms
success.

DSP internal state is reset, so a discontinuity or brief silence during the
switch is expected. If the daemon is unavailable, a locally validated choice
is saved for its next start. If the configured SIMD backend fails its CPU
requirements or library validation at startup, PipeTune falls back to an
available lower SIMD tier or Scalar and reports the actual variant and reason
in the status display.

## Resetting PipeTune configuration

`Restore Defaults` on the `Advanced` page of the PipeTune settings window
previews the following state without saving it:

- DSP changes to bypass mode
- PCM rate is `Automatic` and the PipeWire request is `Suggest`
- Native DSP backend is `Scalar`

Use `Apply` to save the defaults or `Cancel` to restore the previous live
settings. This GTK operation does not restart the service.

You can also reset the configuration from the CLI:

```sh
pipetune config reset
pipetune config reset --yes
```

Without `--yes` (or `-y`), the CLI asks for confirmation. The configuration
file is completely replaced, so this command can also recover from a corrupted
configuration file.

A running user service is restarted immediately; a stopped service remains
stopped.

## Update or remove PipeTune

To update PipeTune, download a newer matching package from
[GitHub Releases](https://github.com/kekyo/pipetune/releases/) and install it
with the same `sudo apt install ./pipetune-*.deb` command.

Then run `pipetune setup` as the regular desktop user.

Before removing the package, undo its per-user integration as the desktop user:

```sh
pipetune unsetup
sudo apt remove pipetune
```

`pipetune unsetup` quits the GTK application, disables and stops the user
service, and removes PipeTune's WirePlumber configuration. It also creates a
per-user autostart mask to prevent the GTK application from starting
automatically. The startup preset selection is preserved.

Use `pipetune unsetup --purge` to also remove PipeTune's application
configuration.

## Logs

View daemon logs with:

```sh
journalctl --user -u pipetune.service
```

---

## More information

- [Daemon operation and developer documentation](pipetune/README.md)
- [GTK application behavior](pipetune-gtk/README.md)
- [Native DSP backends and benchmarking](pipetune/docs/dsp-backends.md)

## Limitations

Room EQ and IR Reverb are not supported in the current version. Their required
assets are stored in EffeTune's IndexedDB and are not included in
`.effetune_preset` files, so PipeTune cannot load them.

## License

Under MIT.
