/*
 * ui.c - the window: on/off, release time, one checkbox per app.
 *
 * Laid out like a preferences pane: a group with the on/off switch and the
 * release time, then the application list; labels on the left, controls on
 * the right. Plain GTK3 widgets (no libadwaita, no client-side title bar),
 * styled by our own stylesheet below so the window looks the same under any
 * theme. Built once; ui_refresh() updates it in place from the model and
 * only rebuilds the app list when the set of apps changes.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "model.h"

#define MAX_ROWS  MAX_STREAMS
#define ICON_SIZE 18

#define EXPLAIN "Evens out volume, so quiet and loud parts play at a similar level."

/* Every property our widgets use is set here, so the user's GTK theme
 * can't leak through. Dark palette after macOS' system colours. The
 * checkmark is the one bundled inside libgtk itself. */
static const char css[] =
"window.leq { background-color: #1e1e20; background-image: none; color: #e8e8ea; }"
".leq label { color: #e8e8ea; }"
".leq label.dim { color: #8e8e93; }"
".leq label.small { font-size: 0.9em; }"
".leq label.heading { font-weight: 600; color: #8e8e93; font-size: 0.9em; }"
".leq label.warn { color: #ff9f0a; }"
".leq .faded { opacity: 0.45; }"
".leq .faded { transition: opacity 150ms ease; }"

/* banner for "PipeWire not running" / "module not loaded" */
".leq .banner { background-color: #3a2d12; border: 1px solid #6b4f12; border-radius: 6px; padding: 8px 10px; }"
".leq .banner label { color: #ffd60a; }"

/* checkbox */
".leq checkbutton { padding: 0; outline: none; }"
".leq checkbutton check {"
"  min-width: 14px; min-height: 14px; margin: 0;"
"  background-color: #3a3a3c; background-image: none;"
"  border: 1px solid #545458; border-radius: 4px; box-shadow: none;"
"  -gtk-icon-source: none; -gtk-icon-shadow: none; color: #ffffff; }"
".leq checkbutton:hover check { border-color: #6e6e73; }"
".leq checkbutton check:checked {"
"  background-color: #0a84ff; border-color: #0a84ff;"
"  -gtk-icon-source: -gtk-recolor(url('resource:///org/gtk/libgtk/theme/Adwaita/assets/check-symbolic.svg')); }"
".leq checkbutton check:disabled { opacity: 0.4; }"

/* release time: combo box and its popup (a separate window: no .leq) */
".leq combobox button {"
"  background-color: #3a3a3c; background-image: none; color: #e8e8ea;"
"  border: 1px solid #545458; border-radius: 5px; box-shadow: none;"
"  padding: 2px 8px; min-height: 22px; outline: none; }"
".leq combobox button:hover { background-color: #444446; }"
".leq combobox button label { color: #e8e8ea; }"
".leq combobox arrow { color: #8e8e93; min-width: 12px; min-height: 12px; }"
"menu, .menu, .context-menu {"
"  background-color: #2c2c2e; background-image: none; color: #e8e8ea;"
"  border: 1px solid #48484a; padding: 4px 0; }"
"menu menuitem { padding: 4px 12px; color: #e8e8ea; background: none; }"
"menu menuitem label { color: #e8e8ea; }"
"menu menuitem:hover { background-color: #0a84ff; }"
"menu menuitem:hover label { color: #ffffff; }"
"menu arrow { min-width: 0; min-height: 0; }"

/* groups and the application list */
".leq .group { background-color: #252527; border: 1px solid #38383a; border-radius: 6px; }"
".leq .group-row { padding: 9px 12px; }"
".leq .group-row.separated { border-top: 1px solid #333335; }"
".leq frame > border { border: 1px solid #38383a; border-radius: 6px; }"
".leq scrolledwindow, .leq viewport { background-color: #252527; border: none; }"
".leq list { background-color: #252527; }"
".leq list row { padding: 0; border: none; border-top: 1px solid #333335; background: none; outline: none; }"
".leq list row:first-child { border-top: none; }"
".leq .placeholder { color: #8e8e93; padding: 16px; }"
".leq scrollbar { background: none; border: none; }"
".leq scrollbar slider {"
"  background-color: rgba(255,255,255,0.25); border: none; border-radius: 999px;"
"  min-width: 6px; min-height: 24px; margin: 2px; }"
".leq scrollbar slider:hover { background-color: rgba(255,255,255,0.4); }";

static const char *monogram_colors[] = {
	"#ff453a", "#ff9f0a", "#ffd60a", "#30d158", "#64d2ff", "#0a84ff", "#5e5ce6", "#bf5af2",
};

static const char *release_names[] = {
	[2] = "Fastest", [3] = "Fast", [4] = "Normal", [5] = "Slow", [6] = "Slower", [7] = "Slowest",
};

struct row {
	char app[128];
	GtkWidget *widget, *note, *check;
	gulong check_handler;
};

static struct {
	struct leq_model *m;
	unsigned seen_version;
	bool first;

	GtkWidget *window, *banner, *banner_label, *global;
	GtkWidget *release_row, *combo;
	GtkWidget *list;
	gulong global_handler, release_handler;

	struct row rows[MAX_ROWS];
	int n_rows;
} ui;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void add_class(GtkWidget *w, const char *cls)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

static void set_class(GtkWidget *w, const char *cls, bool on)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	if (on)
		gtk_style_context_add_class(sc, cls);
	else
		gtk_style_context_remove_class(sc, cls);
}

/* cls: space-separated style classes, or NULL */
static GtkWidget *label(const char *cls, const char *text)
{
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	if (cls) {
		char **v = g_strsplit(cls, " ", -1);
		for (char **c = v; *c; c++)
			add_class(l, *c);
		g_strfreev(v);
	}
	return l;
}

static GtkWidget *hbox(int spacing) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing); }
static GtkWidget *vbox(int spacing) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing); }

/* Windows' release time as a time constant: 64 / (44100 * f(r)) seconds,
 * independent of the sample rate (see the README). */
static double release_seconds(int r)
{
	int a = r < 3 ? r : 3, b = r < 7 ? r : 7;
	return 64.0 / (44100.0 * pow(0.1, a) * pow(0.316, b - a));
}

static void format_seconds(double t, char *buf, size_t size)
{
	if (t < 1.0)
		snprintf(buf, size, "%d ms", (int)lround(t * 1000));
	else if (t < 100)
		snprintf(buf, size, "%.*f s", t < 10 ? 1 : 0, t);
	else
		snprintf(buf, size, "%.1f min", t / 60);
}

static void set_check(GtkWidget *cb, gulong handler, bool on)
{
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb)) == on)
		return;
	g_signal_handler_block(cb, handler);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), on);
	g_signal_handler_unblock(cb, handler);
}

static GtkWidget *plain_check(void)
{
	GtkWidget *cb = gtk_check_button_new();
	gtk_widget_set_valign(cb, GTK_ALIGN_CENTER);
	return cb;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

static void on_global(GtkToggleButton *cb, gpointer data)
{
	pwc_set_enabled(gtk_toggle_button_get_active(cb));
}

static void on_release(GtkComboBox *combo, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(combo);
	if (id)
		pwc_set_release(atoi(id));
}

static void on_app(GtkToggleButton *cb, gpointer data)
{
	struct row *r = data;
	pwc_set_app_enabled(r->app, gtk_toggle_button_get_active(cb));
}

/* ------------------------------------------------------------------------ */
/* App icons                                                                 */
/* ------------------------------------------------------------------------ */

/* coloured rounded square with the app's initial, for apps without an icon */
static gboolean draw_monogram(GtkWidget *w, cairo_t *cr, gpointer data)
{
	const char *app = data;
	int size = gtk_widget_get_allocated_width(w);
	unsigned h = 5381;
	GdkRGBA c;
	char initial[8] = "?";

	for (const char *p = app; *p; p++)
		h = h * 33 + (unsigned char)*p;
	gdk_rgba_parse(&c, monogram_colors[h % G_N_ELEMENTS(monogram_colors)]);
	if (*app) {
		gunichar ch = g_unichar_toupper(g_utf8_get_char(app));
		initial[g_unichar_to_utf8(ch, initial)] = '\0';
	}

	double r = size * 0.22;
	cairo_new_sub_path(cr);
	cairo_arc(cr, size - r, r, r, -M_PI / 2, 0);
	cairo_arc(cr, size - r, size - r, r, 0, M_PI / 2);
	cairo_arc(cr, r, size - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, c.red, c.green, c.blue);
	cairo_fill(cr);

	PangoLayout *layout = gtk_widget_create_pango_layout(w, initial);
	PangoFontDescription *fd = pango_font_description_copy(
		pango_context_get_font_description(gtk_widget_get_pango_context(w)));
	pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
	pango_font_description_set_absolute_size(fd, size * 0.6 * PANGO_SCALE);
	pango_layout_set_font_description(layout, fd);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_move_to(cr, (size - tw) / 2.0, (size - th) / 2.0);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(fd);
	g_object_unref(layout);
	return TRUE;
}

/* "Google Chrome" -> "google-chrome" */
static void icon_guess(const char *app, char *out, size_t size)
{
	size_t n = 0;
	for (; *app && n + 1 < size; app++)
		out[n++] = *app == ' ' ? '-' : (char)tolower((unsigned char)*app);
	out[n] = '\0';
}

static GtkWidget *app_icon(const struct leq_stream *s)
{
	GtkIconTheme *theme = gtk_icon_theme_get_default();
	char guess[128];
	const char *candidates[3] = { s->icon, guess, s->binary };

	icon_guess(s->app, guess, sizeof(guess));
	for (size_t i = 0; i < G_N_ELEMENTS(candidates); i++)
		if (candidates[i][0] && gtk_icon_theme_has_icon(theme, candidates[i])) {
			GtkWidget *img = gtk_image_new_from_icon_name(candidates[i], GTK_ICON_SIZE_MENU);
			gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_SIZE);
			return img;
		}

	GtkWidget *da = gtk_drawing_area_new();
	gtk_widget_set_size_request(da, ICON_SIZE, ICON_SIZE);
	gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
	g_signal_connect(da, "draw", G_CALLBACK(draw_monogram), (gpointer)g_intern_string(s->app));
	return da;
}

/* ------------------------------------------------------------------------ */
/* App list                                                                  */
/* ------------------------------------------------------------------------ */

struct app_info {
	const struct leq_stream *first;   /* for name and icon */
	bool unsupported;                 /* every stream has too many channels */
};

static int collect_apps(struct app_info *apps)
{
	struct leq_model *m = ui.m;
	int n = 0;

	for (int i = 0; i < m->n_streams; i++) {
		const struct leq_stream *s = &m->streams[i];
		bool unsupported = strcmp(s->state, "unsupported") == 0;
		int j;
		if (!s->app[0] || !s->state[0])
			continue;       /* not ours: excluded by rules, internal, ... */
		for (j = 0; j < n; j++)
			if (strcmp(apps[j].first->app, s->app) == 0)
				break;
		if (j == n)
			apps[n++] = (struct app_info){ s, unsupported };
		else
			apps[j].unsupported &= unsupported;
	}
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && g_utf8_collate(apps[j - 1].first->app, apps[j].first->app) > 0; j--) {
			struct app_info t = apps[j]; apps[j] = apps[j - 1]; apps[j - 1] = t;
		}
	return n;
}

/* icon  name ................ note  [x] */
static void build_row(struct row *r, const struct app_info *a)
{
	GtkWidget *box = hbox(8);
	add_class(box, "group-row");
	snprintf(r->app, sizeof(r->app), "%s", a->first->app);

	gtk_box_pack_start(GTK_BOX(box), app_icon(a->first), FALSE, FALSE, 0);
	GtkWidget *name = label(NULL, r->app);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(box), name, TRUE, TRUE, 0);
	r->note = label("small warn", "");
	gtk_box_pack_start(GTK_BOX(box), r->note, FALSE, FALSE, 0);
	r->check = plain_check();
	gtk_widget_set_tooltip_text(r->check, "Equalize this application's volume");
	r->check_handler = g_signal_connect(r->check, "toggled", G_CALLBACK(on_app), r);
	gtk_box_pack_end(GTK_BOX(box), r->check, FALSE, FALSE, 0);

	r->widget = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r->widget), FALSE);
	gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(r->widget), FALSE);
	gtk_container_add(GTK_CONTAINER(r->widget), box);
	gtk_container_add(GTK_CONTAINER(ui.list), r->widget);
	gtk_widget_show_all(r->widget);
}

static void update_row(struct row *r, const struct app_info *a)
{
	struct leq_model *m = ui.m;

	set_check(r->check, r->check_handler, !leq_app_disabled(m, r->app));
	gtk_widget_set_sensitive(r->check, m->have_metadata && !a->unsupported);
	gtk_label_set_text(GTK_LABEL(r->note), a->unsupported ? "Surround not supported" : "");
	gtk_widget_set_tooltip_text(r->widget, a->unsupported ?
		"Streams with more than 8 channels can't be equalized" : NULL);
	set_class(r->widget, "faded", !m->enabled || a->unsupported);
}

static void refresh_list(void)
{
	struct app_info apps[MAX_ROWS];
	int n = collect_apps(apps);
	bool same = n == ui.n_rows;

	for (int i = 0; same && i < n; i++)
		same = strcmp(ui.rows[i].app, apps[i].first->app) == 0;
	if (!same) {
		for (int i = 0; i < ui.n_rows; i++)
			gtk_widget_destroy(ui.rows[i].widget);
		for (int i = 0; i < n; i++)
			build_row(&ui.rows[i], &apps[i]);
		ui.n_rows = n;
	}
	for (int i = 0; i < n; i++)
		update_row(&ui.rows[i], &apps[i]);
}

/* ------------------------------------------------------------------------ */
/* Layout                                                                    */
/* ------------------------------------------------------------------------ */

static GtkWidget *make_banner(void)
{
	ui.banner = hbox(8);
	add_class(ui.banner, "banner");
	ui.banner_label = label(NULL, "");
	gtk_label_set_line_wrap(GTK_LABEL(ui.banner_label), TRUE);
	gtk_box_pack_start(GTK_BOX(ui.banner), ui.banner_label, TRUE, TRUE, 0);
	gtk_widget_set_no_show_all(ui.banner, TRUE);
	gtk_widget_show(ui.banner_label);
	return ui.banner;
}

static GtkWidget *make_combo(void)
{
	ui.combo = gtk_combo_box_text_new();
	for (int r = 2; r <= 7; r++) {
		char id[4], t[32], text[96];
		snprintf(id, sizeof(id), "%d", r);
		format_seconds(release_seconds(r), t, sizeof(t));
		snprintf(text, sizeof(text), "%s · %s%s", release_names[r], t,
			 r == 4 ? " (default)" : "");
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.combo), id, text);
	}
	ui.release_handler = g_signal_connect(ui.combo, "changed", G_CALLBACK(on_release), NULL);
	return ui.combo;
}

static GtkWidget *make_list(void)
{
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	ui.list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui.list), GTK_SELECTION_NONE);
	GtkWidget *empty = label("placeholder", "No application is playing audio");
	gtk_label_set_xalign(GTK_LABEL(empty), 0.5);
	gtk_widget_show(empty);
	gtk_list_box_set_placeholder(GTK_LIST_BOX(ui.list), empty);
	gtk_container_add(GTK_CONTAINER(scroll), ui.list);

	GtkWidget *frame = gtk_frame_new(NULL);
	gtk_container_add(GTK_CONTAINER(frame), scroll);
	gtk_widget_set_vexpand(frame, TRUE);
	return frame;
}

/* title / subtitle on the left, control on the right */
static GtkWidget *group_row(const char *title, const char *subtitle, GtkWidget *control, bool sep)
{
	GtkWidget *row = hbox(12);
	add_class(row, "group-row");
	if (sep)
		add_class(row, "separated");
	GtkWidget *text = vbox(2);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(text), label(NULL, title), FALSE, FALSE, 0);
	GtkWidget *s = label("dim small", subtitle);
	gtk_label_set_line_wrap(GTK_LABEL(s), TRUE);
	gtk_box_pack_start(GTK_BOX(text), s, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
	gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
	gtk_box_pack_end(GTK_BOX(row), control, FALSE, FALSE, 0);
	return row;
}

static void build(GtkWidget *root)
{
	gtk_box_pack_start(GTK_BOX(root), make_banner(), FALSE, FALSE, 0);

	GtkWidget *general = vbox(0);
	add_class(general, "group");
	ui.global = plain_check();
	ui.global_handler = g_signal_connect(ui.global, "toggled", G_CALLBACK(on_global), NULL);
	gtk_box_pack_start(GTK_BOX(general),
		group_row("Loudness equalization", EXPLAIN, ui.global, false), FALSE, FALSE, 0);
	ui.release_row = group_row("Release time", "How fast the volume recovers after loud sounds",
				   make_combo(), true);
	gtk_box_pack_start(GTK_BOX(general), ui.release_row, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), general, FALSE, FALSE, 0);

	GtkWidget *apps = label("heading", "Applications");
	g_object_set(apps, "margin-top", 6, "margin-start", 2, NULL);
	gtk_box_pack_start(GTK_BOX(root), apps, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), make_list(), TRUE, TRUE, 0);
	GtkWidget *hint = label("dim small", "Unchecked applications keep their original volume.");
	g_object_set(hint, "margin-start", 2, NULL);
	gtk_box_pack_start(GTK_BOX(root), hint, FALSE, FALSE, 0);
}

/* ------------------------------------------------------------------------ */

void ui_refresh(void)
{
	struct leq_model *m = ui.m;
	const char *problem = NULL;
	char id[4];

	if (!ui.first && m->version == ui.seen_version)
		return;
	ui.first = false;
	ui.seen_version = m->version;

	if (!m->connected)
		problem = "PipeWire isn't running.";
	else if (!m->have_metadata)
		problem = "The loudness-eq PipeWire module isn't loaded. Install it, then restart PipeWire.";
	if (problem)
		gtk_label_set_text(GTK_LABEL(ui.banner_label), problem);
	gtk_widget_set_visible(ui.banner, problem != NULL);

	set_check(ui.global, ui.global_handler, m->enabled);
	gtk_widget_set_sensitive(ui.global, m->have_metadata);

	snprintf(id, sizeof(id), "%d", m->release);
	gtk_widget_set_sensitive(ui.combo, m->have_metadata);
	g_signal_handler_block(ui.combo, ui.release_handler);
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui.combo), id);
	g_signal_handler_unblock(ui.combo, ui.release_handler);
	set_class(ui.release_row, "faded", !m->enabled);

	refresh_list();
}

void ui_present(void)
{
	gtk_window_present(GTK_WINDOW(ui.window));
}

void ui_create(GtkApplication *app, struct leq_model *m)
{
	GtkCssProvider *provider = gtk_css_provider_new();
	GError *err = NULL;

	ui.m = m;
	ui.first = true;

	if (!gtk_css_provider_load_from_data(provider, css, -1, &err)) {
		g_warning("stylesheet: %s", err->message);
		g_clear_error(&err);
	}
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
			GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);

	/* no header bar: the window manager draws the title bar */
	ui.window = gtk_application_window_new(app);
	add_class(ui.window, "leq");
	gtk_window_set_title(GTK_WINDOW(ui.window), "Loudness Equalization");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 440, 500);
	gtk_window_set_icon_name(GTK_WINDOW(ui.window), "audio-volume-high");

	GtkWidget *root = vbox(8);
	gtk_container_set_border_width(GTK_CONTAINER(root), 14);
	gtk_container_add(GTK_CONTAINER(ui.window), root);
	build(root);

	ui_refresh();
	gtk_widget_show_all(ui.window);
}
