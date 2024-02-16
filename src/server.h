/* Copyright (C) 2019 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#pragma once

#include "render.h"

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include "settings.h"
#include "desktop.h"
#include "input.h"

G_BEGIN_DECLS

#define PHOC_TYPE_SERVER (phoc_server_get_type())

G_DECLARE_FINAL_TYPE (PhocServer, phoc_server, PHOC, SERVER, GObject);

/**
 * PhocServerFlags:
 *
 * PHOC_SHELL_FLAG_SHELL_MODE: Expect a shell to attach
 */
typedef enum _PhocServerFlags {
  PHOC_SERVER_FLAG_NONE       = 0,
  PHOC_SERVER_FLAG_SHELL_MODE = 1 << 0,
} PhocServerFlags;

typedef enum _PhocServerDebugFlags {
  PHOC_SERVER_DEBUG_FLAG_NONE               = 0,
  PHOC_SERVER_DEBUG_FLAG_AUTO_MAXIMIZE      = 1 << 0,
  PHOC_SERVER_DEBUG_FLAG_DAMAGE_TRACKING    = 1 << 1,
  PHOC_SERVER_DEBUG_FLAG_NO_QUIT            = 1 << 2,
  PHOC_SERVER_DEBUG_FLAG_TOUCH_POINTS       = 1 << 3,
  PHOC_SERVER_DEBUG_FLAG_LAYER_SHELL        = 1 << 4,
  PHOC_SERVER_DEBUG_FLAG_CUTOUTS            = 1 << 5,
  PHOC_SERVER_DEBUG_FLAG_DISABLE_ANIMATIONS = 1 << 6,
} PhocServerDebugFlags;

/**
 * PhocServer:
 *
 * The server singleton.
 *
 * Maintains the compositors state.
 */
/* TODO: we keep the struct public due to heaps of direct access
   which will be replaced by getters and setters over time */
struct _PhocServer {
  GObject parent;

  /* Phoc resources */
  PhocServerFlags flags;
  PhocServerDebugFlags debug_flags;

  /* Wayland resources */
  struct wl_display *wl_display;
  guint wl_source;

  /* WLR tools */
  struct wlr_compositor *compositor;
  struct wlr_subcompositor *subcompositor;
  struct wlr_backend    *backend;
  PhocRenderer          *renderer;

  /* Global resources */
  struct wlr_data_device_manager *data_device_manager;
};

PhocServer        *phoc_server_get_default (void);
gboolean           phoc_server_setup (PhocServer *self, PhocConfig *config,
                                      const char *exec, GMainLoop *mainloop,
                                      PhocServerFlags flags,
                                      PhocServerDebugFlags debug_flags);
const char        *phoc_server_get_session_exec (PhocServer *self);
gint               phoc_server_get_session_exit_status (PhocServer *self);
PhocRenderer      *phoc_server_get_renderer (PhocServer *self);
PhocDesktop       *phoc_server_get_desktop (PhocServer *self);
PhocInput         *phoc_server_get_input (PhocServer *self);
PhocConfig        *phoc_server_get_config (PhocServer *self);
const char *const *phoc_server_get_compatibles (PhocServer *self);
PhocSeat          *phoc_server_get_last_active_seat (PhocServer *self);
struct wlr_session *phoc_server_get_session (PhocServer *self);
void               phoc_server_set_linux_dmabuf_surface_feedback (PhocServer  *self,
                                                                  PhocView    *view,
                                                                  PhocOutput  *output,
                                                                  bool         enable);

G_END_DECLS
