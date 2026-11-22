# pw-loudness-eq

Windows' Loudness Equalization for PipeWire, ported from `WMALFXGFXDSP.dll`.

## Install

Requires PipeWire ≥ 1.4.

```sh
# Arch:   sudo pacman -S pipewire pkgconf make gcc
# Debian: sudo apt install libpipewire-0.3-dev libspa-0.2-dev pkg-config make gcc
make
sudo make install
systemctl --user restart pipewire pipewire-pulse wireplumber
```

Uninstall: `sudo make uninstall`, then restart the same services.

Check it's running:

```sh
journalctl --user -u pipewire | grep loudness-eq   # "applying to stream ... (<app>)"
```

## Configuration

Edit `/usr/share/pipewire/pipewire.conf.d/60-loudness-eq.conf`. For a per-user override, copy it to `~/.config/pipewire/pipewire.conf.d/` under the same name.

| Arg | Default | |
|---|---|---|
| `release` | `4` | Windows "Release Time", 2 (fast) to 7 (slow). 4 is the Windows default |
| `stream.rules` | | Exclude apps: `[ { matches = [ { application.name = "Discord" } ] actions = { loudness-eq = false } } ]` |

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
make check                                  # DSP + plugin tests
build/leq-wav [-r release] in.wav out.wav   # process a file offline
```
