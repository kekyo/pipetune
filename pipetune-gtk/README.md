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
- selectable and preferred physical outputs;
- effective physical output and the engine's selection reason;
- effective-default-sink state;
- measured input rate, data rate, and readable stream format;
- average native EffeTune DSP processing time and input-frame budget load;
- overrun, underrun, and DSP processing error counters; and
- warnings for preset nodes omitted by the daemon.

The main window title is **PipeTune**. The status area shows the PipeTune and
EffeTune DSP versions below the output-selection reason.

Select an `.effetune_preset` file and use **Apply and Save** to change the
startup selection, or use **Bypass and Save** to pass audio through without
DSP. When the daemon is connected, the GUI first applies the selected mode
live and writes the startup configuration only after that succeeds. A daemon
rejection therefore leaves the previous startup selection unchanged. If the
live apply succeeds but persistence fails, the new processing mode remains
active and the window reports that partial success.

The **Output preference** drop-down starts with **System default**, followed by
the physical outputs enumerated by the daemon. If a persisted preference is
currently disconnected, an unavailable row keeps that preference visible.
The effective-output and reason fields show whether the daemon is using the
preference, the system default, a fallback, or no device.

Output changes are available only while connected. The GUI sends the requested
preference or clear operation to the daemon first and persists it only after
confirmation. The drop-down is disabled while that request is pending. A
rejection restores the engine-reported selection; a later persistence failure
leaves the confirmed live change active and reports partial success. The GUI
does not calculate fallback or hotplug behavior.

Preset and bypass controls retain their existing disconnected behavior: when
the daemon is disconnected, that DSP selection is saved without a live apply
and takes effect on the next successful service start. The GUI writes:

```text
$XDG_CONFIG_HOME/pipetune/environment
```

When `XDG_CONFIG_HOME` is unset, this resolves to
`~/.config/pipetune/environment`. The directory is mode `0700`, the file is
mode `0600`, and replacement is atomic. A `PIPETUNE_PRESET` assignment selects
a preset; its absence starts the daemon in pass-through mode. A
`PIPETUNE_TARGET` assignment stores a preferred PipeWire `node.name`; its
absence follows the physical system default. Updates to either setting
preserve the other.

## Status subscription

The GUI uses the daemon's same-user Unix control socket. It receives an initial
status event and later daemon publications over a persistent asynchronous GIO
connection. The daemon publishes runtime counters and cumulative native
EffeTune processing time once per second. The GUI derives the displayed
per-frame average between publications. It compares that average with the
frame duration derived from the negotiated input sample rate and displays the
ratio as **Load**; 100% is the theoretical real-time deadline, and values above
100% remain visible. It does not poll for status; a short retry timer is used
only to reconnect after the socket becomes unavailable.

## System tray compatibility

The tray backend prefers a StatusNotifierItem host. On X11 it falls back to
`GtkStatusIcon` and XEmbed using the elder-terms compatibility approach.
`GtkStatusIcon` is intentionally retained despite its GTK deprecation because
compatibility with those notification areas is a project requirement.

Closing the window hides it while a tray host is available. The tray icon
opens and presents the window, and its menu provides Open PipeTune and Quit
actions. A `--hidden` start remains unmapped regardless of tray discovery, so
desktop-session autostart does not open a GTK window. In a session without a
tray host, run `pipetune-gtk` normally to present the existing instance.

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

Build as the desktop user, then install the daemon, GUI, service, desktop
entry, autostart entry, and icon together:

```sh
make PREFIX=/usr
sudo make install PREFIX=/usr
```

Remove files recorded by the most recent installation with:

```sh
sudo make uninstall
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
