/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phoc-xdg-surface"

#include "phoc-config.h"

#include "cursor.h"
#include "server.h"
#include "xdg-surface.h"
#include "xdg-surface-private.h"

#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

enum {
  PROP_0,
  PROP_WLR_XDG_SURFACE,
  PROP_LAST_PROP
};

static GParamSpec *props[PROP_LAST_PROP];

G_DEFINE_TYPE (PhocXdgSurface, phoc_xdg_surface, PHOC_TYPE_VIEW)

static void
phoc_xdg_surface_set_property (GObject      *object,
                               guint         property_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  PhocXdgSurface *self = PHOC_XDG_SURFACE (object);

  switch (property_id) {
  case PROP_WLR_XDG_SURFACE:
    self->xdg_surface = g_value_get_pointer (value);
    self->xdg_surface->data = self;
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

/**
 * phoc_xdg_surface_from_view:
 * @view: A view
 *
 * Returns the [class@XdgSurface] associated with this
 * [type@Phoc.View]. It is a programming error if the [class@View]
 * isn't a [type@XdgSurface].
 *
 * Returns: (transfer none): Returns the [type@XdgSurface]
 */
static PhocXdgSurface *
phoc_xdg_surface_from_view (PhocView *view) {
  g_assert (PHOC_IS_XDG_SURFACE (view));
  return PHOC_XDG_SURFACE (view);
}


static void set_active(PhocView *view, bool active) {
	struct wlr_xdg_surface *xdg_surface =
		phoc_xdg_surface_from_view (view)->xdg_surface;
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wlr_xdg_toplevel_set_activated(xdg_surface, active);
	}
}

static void apply_size_constraints(struct wlr_xdg_surface *xdg_surface,
		uint32_t width, uint32_t height, uint32_t *dest_width,
		uint32_t *dest_height) {
	*dest_width = width;
	*dest_height = height;

	struct wlr_xdg_toplevel_state *state = &xdg_surface->toplevel->current;
	if (width < state->min_width) {
		*dest_width = state->min_width;
	} else if (state->max_width > 0 &&
			width > state->max_width) {
		*dest_width = state->max_width;
	}
	if (height < state->min_height) {
		*dest_height = state->min_height;
	} else if (state->max_height > 0 &&
			height > state->max_height) {
		*dest_height = state->max_height;
	}
}

static void resize(PhocView *view, uint32_t width, uint32_t height) {
	struct wlr_xdg_surface *xdg_surface =
		phoc_xdg_surface_from_view (view)->xdg_surface;
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(xdg_surface, width, height, &constrained_width,
		&constrained_height);

	if (xdg_surface->toplevel->scheduled.width == constrained_width &&
	    xdg_surface->toplevel->scheduled.height == constrained_height)
		return;

	wlr_xdg_toplevel_set_size(xdg_surface, constrained_width,
		constrained_height);

	view_send_frame_done_if_not_visible (view);
}

static void move_resize(PhocView *view, double x, double y,
		uint32_t width, uint32_t height) {
	PhocXdgSurface *xdg_surface =
		phoc_xdg_surface_from_view (view);
	struct wlr_xdg_surface *wlr_xdg_surface = xdg_surface->xdg_surface;
	if (wlr_xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	bool update_x = x != view->box.x;
	bool update_y = y != view->box.y;

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(wlr_xdg_surface, width, height, &constrained_width,
		&constrained_height);

	if (update_x) {
		x = x + width - constrained_width;
	}
	if (update_y) {
		y = y + height - constrained_height;
	}

	view->pending_move_resize.update_x = update_x;
	view->pending_move_resize.update_y = update_y;
	view->pending_move_resize.x = x;
	view->pending_move_resize.y = y;
	view->pending_move_resize.width = constrained_width;
	view->pending_move_resize.height = constrained_height;

	if (wlr_xdg_surface->toplevel->scheduled.width == constrained_width &&
	    wlr_xdg_surface->toplevel->scheduled.height == constrained_height) {
		view_update_position(view, x, y);
	} else {
		xdg_surface->pending_move_resize_configure_serial =
			wlr_xdg_toplevel_set_size(wlr_xdg_surface, constrained_width, constrained_height);
	}

	view_send_frame_done_if_not_visible (view);
}

static bool want_scaling(PhocView *view) {
	return true;
}

static bool want_auto_maximize(PhocView *view) {
	struct wlr_xdg_surface *surface =
		phoc_xdg_surface_from_view (view)->xdg_surface;

	return surface->toplevel && !surface->toplevel->parent;
}

static void set_maximized(PhocView *view, bool maximized) {
	struct wlr_xdg_surface *xdg_surface =
		phoc_xdg_surface_from_view (view)->xdg_surface;
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	wlr_xdg_toplevel_set_maximized(xdg_surface, maximized);
}

static void
set_tiled (PhocView *view, bool tiled)
{
  struct wlr_xdg_surface *xdg_surface = phoc_xdg_surface_from_view (view)->xdg_surface;

  if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
    return;

  if (!tiled) {
    wlr_xdg_toplevel_set_tiled (xdg_surface, WLR_EDGE_NONE);
    return;
  }

  switch (view->tile_direction) {
    case PHOC_VIEW_TILE_LEFT:
      wlr_xdg_toplevel_set_tiled (xdg_surface, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT);
      break;
    case PHOC_VIEW_TILE_RIGHT:
      wlr_xdg_toplevel_set_tiled (xdg_surface, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
      break;
    default:
      g_warn_if_reached ();
  }
}

static void set_fullscreen(PhocView *view, bool fullscreen) {
	struct wlr_xdg_surface *xdg_surface =
		phoc_xdg_surface_from_view (view)->xdg_surface;
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	wlr_xdg_toplevel_set_fullscreen(xdg_surface, fullscreen);
}

static void _close(PhocView *view) {
	struct wlr_xdg_surface *xdg_surface =
		phoc_xdg_surface_from_view (view)->xdg_surface;
	struct wlr_xdg_popup *popup, *tmp = NULL;
	wl_list_for_each_safe (popup, tmp, &xdg_surface->popups, link) {
		wlr_xdg_popup_destroy(popup->base);
	}
	wlr_xdg_toplevel_send_close(xdg_surface);

	view_send_frame_done_if_not_visible (view);
}

static void for_each_surface(PhocView *view,
		wlr_surface_iterator_func_t iterator, void *user_data) {
	struct wlr_xdg_surface *xdg_surface =
		phoc_xdg_surface_from_view (view)->xdg_surface;
	wlr_xdg_surface_for_each_surface(xdg_surface, iterator, user_data);
}

static void get_geometry(PhocView *view, struct wlr_box *geom) {
        phoc_xdg_surface_get_geometry (phoc_xdg_surface_from_view (view), geom);
}


static void get_size(PhocView *view, struct wlr_box *box) {
	struct wlr_xdg_surface *xdg_surface =
		phoc_xdg_surface_from_view (view)->xdg_surface;

	struct wlr_box geo_box;
	wlr_xdg_surface_get_geometry(xdg_surface, &geo_box);
	box->width = geo_box.width;
	box->height = geo_box.height;
}


static
void handle_surface_commit (struct wl_listener *listener, void *data)
{
  PhocXdgSurface *phoc_surface =
    wl_container_of(listener, phoc_surface, surface_commit);
  PhocView *view = PHOC_VIEW (phoc_surface);
  struct wlr_xdg_surface *surface = phoc_surface->xdg_surface;

  if (!surface->mapped) {
    return;
  }

  phoc_view_apply_damage(view);

  struct wlr_box size;
  get_size(view, &size);
  view_update_size(view, size.width, size.height);

  uint32_t pending_serial =
    phoc_surface->pending_move_resize_configure_serial;
  if (pending_serial > 0 && pending_serial >= surface->current.configure_serial) {
    double x = view->box.x;
    double y = view->box.y;
    if (view->pending_move_resize.update_x) {
      if (view_is_floating (view)) {
        x = view->pending_move_resize.x + view->pending_move_resize.width -
          size.width;
      } else {
        x = view->pending_move_resize.x;
      }
    }
    if (view->pending_move_resize.update_y) {
      if (view_is_floating (view)) {
        y = view->pending_move_resize.y + view->pending_move_resize.height -
          size.height;
      } else {
        y = view->pending_move_resize.y;
      }
    }
    view_update_position(view, x, y);

    if (pending_serial == surface->current.configure_serial) {
      phoc_surface->pending_move_resize_configure_serial = 0;
    }
  }

  struct wlr_box geometry;
  phoc_xdg_surface_get_geometry (phoc_surface, &geometry);
  if (phoc_surface->saved_geometry.x != geometry.x || phoc_surface->saved_geometry.y != geometry.y) {
    view_update_position(view,
                         view->box.x + (phoc_surface->saved_geometry.x - geometry.x) * view->scale,
                         view->box.y + (phoc_surface->saved_geometry.y - geometry.y) * view->scale);
  }
  phoc_surface->saved_geometry = geometry;
}


static void
handle_destroy (struct wl_listener *listener, void *data)
{
  PhocXdgSurface *phoc_xdg_surface =
    wl_container_of(listener, phoc_xdg_surface, destroy);
  g_object_unref (phoc_xdg_surface);
}


static void handle_map(struct wl_listener *listener, void *data) {
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, map);
	PhocView *view = PHOC_VIEW (phoc_xdg_surface);

	struct wlr_box box;
	get_size(view, &box);
	view->box.width = box.width;
	view->box.height = box.height;
	phoc_xdg_surface_get_geometry (phoc_xdg_surface, &phoc_xdg_surface->saved_geometry);

	phoc_view_map(view, phoc_xdg_surface->xdg_surface->surface);
	view_setup(view);
}


static void handle_unmap(struct wl_listener *listener, void *data) {
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, unmap);
	view_unmap (PHOC_VIEW (phoc_xdg_surface));
}


static void handle_request_move(struct wl_listener *listener, void *data) {
	PhocServer *server = phoc_server_get_default ();
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, request_move);
	PhocView *view = PHOC_VIEW (phoc_xdg_surface);
	PhocInput *input = server->input;
	struct wlr_xdg_toplevel_move_event *e = data;
	PhocSeat *seat = phoc_input_seat_from_wlr_seat(input, e->seat->seat);

	// TODO verify event serial
	if (!seat || phoc_seat_get_cursor(seat)->mode != PHOC_CURSOR_PASSTHROUGH) {
		return;
	}
	phoc_seat_begin_move(seat, view);
}


static void handle_request_resize(struct wl_listener *listener, void *data) {
	PhocServer *server = phoc_server_get_default ();
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, request_resize);
	PhocView *view = PHOC_VIEW (phoc_xdg_surface);
	PhocInput *input = server->input;
	struct wlr_xdg_toplevel_resize_event *e = data;
	PhocSeat *seat = phoc_input_seat_from_wlr_seat(input, e->seat->seat);

	// TODO verify event serial
	g_assert (seat);
	if (!seat || phoc_seat_get_cursor(seat)->mode != PHOC_CURSOR_PASSTHROUGH) {
		return;
	}
	phoc_seat_begin_resize(seat, view, e->edges);
}


static void handle_request_maximize(struct wl_listener *listener, void *data) {
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, request_maximize);
	PhocView *view = PHOC_VIEW (phoc_xdg_surface);
	struct wlr_xdg_surface *surface = phoc_xdg_surface->xdg_surface;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	if (surface->toplevel->requested.maximized) {
		view_maximize(view, NULL);
	} else {
		view_restore(view);
	}
}


static void handle_request_fullscreen(struct wl_listener *listener,
		void *data) {
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, request_fullscreen);
	PhocView *view = PHOC_VIEW (phoc_xdg_surface);
	struct wlr_xdg_surface *surface = phoc_xdg_surface->xdg_surface;
	struct wlr_xdg_toplevel_set_fullscreen_event *e = data;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	phoc_view_set_fullscreen(view, e->fullscreen, e->output);
}


static void handle_set_title(struct wl_listener *listener, void *data) {
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, set_title);

	view_set_title(&phoc_xdg_surface->view,
			phoc_xdg_surface->xdg_surface->toplevel->title);
}


static void handle_set_app_id(struct wl_listener *listener, void *data) {
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, set_app_id);

	phoc_view_set_app_id(&phoc_xdg_surface->view,
			     phoc_xdg_surface->xdg_surface->toplevel->app_id);
}


static void handle_set_parent(struct wl_listener *listener, void *data) {
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, set_parent);

	if (phoc_xdg_surface->xdg_surface->toplevel->parent) {
		PhocXdgSurface *parent = phoc_xdg_surface->xdg_surface->toplevel->parent->data;
		view_set_parent(&phoc_xdg_surface->view, &parent->view);
	} else {
		view_set_parent(&phoc_xdg_surface->view, NULL);
	}
}


static void handle_new_popup(struct wl_listener *listener, void *data) {
	PhocXdgSurface *phoc_xdg_surface =
		wl_container_of(listener, phoc_xdg_surface, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	phoc_xdg_popup_create (PHOC_VIEW (phoc_xdg_surface), wlr_popup);
}


static void
phoc_xdg_surface_constructed (GObject *object)
{
  PhocXdgSurface *self = PHOC_XDG_SURFACE(object);

  g_assert (self->xdg_surface);

  G_OBJECT_CLASS (phoc_xdg_surface_parent_class)->constructed (object);

  self->surface_commit.notify = handle_surface_commit;
  wl_signal_add (&self->xdg_surface->surface->events.commit, &self->surface_commit);

  self->destroy.notify = handle_destroy;
  wl_signal_add (&self->xdg_surface->events.destroy, &self->destroy);

  self->map.notify = handle_map;
  wl_signal_add (&self->xdg_surface->events.map, &self->map);

  self->unmap.notify = handle_unmap;
  wl_signal_add (&self->xdg_surface->events.unmap, &self->unmap);

  self->request_move.notify = handle_request_move;
  wl_signal_add (&self->xdg_surface->toplevel->events.request_move, &self->request_move);

  self->request_resize.notify = handle_request_resize;
  wl_signal_add(&self->xdg_surface->toplevel->events.request_resize, &self->request_resize);

  self->request_maximize.notify = handle_request_maximize;
  wl_signal_add(&self->xdg_surface->toplevel->events.request_maximize, &self->request_maximize);

  self->request_fullscreen.notify = handle_request_fullscreen;
  wl_signal_add(&self->xdg_surface->toplevel->events.request_fullscreen, &self->request_fullscreen);

  self->set_title.notify = handle_set_title;
  wl_signal_add(&self->xdg_surface->toplevel->events.set_title, &self->set_title);

  self->set_app_id.notify = handle_set_app_id;
  wl_signal_add(&self->xdg_surface->toplevel->events.set_app_id, &self->set_app_id);

  self->set_parent.notify = handle_set_parent;
  wl_signal_add(&self->xdg_surface->toplevel->events.set_parent, &self->set_parent);

  self->new_popup.notify = handle_new_popup;
  wl_signal_add(&self->xdg_surface->events.new_popup, &self->new_popup);
}


static void
phoc_xdg_surface_finalize (GObject *object)
{
  PhocXdgSurface *self = PHOC_XDG_SURFACE(object);

  wl_list_remove(&self->surface_commit.link);
  wl_list_remove(&self->destroy.link);
  wl_list_remove(&self->new_popup.link);
  wl_list_remove(&self->map.link);
  wl_list_remove(&self->unmap.link);
  wl_list_remove(&self->request_move.link);
  wl_list_remove(&self->request_resize.link);
  wl_list_remove(&self->request_maximize.link);
  wl_list_remove(&self->request_fullscreen.link);
  wl_list_remove(&self->set_title.link);
  wl_list_remove(&self->set_app_id.link);
  wl_list_remove(&self->set_parent.link);
  self->xdg_surface->data = NULL;

  G_OBJECT_CLASS (phoc_xdg_surface_parent_class)->finalize (object);
}


static void
phoc_xdg_surface_class_init (PhocXdgSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  PhocViewClass *view_class = PHOC_VIEW_CLASS (klass);

  object_class->constructed = phoc_xdg_surface_constructed;
  object_class->finalize = phoc_xdg_surface_finalize;
  object_class->set_property = phoc_xdg_surface_set_property;

  view_class->resize = resize;
  view_class->move_resize = move_resize;
  view_class->want_auto_maximize = want_auto_maximize;
  view_class->want_scaling = want_scaling;
  view_class->set_active = set_active;
  view_class->set_fullscreen = set_fullscreen;
  view_class->set_maximized = set_maximized;
  view_class->set_tiled = set_tiled;
  view_class->close = _close;
  view_class->for_each_surface = for_each_surface;
  view_class->get_geometry = get_geometry;

  /**
   * PhocXdgSurface:wlr-xdg-surface:
   *
   * The underlying wlroots xdg-surface
   */
  props[PROP_WLR_XDG_SURFACE] =
    g_param_spec_pointer ("wlr-xdg-surface", "", "",
                          G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);
}


static void
phoc_xdg_surface_init (PhocXdgSurface *self)
{
  PHOC_VIEW (self)->type = PHOC_XDG_SHELL_VIEW;
}


PhocXdgSurface *
phoc_xdg_surface_new (struct wlr_xdg_surface *wlr_xdg_surface)
{
  return PHOC_XDG_SURFACE (g_object_new (PHOC_TYPE_XDG_SURFACE,
                                         "wlr-xdg-surface", wlr_xdg_surface,
                                         NULL));
}

void
phoc_xdg_surface_get_geometry (PhocXdgSurface *self, struct wlr_box *geom)
{
  wlr_xdg_surface_get_geometry (self->xdg_surface, geom);
}


struct wlr_surface *
phoc_xdg_surface_get_wlr_surface_at (PhocXdgSurface *self,
                                     double           sx,
                                     double           sy,
                                     double          *sub_x,
                                     double          *sub_y)
{
  g_assert (PHOC_IS_XDG_SURFACE (self));

  return wlr_xdg_surface_surface_at (self->xdg_surface, sx, sy, sub_x, sub_y);
}


void
phoc_xdg_surface_set_decoration (PhocXdgSurface            *self,
                                 PhocXdgToplevelDecoration *decoration)
{
  g_assert (PHOC_IS_XDG_SURFACE (self));

  self->decoration = decoration;
}


PhocXdgToplevelDecoration *
phoc_xdg_surface_get_decoration (PhocXdgSurface *self)
{
  g_assert (PHOC_IS_XDG_SURFACE (self));

  return self->decoration;
}


struct wlr_xdg_surface *
phoc_xdg_surface_get_wlr_xdg_surface (PhocXdgSurface *self)
{
  g_assert (PHOC_IS_XDG_SURFACE (self));

  return self->xdg_surface;
}
