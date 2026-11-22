/*
 * model.h - what the app knows about the module, filled from PipeWire.
 *
 * Everything comes from the "loudness-eq" metadata object the module owns
 * (settings on subject 0, a "state" per stream id) plus the registry (app
 * names of those streams). The UI reads it; writes go through pwc_set_*().
 */
#ifndef LEQ_MODEL_H
#define LEQ_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_STREAMS 64

struct leq_stream {
	uint32_t id;
	char app[128];           /* application.name, else node.name: the module's key */
	char icon[64];           /* application.icon-name, may be "" */
	char binary[64];         /* application.process.binary, may be "" */
	char state[16];          /* active | off | unsupported | pending, "" = unknown */
	bool has_node;           /* seen in the registry */
};

struct leq_model {
	bool connected;          /* to PipeWire */
	bool have_metadata;      /* module running (its metadata exists) */

	bool enabled;
	int release;
	char disabled_apps[4096];   /* JSON array of strings */

	struct leq_stream streams[MAX_STREAMS];
	int n_streams;

	unsigned version;        /* bumped on every change */
};

/* pw-client.c */
int  pwc_init(struct leq_model *m);
int  pwc_get_fd(void);
void pwc_iterate(void);      /* non-blocking, also retries a lost connection */
void pwc_deinit(void);

void pwc_set_enabled(bool enabled);
void pwc_set_release(int release);
void pwc_set_app_enabled(const char *app, bool enabled);

bool leq_app_disabled(const struct leq_model *m, const char *app);

/* ui.c */
struct _GtkApplication;
void ui_create(struct _GtkApplication *app, struct leq_model *m);
void ui_present(void);
void ui_refresh(void);       /* cheap when nothing changed */

#endif
