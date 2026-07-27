# PipeTune

----

[(Japanese language is here/日本語はこちら)](./README_ja.md)

> Please note that this English version of the document was machine-translated and then partially edited, so it may contain inaccuracies.
> We welcome pull requests to correct any errors in the text.

## What Is This?

PipeTune applies an [EffeTune](https://github.com/Frieve-A/effetune) DSP preset
to all audio in one Linux desktop session.

It inserts a virtual PipeWire sink in front of the selected physical output and includes a GTK 3 control
application that remains available through the desktop system tray.

### Features

- Loads canonical and legacy EffeTune preset files with the
  `.effetune_preset` extension.
- Applies the enabled native EffeTune DSP pipeline to desktop audio.
- Tracks the physical default output and follows default-device and hotplug
  changes.
- Changes presets without restarting the daemon.
- Starts safely in pass-through mode when no preset has been selected.
- Sets up or removes all per-user integration with one CLI command.
- Displays runtime state and audio error counters in the GTK application.
- Runs `pipetune-gtk` in the system tray using StatusNotifierItem or the
  `GtkStatusIcon` compatibility fallback.
- Restores a physical default output when PipeTune stops.

The default stream format is 48 kHz stereo. PipeWire converts streams from
applications that use another format.

### Supported systems

PipeTune requires a PipeWire desktop session and systemd user services. A
standalone PulseAudio session is not supported.

Prebuilt Debian packages are published for:

| Distribution | Release | Architectures |
| --- | --- | --- |
| Debian | bookworm | amd64, i386, arm64, armhf |
| Debian | trixie | amd64, i386, arm64, armhf, riscv64 |
| Ubuntu | 24.04 | amd64, arm64 |
| Ubuntu | 26.04 | amd64, arm64 |

Use the following command if you are unsure which package architecture to
download:

```sh
dpkg --print-architecture
```

---

## Download and install

Download the `.deb` matching your distribution, release, and architecture
from the [PipeTune GitHub Releases page](https://github.com/kekyo/pipetune/releases/).

Open a terminal in the directory containing the downloaded package. If that
directory contains only the intended PipeTune package, install it with:

```sh
sudo apt install ./pipetune-*.deb
```

`apt` installs the package and its required runtime dependencies. The package
contains the PipeTune daemon, GTK application, systemd user service, desktop
entry, system-tray autostart entry, icon, and configuration example.

Verify the installation:

```sh
pipetune --version
pipetune-gtk --version
```

---

## Initial setup

Run setup as the desktop user, without `sudo`:

```sh
pipetune setup
```

No preset is required. On a fresh installation, PipeTune starts in bypass mode:
audio still passes through its virtual sink to the physical output, but no DSP
is applied. If setup is run again without `--preset`, the existing startup
selection is preserved.

To select a preset during setup, provide its path:

```sh
pipetune setup --preset /absolute/path/to/example.effetune_preset
```

An explicitly supplied preset is validated before setup changes the service.
Setup then reloads, enables, and restarts the systemd user service, verifies
that it is active, and launches PipeTune GTK hidden. If no compatible system
tray is available, the GTK window is shown so that it remains reachable.

Selecting a preset in the GTK application applies it immediately when the
daemon is connected and saves it for later service starts. Closing the window
hides it while a compatible system tray is available. The installed XDG
autostart entry starts it hidden at later desktop logins.

## Logs and recovery

View daemon logs with:

```sh
journalctl --user -u pipetune.service
```

If a manually launched PipeTune process was terminated without restoring the
physical default output, recover it with:

```sh
pipetune --restore-default
```

## Update or remove

To update PipeTune, download a newer matching package from
[GitHub Releases](https://github.com/kekyo/pipetune/releases) and install it
with the same `sudo apt install ./pipetune-*.deb` command. Then run
`pipetune setup` as the desktop user; omitting `--preset` preserves the current
selection while reloading and restarting the updated service.

Before removing the package, undo its per-user integration as the desktop user:

```sh
pipetune unsetup
sudo apt remove pipetune
```

`unsetup` quits the GTK application, disables and stops the user service,
restores a physical default output, and installs a user autostart mask so the
GTK application stays disabled. It preserves the startup selection. Use
`pipetune unsetup --purge` to also remove PipeTune's application
configuration.

If a custom user autostart entry must be masked, `unsetup` keeps it in a
PipeTune-managed backup. A later `pipetune setup` restores that backup instead
of overwriting it. Package removal alone does not delete per-user
configuration or autostart overrides.

## More information

- [Daemon operation and developer documentation](pipetune/README.md)
- [GTK application behavior](pipetune-gtk/README.md)

## License

MIT
