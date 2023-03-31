/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phoc-xwayland-surface"

#include "phoc-config.h"
#include "cursor.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include "xwayland-surface.h"

#include <wlr/xwayland.h>

enum {
  PROP_0,
  PROP_WLR_XWAYLAND_SURFACE,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

G_DEFINE_TYPE (PhocXWaylandSurface, phoc_xwayland_surface, PHOC_TYPE_VIEW)

static
bool is_moveable(PhocView *view)
{
	PhocServer *server = phoc_server_get_default ();
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view (view)->xwayland_surface;

	if (xwayland_surface->window_type == NULL)
		return true;

	for (guint i = 0; i < xwayland_surface->window_type_len; i++)
		if (xwayland_surface->window_type[i] != server->desktop->xwayland_atoms[NET_WM_WINDOW_TYPE_NORMAL] &&
		    xwayland_surface->window_type[i] != server->desktop->xwayland_atoms[NET_WM_WINDOW_TYPE_DIALOG])
			return false;

	return true;
}

static void set_active(PhocView *view, bool active) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;
	wlr_xwayland_surface_activate(xwayland_surface, active);
	wlr_xwayland_surface_restack(xwayland_surface, NULL, XCB_STACK_MODE_ABOVE);
}

static void move(PhocView *view, double x, double y) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;

	if (!is_moveable (view))
		return;

	view_update_position(view, x, y);
	wlr_xwayland_surface_configure(xwayland_surface, x, y,
		xwayland_surface->width, xwayland_surface->height);
}

static void apply_size_constraints(PhocView *view,
		struct wlr_xwayland_surface *xwayland_surface, uint32_t width,
		uint32_t height, uint32_t *dest_width, uint32_t *dest_height) {
	*dest_width = width;
	*dest_height = height;

	if (view_is_maximized(view))
		return;

	struct wlr_xwayland_surface_size_hints *size_hints =
		xwayland_surface->size_hints;
	if (size_hints != NULL) {
		if (width < (uint32_t)size_hints->min_width) {
			*dest_width = size_hints->min_width;
		} else if (size_hints->max_width > 0 &&
				width > (uint32_t)size_hints->max_width) {
			*dest_width = size_hints->max_width;
		}
		if (height < (uint32_t)size_hints->min_height) {
			*dest_height = size_hints->min_height;
		} else if (size_hints->max_height > 0 &&
				height > (uint32_t)size_hints->max_height) {
			*dest_height = size_hints->max_height;
		}
	}
}

static void resize(PhocView *view, uint32_t width, uint32_t height) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(view, xwayland_surface, width, height, &constrained_width,
		&constrained_height);

	wlr_xwayland_surface_configure(xwayland_surface, xwayland_surface->x,
			xwayland_surface->y, constrained_width, constrained_height);
}

static void move_resize(PhocView *view, double x, double y,
		uint32_t width, uint32_t height) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;

	if (!is_moveable (view)) {
		x = view->box.x;
		y = view->box.y;
	}

	bool update_x = x != view->box.x;
	bool update_y = y != view->box.y;

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(view, xwayland_surface, width, height, &constrained_width,
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

	wlr_xwayland_surface_configure(xwayland_surface, x, y, constrained_width,
		constrained_height);
}

static void _close(PhocView *view) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;
	wlr_xwayland_surface_close(xwayland_surface);
}


static bool want_scaling(PhocView *view) {
	return false;
}

static bool want_auto_maximize(PhocView *view) {
	struct wlr_xwayland_surface *surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;

	if (surface->size_hints &&
	    (surface->size_hints->min_width > 0 && surface->size_hints->min_width == surface->size_hints->max_width) &&
	    (surface->size_hints->min_height > 0 && surface->size_hints->min_height == surface->size_hints->max_height))
		return false;

	return is_moveable(view);
}

static void set_maximized(PhocView *view, bool maximized) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;
	wlr_xwayland_surface_set_maximized(xwayland_surface, maximized);
}

static void set_fullscreen(PhocView *view, bool fullscreen) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;
	wlr_xwayland_surface_set_fullscreen(xwayland_surface, fullscreen);
}

static void
phoc_xwayland_surface_set_property (GObject      *object,
                                    guint         property_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  PhocXWaylandSurface *self = PHOC_XWAYLAND_SURFACE (object);

  switch (property_id) {
  case PROP_WLR_XWAYLAND_SURFACE:
    self->xwayland_surface = g_value_get_pointer (value);
    self->xwayland_surface->data = self;
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


/* {{{ wlr_xwayland_surface signal handlers */

static void
handle_destroy (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, destroy);

  g_signal_emit_by_name (phoc_surface, "surface-destroy");
  g_object_unref (phoc_surface);
}

static void
handle_request_configure (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface =  wl_container_of (listener, phoc_surface, request_configure);
  struct wlr_xwayland_surface *xwayland_surface = phoc_surface->xwayland_surface;
  struct wlr_xwayland_surface_configure_event *event = data;

  view_update_position (PHOC_VIEW (phoc_surface), event->x, event->y);

  wlr_xwayland_surface_configure (xwayland_surface, event->x, event->y, event->width, event->height);
}

static PhocSeat *
guess_seat_for_view (PhocView *view)
{
  // the best we can do is to pick the first seat that has the surface focused
  // for the pointer
  PhocServer *server = phoc_server_get_default ();
  PhocInput *input = server->input;

  for (GSList *elem = phoc_input_get_seats (input); elem; elem = elem->next) {
    PhocSeat *seat = PHOC_SEAT (elem->data);

    g_assert (PHOC_IS_SEAT (seat));
    if (seat->seat->pointer_state.focused_surface == view->wlr_surface) {
      return seat;
    }
  }
  return NULL;
}


static void
handle_request_move (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, request_move);
  PhocView *view = PHOC_VIEW (phoc_surface);
  PhocSeat *seat = guess_seat_for_view (view);

  if (!seat || phoc_seat_get_cursor (seat)->mode != PHOC_CURSOR_PASSTHROUGH)
    return;

  phoc_seat_begin_move (seat, view);
}

static void
handle_request_resize (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, request_resize);
  PhocView *view = PHOC_VIEW (phoc_surface);
  PhocSeat *seat = guess_seat_for_view (view);
  struct wlr_xwayland_resize_event *e = data;

  if (!seat || phoc_seat_get_cursor (seat)->mode != PHOC_CURSOR_PASSTHROUGH)
    return;

  phoc_seat_begin_resize (seat, view, e->edges);
}

static void
handle_request_maximize (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, request_maximize);
  PhocView *view = PHOC_VIEW (phoc_surface);
  struct wlr_xwayland_surface *xwayland_surface = phoc_surface->xwayland_surface;
  bool maximized = xwayland_surface->maximized_vert && xwayland_surface->maximized_horz;

  if (maximized)
    view_maximize (view, NULL);
  else
    view_restore (view);
}

static void
handle_request_fullscreen (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, request_fullscreen);
  PhocView *view = PHOC_VIEW (phoc_surface);
  struct wlr_xwayland_surface *xwayland_surface = phoc_surface->xwayland_surface;

  phoc_view_set_fullscreen (view, xwayland_surface->fullscreen, NULL);
}

static void
handle_set_title (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, set_title);

  view_set_title (PHOC_VIEW (phoc_surface), phoc_surface->xwayland_surface->title);
}

static void
handle_set_class (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, set_class);

  phoc_view_set_app_id (PHOC_VIEW (phoc_surface), phoc_surface->xwayland_surface->class);
}


static void
handle_set_startup_id (struct wl_listener *listener, void *data)
{
  PhocServer *server = phoc_server_get_default ();
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, set_startup_id);

  g_debug ("Got startup-id %s", phoc_surface->xwayland_surface->startup_id);
  phoc_phosh_private_notify_startup_id (server->desktop->phosh,
                                        phoc_surface->xwayland_surface->startup_id,
                                        PHOSH_PRIVATE_STARTUP_TRACKER_PROTOCOL_X11);
}

static void
handle_surface_commit (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, surface_commit);
  PhocView *view = PHOC_VIEW (phoc_surface);
  struct wlr_surface *wlr_surface = view->wlr_surface;

  phoc_view_apply_damage (view);

  int width = wlr_surface->current.width;
  int height = wlr_surface->current.height;

  view_update_size (view, width, height);

  double x = view->box.x;
  double y = view->box.y;

  if (view->pending_move_resize.update_x) {
    if (view_is_floating (view))
      x = view->pending_move_resize.x + view->pending_move_resize.width - width;
    else
      x = view->pending_move_resize.x;

    view->pending_move_resize.update_x = false;
  }

  if (view->pending_move_resize.update_y) {
    if (view_is_floating (view))
      y = view->pending_move_resize.y + view->pending_move_resize.height - height;
    else
      y = view->pending_move_resize.y;

    view->pending_move_resize.update_y = false;
  }
  view_update_position (view, x, y);
}

static void
handle_map (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, map);
  struct wlr_xwayland_surface *surface = data;
  PhocView *view = PHOC_VIEW (phoc_surface);

  view->box.x = surface->x;
  view->box.y = surface->y;
  view->box.width = surface->surface->current.width;
  view->box.height = surface->surface->current.height;

  phoc_surface->surface_commit.notify = handle_surface_commit;
  wl_signal_add (&surface->surface->events.commit, &phoc_surface->surface_commit);

  if (surface->maximized_horz && surface->maximized_vert)
    view_maximize (view, NULL);

  view_auto_maximize (view);

  phoc_view_map (view, surface->surface);

  if (!surface->override_redirect) {
    if (surface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL)
      phoc_view_set_decoration (view, TRUE, 12, 4);
    view_setup (view);
  } else {
    view_initial_focus (view);
  }
}

static void
handle_unmap (struct wl_listener *listener, void *data)
{
  PhocXWaylandSurface *phoc_surface = wl_container_of (listener, phoc_surface, unmap);
  PhocView *view = PHOC_VIEW (phoc_surface);

  wl_list_remove (&phoc_surface->surface_commit.link);
  view_unmap (view);
}

/* }}} */


static void
phoc_xwayland_surface_constructed (GObject *object)
{
  PhocXWaylandSurface *self = PHOC_XWAYLAND_SURFACE (object);
  struct wlr_xwayland_surface *surface;

  g_assert (self->xwayland_surface);
  surface = self->xwayland_surface;

  G_OBJECT_CLASS (phoc_xwayland_surface_parent_class)->constructed (object);

  self->view.box.x = surface->x;
  self->view.box.y = surface->y;
  self->view.box.width = surface->width;
  self->view.box.height = surface->height;

  view_set_title (PHOC_VIEW (self), surface->title);
  phoc_view_set_app_id (PHOC_VIEW (self), surface->class);

  self->destroy.notify = handle_destroy;
  wl_signal_add(&surface->events.destroy, &self->destroy);

  self->request_configure.notify = handle_request_configure;
  wl_signal_add(&surface->events.request_configure, &self->request_configure);

  self->map.notify = handle_map;
  wl_signal_add(&surface->events.map, &self->map);

  self->unmap.notify = handle_unmap;
  wl_signal_add(&surface->events.unmap, &self->unmap);

  self->request_move.notify = handle_request_move;
  wl_signal_add(&surface->events.request_move, &self->request_move);

  self->request_resize.notify = handle_request_resize;
  wl_signal_add(&surface->events.request_resize, &self->request_resize);

  self->request_maximize.notify = handle_request_maximize;
  wl_signal_add(&surface->events.request_maximize, &self->request_maximize);

  self->request_fullscreen.notify = handle_request_fullscreen;
  wl_signal_add(&surface->events.request_fullscreen, &self->request_fullscreen);

  self->set_title.notify = handle_set_title;
  wl_signal_add(&surface->events.set_title, &self->set_title);

  self->set_class.notify = handle_set_class;
  wl_signal_add(&surface->events.set_class, &self->set_class);

  self->set_startup_id.notify = handle_set_startup_id;
  wl_signal_add(&surface->events.set_startup_id, &self->set_startup_id);
}


static void
phoc_xwayland_surface_finalize (GObject *object)
{
  PhocXWaylandSurface *self = PHOC_XWAYLAND_SURFACE(object);

  wl_list_remove(&self->destroy.link);
  wl_list_remove(&self->request_configure.link);
  wl_list_remove(&self->request_move.link);
  wl_list_remove(&self->request_resize.link);
  wl_list_remove(&self->request_maximize.link);
  wl_list_remove(&self->set_title.link);
  wl_list_remove(&self->set_class.link);
  wl_list_remove(&self->set_startup_id.link);
  wl_list_remove(&self->map.link);
  wl_list_remove(&self->unmap.link);

  self->xwayland_surface->data = NULL;

  G_OBJECT_CLASS (phoc_xwayland_surface_parent_class)->finalize (object);
}


static void
phoc_xwayland_surface_class_init (PhocXWaylandSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  PhocViewClass *view_class = PHOC_VIEW_CLASS (klass);

  object_class->set_property = phoc_xwayland_surface_set_property;
  object_class->constructed = phoc_xwayland_surface_constructed;
  object_class->finalize = phoc_xwayland_surface_finalize;

  view_class->resize = resize;
  view_class->move = move;
  view_class->move_resize = move_resize;
  view_class->want_scaling = want_scaling;
  view_class->want_auto_maximize = want_auto_maximize;
  view_class->set_active = set_active;
  view_class->set_fullscreen = set_fullscreen;
  view_class->set_maximized = set_maximized;
  view_class->close = _close;

  /**
   * PhocXWaylandSurface:wlr-xwayland-surface:
   *
   * The underlying wlroots xwayland-surface
   */
  props[PROP_WLR_XWAYLAND_SURFACE] =
    g_param_spec_pointer ("wlr-xwayland-surface", "", "",
			  G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);
}


static void
phoc_xwayland_surface_init (PhocXWaylandSurface *self)
{
}


PhocXWaylandSurface *
phoc_xwayland_surface_new (struct wlr_xwayland_surface *surface)
{
  return PHOC_XWAYLAND_SURFACE (g_object_new (PHOC_TYPE_XWAYLAND_SURFACE,
                                              "wlr-xwayland-surface", surface,
                                              NULL));
}

/**
 * phoc_xwayland_surface_from_view:
 * @view: A view
 *
 * Returns the [class@XWaylandSurface] associated with this
 * [type@Phoc.View]. It is a programming error if the [class@View]
 * isn't a [type@XWaylandSurface].
 *
 * Returns: (transfer none): Returns the [type@XWaylandSurface]
 */
PhocXWaylandSurface *
phoc_xwayland_surface_from_view (PhocView *view)
{
  g_assert (PHOC_IS_XWAYLAND_SURFACE (view));
  return PHOC_XWAYLAND_SURFACE (view);
}


/**
 * phoc_xwayland_surface_get_wlr_surface:
 * @self: The PhocXWaylandSurface
 *
 * Returns the `wlr_xwayland_surface` associated with this
 * [type@Phoc.XWaylandSurface].
 *
 * TODO: This is a temporary measure to not expose the full [type@PhocXWaylandSurface].
 *   We'll replace this with more specific functions later on.
 *
 * Returns: (transfer none): Returns the `wlr_xwayland_surface`.
 */
struct wlr_xwayland_surface *
phoc_xwayland_surface_get_wlr_surface (PhocXWaylandSurface *self)
{
  g_assert (PHOC_IS_XWAYLAND_SURFACE (self));
  return self->xwayland_surface;
}
