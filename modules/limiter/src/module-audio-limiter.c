/*
 * module-audio-limiter.c - the Windows audio engine's output limiter on
 * every playback device.
 *
 * Windows (audiodg.exe, CPipeInstance::CreateDevicePipeInstance) puts
 * CAudioLimiter in the pipeline of every render device, after the mix and
 * after all software volume, always, with no setting. This module does the
 * same in PipeWire:
 *
 *   - it watches the registry for playback devices (media.class =
 *     Audio/Sink with a device.id, not virtual, not part of a filter);
 *   - it subscribes to each device's Format (channel count; non-PCM formats
 *     such as IEC958 passthrough are left alone, as Windows bypasses the
 *     engine for them) and Props (software volume);
 *   - it sets the device's audioconvert property
 *       audioconvert.filter-graph.<order> = { nodes = [ { type = ladspa
 *           plugin = pw_audio_limiter label = audio_limiter_<N>ch ... } ] }
 *     with the lowest order, so it runs after any other graph there.
 *
 * Volume: PipeWire applies a device's software volume after its filter
 * graphs; Windows applies it before the limiter. The plugin is told the
 * volume per channel ("Volume N" controls, kept in sync here with the
 * device's softVolumes x volume, 0 when muted), measures peaks after it and
 * applies only its gain, so the result is Windows' order (see limiter.h).
 * Hardware volume comes after the limiter on both systems.
 *
 * On unload the graphs are removed again.
 *
 * Arguments (all optional):
 *   graph.order = 0          audioconvert.filter-graph.<order> slot to use
 *   plugin = pw_audio_limiter
 *   device.rules = [ { matches = [ { node.name = "foo" } ]
 *                      actions = { audio-limiter = false } } ]
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/list.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/impl.h>

#define NAME "audio-limiter"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MAX_CHANNELS    64      /* SPA_AUDIO_MAX_CHANNELS, and the plugin's */
#define DEFAULT_ORDER   0
#define MAX_VOLUME      10.0f   /* audioconvert's default volume range */
#define GRAPH_NODE      "audio_limiter"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "pw-loudness-eq contributors" },
	{ PW_KEY_MODULE_DESCRIPTION, "Windows audio engine output limiter on every playback device" },
	{ PW_KEY_MODULE_USAGE, "( graph.order=<int, default 0> ) "
			       "( plugin=<ladspa name, default pw_audio_limiter> ) "
			       "( device.rules=[ { matches=[ ... ] actions={ audio-limiter=false } } ] )" },
	{ PW_KEY_MODULE_VERSION, "1.0.0" },
};

struct impl;

struct device {
	struct spa_list link;
	struct impl *impl;
	uint32_t id;
	char name[128];
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook node_listener;

	/* Format */
	uint32_t format_channels;          /* 0 = unknown or not PCM */
	uint32_t position[MAX_CHANNELS];

	/* Props: the software volume audioconvert applies after the graph */
	float volume;                      /* master */
	bool mute;
	uint32_t n_soft;
	float soft[MAX_CHANNELS];
	uint32_t n_map;
	uint32_t map[MAX_CHANNELS];        /* channel position of each soft volume */

	uint32_t channels;                 /* graph built for, 0 = none */
	float sent[MAX_CHANNELS];          /* volume controls last sent */
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

	struct spa_list devices;

	char key[64];            /* audioconvert.filter-graph.<order> */
	char plugin[256];
	char *rules;             /* JSON, may be NULL */

	bool unloading;
};

/* ------------------------------------------------------------------------ */
/* Volume                                                                    */
/* ------------------------------------------------------------------------ */

/* What audioconvert's channelmix multiplies format channel i by, after the
 * graph: softVolume (matched by position) x volume, 0 when muted. */
static float channel_volume(struct device *d, uint32_t i)
{
	float v = 1.0f;
	uint32_t j;

	if (d->mute)
		return 0.0f;
	for (j = 0; j < d->n_map && j < d->n_soft; j++)
		if (d->map[j] == d->position[i])
			break;
	if (j < d->n_map && j < d->n_soft)
		v = d->soft[j];
	else if (i < d->n_soft)
		v = d->soft[i];
	return SPA_CLAMPF(v, 0.0f, MAX_VOLUME) * SPA_CLAMPF(d->volume, 0.0f, MAX_VOLUME);
}

/* ------------------------------------------------------------------------ */
/* Applying the graph                                                        */
/* ------------------------------------------------------------------------ */

static int set_props(struct device *d, const char *graph, bool volumes)
{
	uint8_t buffer[8192];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod_frame f[2];
	struct spa_pod *param;
	char key[64];

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&b, &f[1]);
	if (graph) {
		spa_pod_builder_string(&b, d->impl->key);
		spa_pod_builder_string(&b, graph);
	}
	if (volumes) {
		for (uint32_t i = 0; i < d->channels; i++) {
			snprintf(key, sizeof(key), GRAPH_NODE ":Volume %u", i + 1);
			spa_pod_builder_string(&b, key);
			spa_pod_builder_float(&b, d->sent[i]);
		}
	}
	spa_pod_builder_pop(&b, &f[1]);
	param = spa_pod_builder_pop(&b, &f[0]);
	if (param == NULL)
		return -ENOSPC;
	return pw_node_set_param((struct pw_node *)d->proxy, SPA_PARAM_Props, 0, param);
}

static int set_graph(struct device *d, uint32_t channels)
{
	struct impl *impl = d->impl;
	char graph[4096] = "";
	size_t len;

	d->channels = channels;
	if (channels > 0) {
		len = (size_t)snprintf(graph, sizeof(graph),
			"{ nodes = [ { type = ladspa name = " GRAPH_NODE " plugin = \"%s\" "
			"label = audio_limiter_%uch control = {", impl->plugin, channels);
		for (uint32_t i = 0; i < channels && len < sizeof(graph); i++) {
			d->sent[i] = channel_volume(d, i);
			len += (size_t)snprintf(graph + len, sizeof(graph) - len,
						" \"Volume %u\" = %.9g", i + 1, d->sent[i]);
		}
		if (len < sizeof(graph))
			snprintf(graph + len, sizeof(graph) - len, " } } ] }");
	}
	pw_log_info("%s device %u (%s) %s", channels ? "applying to" : "removing from",
		    d->id, d->name, graph);
	return set_props(d, graph, false);
}

/* Bring the graph and its volume controls in line with Format and Props. */
static void update_device(struct device *d)
{
	bool changed = false;

	if (d->format_channels != d->channels) {
		set_graph(d, d->format_channels);
		return;
	}
	for (uint32_t i = 0; i < d->channels; i++) {
		float v = channel_volume(d, i);
		if (v != d->sent[i]) {
			d->sent[i] = v;
			changed = true;
		}
	}
	if (changed) {
		pw_log_debug("device %u (%s): volume controls updated", d->id, d->name);
		set_props(d, NULL, true);
	}
}

/* ------------------------------------------------------------------------ */
/* Device params                                                             */
/* ------------------------------------------------------------------------ */

static void parse_format(struct device *d, const struct spa_pod *param)
{
	struct spa_audio_info_raw info;
	uint32_t media_type, media_subtype;

	d->format_channels = 0;
	if (param == NULL ||
	    spa_format_parse(param, &media_type, &media_subtype) < 0 ||
	    media_type != SPA_MEDIA_TYPE_audio)
		return;
	if (media_subtype != SPA_MEDIA_SUBTYPE_raw) {
		/* IEC958/DSD passthrough: not mixed, Windows doesn't limit it either */
		pw_log_info("device %u (%s): not PCM, leaving it alone", d->id, d->name);
		return;
	}
	spa_zero(info);
	if (spa_format_audio_raw_parse(param, &info) < 0 ||
	    info.channels == 0 || info.channels > MAX_CHANNELS)
		return;
	d->format_channels = info.channels;
	memcpy(d->position, info.position, sizeof(uint32_t) * info.channels);
}

static void parse_props(struct device *d, const struct spa_pod *param)
{
	const struct spa_pod_object *obj = (const struct spa_pod_object *)param;
	const struct spa_pod_prop *prop;

	if (!spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props))
		return;
	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			spa_pod_get_float(&prop->value, &d->volume);
			break;
		case SPA_PROP_softMute:
			spa_pod_get_bool(&prop->value, &d->mute);
			break;
		case SPA_PROP_softVolumes:
			d->n_soft = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
						       d->soft, MAX_CHANNELS);
			break;
		case SPA_PROP_channelMap:
			d->n_map = spa_pod_copy_array(&prop->value, SPA_TYPE_Id,
						      d->map, MAX_CHANNELS);
			break;
		}
	}
}

static void device_param(void *data, int seq, uint32_t id, uint32_t index,
			 uint32_t next, const struct spa_pod *param)
{
	struct device *d = data;

	if (id == SPA_PARAM_Format)
		parse_format(d, param);
	else if (id == SPA_PARAM_Props && param != NULL)
		parse_props(d, param);
	else
		return;
	update_device(d);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.param = device_param,
};

static void proxy_removed(void *data)
{
	struct device *d = data;
	pw_proxy_destroy(d->proxy);
}

static void proxy_destroy(void *data)
{
	struct device *d = data;
	spa_hook_remove(&d->node_listener);
	spa_hook_remove(&d->proxy_listener);
	spa_list_remove(&d->link);
	free(d);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
};

/* ------------------------------------------------------------------------ */
/* Device selection                                                          */
/* ------------------------------------------------------------------------ */

static int rule_action(void *data, const char *location, const char *action,
		       const char *val, size_t len)
{
	bool *enabled = data;
	if (spa_streq(action, "audio-limiter"))
		*enabled = len == 4 && strncmp(val, "true", 4) == 0;
	return 0;
}

static bool wants_device(struct impl *impl, const struct spa_dict *props)
{
	const char *str;
	bool enabled;

	if (props == NULL ||
	    !spa_streq(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS), "Audio/Sink"))
		return false;
	/* Windows' render endpoints are devices; virtual sinks and the
	 * internal nodes of filter-chain/loopback/combine feed one, which is
	 * limited itself */
	enabled = spa_dict_lookup(props, PW_KEY_DEVICE_ID) != NULL &&
		  spa_dict_lookup(props, PW_KEY_NODE_LINK_GROUP) == NULL &&
		  !((str = spa_dict_lookup(props, PW_KEY_NODE_VIRTUAL)) != NULL && spa_atob(str));

	if (impl->rules)
		pw_conf_match_rules(impl->rules, strlen(impl->rules), NAME, props,
				    rule_action, &enabled);
	return enabled;
}

static void registry_global(void *data, uint32_t id, uint32_t permissions,
			    const char *type, uint32_t version, const struct spa_dict *props)
{
	struct impl *impl = data;
	struct device *d;
	const char *str;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node) || !wants_device(impl, props))
		return;

	d = calloc(1, sizeof(*d));
	if (d == NULL)
		return;
	d->impl = impl;
	d->id = id;
	d->volume = 1.0f;
	if ((str = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION)) == NULL &&
	    (str = spa_dict_lookup(props, PW_KEY_NODE_NAME)) == NULL)
		str = "?";
	snprintf(d->name, sizeof(d->name), "%s", str);

	d->proxy = pw_registry_bind(impl->registry, id, PW_TYPE_INTERFACE_Node,
				    PW_VERSION_NODE, 0);
	if (d->proxy == NULL) {
		free(d);
		return;
	}
	spa_list_append(&impl->devices, &d->link);
	pw_proxy_add_listener(d->proxy, &d->proxy_listener, &proxy_events, d);
	pw_node_add_listener((struct pw_node *)d->proxy, &d->node_listener, &node_events, d);

	uint32_t ids[] = { SPA_PARAM_Props, SPA_PARAM_Format };
	pw_node_subscribe_params((struct pw_node *)d->proxy, ids, SPA_N_ELEMENTS(ids));
	pw_log_debug("watching device %u (%s)", id, d->name);
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
		pw_log_debug("error id:%u seq:%d res:%d (%s): %s", id, seq, res,
			     spa_strerror(res), message);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroy(void *data)
{
	struct impl *impl = data;
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
	struct device *d, *t;

	spa_list_for_each_safe(d, t, &impl->devices, link) {
		if (d->channels && impl->core)
			set_graph(d, 0);
		pw_proxy_destroy(d->proxy);   /* frees d via proxy_destroy */
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
	spa_list_init(&impl->devices);
	snprintf(impl->plugin, sizeof(impl->plugin), "pw_audio_limiter");

	props = args ? pw_properties_new_string(args) : pw_properties_new(NULL, NULL);
	if (props == NULL) {
		res = -errno;
		goto error;
	}
	if ((str = pw_properties_get(props, "graph.order")) != NULL)
		order = atoi(str);
	snprintf(impl->key, sizeof(impl->key), "audioconvert.filter-graph.%d", order);
	if ((str = pw_properties_get(props, "plugin")) != NULL)
		snprintf(impl->plugin, sizeof(impl->plugin), "%s", str);
	if ((str = pw_properties_get(props, "device.rules")) != NULL)
		impl->rules = strdup(str);

	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		/* inside a daemon, talk to that daemon, not the default remote */
		const struct pw_properties *cp = pw_context_get_properties(context);
		const char *daemon = pw_properties_get(cp, PW_KEY_CORE_DAEMON);
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		if (str == NULL && daemon && spa_atob(daemon))
			str = pw_properties_get(cp, PW_KEY_CORE_NAME);
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

	pw_log_info("loaded: key=%s plugin=%s", impl->key, impl->plugin);

	pw_properties_free(props);
	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	return 0;

error:
	pw_properties_free(props);
	impl_destroy(impl);
	return res;
}
