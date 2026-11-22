# pw-loudness-eq - top-level: builds each component in its own folder
#   modules/loudness/  Loudness Equalization: PipeWire module + LADSPA plugin (per app stream)
#   modules/limiter/   Windows audio engine limiter: PipeWire module + LADSPA plugin (per device)
#   app/               control window (GTK3)
SUBDIRS := modules/loudness modules/limiter app

all check install uninstall clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@ || exit 1; done

.PHONY: all check install uninstall clean
