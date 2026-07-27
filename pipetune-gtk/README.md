# PipeTune GTK

`pipetune-gtk` is the GTK 3 control application for the per-user PipeTune
daemon. It is a single-instance application that can remain resident in the
desktop system tray.

## Controls and status

The window displays:

- daemon connection state;
- active and startup preset paths;
- active native DSP node count;
- selected physical output target;
- effective-default-sink state;
- overrun, underrun, and DSP processing error counters; and
- warnings for preset nodes omitted by the daemon.

Select an `.effetune_preset` file and use the action button to change the
startup selection. When the daemon is connected, the GUI first applies the
preset live and writes the startup override only after that succeeds. A daemon
rejection therefore leaves the previous startup selection unchanged. If the
live apply succeeds but persistence fails, the new DSP remains active and the
window reports that partial success.

When the daemon is disconnected, the selection is saved without a live apply
and takes effect on the next successful service start. The GUI writes:

```text
$XDG_CONFIG_HOME/pipetune/environment.gtk
```

When `XDG_CONFIG_HOME` is unset, this resolves to
`~/.config/pipetune/environment.gtk`. The directory is mode `0700`, the file
is mode `0600`, and replacement is atomic. The installed systemd user unit
reads the normal `environment` file first and this optional GUI-managed file
second, so the GUI selection wins.

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
window. Use `pipetune-gtk --help` and `pipetune-gtk --version` for the remaining
command-line information.

## Install and autostart

The root installation installs the daemon, GUI, service, desktop entry,
autostart entry, and icon together:

```sh
sudo make install PREFIX=/usr
```

With that prefix, the GUI integration is installed as:

```text
/usr/bin/pipetune-gtk
/usr/share/applications/net.kekyo.pipetune-gtk.desktop
/usr/share/icons/hicolor/scalable/apps/pipetune.svg
/etc/xdg/autostart/net.kekyo.pipetune-gtk.desktop
```

The autostart entry runs `pipetune-gtk --hidden` at desktop login. A user can
disable it without modifying the system file:

```sh
install -d "$HOME/.config/autostart"
cp /etc/xdg/autostart/net.kekyo.pipetune-gtk.desktop \
  "$HOME/.config/autostart/"
desktop-file-edit --set-key=Hidden --set-value=true \
  "$HOME/.config/autostart/net.kekyo.pipetune-gtk.desktop"
```

Before enabling the daemon service, create its required base environment as
described in the [PipeTune documentation](../pipetune/README.md).
