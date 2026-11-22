# pw-loudness-eq

Windows' Loudness Equalization for PipeWire, ported from `WMALFXGFXDSP.dll`. It evens out volume per application, so quiet and loud parts play at a similar level, with a small window to turn it on or off globally or per app.

<p align="center">
  <img src="docs/screenshot.png" alt="The Loudness Equalization window">
</p>

Two parts:

- **[`module/`](module)**: the PipeWire module and the LADSPA plugin that does the processing. It runs inside PipeWire and applies the effect to every application's playback stream.
- **[`app/`](app)**: the **Loudness Equalization** window (`pw-loudness-eq`), a small GTK3 program for turning it on or off, globally or per app, and setting the release time. It's optional: the module works without it, and everything it does can also be done with `pw-metadata`.

## Install

Requires PipeWire ≥ 1.4 and, for the window, GTK 3.

```sh
# Arch:   sudo pacman -S pipewire gtk3 pkgconf make gcc
# Debian: sudo apt install libpipewire-0.3-dev libspa-0.2-dev libgtk-3-dev pkg-config make gcc
make
sudo make install
systemctl --user restart pipewire pipewire-pulse wireplumber
```

This installs both parts. To install only one: `make -C module && sudo make -C module install`, or the same with `app`.

Uninstall: `sudo make uninstall`, then restart the same services.

Check it's running:

```sh
journalctl --user -u pipewire | grep loudness-eq   # "applying to stream ... (<app>)"
```

## Configuration

Edit `/usr/share/pipewire/pipewire.conf.d/60-loudness-eq.conf`. For a per-user override, copy it to `~/.config/pipewire/pipewire.conf.d/` under the same name.

| Arg | Default | |
|---|---|---|
| `enabled` | `true` | Start enabled |
| `release` | `4` | Windows "Release Time", 2 (fast) to 7 (slow). 4 is the Windows default |
| `stream.rules` | | Exclude apps entirely: `[ { matches = [ { application.name = "Discord" } ] actions = { loudness-eq = false } } ]` |

## Turning it on and off

Open **Loudness Equalization** from your application menu, or run `pw-loudness-eq`. It has a global checkbox, the release time and a checkbox for each app playing audio.

The same settings are in a PipeWire metadata object, so scripts can use them too:

```sh
pw-metadata -n loudness-eq 0 enabled false                               # off
pw-metadata -n loudness-eq 0 enabled true                                # on
pw-metadata -n loudness-eq 0 release 6                                   # release time
pw-metadata -n loudness-eq 0 disabled-apps '["Discord"]' Spa:String:JSON  # off for these apps
pw-metadata -n loudness-eq                                               # show settings and per-stream state
```

Changes apply immediately and are saved to `~/.local/state/pw-loudness-eq/state.conf`, which takes precedence over the config file on the next start. Deleting a key (`pw-metadata -n loudness-eq -d 0 enabled`) restores the config value. Per-app switches use `application.name` (or `node.name` when it's missing).

## How it works

- `pw_loudness_eq.so` (LADSPA, installed to `/usr/lib/ladspa`) contains the DSP.
- `libpipewire-module-loudness-eq.so` watches app playback streams and sets `audioconvert.filter-graph.90` on each one, so the plugin runs inside that stream, before its volume and before mixing.

Not processed:
- JACK clients
- streams with more than 8 channels
- Flatpak apps using PipeWire natively (Flatpak apps using PulseAudio are processed)

## The algorithm

A broadband automatic gain control. Despite the name, it doesn't equalize the spectrum.

1. Frame size N = 2^⌈log2(fs/100)⌉ (512 at 44.1/48 kHz), with 50% overlap.
2. Each channel is windowed with `w[n] = 16384·(1 − cos(2π(n+½)/N)) / (N/2)` and run through a real FFT to get the power spectrum.
3. Power is summed into 25 bands with edges at 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500 and 24000 Hz. Channels are summed together.
4. Loudness `L = 1.72 · Σ max(E_b − T_b, 1e-32)^0.25`, with T = `1000, 100, 31.6, 10, 3.16, 2, 1, 1, .316, .316, 1×11, 4, 10, 100, 1000`.
5. Target gain `= min((1300/L)^1.6, 10)`, so at most +20 dB.
6. When the target is lower than the current gain, the gain jumps to it instantly. Otherwise it moves toward it by `w = (N/128)·(44100/fs)·0.1^min(r,3)·0.316^(min(r,7)−min(r,3))` per block. At r = 4 that's a time constant of about 4.6 s.
7. The gain is crossfaded linearly across each half-frame. Latency is N samples.

There's no limiter, so pure tones can go above 0 dBFS (Windows does the same).

## Development

```sh
make check                                         # DSP + plugin tests
module/build/leq-wav [-r release] in.wav out.wav  # process a file offline
```

Layout:
- `module/` PipeWire module and LADSPA DSP plugin (`src/`, `conf/`, `tests/`, `tools/`)
- `app/` control window: GTK3, no libadwaita, talks to the module only through the `loudness-eq` metadata

`make` at the top level builds every component; each folder also builds on its own (`make -C module`).

