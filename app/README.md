# app

`pw-loudness-eq`: the control window for the loudness-eq module, laid out like a preferences pane: on/off and the release time at the top, then one checkbox per app currently playing audio.

GTK3 without libadwaita or client-side decorations, styled by its own stylesheet (`src/ui.c`). It talks to the module only through the `loudness-eq` PipeWire metadata object (`src/pw-client.c`), so it works the same as `pw-metadata`. Only one copy runs at a time; launching it again raises the open window.

```sh
make -C app && app/build/pw-loudness-eq
```

