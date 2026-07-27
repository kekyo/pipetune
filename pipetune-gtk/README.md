# PipeTune GTK

`pipetune-gtk` is the GTK 3 control application for the per-user PipeTune
daemon. It is a single-instance application that can remain resident in the
desktop system tray.

## Controls and status

The window displays:

- daemon connection state;
- active processing mode (`Preset` or `Bypass`);
- active and startup preset paths;
- active native DSP node count;
- selected physical output target;
- effective-default-sink state;
- overrun, underrun, and DSP processing error counters; and
- warnings for preset nodes omitted by the daemon.

Select an `.effetune_preset` file and use **Apply and Save** to change the
startup selection, or use **Bypass and Save** to pass audio through without
DSP. When the daemon is connected, the GUI first applies the selected mode
live and writes the startup configuration only after that succeeds. A daemon
rejection therefore leaves the previous startup selection unchanged. If the
live apply succeeds but persistence fails, the new processing mode remains
active and the window reports that partial success.

When the daemon is disconnected, the selection is saved without a live apply
and takes effect on the next successful service start. The GUI writes:

```text
$XDG_CONFIG_HOME/pipetune/environment
```

When `XDG_CONFIG_HOME` is unset, this resolves to
`~/.config/pipetune/environment`. The directory is mode `0700`, the file is
mode `0600`, and replacement is atomic. A file containing
`PIPETUNE_PRESET` selects a preset. An absent file or a saved bypass selection
contains no preset assignment and starts the daemon in pass-through mode.

## Status subscription

The GUI uses the daemon's same-user Unix control socket. It receives an initial
status event and later daemon publications over a persistent asynchronous GIO
connection. It does not poll for status. The Refresh button is an explicit
one-shot request, while a short retry timer is used only to reconnect after the
socket becomes unavailable.

## System tray compatibility

The tray backend prefers a StatusNotifierItem host. On X11 it falls back to
`GtkStatusIcon` and XEmbed using the elder-terms compatibility approach.
`GtkStatusIcon` is intentionally retained despite its GTK deprecation because
compatibility with those notification areas is a project requirement.

Closing the window hides it while a tray host is available. The tray icon
opens the window, and its menu provides Open PipeTune and Quit actions. If a
hidden start cannot find either a StatusNotifierItem host or an XEmbed tray
host, the main window is shown instead of leaving an unreachable background
process.

## Run

Build from the workspace root:

```sh
make
make test
```

Start with the window visible:

```sh
./build/release/pipetune-gtk
```

Start without initially presenting the window:

```sh
./build/release/pipetune-gtk --hidden
```

A later ordinary launch activates the existing instance and presents its
window. Ask the running singleton to exit with:

```sh
./build/release/pipetune-gtk --quit
```

The same command exits successfully without presenting a window when no
instance is running. Use `pipetune-gtk --help` and `pipetune-gtk --version` for
the remaining command-line information.

## Install and autostart

The root installation installs the daemon, GUI, service, desktop entry,
autostart entry, and icon together:

```sh
sudo make install PREFIX=/usr
```

For end-user installation from a prebuilt Debian package, see the
[workspace installation guide](../README.md#download-and-install). Developers
can build the complete package matrix using the
[Debian package build instructions](../pipetune/README.md#debian-package-builds).

With that prefix, the GUI integration is installed as:

```text
/usr/bin/pipetune-gtk
/usr/share/applications/net.kekyo.pipetune-gtk.desktop
/usr/share/icons/hicolor/scalable/apps/pipetune.svg
/etc/xdg/autostart/net.kekyo.pipetune-gtk.desktop
```

The system autostart entry runs `pipetune-gtk --hidden` at desktop login.
Normal per-user setup restores PipeTune-managed autostart state and starts the
GTK application immediately:

```sh
pipetune setup
```

Disable the GTK application and the daemon together with:

```sh
pipetune unsetup
```

Unsetup writes a user override with `Hidden=true` and a PipeTune ownership
marker at the same desktop filename. If a custom override already exists, it
is moved to a non-desktop backup first; an existing backup is never
overwritten. A later setup removes only PipeTune's own mask and restores the
backup. Unmanaged targets and orphaned backups are preserved with warnings.
See the [PipeTune documentation](../pipetune/README.md#install-as-a-user-service)
for service behavior, optional presets, purge semantics, and recovery.
