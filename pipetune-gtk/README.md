# PipeTune GTK

`pipetune-gtk` is the GTK 3 control application for the per-user PipeTune
daemon. It is a single-instance application that can remain resident in the
desktop system tray.

## Controls and status

The main window uses a persistent two-pane layout. The left pane remains
visible while the right pane switches between **Processing**, **Output**,
**Rate**, **DSP**, and **Advanced** settings. The default window size is
1080 × 680 pixels and its supported minimum is 900 × 560 pixels. Both panes
scroll independently at compact sizes.

The left pane uses sectioned, non-selectable list rows instead of combining
unrelated values into one line. Its always-expanded sections are:

- **System**;
- **Live Configuration**;
- **Saved Configuration**;
- **Routing**;
- **Input / Rates**;
- **DSP / Performance**; and
- **Errors**.

Together they display the daemon connection and virtual-sink state, live and
saved processing choices, routing decisions, input format and rates, DSP
backend and idle state, processing time and load, error counters, and
diagnostics. Long values are ellipsized in the row and remain available in a
tooltip. **Load** is a horizontal level meter aligned to the right of its row.
It grows with the status pane from 150 to 280 pixels and overlays the existing
percentage text at the right edge. The meter uses 11 restrained-saturation hue
steps from teal through muted red. Its graphical fill is capped at 100%, while
the text preserves the measured value above 100% so overload remains explicit.
Numeric status items retain their value, unit, and range separately from their
text presentation, allowing other bounded measurements to adopt the same
component later without changing status acquisition.

The physical-output chooser is a menu popover rather than a plain combo box.
It shows a short, disambiguated device description as the primary label, the
PipeWire node name as ellipsized secondary text, and availability or connector
badges where applicable. The full description and node name are exposed
through the tooltip and accessible description.

## Live preview and persistence

The window treats all settings as one transaction. Opening it captures the
saved startup configuration and the daemon's live configuration. Changing any
control immediately previews that choice in PipeTune; no per-setting apply
button remains. When several fields differ, requests are serialized in this
dependency order:

1. output;
2. sample-rate policy;
3. DSP backend;
4. DSP idle policy; and
5. processing mode or preset.

The global **Apply** button becomes available only after the daemon has
confirmed every requested live change. It atomically writes the complete
configuration snapshot and leaves the window open. The newly saved and live
state then becomes the transaction baseline.

**Cancel**, Escape, and the title-bar close button first restore the live
configuration captured when the window opened, or the latest successfully
applied baseline, and hide the window only after the daemon confirms the
rollback. The startup configuration is not modified. **Restore Defaults** on
the Advanced page follows the same rules: defaults are previewed live and
remain unsaved until the global Apply button is used.

Settings become read-only while the daemon is disconnected. If the connection
drops during a transaction, the desired live state is retained and reapplied
after reconnection. If a subscribed live configuration changes outside the
dialog, editing and Apply stop until the window is closed and reopened, so an
external change cannot be silently overwritten.

If a live request fails, the failed choice is not persisted. Adjusting a
setting permits a retry. If live preview succeeds but atomic persistence
fails, the live choices remain active, the saved snapshot remains unchanged,
the window stays open, and the action log opens with the diagnostic.

## Action log

The full-width **Action Log** drawer at the bottom retains the latest 500
connection, settings, persistence, and application actions in memory.
Pending, successful, warning, and failed actions keep their timestamp,
summary, and diagnostic. The drawer can show all entries, warnings and errors,
or errors only. **Copy** copies the currently filtered history and **Clear**
removes the retained history. A failed action opens the drawer automatically;
closing the drawer does not close the settings window.

## Processing presets

The **EffeTune presets** drop-down contains the standard presets bundled with
the pinned EffeTune release and named presets saved by the EffeTune desktop
application. The standard files are installed below
`$prefix/share/pipetune/effetune-presets`. EffeTune's Linux AppImage stores its
named presets together in:

```text
$XDG_CONFIG_HOME/effetune/effetune_presets.json
```

When `XDG_CONFIG_HOME` is unset, that path resolves to
`~/.config/effetune/effetune_presets.json`. The file is monitored while the
application runs and is also checked whenever the window is presented. Once
an updated file parses as a complete preset object, only the previous
**Saved in EffeTune** entries are replaced. **Standard** entries are retained.
Malformed updates and file deletion retain the last valid entries. Monitoring
does not change the file chooser, its standalone snapshot, or the preset
already loaded by the daemon.

Selecting a named EffeTune preset atomically writes a mode-`0600` standalone
snapshot below
`$XDG_CONFIG_HOME/pipetune/effetune-presets`, with the same HOME fallback as
the startup configuration. The snapshot lets the daemon load one entry from
EffeTune's multi-preset JSON file and remains valid after the AppImage exits.

The **Preset file** chooser remains available for any standalone
`.effetune_preset` file. The **Enable DSP processing** switch selects preset
processing or pass-through bypass. Both the preset selection and the switch
participate in the dialog-wide live preview and persistence transaction.

## DSP backend and idle policy

The DSP page's **Native backend** drop-down selects **Scalar**,
**SIMD (Auto)**, or an applicable baseline, x86-64-v3, x86-64-v4, or Arm64
SVE tier. Scalar is the compatibility default. Each row shows the availability
and CPU requirement reported by the daemon; the status pane also shows the
concrete effective tier, startup fallback, and validation diagnostics. A live
backend change rebuilds and atomically replaces the active preset pipeline.
DSP histories reset during replacement, and a discontinuity or brief silence
is allowed.

The **Idle policy** drop-down selects **Conservative** or **Exact**.
Conservative is the default and permits sleep after five seconds of exact-zero
input plus one second of final DSP output at or below -150 dBFS. Exact uses the
same input interval but requires the final output to remain mathematically zero
for one second.

The status pane shows the configured policy, effective DSP state, cumulative
skipped frames, sleep transitions, and whether the PipeWire graph is idle.
Any nonzero input wakes DSP processing in the same callback block. PipeTune
resets the active EffeTune engine through its real-time-safe reset API before
sleeping. See the
[DSP and PipeWire idling notes](../pipetune/docs/dsp-idle.md) for EMPTY/GAP
propagation and the complete state machine.

## Output and sample rate

The **Preferred physical output** menu starts with **System default**, followed
by the physical outputs enumerated by the daemon. If a persisted preference is
currently disconnected, an unavailable row keeps that preference visible.
The routing status shows whether the daemon is using the preference, the
system default, a fallback, or no device.

The **DSP rate** drop-down contains **Max** followed by 44.1, 48, 96, 192, and
384 kHz. Each fixed row is marked **supported**, **unsupported; PipeWire will
resample**, or **support unknown** using the selected output's capabilities
reported by the daemon. The GUI does not probe the device or resolve rates.

**Max** asks the daemon to use the highest selectable rate supported by the
selected output. A fixed selection remains the input and EffeTune DSP rate
even when unsupported; the daemon chooses a compatible output rate and
PipeWire resamples between them. The **PipeWire request** drop-down selects
**Suggest** or **Force**. `node.rate` remains a PipeWire request rather than a
guaranteed graph rate. Force applies only while PipeTune's playback node is
active and does not rewrite the global PipeWire clock configuration.

The **Effective rates** field passively displays the daemon's final input/DSP
rate, selected output rate, and active physical rate. An idle device reports
`idle`; an R/H difference is labeled **PipeWire resampling**. During a live
transition the field and connection status say that switching is in progress,
and the PCM rate controls are disabled.

The GUI writes the complete applied snapshot to:

```text
$XDG_CONFIG_HOME/pipetune/environment
```

When `XDG_CONFIG_HOME` is unset, this resolves to
`~/.config/pipetune/environment`. The directory is mode `0700`, the file is
mode `0600`, and replacement is atomic. A `PIPETUNE_PRESET` assignment selects
a preset; its absence starts the daemon in pass-through mode. A
`PIPETUNE_TARGET` assignment stores a preferred PipeWire `node.name`; its
absence follows the physical system default. `PIPETUNE_RATE` stores `max` or
one of the five fixed rates, and `PIPETUNE_RATE_ENFORCEMENT` stores `suggest`
or `force`. `PIPETUNE_DSP_BACKEND` stores `scalar` or `simd`, and
`PIPETUNE_DSP_SIMD_VARIANT` stores `auto`, `baseline`, `x86-64-v3`,
`x86-64-v4`, or `sve`. `PIPETUNE_DSP_IDLE_POLICY` stores `conservative` or
`exact`. Missing rate assignments use Max-and-suggest, a missing backend
assignment uses Scalar, a missing SIMD variant uses Auto, and a missing idle
assignment uses Conservative. Apply replaces this file atomically while
retaining restrictive directory and file permissions.

The Advanced page's **Restore Defaults** selects bypass, System default,
Max with Suggest, Scalar with an Auto SIMD preference, and Conservative DSP
idling. It does not restart the service and does not write the environment
file until Apply succeeds.

## Status subscription

The GUI uses the daemon's same-user Unix control socket. It receives an initial
status event and later daemon publications over a persistent asynchronous GIO
connection. The daemon publishes runtime counters and cumulative native
EffeTune processing time, DSP idle counters/state, and PipeWire graph-idle
state once per second. The GUI derives the displayed per-frame average between
publications. It compares that average with the frame duration derived from
the negotiated input sample rate and displays the ratio as **Load**; 100% is
the theoretical real-time deadline, and values above 100% remain visible.
While PipeWire is paused, the DSP controller is sleeping, or the latest active
interval contains no DSP frames, **EffeTune DSP time** displays `—` rather
than retaining an earlier load measurement. It does not poll for status; a
short retry timer is used only to reconnect after the socket becomes
unavailable.

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

## End-to-end tests

`test/e2e` is a private, non-distributed TypeScript project for the GTK dialog.
It requires Node.js 20 or later, pins `gestament` 1.4.0, and uses Vite,
Vitest, and prettier-max. It intentionally has no package-release or screw-up
configuration.

The repository-wide `make test` command installs the locked npm dependencies,
builds the production GTK executable with stable test-only accessibility IDs,
starts a deterministic fake control daemon that speaks the production control
protocol, and runs the dialog under Xvfb. The scenarios verify:

- the persistent status pane, all five settings pages, minimum geometry, and
  compact output-device presentation;
- the DSP Load meter's accessible range, measured value, responsive
  right-aligned width, rendered fill, and hue;
- immediate live changes followed by one dialog-wide atomic Apply;
- rollback before hide through Escape and title-bar close;
- live default restoration without persistence before Apply;
- retained, filtered, cleared, and automatically revealed failure logs;
- persistence failure without loss of the saved snapshot; and
- read-only disconnect behavior followed by pending-state reapplication after
  reconnect.

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
/usr/lib/pipetune/libeffetune-dsp-scalar.so
/usr/lib/pipetune/libeffetune-dsp-simd.so
/usr/share/applications/net.kekyo.pipetune-gtk.desktop
/usr/share/icons/hicolor/scalable/apps/pipetune.svg
/usr/share/pipetune/effetune-presets/
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
for service behavior, optional presets, purge semantics, and recovery. See the
[DSP backend notes](../pipetune/docs/dsp-backends.md) for architecture,
expected preset effects, and benchmarking.
