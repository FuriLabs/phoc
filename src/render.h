/*
 * Copyright (C) 2020,2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib-object.h>

#include <wlr/render/wlr_renderer.h>

G_BEGIN_DECLS

#define PHOC_TYPE_RENDERER (phoc_renderer_get_type ())

G_DECLARE_FINAL_TYPE (PhocRenderer, phoc_renderer, PHOC, RENDERER, GObject)

typedef struct _PhocOutput PhocOutput;
typedef struct _PhocView PhocView;
typedef struct _PhocLayerSurface PhocLayerSurface;


typedef struct _PhocRenderContext {
  PhocOutput                 *output;
  pixman_region32_t          *damage;
  float                       alpha;
  struct wlr_render_pass     *render_pass;
  enum wlr_scale_filter_mode  tex_filter;
  /* The renderer driving this frame, set by phoc_renderer_render_output() */
  PhocRenderer               *renderer;
  /* The largest blur radius asked for on this output, walked once by the
   * output rather than rediscovered per surface. 0 means nothing is blurred,
   * or the blur cannot be drawn at all. */
  guint                       blur_radius;
  /* This frame's damage in buffer coordinates, set while the frame is drawn.
   * Raw GL draws clip to it the way the wlroots render pass does. */
  pixman_region32_t          *buffer_damage;
} PhocRenderContext;


PhocRenderer *phoc_renderer_new (struct wlr_backend *wlr_backend, GError **error);

void          phoc_renderer_render_output (PhocRenderer      *self,
                                           PhocOutput        *output,
                                           PhocRenderContext *context);
gboolean      phoc_renderer_render_view_to_buffer (PhocRenderer           *self,
                                                   PhocView               *view,
                                                   struct wlr_buffer      *data);
void          phoc_renderer_set_blur_enabled      (PhocRenderer           *self,
                                                   gboolean                enabled);
gboolean      phoc_renderer_blur_wants_whole_frame (PhocRenderer          *self,
                                                    PhocOutput            *output,
                                                    guint                  radius);
void          phoc_renderer_blur_source_changed   (PhocRenderer           *self,
                                                   PhocOutput             *output,
                                                   PhocLayerSurface       *layer_surface,
                                                   gboolean                everything);
gboolean      phoc_renderer_get_blur_enabled      (PhocRenderer           *self);
void          phoc_renderer_finish_frame          (PhocRenderer           *self);
void          phoc_renderer_forget_output         (PhocRenderer           *self,
                                                   PhocOutput             *output);

G_END_DECLS
