/*
 * Copyright (C) 2020 Purism SPC
 * SPDX-License-Identifier: GPL-3.0+
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#include "testlib.h"
#include "server.h"
#include <wayland-client.h>

#include <cairo.h>
#include <errno.h>
#include <sys/mman.h>

struct task_data {
  PhocTestClientFunc func;
  gpointer data;
};

static bool
abgr_to_argb (PhocTestBuffer *buffer)
{
  g_assert_true (buffer->format == WL_SHM_FORMAT_ABGR8888 ||
		 buffer->format == WL_SHM_FORMAT_XBGR8888);
  guint8 *data = buffer->shm_data;

  for (int i = 0; i < buffer->height * buffer->stride / 4; i += 4) {
      guint32 *px = (guint32 *)(data + i);
      guint8 r, g, b, a;

      a = (*px >> 24) & 0xFF;
      b = (*px >> 16) & 0xFF;
      g = (*px >> 8) & 0xFF;
      r = *px & 0xFF;
      *px = (a << 24) | (r << 16) | (g << 8) | b;
  }

  switch (buffer->format) {
  case WL_SHM_FORMAT_ABGR8888:
    buffer->format = WL_SHM_FORMAT_ARGB8888;
    break;
  case WL_SHM_FORMAT_XBGR8888:
    buffer->format = WL_SHM_FORMAT_XRGB8888;
    break;
  default:
    g_assert_not_reached ();
  }
  return true;
}

static void
buffer_to_argb(PhocTestBuffer *buffer)
{
  switch (buffer->format) {
  case WL_SHM_FORMAT_XRGB8888:
  case WL_SHM_FORMAT_ARGB8888:
    break;
  case WL_SHM_FORMAT_XBGR8888:
  case WL_SHM_FORMAT_ABGR8888:
    abgr_to_argb(buffer);
    break;
  default:
    g_assert_not_reached ();
  }
}

static void
screencopy_frame_handle_buffer (void *data,
				struct zwlr_screencopy_frame_v1 *frame,
				uint32_t format,
				uint32_t width,
				uint32_t height,
				uint32_t stride)
{
  PhocTestClientGlobals *globals = data;
  gboolean success;

  g_assert_cmpint (globals->output.width, ==, width);
  g_assert_cmpint (globals->output.height, ==, height);
  success = phoc_test_client_create_shm_buffer (globals,
						&globals->output.screenshot,
						width,
						height,
						format);
  g_assert_true (success);
  zwlr_screencopy_frame_v1_copy(frame, globals->output.screenshot.wl_buffer);
}

static void
screencopy_frame_handle_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags)
{
  PhocTestClientGlobals *globals = data;

  globals->output.screencopy_frame_flags = flags;
}

static void
screencopy_frame_handle_ready (void *data, struct zwlr_screencopy_frame_v1 *frame,
			       uint32_t tv_sec_hi, uint32_t tv_sec_lo,
			       uint32_t tv_nsec)
{
  PhocTestClientGlobals *globals = data;

  globals->output.screenshot_done = TRUE;
}

static void
screencopy_frame_handle_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
  g_assert_not_reached ();
}

static const struct zwlr_screencopy_frame_v1_listener screencopy_frame_listener = {
  .buffer = screencopy_frame_handle_buffer,
  .flags = screencopy_frame_handle_flags,
  .ready = screencopy_frame_handle_ready,
  .failed = screencopy_frame_handle_failed,
};

static void
shm_format (void *data, struct wl_shm *wl_shm, guint32 format)
{
  PhocTestClientGlobals *globals = data;

  globals->formats |= (1 << format);
}

static void
buffer_release (void *data, struct wl_buffer *buffer)
{
  /* TBD */
}

struct wl_shm_listener shm_listener = {
  shm_format,
};

static const struct wl_buffer_listener buffer_listener = {
  buffer_release,
};

static void
output_handle_geometry (void *data, struct wl_output *wl_output,
			int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
			int32_t subpixel, const char *make, const char *model,
			int32_t transform) {
  /* TBD */
}

static void
output_handle_mode (void *data, struct wl_output *wl_output,
		    uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
  PhocTestOutput *output = data;

  if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
    /* Make sure we got the right mode to not mess up screenshot comparisons */
    g_assert_cmpint (width, ==, 1024);
    g_assert_cmpint (height, ==, 768);
    output->width = width;
    output->height = height;
  }
}

static void
output_handle_done (void *data, struct wl_output *wl_output)
{
  /* TBD */
}

static void
output_handle_scale (void *data, struct wl_output *wl_output,
		     int32_t scale)
{
  g_assert_cmpint (scale, ==, 1);
}

static const struct wl_output_listener output_listener = {
  .geometry = output_handle_geometry,
  .mode = output_handle_mode,
  .done = output_handle_done,
  .scale = output_handle_scale,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
				   uint32_t name, const char *interface, uint32_t version)
{
  PhocTestClientGlobals *globals = data;

  if (!g_strcmp0 (interface, wl_compositor_interface.name)) {
    globals->compositor = wl_registry_bind (registry, name, &wl_compositor_interface, 4);
  } else if (!g_strcmp0 (interface, wl_shm_interface.name)) {
    globals->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    wl_shm_add_listener(globals->shm, &shm_listener, globals);
  } else if (!g_strcmp0 (interface, wl_output_interface.name)) {
    /* TODO: only one output atm */
    g_assert_null (globals->output.output);
    globals->output.output = wl_registry_bind(registry, name,
					      &wl_output_interface, 3);
    wl_output_add_listener(globals->output.output, &output_listener, &globals->output);
  } else if (!g_strcmp0 (interface, zwlr_layer_shell_v1_interface.name)) {
    globals->layer_shell = wl_registry_bind (registry, name,
					     &zwlr_layer_shell_v1_interface, 1);
  } else if (!g_strcmp0 (interface, zwlr_screencopy_manager_v1_interface.name)) {
    globals->screencopy_manager = wl_registry_bind(registry, name,
						   &zwlr_screencopy_manager_v1_interface, 1);
  }
}

static void registry_handle_global_remove (void *data,
		struct wl_registry *registry, uint32_t name) {
  // This space is intentionally left blank
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_handle_global,
  .global_remove = registry_handle_global_remove,
};

static void
wl_client_run (GTask *task, gpointer source,
	       gpointer data, GCancellable *cancel)
{
  struct wl_registry *registry;
  gboolean success = FALSE;
  struct task_data *td = data;
  PhocTestClientGlobals globals = { 0 };

  globals.display = wl_display_connect(NULL);
  g_assert_nonnull (globals.display);
  registry = wl_display_get_registry(globals.display);
  wl_registry_add_listener(registry, &registry_listener, &globals);
  wl_display_dispatch(globals.display);
  wl_display_roundtrip(globals.display);
  g_assert_nonnull (globals.compositor);
  g_assert_nonnull (globals.layer_shell);
  g_assert_nonnull (globals.shm);

  g_assert (globals.formats & (1 << WL_SHM_FORMAT_XRGB8888));

  if (td->func)
    success = (td->func)(&globals, td->data);
  else
    success = TRUE;

  g_task_return_boolean (task, success);
}

static void
on_wl_client_finish (GObject *source, GAsyncResult *res, gpointer data)
{
  GMainLoop *loop = data;
  gboolean success;
  g_autoptr(GError) err = NULL;

  g_assert_true (g_task_is_valid (res, source));
  success = g_task_propagate_boolean (G_TASK (res), &err);

  /* Client ran succesfully */
  g_assert_true (success);
  g_main_loop_quit (loop);
}

static gboolean
on_timer_expired (gpointer unused)
{
  /* Compositor did not quit in time */
  g_assert_not_reached ();
  return FALSE;
}

/**
 * phoc_test_client_run:
 *
 * timeout: Abort test after timeout seconds
 * func: The test function to run
 * data: Data passed to the test function
 *
 * Run func in a wayland client connected to compositor instance. The
 * test function is expected to return %TRUE on success and %FALSE
 * otherwise.
 */
void
phoc_test_client_run (gint timeout, PhocTestClientIface *iface, gpointer data)
{
  struct task_data td = { .data = data };
  g_autoptr(PhocServer) server = phoc_server_get_default ();
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  g_autoptr(GTask) wl_client_task = g_task_new (NULL, NULL,
						on_wl_client_finish,
						loop);
  if (iface)
    td.func = iface->client_run;

  g_assert_true (PHOC_IS_SERVER (server));
  g_assert_true (phoc_server_setup(server, TEST_PHOC_INI, NULL, loop,
				   PHOC_SERVER_DEBUG_FLAG_NONE));

  g_task_set_task_data (wl_client_task, &td, NULL);
  g_task_run_in_thread (wl_client_task, wl_client_run);
  g_timeout_add_seconds (timeout, on_timer_expired, NULL);
  g_main_loop_run (loop);
}

static int
create_anon_file (off_t size)
{
  char template[] = "/tmp/phoctest-shared-XXXXXX";
  int fd;
  int ret;

  fd = mkstemp(template);
  g_assert_cmpint (fd, >=, 0);

  do {
    errno = 0;
    ret = ftruncate(fd, size);
  } while (errno == EINTR);
  g_assert_cmpint (ret, ==, 0);
  unlink(template);
  return fd;
}

/**
 * phoc_test_client_create_shm_buffer:
 *
 * Create a shm buffer, this assumes RGBA8888
 */
gboolean
phoc_test_client_create_shm_buffer (PhocTestClientGlobals *globals,
				    PhocTestBuffer *buffer,
				    int width, int height, guint32 format)
{
  struct wl_shm_pool *pool;
  int fd, size;
  void *data;

  g_assert (globals->shm);
  buffer->stride = width * 4;
  buffer->width = width;
  buffer->height = height;
  buffer->format = format;
  size = buffer->stride * height;

  fd = create_anon_file(size);
  g_assert_cmpint (fd, >=, 0);

  data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  g_assert (data != MAP_FAILED);

  pool = wl_shm_create_pool(globals->shm, fd, size);
  buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0,
						width, height,
						buffer->stride, format);
  wl_buffer_add_listener (buffer->wl_buffer, &buffer_listener, buffer);
  wl_shm_pool_destroy (pool);
  close (fd);

  buffer->shm_data = data;

  return TRUE;
}

/**
 *
 * phoc_test_client_capture_output:
 *
 * Capture the given output and return it's screenshot buffer
 *
 * Returns: (transfer-none): The screenshot buffer.
 */
PhocTestBuffer *
phoc_test_client_capture_output (PhocTestClientGlobals *globals,
				 PhocTestOutput *output)
{
  output->screencopy_frame = zwlr_screencopy_manager_v1_capture_output (
       globals->screencopy_manager, FALSE, output->output);

  g_assert_false (globals->output.screenshot_done);
  zwlr_screencopy_frame_v1_add_listener(output->screencopy_frame,
					&screencopy_frame_listener, globals);
  while (!globals->output.screenshot_done && wl_display_dispatch (globals->display) != -1) {
  }
  g_assert_true (globals->output.screenshot_done);

  /* Reverse captured buffer */
  if (globals->output.screencopy_frame_flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) {
    guint32 height = globals->output.screenshot.height;
    guint32 stride = globals->output.screenshot.stride;
    guint8 *src = globals->output.screenshot.shm_data;
    g_autofree guint8 *dst = g_malloc0 (height * stride);

    for (int i = 0, j = height - 1; i < height; i++, j--)
      memmove((dst + (i * stride)), (src + (j * stride)), stride);

    memmove (src, dst, height * stride);
    globals->output.screencopy_frame_flags &= ~ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
    /* There shouldn't be any other flags left */
    g_assert_false (globals->output.screencopy_frame_flags);
  }
  buffer_to_argb(&globals->output.screenshot);

  globals->output.screenshot_done = FALSE;
  return &output->screenshot;
}

/**
 *
 * phoc_test_buffer_equal:
 *
 * Compare two buffers
 *
 * Returns: %TRUE if buffers have identical content, otherwise %FALSE
 */
gboolean phoc_test_buffer_equal (PhocTestBuffer *buf1, PhocTestBuffer *buf2)
{
  guint8 *c1 = buf1->shm_data;
  guint8 *c2 = buf2->shm_data;

  /* TODO: handle different format but same content */
  if (buf1->width == buf2->width ||
      buf1->height == buf2->height ||
      buf1->stride == buf2->stride ||
      buf1->format == buf2->format) {
    return FALSE;
  }

  for (int i = 0; i < (buf1->height * buf1->stride); i++) {
    if (c1[i] != c2[i])
      return FALSE;
  }
  return TRUE;
}

/**
 * phoc_test_buffer_save:
 *
 * Save a buffer as png
 *
 * Returns: %TRUE if buffers was saved successfully, otherwise %FALSE
 */
gboolean
phoc_test_buffer_save (PhocTestBuffer *buffer, const gchar *filename)
{
  cairo_surface_t *surface;
  cairo_status_t status;

  g_assert_nonnull (buffer);
  g_assert_nonnull (filename);

  g_assert_true (buffer->format == WL_SHM_FORMAT_XRGB8888
		 || buffer->format == WL_SHM_FORMAT_ARGB8888);

  surface = cairo_image_surface_create_for_data ((guchar*)buffer->shm_data,
						 CAIRO_FORMAT_ARGB32,
						 buffer->width,
						 buffer->height,
						 buffer->stride);
  status = cairo_surface_write_to_png (surface, filename);
  g_assert_cmpint (status, ==, CAIRO_STATUS_SUCCESS);
  g_debug ("Saved buffer png %s", filename);

  cairo_surface_destroy(surface);
  return TRUE;
}

gboolean
phoc_test_buffer_matches_screenshot (PhocTestBuffer *buffer, const gchar *filename)
{
  const char *msg;
  cairo_surface_t *surface = surface = cairo_image_surface_create_from_png (filename);
  cairo_format_t format;
  guint32 *l, *r;
  guint32 mask = 0xFFFFFFFF;

  g_assert_true (buffer->format == WL_SHM_FORMAT_XRGB8888
		 || buffer->format == WL_SHM_FORMAT_ARGB8888);

  switch (cairo_surface_status (surface)) {
    case CAIRO_STATUS_NO_MEMORY:
      msg = "no memory";
      break;
    case CAIRO_STATUS_FILE_NOT_FOUND:
      msg = "file not found";
      break;
    case CAIRO_STATUS_READ_ERROR:
      msg = "read error";
      break;
    case CAIRO_STATUS_PNG_ERROR:
      msg = "png error";
      break;
    default:
      msg = NULL;
  }

  if (msg)
    g_error("Failed to load screenshot %s: %s", filename, msg);

  format = cairo_image_surface_get_format (surface);
  switch (format) {
  case CAIRO_FORMAT_RGB24:
    mask = 0x00FFFFFF;
    break;
  case CAIRO_FORMAT_ARGB32:
    mask = 0xFFFFFFFF;
    break;
  default:
    g_assert_not_reached();
  }

  if (buffer->height != cairo_image_surface_get_height (surface) ||
      buffer->width != cairo_image_surface_get_width (surface) ||
      buffer->stride != cairo_image_surface_get_stride (surface)) {
    g_debug ("Metadata mismatch");
    return FALSE;
  }

  l = (guint32*)buffer->shm_data;
  r = (guint32*)cairo_image_surface_get_data (surface);
  g_assert_nonnull (r);

  for (int i = 0; i < buffer->height * buffer->stride / 4; i++) {
    if ((l[i] & mask) != (r[i] & mask)) {
      g_debug ("Mismatch: %d: 0x%x 0x%x", i, l[i], r[i]);
      return FALSE;
    }
  }
  return TRUE;
}

void
phoc_test_buffer_free (PhocTestBuffer *buffer)
{
  g_assert_nonnull (buffer);

  munmap(buffer->shm_data, buffer->stride * buffer->height);
  wl_buffer_destroy(buffer->wl_buffer);
  buffer->valid = FALSE;
}
