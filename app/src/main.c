/*
 * pw-loudness-eq - control and monitor app for the loudness-eq module.
 *
 * GTK3 (no libadwaita, no client-side decorations), native on Wayland and X11. A single instance:
 * launching it again raises the open window. PipeWire runs on its own
 * pw_loop, driven from the GTK main loop.
 */
#include <gtk/gtk.h>
#include <glib-unix.h>

#include "model.h"

#define APP_ID "io.github.pw_loudness_eq"

static struct leq_model model;
static bool started;

static gboolean on_pipewire(gint fd, GIOCondition cond, gpointer data)
{
	pwc_iterate();
	ui_refresh();
	return G_SOURCE_CONTINUE;
}

/* reconnect retries are timed, not fd-driven */
static gboolean on_tick(gpointer data)
{
	pwc_iterate();
	ui_refresh();
	return G_SOURCE_CONTINUE;
}

static void activate(GtkApplication *app, gpointer data)
{
	if (started) {
		ui_present();
		return;
	}
	started = true;
	if (pwc_init(&model) < 0)
		g_warning("can't start the PipeWire client");
	else
		g_unix_fd_add(pwc_get_fd(), G_IO_IN, on_pipewire, NULL);
	g_timeout_add_seconds(1, on_tick, NULL);
	ui_create(app, &model);
}

int main(int argc, char *argv[])
{
	GtkApplication *app;
	int status;

	g_set_prgname("pw-loudness-eq");
	g_set_application_name("Loudness Equalization");
	app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	if (started)
		pwc_deinit();
	g_object_unref(app);
	return status;
}
