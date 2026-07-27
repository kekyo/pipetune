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

## Initial configuration

Create the required per-user service configuration:

```sh
install -d -m 700 "$HOME/.config/pipetune"
install -m 600 \
  /usr/share/doc/pipetune/environment.example \
  "$HOME/.config/pipetune/environment"
```

Edit `~/.config/pipetune/environment` and set an absolute path to an EffeTune
preset:

```text
PIPETUNE_PRESET=/home/user/presets/example.effetune_preset
```

Quote the value if the path contains spaces:

```text
PIPETUNE_PRESET="/home/user/My Presets/example.effetune_preset"
```

Enable and start PipeTune for the current user:

```sh
systemctl --user daemon-reload
systemctl --user enable --now pipetune.service
systemctl --user status pipetune.service
```

Launch PipeTune GTK from the desktop application menu or a terminal:

```sh
pipetune-gtk
```

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
with the same `sudo apt install ./pipetune-*.deb` command.

To remove PipeTune:

```sh
systemctl --user disable --now pipetune.service
sudo apt remove pipetune
systemctl --user daemon-reload
```

The package removal does not delete configuration files in
`~/.config/pipetune`.

## More information

- [Daemon operation and developer documentation](pipetune/README.md)
- [GTK application behavior](pipetune-gtk/README.md)

## License

MIT
