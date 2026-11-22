# pw-loudness-eq - top-level: builds each component in its own folder
#   module/  PipeWire module + LADSPA DSP plugin
#   app/     control/monitor GUI (not built yet)
SUBDIRS := module

all check install uninstall clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@ || exit 1; done

.PHONY: all check install uninstall clean
