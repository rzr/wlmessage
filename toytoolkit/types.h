/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2012-2013 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include <cairo.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#ifdef HAVE_CAIRO_EGL
#include <wayland-egl.h>

#ifdef USE_CAIRO_GLESV2
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#include <GL/gl.h>
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cairo-gl.h>
#else /* HAVE_CAIRO_EGL */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#endif /* no HAVE_CAIRO_EGL */

#include <xkbcommon/xkbcommon.h>
#include <wayland-cursor.h>

#include <linux/input.h>
#include <wayland-client.h>
#include "./shared/cairo-util.h"
#include "xdg-shell-client-protocol.h"
#include "text-cursor-position-client-protocol.h"
#include "workspaces-client-protocol.h"
#include "./shared/os-compatibility.h"

#include "window.h"

struct shm_pool;

struct global {
	uint32_t name;
	char *interface;
	uint32_t version;
	struct wl_list link;
};

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct wl_data_device_manager *data_device_manager;
	struct text_cursor_position *text_cursor_position;
	struct workspace_manager *workspace_manager;
	struct xdg_shell *xdg_shell;
	EGLDisplay dpy;
	EGLConfig argb_config;
	EGLContext argb_ctx;
	cairo_device_t *argb_device;
	uint32_t serial;

	int display_fd;
	uint32_t display_fd_events;
	struct task display_task;

	int epoll_fd;
	struct wl_list deferred_list;

	int timeout;
	struct timeval time;

	int running;

	struct wl_list global_list;
	struct wl_list window_list;
	struct wl_list input_list;
	struct wl_list output_list;

	struct theme *theme;

	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor **cursors;

	display_output_handler_t output_configure_handler;
	display_global_handler_t global_handler;
	display_global_handler_t global_handler_remove;

	void *user_data;

	struct xkb_context *xkb_context;

	uint32_t workspace;
	uint32_t workspace_count;

	/* A hack to get text extents for tooltips */
	cairo_surface_t *dummy_surface;
	void *dummy_surface_data;

	int has_rgb565;
	int seat_version;
};

struct window_output {
	struct output *output;
	struct wl_list link;
};

struct toysurface {
	/*
	 * Prepare the surface for drawing. Makes sure there is a surface
	 * of the right size available for rendering, and returns it.
	 * dx,dy are the x,y of wl_surface.attach.
	 * width,height are the new buffer size.
	 * If flags has SURFACE_HINT_RESIZE set, the user is
	 * doing continuous resizing.
	 * Returns the Cairo surface to draw to.
	 */
	cairo_surface_t *(*prepare)(struct toysurface *base, int dx, int dy,
				    int32_t width, int32_t height, uint32_t flags,
				    enum wl_output_transform buffer_transform, int32_t buffer_scale);

	/*
	 * Post the surface to the server, returning the server allocation
	 * rectangle. The Cairo surface from prepare() must be destroyed
	 * after calling this.
	 */
	void (*swap)(struct toysurface *base,
		     enum wl_output_transform buffer_transform, int32_t buffer_scale,
		     struct rectangle *server_allocation);

	/*
	 * Make the toysurface current with the given EGL context.
	 * Returns 0 on success, and negative of failure.
	 */
	int (*acquire)(struct toysurface *base, EGLContext ctx);

	/*
	 * Release the toysurface from the EGL context, returning control
	 * to Cairo.
	 */
	void (*release)(struct toysurface *base);

	/*
	 * Destroy the toysurface, including the Cairo surface, any
	 * backing storage, and the Wayland protocol objects.
	 */
	void (*destroy)(struct toysurface *base);
};

struct surface {
	struct window *window;

	struct wl_surface *surface;
	struct wl_subsurface *subsurface;
	int synchronized;
	int synchronized_default;
	struct toysurface *toysurface;
	struct widget *widget;
	int redraw_needed;
	struct wl_callback *frame_cb;
	uint32_t last_time;

	struct rectangle allocation;
	struct rectangle server_allocation;

	struct wl_region *input_region;
	struct wl_region *opaque_region;

	enum window_buffer_type buffer_type;
	enum wl_output_transform buffer_transform;
	int32_t buffer_scale;

	cairo_surface_t *cairo_surface;

	struct wl_list link;
};

struct window {
	struct display *display;
	struct wl_list window_output_list;
	char *title;
	struct rectangle saved_allocation;
	struct rectangle min_allocation;
	struct rectangle pending_allocation;
	int x, y;
	int redraw_needed;
	int redraw_task_scheduled;
	struct task redraw_task;
	int resize_needed;
	int custom;
	int focused;

	int resizing;

	int fullscreen;
	int maximized;

	enum preferred_format preferred_format;

	window_key_handler_t key_handler;
	window_keyboard_focus_handler_t keyboard_focus_handler;
	window_data_handler_t data_handler;
	window_drop_handler_t drop_handler;
	window_close_handler_t close_handler;
	window_fullscreen_handler_t fullscreen_handler;
	window_output_handler_t output_handler;

	struct surface *main_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_popup *xdg_popup;

	struct window *transient_for;

	struct window_frame *frame;

	/* struct surface::link, contains also main_surface */
	struct wl_list subsurface_list;

	void *user_data;
	struct wl_list link;
};

struct widget {
	struct window *window;
	struct surface *surface;
	struct tooltip *tooltip;
	struct wl_list child_list;
	struct wl_list link;
	struct rectangle allocation;
	widget_resize_handler_t resize_handler;
	widget_redraw_handler_t redraw_handler;
	widget_enter_handler_t enter_handler;
	widget_leave_handler_t leave_handler;
	widget_motion_handler_t motion_handler;
	widget_button_handler_t button_handler;
	widget_touch_down_handler_t touch_down_handler;
	widget_touch_up_handler_t touch_up_handler;
	widget_touch_motion_handler_t touch_motion_handler;
	widget_touch_frame_handler_t touch_frame_handler;
	widget_touch_cancel_handler_t touch_cancel_handler;
	widget_axis_handler_t axis_handler;
	void *user_data;
	int opaque;
	int tooltip_count;
	int default_cursor;
	/* If this is set to false then no cairo surface will be
	 * created before redrawing the surface. This is useful if the
	 * redraw handler is going to do completely custom rendering
	 * such as using EGL directly */
	int use_cairo;
};

struct touch_point {
	int32_t id;
	float x, y;
	struct widget *widget;
	struct wl_list link;
};

struct input {
	struct display *display;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_touch *touch;
	struct wl_list touch_point_list;
	struct window *pointer_focus;
	struct window *keyboard_focus;
	struct window *touch_focus;
	int current_cursor;
	uint32_t cursor_anim_start;
	struct wl_callback *cursor_frame_cb;
	struct wl_surface *pointer_surface;
	uint32_t modifiers;
	uint32_t pointer_enter_serial;
	uint32_t cursor_serial;
	float sx, sy;
	struct wl_list link;

	struct widget *focus_widget;
	struct widget *grab;
	uint32_t grab_button;

	struct wl_data_device *data_device;
	struct data_offer *drag_offer;
	struct data_offer *selection_offer;
	uint32_t touch_grab;
	int32_t touch_grab_id;
	float drag_x, drag_y;
	struct window *drag_focus;
	uint32_t drag_enter_serial;

	struct {
		struct xkb_keymap *keymap;
		struct xkb_state *state;
		xkb_mod_mask_t control_mask;
		xkb_mod_mask_t alt_mask;
		xkb_mod_mask_t shift_mask;
	} xkb;

	struct task repeat_task;
	int repeat_timer_fd;
	uint32_t repeat_sym;
	uint32_t repeat_key;
	uint32_t repeat_time;
};

struct output {
	struct display *display;
	struct wl_output *output;
	uint32_t server_output_id;
	struct rectangle allocation;
	struct wl_list link;
	int transform;
	int scale;
	char *make;
	char *model;

	display_output_handler_t destroy_handler;
	void *user_data;
};

struct window_frame {
	struct widget *widget;
	struct widget *child;
	struct frame *frame;
};

struct menu {
	struct window *window;
	struct window *parent;
	struct widget *widget;
	struct input *input;
	struct frame *frame;
	const char **entries;
	uint32_t time;
	int current;
	int count;
	int release_count;
	menu_func_t func;
};

struct tooltip {
	struct widget *parent;
	struct widget *widget;
	char *entry;
	struct task tooltip_task;
	int tooltip_fd;
	float x, y;
};

struct shm_pool {
	struct wl_shm_pool *pool;
	size_t size;
	size_t used;
	void *data;
};

