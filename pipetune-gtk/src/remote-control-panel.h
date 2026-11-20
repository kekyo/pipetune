/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_REMOTE_CONTROL_PANEL_H
#define PIPETUNE_GTK_REMOTE_CONTROL_PANEL_H

#include "control-client.h"
#include <gtk/gtk.h>

namespace pipetune_gtk {

/** State of the remote control page in one open GTK window. */
struct RemoteControlPanel;

/**
 * Connects the receiver page to its controls.
 * @param builder Builder that owns the main window widgets.
 * @return Page state owned by the caller until destroyRemoteControlPanel().
 */
RemoteControlPanel *createRemoteControlPanel(GtkBuilder *builder);

/**
 * Presents the authoritative listener state and fetches its pairing link on demand.
 * @param panel Receiver page owned by the GTK window.
 * @param client Same-user control connection for listener commands.
 * @param listener Current status, or null when disconnected.
 */
void updateRemoteControlPanel(RemoteControlPanel *panel, ControlClient *client,
                              const pipetune::RemoteListenerStatus *listener);

/**
 * Disconnects the page signals after pending control callbacks have been canceled.
 * @param panel Page state to release, or null.
 */
void destroyRemoteControlPanel(RemoteControlPanel *panel);

} // namespace pipetune_gtk
#endif
