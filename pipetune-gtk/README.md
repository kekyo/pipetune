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
- configured and effective native DSP backends, availability, CPU requirement,
  fallback state, and validation diagnostics;
- configured DSP idle policy, effective active/draining/sleeping/paused state,
  cumulative skipped frames and sleep transitions, and PipeWire graph-idle
  state;
- selectable and preferred physical outputs;
- effective physical output and the engine's selection reason;
- Max or fixed PCM rate and suggest or force behavior;
- selected-device support for 44.1, 48, 96, 192, and 384 kHz;
- final input/DSP, selected output, and active physical rates, including
  PipeWire resampling and transition state;
- effective-default-sink state;
- measured input rate, data rate, and readable stream format;
- average native EffeTune DSP processing time and input-frame budget load;
- overrun, underrun, and DSP processing error counters; and
- warnings for preset nodes omitted by the daemon.

The main window title is **PipeTune**. The status area shows the PipeTune and
EffeTune DSP versions below the output-selection reason.

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
`.effetune_preset` file. Use **Apply and Save** to change the startup
selection, or use **Bypass and Save** to pass audio through without DSP. When
the daemon is connected, the GUI first applies the selected mode live and
writes the startup configuration only after that succeeds. A daemon rejection
therefore leaves the previous startup selection unchanged. If the live apply
succeeds but persistence fails, the new processing mode remains active and
the window reports that partial success.

The **DSP backend** section's **Native engine** drop-down selects **Scalar**,
**SIMD (Auto)**, or an applicable baseline, x86-64-v3, x86-64-v4, or Arm64
SVE tier. Scalar is the compatibility default. Each row shows the availability
and CPU requirement reported by the daemon; the effective field also shows
the concrete runtime tier, startup fallback, and validation diagnostics.

When connected, **Apply and Save** asks the daemon to rebuild and atomically
replace the active preset pipeline before saving the confirmed choice. DSP
histories reset during replacement, and a discontinuity or brief silence is
allowed. A rejection leaves both the previous live backend and persisted
choice unchanged. If live apply succeeds but persistence fails, the new
backend remains active and the GUI reports partial success. Backend controls
are disabled during a PCM rate transition.

When disconnected, **Save for Next Start** performs local CPU, file, ABI, and
catalog validation before persistence. An unavailable pinned SIMD tier is not
saved by that operation. If a previously configured SIMD tier becomes
unavailable at daemon startup, the status displays its lower-SIMD or scalar
fallback and diagnostic.

The **DSP idle** section's **Sleep policy** drop-down selects
**Conservative** or **Exact**. Conservative is the default and permits sleep
after five seconds of exact-zero input plus one second of final DSP output at
or below -150 dBFS. Exact uses the same input interval but requires the final
output to remain mathematically zero for one second.

The **Runtime state** field shows the configured policy, effective DSP state,
cumulative skipped frames, sleep transitions, and whether both PipeWire
streams are paused. When PipeWire has stopped both process callbacks, the GUI
shows **DSP Paused** instead of the controller's retained active or draining
state. A controller that is already sleeping remains **DSP Sleeping**. Any
nonzero input wakes DSP processing in the same callback block. PipeTune resets
the active EffeTune engine through its real-time-safe reset API before
sleeping.

When connected, **Apply and Save** changes the daemon policy live, confirms
the returned policy, and only then updates startup configuration. A rejection
leaves the previous saved choice unchanged. If persistence fails after live
confirmation, the new policy remains active and the window reports partial
success. When disconnected, **Save for Next Start** stores the choice for the
next daemon start. See the
[DSP and PipeWire idling notes](../pipetune/docs/dsp-idle.md) for EMPTY/GAP
propagation and the complete state machine.

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

When connected, **Apply and Save** sends the rate policy to the daemon and
persists it only after the complete live transition succeeds. A daemon
rejection preserves the previous startup policy. A persistence failure leaves
the confirmed live policy active and reports partial success. When
disconnected, **Save for Next Start** writes the selection without attempting
a live transition.

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
absence follows the physical system default. `PIPETUNE_RATE` stores `max` or
one of the five fixed rates, and `PIPETUNE_RATE_ENFORCEMENT` stores `suggest`
or `force`. `PIPETUNE_DSP_BACKEND` stores `scalar` or `simd`, and
`PIPETUNE_DSP_SIMD_VARIANT` stores `auto`, `baseline`, `x86-64-v3`,
`x86-64-v4`, or `sve`. `PIPETUNE_DSP_IDLE_POLICY` stores `conservative` or
`exact`. Missing rate assignments use Max-and-suggest, a missing backend
assignment uses Scalar, a missing SIMD variant uses Auto, and a missing idle
assignment uses Conservative. Updates to any selection preserve the others.

The constant **Configuration** section provides **Reset Configuration…**.
Its modal confirmation defaults to **Cancel**. Confirming invokes the
installed CLI asynchronously as `pipetune config reset --yes`, so the GTK main
loop remains responsive while the configuration is replaced and an active
service is restarted. The GUI then reloads the shared configuration, clears
the preset selection when the reset succeeded, restores the Max-and-suggest
controls, Scalar backend, Auto SIMD preference, and Conservative DSP idle
policy, and reconnects its daemon subscription.

The reset selects startup bypass, removes the preferred output so the system
default is followed, selects Max with Suggest, and selects Scalar with an Auto
SIMD preference and Conservative DSP idling. It replaces unsupported legacy
configuration without backing it up. An inactive service remains inactive. If
restarting an active service fails, the window reports the partial failure
while retaining and displaying the reset startup choices.

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
