#define G_LOG_DOMAIN "phoc-xdg-shell"

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "cursor.h"
#include "desktop.h"
#include "input.h"
#include "server.h"
#include "view.h"

static const struct phoc_view_child_interface popup_impl;

static void popup_destroy(PhocViewChild *child) {
	assert(child->impl == &popup_impl);
	struct roots_xdg_popup *popup = (struct roots_xdg_popup *)child;
	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->new_popup.link);
	wl_list_remove(&popup->map.link);
	wl_list_remove(&popup->unmap.link);
	free(popup);
}

static const struct phoc_view_child_interface popup_impl = {
	.destroy = popup_destroy,
};

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct roots_xdg_popup *popup =
		wl_container_of(listener, popup, destroy);
	phoc_view_child_destroy(&popup->child);
}

static void popup_handle_map(struct wl_listener *listener, void *data) {
	PhocServer *server = phoc_server_get_default ();
	struct roots_xdg_popup *popup = wl_container_of(listener, popup, map);
	phoc_view_child_damage_whole (&popup->child);
	phoc_input_update_cursor_focus(server->input);
	popup->child.mapped = true;
}

static void popup_handle_unmap(struct wl_listener *listener, void *data) {
	PhocServer *server = phoc_server_get_default ();
	struct roots_xdg_popup *popup = wl_container_of(listener, popup, unmap);
	phoc_view_child_damage_whole (&popup->child);
	phoc_input_update_cursor_focus(server->input);
	popup->child.mapped = false;
}

static struct roots_xdg_popup *popup_create(PhocView *view,
	struct wlr_xdg_popup *wlr_popup);

static void popup_handle_new_popup(struct wl_listener *listener, void *data) {
	struct roots_xdg_popup *popup =
		wl_container_of(listener, popup, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	popup_create(popup->child.view, wlr_popup);
}

static void popup_unconstrain(struct roots_xdg_popup *popup) {
	// get the output of the popup's positioner anchor point and convert it to
	// the toplevel parent's coordinate system and then pass it to
	// wlr_xdg_popup_unconstrain_from_box
	PhocView *view = popup->child.view;

	struct wlr_output_layout *layout = view->desktop->layout;
	struct wlr_xdg_popup *wlr_popup = popup->wlr_popup;

	int anchor_lx, anchor_ly;
	wlr_xdg_popup_get_anchor_point(wlr_popup, &anchor_lx, &anchor_ly);

	int popup_lx, popup_ly;
	wlr_xdg_popup_get_toplevel_coords(wlr_popup, wlr_popup->geometry.x,
		wlr_popup->geometry.y, &popup_lx, &popup_ly);
	popup_lx += view->box.x;
	popup_ly += view->box.y;

	anchor_lx += popup_lx;
	anchor_ly += popup_ly;

	double dest_x = 0, dest_y = 0;
	wlr_output_layout_closest_point(layout, NULL, anchor_lx, anchor_ly,
		&dest_x, &dest_y);

	struct wlr_output *output =
		wlr_output_layout_output_at(layout, dest_x, dest_y);
	if (output == NULL) {
		return;
	}

	struct wlr_box *output_box =
		wlr_output_layout_get_box(view->desktop->layout, output);
	PhocOutput *phoc_output = output->data;
	struct wlr_box usable_area = phoc_output->usable_area;
	usable_area.x += output_box->x;
	usable_area.y += output_box->y;

	// the output box expressed in the coordinate system of the toplevel parent
	// of the popup
	struct wlr_box output_toplevel_sx_box = {
		.x = usable_area.x - view->box.x,
		.y = usable_area.y - view->box.y,
		.width = usable_area.width,
		.height = usable_area.height,
	};

	wlr_xdg_popup_unconstrain_from_box(
			popup->wlr_popup, &output_toplevel_sx_box);
}

static struct roots_xdg_popup *popup_create(PhocView *view,
		struct wlr_xdg_popup *wlr_popup) {
	struct roots_xdg_popup *popup =
		calloc(1, sizeof(struct roots_xdg_popup));
	if (popup == NULL) {
		return NULL;
	}
	popup->wlr_popup = wlr_popup;
	phoc_view_child_init(&popup->child, &popup_impl,
			     view, wlr_popup->base->surface);
	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&wlr_popup->base->events.destroy, &popup->destroy);
	popup->map.notify = popup_handle_map;
	wl_signal_add(&wlr_popup->base->events.map, &popup->map);
	popup->unmap.notify = popup_handle_unmap;
	wl_signal_add(&wlr_popup->base->events.unmap, &popup->unmap);
	popup->new_popup.notify = popup_handle_new_popup;
	wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);

	popup_unconstrain(popup);

	return popup;
}


static void get_size(PhocView *view, struct wlr_box *box) {
	struct wlr_xdg_surface *xdg_surface =
		roots_xdg_surface_from_view(view)->xdg_surface;

	struct wlr_box geo_box;
	wlr_xdg_surface_get_geometry(xdg_surface, &geo_box);
	box->width = geo_box.width;
	box->height = geo_box.height;
}

static void set_active(PhocView *view, bool active) {
	struct wlr_xdg_surface *xdg_surface =
		roots_xdg_surface_from_view(view)->xdg_surface;
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
		roots_xdg_surface_from_view(view)->xdg_surface;
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(xdg_surface, width, height, &constrained_width,
		&constrained_height);

	wlr_xdg_toplevel_set_size(xdg_surface, constrained_width,
		constrained_height);

	view_send_frame_done_if_not_visible (view);
}

static void move_resize(PhocView *view, double x, double y,
		uint32_t width, uint32_t height) {
	struct roots_xdg_surface *xdg_surface =
		roots_xdg_surface_from_view(view);
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

	uint32_t serial = wlr_xdg_toplevel_set_size(wlr_xdg_surface,
		constrained_width, constrained_height);
	if (serial > 0) {
		xdg_surface->pending_move_resize_configure_serial = serial;
	} else if (xdg_surface->pending_move_resize_configure_serial == 0) {
		view_update_position(view, x, y);
	}

	view_send_frame_done_if_not_visible (view);
}

static bool want_scaling(PhocView *view) {
	return true;
}

static bool want_auto_maximize(PhocView *view) {
	struct wlr_xdg_surface *surface =
		roots_xdg_surface_from_view(view)->xdg_surface;

	return surface->toplevel && !surface->toplevel->parent;
}

static void set_maximized(PhocView *view, bool maximized) {
	struct wlr_xdg_surface *xdg_surface =
		roots_xdg_surface_from_view(view)->xdg_surface;
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	wlr_xdg_toplevel_set_maximized(xdg_surface, maximized);
}

static void
set_tiled (PhocView *view, bool tiled)
{
  struct wlr_xdg_surface *xdg_surface = roots_xdg_surface_from_view(view)->xdg_surface;

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
		roots_xdg_surface_from_view(view)->xdg_surface;
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	wlr_xdg_toplevel_set_fullscreen(xdg_surface, fullscreen);
}

static void _close(PhocView *view) {
	struct wlr_xdg_surface *xdg_surface =
		roots_xdg_surface_from_view(view)->xdg_surface;
	struct wlr_xdg_popup *popup = NULL;
	wl_list_for_each(popup, &xdg_surface->popups, link) {
		wlr_xdg_popup_destroy(popup->base);
	}
	wlr_xdg_toplevel_send_close(xdg_surface);

	view_send_frame_done_if_not_visible (view);
}

static void for_each_surface(PhocView *view,
		wlr_surface_iterator_func_t iterator, void *user_data) {
	struct wlr_xdg_surface *xdg_surface =
		roots_xdg_surface_from_view(view)->xdg_surface;
	wlr_xdg_surface_for_each_surface(xdg_surface, iterator, user_data);
}

static void get_geometry(PhocView *view, struct wlr_box *geom) {
	wlr_xdg_surface_get_geometry (roots_xdg_surface_from_view(view)->xdg_surface, geom);
}

static void destroy(PhocView *view) {
	struct roots_xdg_surface *roots_xdg_surface =
		roots_xdg_surface_from_view(view);
	wl_list_remove(&roots_xdg_surface->surface_commit.link);
	wl_list_remove(&roots_xdg_surface->destroy.link);
	wl_list_remove(&roots_xdg_surface->new_popup.link);
	wl_list_remove(&roots_xdg_surface->map.link);
	wl_list_remove(&roots_xdg_surface->unmap.link);
	wl_list_remove(&roots_xdg_surface->request_move.link);
	wl_list_remove(&roots_xdg_surface->request_resize.link);
	wl_list_remove(&roots_xdg_surface->request_maximize.link);
	wl_list_remove(&roots_xdg_surface->request_fullscreen.link);
	wl_list_remove(&roots_xdg_surface->set_title.link);
	wl_list_remove(&roots_xdg_surface->set_app_id.link);
	wl_list_remove(&roots_xdg_surface->set_parent.link);
	roots_xdg_surface->xdg_surface->data = NULL;
	free(roots_xdg_surface);
}

static const PhocViewInterface view_impl = {
	.resize = resize,
	.move_resize = move_resize,
	.want_auto_maximize = want_auto_maximize,
	.want_scaling = want_scaling,
	.set_active = set_active,
	.set_fullscreen = set_fullscreen,
	.set_maximized = set_maximized,
	.set_tiled = set_tiled,
	.close = _close,
	.for_each_surface = for_each_surface,
	.get_geometry = get_geometry,
	.destroy = destroy,
};

static void handle_request_move(struct wl_listener *listener, void *data) {
	PhocServer *server = phoc_server_get_default ();
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, request_move);
	PhocView *view = &roots_xdg_surface->view;
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
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, request_resize);
	PhocView *view = &roots_xdg_surface->view;
	PhocInput *input = server->input;
	struct wlr_xdg_toplevel_resize_event *e = data;
	PhocSeat *seat = phoc_input_seat_from_wlr_seat(input, e->seat->seat);

	// TODO verify event serial
	assert(seat);
	if (!seat || phoc_seat_get_cursor(seat)->mode != PHOC_CURSOR_PASSTHROUGH) {
		return;
	}
	phoc_seat_begin_resize(seat, view, e->edges);
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, request_maximize);
	PhocView *view = &roots_xdg_surface->view;
	struct wlr_xdg_surface *surface = roots_xdg_surface->xdg_surface;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	if (surface->toplevel->client_pending.maximized) {
		view_maximize(view, NULL);
	} else {
		view_restore(view);
	}
}

static void handle_request_fullscreen(struct wl_listener *listener,
		void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, request_fullscreen);
	PhocView *view = &roots_xdg_surface->view;
	struct wlr_xdg_surface *surface = roots_xdg_surface->xdg_surface;
	struct wlr_xdg_toplevel_set_fullscreen_event *e = data;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	phoc_view_set_fullscreen(view, e->fullscreen, e->output);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, set_title);

	view_set_title(&roots_xdg_surface->view,
			roots_xdg_surface->xdg_surface->toplevel->title);
}

static void handle_set_app_id(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, set_app_id);

	view_set_app_id(&roots_xdg_surface->view,
			roots_xdg_surface->xdg_surface->toplevel->app_id);
}

static void handle_set_parent(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, set_parent);

	if (roots_xdg_surface->xdg_surface->toplevel->parent) {
		struct roots_xdg_surface *parent = roots_xdg_surface->xdg_surface->toplevel->parent->data;
		view_set_parent(&roots_xdg_surface->view, &parent->view);
	} else {
		view_set_parent(&roots_xdg_surface->view, NULL);
	}
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_surface =
		wl_container_of(listener, roots_surface, surface_commit);
	PhocView *view = &roots_surface->view;
	struct wlr_xdg_surface *surface = roots_surface->xdg_surface;

	if (!surface->mapped) {
		return;
	}

	phoc_view_apply_damage(view);

	struct wlr_box size;
	get_size(view, &size);
	view_update_size(view, size.width, size.height);

	uint32_t pending_serial =
		roots_surface->pending_move_resize_configure_serial;
	if (pending_serial > 0 && pending_serial >= surface->configure_serial) {
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

		if (pending_serial == surface->configure_serial) {
			roots_surface->pending_move_resize_configure_serial = 0;
		}
	}

	struct wlr_box geometry;
	get_geometry(view, &geometry);
	if (roots_surface->saved_geometry.x != geometry.x || roots_surface->saved_geometry.y != geometry.y) {
		view_update_position(view,
		                     view->box.x + (roots_surface->saved_geometry.x - geometry.x) * view->scale,
		                     view->box.y + (roots_surface->saved_geometry.y - geometry.y) * view->scale);
	}
	roots_surface->saved_geometry = geometry;
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	popup_create(&roots_xdg_surface->view, wlr_popup);
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, map);
	PhocView *view = &roots_xdg_surface->view;

	struct wlr_box box;
	get_size(view, &box);
	view->box.width = box.width;
	view->box.height = box.height;
	get_geometry(view, &roots_xdg_surface->saved_geometry);

	phoc_view_map(view, roots_xdg_surface->xdg_surface->surface);
	view_setup(view);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, unmap);
	view_unmap(&roots_xdg_surface->view);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, destroy);
	view_destroy(&roots_xdg_surface->view);
}

void handle_xdg_shell_surface(struct wl_listener *listener, void *data) {
	struct wlr_xdg_surface *surface = data;
	assert(surface->role != WLR_XDG_SURFACE_ROLE_NONE);

	if (surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		g_debug ("new xdg popup");
		return;
	}

	PhocDesktop *desktop =
		wl_container_of(listener, desktop, xdg_shell_surface);

	g_debug ("new xdg toplevel: title=%s, app_id=%s",
		surface->toplevel->title, surface->toplevel->app_id);

	wlr_xdg_surface_ping(surface);

	struct roots_xdg_surface *roots_surface =
		calloc(1, sizeof(struct roots_xdg_surface));
	if (!roots_surface) {
		return;
	}

	view_init(&roots_surface->view, &view_impl, ROOTS_XDG_SHELL_VIEW, desktop);
	roots_surface->xdg_surface = surface;
	surface->data = roots_surface;

	// catch up with state accumulated before commiting
	if (surface->toplevel->parent) {
		struct roots_xdg_surface *parent = surface->toplevel->parent->data;
		view_set_parent(&roots_surface->view, &parent->view);
	}
	if (surface->toplevel->client_pending.maximized) {
		view_maximize(&roots_surface->view, NULL);
	}
	phoc_view_set_fullscreen(&roots_surface->view, surface->toplevel->client_pending.fullscreen,
		surface->toplevel->client_pending.fullscreen_output);
	view_auto_maximize(&roots_surface->view);
	view_set_title(&roots_surface->view, surface->toplevel->title);

	// Check for app-id override coming from gtk-shell
	PhocGtkSurface *gtk_surface = phoc_gtk_shell_get_gtk_surface_from_wlr_surface (desktop->gtk_shell, surface->surface);
	if (gtk_surface && gtk_surface->app_id) {
		view_set_app_id (&roots_surface->view, gtk_surface->app_id);
	} else {
		view_set_app_id (&roots_surface->view, surface->toplevel->app_id);
	}

	roots_surface->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surface->surface->events.commit,
		&roots_surface->surface_commit);
	roots_surface->destroy.notify = handle_destroy;
	wl_signal_add(&surface->events.destroy, &roots_surface->destroy);
	roots_surface->map.notify = handle_map;
	wl_signal_add(&surface->events.map, &roots_surface->map);
	roots_surface->unmap.notify = handle_unmap;
	wl_signal_add(&surface->events.unmap, &roots_surface->unmap);
	roots_surface->request_move.notify = handle_request_move;
	wl_signal_add(&surface->toplevel->events.request_move,
		&roots_surface->request_move);
	roots_surface->request_resize.notify = handle_request_resize;
	wl_signal_add(&surface->toplevel->events.request_resize,
		&roots_surface->request_resize);
	roots_surface->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&surface->toplevel->events.request_maximize,
		&roots_surface->request_maximize);
	roots_surface->request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&surface->toplevel->events.request_fullscreen,
		&roots_surface->request_fullscreen);
	roots_surface->set_title.notify = handle_set_title;
	wl_signal_add(&surface->toplevel->events.set_title, &roots_surface->set_title);
	roots_surface->set_app_id.notify = handle_set_app_id;
	wl_signal_add(&surface->toplevel->events.set_app_id,
		&roots_surface->set_app_id);
	roots_surface->set_parent.notify = handle_set_parent;
	wl_signal_add(&surface->toplevel->events.set_parent, &roots_surface->set_parent);
	roots_surface->new_popup.notify = handle_new_popup;
	wl_signal_add(&surface->events.new_popup, &roots_surface->new_popup);
}


static void decoration_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct roots_xdg_toplevel_decoration *decoration =
		wl_container_of(listener, decoration, destroy);

	decoration->surface->xdg_toplevel_decoration = NULL;
	view_update_decorated(&decoration->surface->view, false);
	wl_list_remove(&decoration->destroy.link);
	wl_list_remove(&decoration->request_mode.link);
	wl_list_remove(&decoration->surface_commit.link);
	free(decoration);
}

static void decoration_handle_request_mode(struct wl_listener *listener,
		void *data) {
	struct roots_xdg_toplevel_decoration *decoration =
		wl_container_of(listener, decoration, request_mode);

	enum wlr_xdg_toplevel_decoration_v1_mode mode =
		decoration->wlr_decoration->client_pending_mode;
	if (mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration->wlr_decoration, mode);
}

static void decoration_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	struct roots_xdg_toplevel_decoration *decoration =
		wl_container_of(listener, decoration, surface_commit);

	bool decorated = decoration->wlr_decoration->current_mode ==
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	view_update_decorated(&decoration->surface->view, decorated);
}

void handle_xdg_toplevel_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration = data;

	g_debug ("new xdg toplevel decoration");

	struct roots_xdg_surface *xdg_surface = wlr_decoration->surface->data;
	assert(xdg_surface != NULL);
	struct wlr_xdg_surface *wlr_xdg_surface = xdg_surface->xdg_surface;

	struct roots_xdg_toplevel_decoration *decoration =
		calloc(1, sizeof(struct roots_xdg_toplevel_decoration));
	if (decoration == NULL) {
		return;
	}
	decoration->wlr_decoration = wlr_decoration;
	decoration->surface = xdg_surface;
	xdg_surface->xdg_toplevel_decoration = decoration;

	decoration->destroy.notify = decoration_handle_destroy;
	wl_signal_add(&wlr_decoration->events.destroy, &decoration->destroy);
	decoration->request_mode.notify = decoration_handle_request_mode;
	wl_signal_add(&wlr_decoration->events.request_mode,
		&decoration->request_mode);
	decoration->surface_commit.notify = decoration_handle_surface_commit;
	wl_signal_add(&wlr_xdg_surface->surface->events.commit,
		&decoration->surface_commit);

	decoration_handle_request_mode(&decoration->request_mode, wlr_decoration);
}

struct roots_xdg_surface *roots_xdg_surface_from_view(PhocView *view) {
	assert(view->impl == &view_impl);
	return (struct roots_xdg_surface *)view;
}
