/*
 * Copyright (C) 2024 The Phosh Developers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phoc-view-child"

#include "phoc-config.h"

#include "desktop.h"
#include "input.h"
#include "output.h"
#include "server.h"
#include "utils.h"
#include "view-private.h"
#include "view-child-private.h"


enum {
  PROP_0,
  PROP_VIEW,
  PROP_WLR_SURFACE,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];


G_DEFINE_TYPE (PhocViewChild, phoc_view_child, G_TYPE_OBJECT)


static bool
phoc_view_child_is_mapped (PhocViewChild *self)
{
  while (self) {
    if (!self->mapped)
      return false;

    self = self->parent;
  }
  return true;
}


static void
phoc_view_child_set_property (GObject      *object,
                              guint         property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  PhocViewChild *self = PHOC_VIEW_CHILD (object);

  switch (property_id) {
  case PROP_VIEW:
    /* TODO: Should hold a ref */
    self->view = g_value_get_object (value);
    break;
  case PROP_WLR_SURFACE:
    self->wlr_surface = g_value_get_pointer (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phoc_view_child_get_property (GObject    *object,
                              guint       property_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  PhocViewChild *self = PHOC_VIEW_CHILD (object);

  switch (property_id) {
  case PROP_VIEW:
    g_value_set_object (value, self->view);
    break;
  case PROP_WLR_SURFACE:
    g_value_set_pointer (value, self->wlr_surface);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phoc_view_child_constructed (GObject *object)
{
  G_OBJECT_CLASS (phoc_view_child_parent_class)->constructed (object);
}


static void
phoc_view_child_finalize (GObject *object)
{
  PhocViewChild *self = PHOC_VIEW_CHILD (object);

  self->view = NULL;
  self->wlr_surface = NULL;

  G_OBJECT_CLASS (phoc_view_child_parent_class)->finalize (object);
}


static void
phoc_view_child_class_init (PhocViewChildClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = phoc_view_child_get_property;
  object_class->set_property = phoc_view_child_set_property;
  object_class->constructed = phoc_view_child_constructed;
  object_class->finalize = phoc_view_child_finalize;

  props[PROP_VIEW] =
    g_param_spec_object ("view", "", "",
                         PHOC_TYPE_VIEW,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_WLR_SURFACE] =
    g_param_spec_pointer ("wlr-surface", "", "",
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);
}


static void
phoc_view_child_init (PhocViewChild *self)
{
}

/**
 * phoc_view_child_apply_damage:
 * @self: A view child
 *
 * This is the equivalent of `phoc_view_apply_damage` but for [type@ViewChild].
 */
void
phoc_view_child_apply_damage (PhocViewChild *self)
{
  if (!self || !phoc_view_child_is_mapped (self) || !phoc_view_is_mapped (self->view))
    return;

  phoc_view_apply_damage (self->view);
}

/**
 * phoc_view_child_damage_whole:
 * @self: A view child
 *
 * This is the equivalent of [method@View.damage_whole] but for
 * [type@ViewChild].
 */
void
phoc_view_child_damage_whole (PhocViewChild *self)
{
  PhocOutput *output;
  int sx, sy;
  struct wlr_box view_box;

  if (!self || !phoc_view_child_is_mapped (self) || !phoc_view_is_mapped (self->view))
    return;

  phoc_view_get_box (self->view, &view_box);
  self->impl->get_pos (self, &sx, &sy);

  wl_list_for_each (output, &self->view->desktop->outputs, link) {
    struct wlr_box output_box;
    wlr_output_layout_get_box (self->view->desktop->layout, output->wlr_output, &output_box);
    phoc_output_damage_whole_local_surface (output, self->wlr_surface,
                                            view_box.x + sx - output_box.x,
                                            view_box.y + sy - output_box.y);

  }
}


void
phoc_view_child_unmap (PhocViewChild *self)
{
  PhocInput *input = phoc_server_get_input (phoc_server_get_default ());

  phoc_view_child_damage_whole (self);
  phoc_input_update_cursor_focus (input);
  self->mapped = false;
}


void
phoc_view_child_map (PhocViewChild *self, struct wlr_surface *wlr_surface)
{
  PhocInput *input = phoc_server_get_input (phoc_server_get_default ());
  PhocView *view = self->view;

  self->mapped = true;
  phoc_view_child_damage_whole (self);

  struct wlr_box box;
  phoc_view_get_box (view, &box);

  PhocOutput *output;
  wl_list_for_each (output, &view->desktop->outputs, link) {
    bool intersects = wlr_output_layout_intersects (view->desktop->layout,
                                                    output->wlr_output, &box);
    if (intersects)
      phoc_utils_wlr_surface_enter_output (wlr_surface, output->wlr_output);
  }

  phoc_input_update_cursor_focus (input);
}
