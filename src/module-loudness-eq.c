/*
 * module-loudness-eq.c - per-application Loudness Equalization for PipeWire.
 *
 * Windows runs Loudness Equalization as a stream effect (SFX): one instance
 * per application stream, before mixing, with its own gain. This module does
 * the same without creating any virtual device:
 *
 *   - it watches the registry for application playback streams
 *     (media.class = Stream/Output/Audio);
 *   - it subscribes to each stream's Format param to learn its channel count;
 *   - it sets the stream's audioconvert property
 *       audioconvert.filter-graph.<order> = { nodes = [ { type = ladspa
 *           plugin = pw_loudness_eq label = loudness_eq_<N>ch ... } ] }
 *     through the node's Props param. audioconvert then runs the LADSPA
 *     plugin inside that stream, before the stream's own volume and before
 *     mixing, and adds the plugin's latency to the stream's latency.
 *
 * On unload, the graphs are removed again (the property is set to "").
 *
 * Arguments (all optional):
 *   release = 4              Windows "Release Time" (2..7, default 4)
 *   graph.order = 90         audioconvert.filter-graph.<order> slot to use
 *   plugin = pw_loudness_eq  LADSPA library name (searched in LADSPA_PATH or
 *                            /usr/lib/ladspa; PipeWire refuses absolute paths
 *                            for graphs set this way)
 *   stream.rules = [ { matches = [ { application.name = "foo" } ]
 *                      actions = { loudness-eq = false } } ]
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/list.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/impl.h>

#define NAME "loudness-eq"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MAX_CHANNELS    8
#define DEFAULT_RELEASE 4   /* Windows default */
#define DEFAULT_ORDER   90

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "pw-loudness-eq contributors" },
	{ PW_KEY_MODULE_DESCRIPTION, "Per-application Loudness Equalization (Windows algorithm)" },
	{ PW_KEY_MODULE_USAGE, "( release=<2..7, default 4> ) ( graph.order=<int, default 90> ) "
			       "( plugin=<ladspa name, default pw_loudness_eq> ) "
			       "( stream.rules=[ { matches=[ ... ] actions={ loudness-eq=false } } ] )" },
	{ PW_KEY_MODULE_VERSION, "2.0.0" },
};

struct impl;

struct stream {
	struct spa_list link;
	struct impl *impl;
	uint32_t id;
	char name[128];
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook node_listener;
	uint32_t channels;       /* channel count the graph was built for, 0 = none */
};

struct impl {
	struct pw_context *context;
	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
	bool do_disconnect;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_list streams;

	unsigned release;
	char key[64];            /* audioconvert.filter-graph.<order> */
	char plugin[256];
	char *rules;             /* JSON, may be NULL */

	bool unloading;
};

/* ------------------------------------------------------------------------ */
/* Applying the graph                                                        */
/* ------------------------------------------------------------------------ */

static int set_graph(struct stream *s, uint32_t channels)
{
	struct impl *impl = s->impl;
	char graph[512] = "";
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod_frame f[2];
	struct spa_pod *param;

	if (channels > 0)
		snprintf(graph, sizeof(graph),
			 "{ nodes = [ { type = ladspa name = loudness_eq plugin = \"%s\" "
			 "label = loudness_eq_%uch control = { \"Release\" = %u } } ] }",
			 impl->plugin, channels, impl->release);

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&b, &f[1]);
	spa_pod_builder_string(&b, impl->key);
	spa_pod_builder_string(&b, graph);
	spa_pod_builder_pop(&b, &f[1]);
	param = spa_pod_builder_pop(&b, &f[0]);
	if (param == NULL)
		return -ENOSPC;

	pw_log_info("%s stream %u (%s) %s", channels ? "applying to" : "removing from",
		    s->id, s->name, graph);
	s->channels = channels;
	return pw_node_set_param((struct pw_node *)s->proxy, SPA_PARAM_Props, 0, param);
}

static void stream_param(void *data, int seq, uint32_t id, uint32_t index,
			 uint32_t next, const struct spa_pod *param)
{
	struct stream *s = data;
	struct spa_audio_info_raw info;
	uint32_t media_type, media_subtype;

	if (id != SPA_PARAM_Format || param == NULL)
		return;
	if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
	    media_type != SPA_MEDIA_TYPE_audio || media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;
	spa_zero(info);
	if (spa_format_audio_raw_parse(param, &info) < 0 || info.channels == 0)
		return;

	if (info.channels > MAX_CHANNELS) {
		pw_log_info("stream %u (%s): %u channels not supported, skipping",
			    s->id, s->name, info.channels);
		if (s->channels)
			set_graph(s, 0);
		return;
	}
	if (info.channels != s->channels)
		set_graph(s, info.channels);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.param = stream_param,
};

static void proxy_removed(void *data)
{
	struct stream *s = data;
	pw_proxy_destroy(s->proxy);
}

static void proxy_destroy(void *data)
{
	struct stream *s = data;
	spa_hook_remove(&s->node_listener);
	spa_hook_remove(&s->proxy_listener);
	spa_list_remove(&s->link);
	free(s);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
};

/* ------------------------------------------------------------------------ */
/* Stream selection                                                          */
/* ------------------------------------------------------------------------ */

static int rule_action(void *data, const char *location, const char *action,
		       const char *val, size_t len)
{
	bool *enabled = data;
	if (spa_streq(action, "loudness-eq"))
		*enabled = len == 4 && strncmp(val, "true", 4) == 0;
	return 0;
}

static bool wants_stream(struct impl *impl, const struct spa_dict *props)
{
	const char *str;
	bool enabled = true;

	if (props == NULL)
		return false;
	if (!spa_streq(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS), "Stream/Output/Audio"))
		return false;
	/* Internal streams of filter-chain / loopback / echo-cancel / combine
	 * modules carry already-mixed audio. Windows' SFX never sees those. */
	if (spa_dict_lookup(props, PW_KEY_NODE_LINK_GROUP) != NULL)
		return false;
	if ((str = spa_dict_lookup(props, PW_KEY_NODE_VIRTUAL)) != NULL && spa_atob(str))
		return false;

	if (impl->rules)
		pw_conf_match_rules(impl->rules, strlen(impl->rules), NAME, props,
				    rule_action, &enabled);
	return enabled;
}

static void registry_global(void *data, uint32_t id, uint32_t permissions,
			    const char *type, uint32_t version, const struct spa_dict *props)
{
	struct impl *impl = data;
	struct stream *s;
	const char *str;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node) || !wants_stream(impl, props))
		return;

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return;
	s->impl = impl;
	s->id = id;
	if ((str = spa_dict_lookup(props, PW_KEY_APP_NAME)) == NULL &&
	    (str = spa_dict_lookup(props, PW_KEY_NODE_NAME)) == NULL)
		str = "?";
	snprintf(s->name, sizeof(s->name), "%s", str);

	s->proxy = pw_registry_bind(impl->registry, id, PW_TYPE_INTERFACE_Node,
				    PW_VERSION_NODE, 0);
	if (s->proxy == NULL) {
		free(s);
		return;
	}
	spa_list_append(&impl->streams, &s->link);
	pw_proxy_add_listener(s->proxy, &s->proxy_listener, &proxy_events, s);
	pw_node_add_listener((struct pw_node *)s->proxy, &s->node_listener, &node_events, s);

	uint32_t ids[] = { SPA_PARAM_Format };
	pw_node_subscribe_params((struct pw_node *)s->proxy, ids, SPA_N_ELEMENTS(ids));
	pw_log_debug("watching stream %u (%s)", id, s->name);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
};

/* ------------------------------------------------------------------------ */
/* Core / module plumbing                                                    */
/* ------------------------------------------------------------------------ */

static void unload_module(struct impl *impl)
{
	if (!impl->unloading) {
		impl->unloading = true;
		pw_impl_module_schedule_destroy(impl->module);
	}
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;
	if (id == PW_ID_CORE && res == -EPIPE) {
		pw_log_error("connection lost: %s", message);
		unload_module(impl);
	} else {
		/* e.g. a stream disappearing while we talk to it: not our problem */
		pw_log_debug("error id:%u seq:%d res:%d (%s): %s", id, seq, res,
			     spa_strerror(res), message);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	/* all proxies (registry, nodes) are destroyed with the core and their
	 * destroy handlers free our stream entries */
	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->core_proxy_listener);
	impl->registry = NULL;
	impl->core = NULL;
	unload_module(impl);
}

static const struct pw_proxy_events core_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	struct stream *s, *t;

	/* take the graphs out of every stream we touched */
	spa_list_for_each_safe(s, t, &impl->streams, link) {
		if (s->channels && impl->core)
			set_graph(s, 0);
		pw_proxy_destroy(s->proxy);   /* frees s via proxy_destroy */
	}
	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy *)impl->registry);
	}
	if (impl->core) {
		spa_hook_remove(&impl->core_listener);
		spa_hook_remove(&impl->core_proxy_listener);
		if (impl->do_disconnect)
			pw_core_disconnect(impl->core);
	}
	free(impl->rules);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	impl->unloading = true;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct impl *impl;
	const char *str;
	int res, order = DEFAULT_ORDER;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return -errno;
	impl->module = module;
	impl->context = context;
	impl->release = DEFAULT_RELEASE;
	spa_list_init(&impl->streams);
	snprintf(impl->plugin, sizeof(impl->plugin), "pw_loudness_eq");

	props = args ? pw_properties_new_string(args) : pw_properties_new(NULL, NULL);
	if (props == NULL) {
		res = -errno;
		goto error;
	}
	if ((str = pw_properties_get(props, "release")) != NULL) {
		int r = atoi(str);
		impl->release = (unsigned)SPA_CLAMP(r, 0, 7);
	}
	if ((str = pw_properties_get(props, "graph.order")) != NULL)
		order = atoi(str);
	snprintf(impl->key, sizeof(impl->key), "audioconvert.filter-graph.%d", order);
	if ((str = pw_properties_get(props, "plugin")) != NULL)
		snprintf(impl->plugin, sizeof(impl->plugin), "%s", str);
	if ((str = pw_properties_get(props, "stream.rules")) != NULL)
		impl->rules = strdup(str);

	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(context,
				pw_properties_new(PW_KEY_REMOTE_NAME, str, NULL), 0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}
	pw_proxy_add_listener((struct pw_proxy *)impl->core, &impl->core_proxy_listener,
			      &core_proxy_events, impl);
	pw_core_add_listener(impl->core, &impl->core_listener, &core_events, impl);

	impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
	if (impl->registry == NULL) {
		res = -errno;
		goto error;
	}
	pw_registry_add_listener(impl->registry, &impl->registry_listener,
				 &registry_events, impl);

	pw_log_info("loaded: release=%u key=%s plugin=%s",
		    impl->release, impl->key, impl->plugin);

	pw_properties_free(props);
	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	return 0;

error:
	pw_properties_free(props);
	impl_destroy(impl);
	return res;
}
