/*
 * pw-client.c - PipeWire side of the app.
 *
 * Runs on its own pw_loop, whose fd the GTK main loop watches (pwc_get_fd)
 * and iterates without blocking, so everything stays on the GTK thread.
 * Reconnects when PipeWire restarts.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#include "model.h"

#define METADATA_NAME "loudness-eq"

static struct {
	struct leq_model *m;

	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_metadata *metadata;
	struct spa_hook metadata_listener;
	struct spa_hook metadata_proxy_listener;
	uint32_t metadata_id;

	bool lost;               /* connection dropped, reconnect pending */
	double next_retry;
} g;

static void changed(void) { g.m->version++; }

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------------ */
/* Model helpers                                                             */
/* ------------------------------------------------------------------------ */

static struct leq_stream *find_stream(uint32_t id, bool create)
{
	struct leq_model *m = g.m;
	for (int i = 0; i < m->n_streams; i++)
		if (m->streams[i].id == id)
			return &m->streams[i];
	if (!create || m->n_streams == MAX_STREAMS)
		return NULL;
	struct leq_stream *s = &m->streams[m->n_streams++];
	memset(s, 0, sizeof(*s));
	s->id = id;
	return s;
}

static void drop_stream(uint32_t id)
{
	struct leq_model *m = g.m;
	for (int i = 0; i < m->n_streams; i++)
		if (m->streams[i].id == id) {
			m->streams[i] = m->streams[--m->n_streams];
			changed();
			return;
		}
}

static void forget_metadata_state(void)
{
	struct leq_model *m = g.m;
	m->have_metadata = false;
	for (int i = 0; i < m->n_streams; i++)
		m->streams[i].state[0] = '\0';
	changed();
}

bool leq_app_disabled(const struct leq_model *m, const char *app)
{
	struct spa_json it;
	char name[256];

	if (spa_json_begin_array(&it, m->disabled_apps, strlen(m->disabled_apps)) <= 0)
		return false;
	while (spa_json_get_string(&it, name, sizeof(name)) > 0)
		if (spa_streq(name, app))
			return true;
	return false;
}

/* ------------------------------------------------------------------------ */
/* Metadata                                                                  */
/* ------------------------------------------------------------------------ */

static int metadata_property(void *data, uint32_t subject, const char *key,
			     const char *type, const char *value)
{
	struct leq_model *m = g.m;

	if (subject == PW_ID_CORE) {
		if (key == NULL)
			return 0;       /* the module writes the defaults back */
		if (spa_streq(key, "enabled"))
			m->enabled = value ? spa_atob(value) : true;
		else if (spa_streq(key, "release"))
			m->release = value ? atoi(value) : 4;
		else if (spa_streq(key, "disabled-apps"))
			snprintf(m->disabled_apps, sizeof(m->disabled_apps), "%s",
				 value ? value : "[ ]");
		else
			return 0;
	} else if (key == NULL || spa_streq(key, "state")) {
		struct leq_stream *s = find_stream(subject, value != NULL);
		if (s == NULL)
			return 0;
		snprintf(s->state, sizeof(s->state), "%s", value ? value : "");
		if (!s->has_node && !s->state[0])
			drop_stream(subject);
	} else {
		return 0;
	}
	changed();
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void metadata_proxy_destroy(void *data)
{
	spa_hook_remove(&g.metadata_listener);
	spa_hook_remove(&g.metadata_proxy_listener);
	g.metadata = NULL;
	forget_metadata_state();
}

static const struct pw_proxy_events metadata_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = metadata_proxy_destroy,
};

static void set_property(const char *key, const char *type, const char *value)
{
	if (g.metadata)
		pw_metadata_set_property(g.metadata, PW_ID_CORE, key, type, value);
}

void pwc_set_enabled(bool enabled)
{
	set_property("enabled", NULL, enabled ? "true" : "false");
}

void pwc_set_release(int release)
{
	char v[16];
	snprintf(v, sizeof(v), "%d", release);
	set_property("release", NULL, v);
}

static void json_append_string(char *buf, size_t size, const char *s)
{
	size_t n = strlen(buf);
	if (n + 2 >= size)
		return;
	buf[n++] = ' ';
	buf[n++] = '"';
	for (; *s && n + 3 < size; s++) {
		if (*s == '"' || *s == '\\')
			buf[n++] = '\\';
		buf[n++] = *s;
	}
	buf[n++] = '"';
	buf[n] = '\0';
}

void pwc_set_app_enabled(const char *app, bool enabled)
{
	struct leq_model *m = g.m;
	char out[sizeof(m->disabled_apps)] = "[", name[256];
	struct spa_json it;

	/* keep every other entry, including apps that aren't playing now */
	if (spa_json_begin_array(&it, m->disabled_apps, strlen(m->disabled_apps)) > 0)
		while (spa_json_get_string(&it, name, sizeof(name)) > 0)
			if (!spa_streq(name, app))
				json_append_string(out, sizeof(out), name);
	if (!enabled)
		json_append_string(out, sizeof(out), app);
	strncat(out, " ]", sizeof(out) - strlen(out) - 1);
	set_property("disabled-apps", "Spa:String:JSON", out);
}

/* ------------------------------------------------------------------------ */
/* Registry                                                                  */
/* ------------------------------------------------------------------------ */

static void registry_global(void *data, uint32_t id, uint32_t permissions,
			    const char *type, uint32_t version, const struct spa_dict *props)
{
	const char *str;

	if (props == NULL)
		return;

	if (spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
		if (g.metadata || !spa_streq(spa_dict_lookup(props, PW_KEY_METADATA_NAME),
					     METADATA_NAME))
			return;
		g.metadata = pw_registry_bind(g.registry, id, PW_TYPE_INTERFACE_Metadata,
					      PW_VERSION_METADATA, 0);
		if (g.metadata == NULL)
			return;
		g.metadata_id = id;
		pw_metadata_add_listener(g.metadata, &g.metadata_listener, &metadata_events, NULL);
		pw_proxy_add_listener((struct pw_proxy *)g.metadata, &g.metadata_proxy_listener,
				      &metadata_proxy_events, NULL);
		g.m->have_metadata = true;
		changed();
		return;
	}

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node) ||
	    !spa_streq(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS), "Stream/Output/Audio"))
		return;

	struct leq_stream *s = find_stream(id, true);
	if (s == NULL)
		return;
	/* same rule as the module */
	if ((str = spa_dict_lookup(props, PW_KEY_APP_NAME)) == NULL &&
	    (str = spa_dict_lookup(props, PW_KEY_NODE_NAME)) == NULL)
		str = "?";
	snprintf(s->app, sizeof(s->app), "%s", str);
	str = spa_dict_lookup(props, PW_KEY_APP_ICON_NAME);
	snprintf(s->icon, sizeof(s->icon), "%s", str ? str : "");
	str = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
	snprintf(s->binary, sizeof(s->binary), "%s", str ? str : "");
	s->has_node = true;
	changed();
}

static void registry_global_remove(void *data, uint32_t id)
{
	if (g.metadata && id == g.metadata_id) {
		pw_proxy_destroy((struct pw_proxy *)g.metadata);
		return;
	}
	drop_stream(id);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------------ */
/* Connection                                                                */
/* ------------------------------------------------------------------------ */

static void disconnect(void)
{
	if (g.metadata)
		pw_proxy_destroy((struct pw_proxy *)g.metadata);
	if (g.registry) {
		spa_hook_remove(&g.registry_listener);
		pw_proxy_destroy((struct pw_proxy *)g.registry);
		g.registry = NULL;
	}
	if (g.core) {
		spa_hook_remove(&g.core_listener);
		pw_core_disconnect(g.core);
		g.core = NULL;
	}
	g.m->connected = false;
	g.m->n_streams = 0;
	changed();
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	if (id == PW_ID_CORE && res == -EPIPE) {
		fprintf(stderr, "pw-loudness-eq: lost PipeWire connection, retrying\n");
		g.lost = true;
		g.next_retry = now() + 1.0;
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static int connect_core(void)
{
	g.core = pw_context_connect(g.context, NULL, 0);
	if (g.core == NULL)
		return -errno;
	pw_core_add_listener(g.core, &g.core_listener, &core_events, NULL);
	g.registry = pw_core_get_registry(g.core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(g.registry, &g.registry_listener, &registry_events, NULL);
	g.m->connected = true;
	changed();
	return 0;
}

int pwc_init(struct leq_model *m)
{
	pw_init(NULL, NULL);
	g.m = m;
	m->enabled = true;
	m->release = 4;
	snprintf(m->disabled_apps, sizeof(m->disabled_apps), "[ ]");

	g.loop = pw_loop_new(NULL);
	if (g.loop == NULL)
		return -errno;
	g.context = pw_context_new(g.loop,
			pw_properties_new(PW_KEY_APP_NAME, "Loudness Equalization",
					  PW_KEY_APP_ID, "pw-loudness-eq", NULL), 0);
	if (g.context == NULL)
		return -errno;
	pw_loop_enter(g.loop);
	if (connect_core() < 0) {
		g.lost = true;
		g.next_retry = now() + 1.0;
	}
	return 0;
}

int pwc_get_fd(void)
{
	return pw_loop_get_fd(g.loop);
}

void pwc_iterate(void)
{
	pw_loop_iterate(g.loop, 0);
	if (g.lost && now() >= g.next_retry) {
		disconnect();
		g.lost = connect_core() < 0;
		g.next_retry = now() + 1.0;
	}
}

void pwc_deinit(void)
{
	disconnect();
	pw_loop_leave(g.loop);
	pw_context_destroy(g.context);
	pw_loop_destroy(g.loop);
	pw_deinit();
}
