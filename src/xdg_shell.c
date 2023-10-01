#define G_LOG_DOMAIN "phoc-xdg-shell"

#include "phoc-config.h"
#include "xdg-surface.h"
#include "xdg-surface-private.h"

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include "cursor.h"
#include "desktop.h"
#include "input.h"
#include "server.h"
#include "view.h"

typedef struct phoc_xdg_toplevel_decoration {
  struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration;
  PhocXdgSurface *surface;
  struct wl_listener destroy;
  struct wl_listener request_mode;
  struct wl_listener surface_commit;
} PhocXdgToplevelDecoration;

typedef struct phoc_xdg_popup {
  PhocViewChild child;
  struct wlr_xdg_popup *wlr_popup;

  struct wl_listener destroy;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener new_popup;
} PhocXdgPopup;

static const struct phoc_view_child_interface popup_impl;

static void
popup_destroy (PhocViewChild *child)
{
  g_assert (child->impl == &popup_impl);
  PhocXdgPopup *popup = (PhocXdgPopup *)child;

  wl_list_remove (&popup->new_popup.link);
  wl_list_remove (&popup->unmap.link);
  wl_list_remove (&popup->map.link);
  wl_list_remove (&popup->destroy.link);

  free (popup);
}


static void
popup_get_pos (PhocViewChild *child, int *sx, int *sy)
{
  PhocXdgPopup *popup = (PhocXdgPopup *)child;
  struct wlr_xdg_popup *wlr_popup = popup->wlr_popup;

  wlr_xdg_popup_get_toplevel_coords (wlr_popup,
                                     wlr_popup->current.geometry.x - wlr_popup->base->current.geometry.x,
                                     wlr_popup->current.geometry.y - wlr_popup->base->current.geometry.y,
                                     sx, sy);
}


static const struct phoc_view_child_interface popup_impl = {
  .get_pos = popup_get_pos,
  .destroy = popup_destroy,
};

static void
popup_handle_destroy (struct wl_listener *listener, void *data)
{
  PhocXdgPopup *popup = wl_container_of (listener, popup, destroy);

  phoc_view_child_destroy (&popup->child);
}

static void
popup_handle_map (struct wl_listener *listener, void *data)
{
  PhocServer *server = phoc_server_get_default ();
  PhocXdgPopup *popup = wl_container_of (listener, popup, map);

  phoc_view_child_damage_whole (&popup->child);
  phoc_input_update_cursor_focus (server->input);
  popup->child.mapped = true;
}

static void
popup_handle_unmap (struct wl_listener *listener, void *data)
{
  PhocServer *server = phoc_server_get_default ();
  PhocXdgPopup *popup = wl_container_of (listener, popup, unmap);

  phoc_view_child_damage_whole (&popup->child);
  phoc_input_update_cursor_focus (server->input);
  popup->child.mapped = false;
}


static void
popup_handle_new_popup (struct wl_listener *listener, void *data)
{
  PhocXdgPopup *popup = wl_container_of (listener, popup, new_popup);
  struct wlr_xdg_popup *wlr_popup = data;

  phoc_xdg_popup_create (popup->child.view, wlr_popup);
}

static void
popup_unconstrain (PhocXdgPopup* popup)
{
  // get the output of the popup's positioner anchor point and convert it to
  // the toplevel parent's coordinate system and then pass it to
  // wlr_xdg_popup_unconstrain_from_box
  PhocView *view = PHOC_VIEW (popup->child.view);

  PhocOutput *output = phoc_desktop_layout_get_output (view->desktop, view->box.x, view->box.y);
  if (output == NULL)
    return;

  struct wlr_box output_box;
  wlr_output_layout_get_box (view->desktop->layout, output->wlr_output, &output_box);
  struct wlr_box usable_area = output->usable_area;
  usable_area.x += output_box.x;
  usable_area.y += output_box.y;

  // the output box expressed in the coordinate system of the toplevel parent
  // of the popup
  struct wlr_box output_toplevel_sx_box = {
    .x = usable_area.x - view->box.x,
    .y = usable_area.y - view->box.y,
    .width = usable_area.width,
    .height = usable_area.height,
  };

  wlr_xdg_popup_unconstrain_from_box (popup->wlr_popup, &output_toplevel_sx_box);
}

PhocXdgPopup *
phoc_xdg_popup_create (PhocView *view, struct wlr_xdg_popup *wlr_popup)
{
  PhocXdgPopup *popup = calloc (1, sizeof(PhocXdgPopup));

  if (popup == NULL) {
    return NULL;
  }
  popup->wlr_popup = wlr_popup;
  phoc_view_child_init (&popup->child, &popup_impl,
                        view, wlr_popup->base->surface);
  popup->destroy.notify = popup_handle_destroy;
  wl_signal_add (&wlr_popup->base->events.destroy, &popup->destroy);
  popup->map.notify = popup_handle_map;
  wl_signal_add (&wlr_popup->base->events.map, &popup->map);
  popup->unmap.notify = popup_handle_unmap;
  wl_signal_add (&wlr_popup->base->events.unmap, &popup->unmap);
  popup->new_popup.notify = popup_handle_new_popup;
  wl_signal_add (&wlr_popup->base->events.new_popup, &popup->new_popup);

  popup_unconstrain (popup);

  return popup;
}


void
handle_xdg_shell_surface (struct wl_listener *listener, void *data)
{
  struct wlr_xdg_surface *surface = data;

  g_assert (surface->role != WLR_XDG_SURFACE_ROLE_NONE);
  if (surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
    g_debug ("new xdg popup");
    return;
  }

  PhocDesktop *desktop = wl_container_of(listener, desktop, xdg_shell_surface);
  g_debug ("new xdg toplevel: title=%s, app_id=%s",
           surface->toplevel->title, surface->toplevel->app_id);

  wlr_xdg_surface_ping (surface);
  PhocXdgSurface *phoc_surface = phoc_xdg_surface_new (surface);

  // Check for app-id override coming from gtk-shell
  PhocGtkShell *gtk_shell = phoc_desktop_get_gtk_shell (desktop);
  PhocGtkSurface *gtk_surface = phoc_gtk_shell_get_gtk_surface_from_wlr_surface (gtk_shell,
                                                                                 surface->surface);
  if (gtk_surface && phoc_gtk_surface_get_app_id (gtk_surface))
    phoc_view_set_app_id (PHOC_VIEW (phoc_surface), phoc_gtk_surface_get_app_id (gtk_surface));
  else
    phoc_view_set_app_id (PHOC_VIEW (phoc_surface), surface->toplevel->app_id);
}


static void
decoration_handle_destroy (struct wl_listener *listener, void *data)
{
  struct phoc_xdg_toplevel_decoration *decoration = wl_container_of (listener, decoration, destroy);

  g_debug ("Destroy xdg toplevel decoration %p", decoration);

  if (decoration->surface) {
    phoc_xdg_surface_set_decoration (decoration->surface, NULL);
    view_update_decorated (PHOC_VIEW (decoration->surface), false);
    g_signal_handlers_disconnect_by_data (decoration->surface, decoration);
  }
  wl_list_remove (&decoration->destroy.link);
  wl_list_remove (&decoration->request_mode.link);
  wl_list_remove (&decoration->surface_commit.link);

  free (decoration);
}

static void
decoration_handle_request_mode (struct wl_listener *listener, void *data)
{
  struct phoc_xdg_toplevel_decoration *decoration =
    wl_container_of (listener, decoration, request_mode);

  enum wlr_xdg_toplevel_decoration_v1_mode mode = decoration->wlr_decoration->requested_mode;

  if (mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
    mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
  }
  wlr_xdg_toplevel_decoration_v1_set_mode (decoration->wlr_decoration, mode);
}

static void
decoration_handle_surface_commit (struct wl_listener *listener, void *data)
{
  struct phoc_xdg_toplevel_decoration *decoration =
    wl_container_of (listener, decoration, surface_commit);

  bool decorated = decoration->wlr_decoration->current.mode ==
    WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
  view_update_decorated (PHOC_VIEW (decoration->surface), decorated);
}


static void
on_xdg_surface_destroy (PhocXdgSurface *surface, PhocXdgToplevelDecoration *decoration)
{
  g_assert (PHOC_IS_XDG_SURFACE (surface));

  decoration->surface = NULL;
}


void
handle_xdg_toplevel_decoration (struct wl_listener *listener, void *data)
{
  struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration = data;
  PhocXdgSurface *xdg_surface = wlr_decoration->surface->data;
  g_assert (xdg_surface != NULL);
  struct wlr_xdg_surface *wlr_xdg_surface = phoc_xdg_surface_get_wlr_xdg_surface (xdg_surface);
  PhocXdgToplevelDecoration *decoration = g_new0 (PhocXdgToplevelDecoration, 1);

  g_debug ("New xdg toplevel decoration %p", decoration);

  decoration->wlr_decoration = wlr_decoration;
  decoration->surface = xdg_surface;
  phoc_xdg_surface_set_decoration (xdg_surface, decoration);

  decoration->destroy.notify = decoration_handle_destroy;
  wl_signal_add (&wlr_decoration->events.destroy, &decoration->destroy);

  decoration->request_mode.notify = decoration_handle_request_mode;
  wl_signal_add (&wlr_decoration->events.request_mode, &decoration->request_mode);

  decoration->surface_commit.notify = decoration_handle_surface_commit;
  wl_signal_add (&wlr_xdg_surface->surface->events.commit, &decoration->surface_commit);

  g_signal_connect (xdg_surface, "surface-destroy", G_CALLBACK (on_xdg_surface_destroy), decoration);

  decoration_handle_request_mode (&decoration->request_mode, wlr_decoration);
}
