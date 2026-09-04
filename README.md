# STJ Modules

Collection of modules for VCV Rack 2 by STJ Modules (Santi Jay) — https://github.com/SantiJay7

## Modules

### Vocal Lamma (`VocalLamma`)
Tibetan monk formant vocal synthesizer with stereo ping-pong delay (OO-OH-AH-AY-EE, Rosenberg-Klatt glottal pulse).

### Balatube (`Balatube`)
Physically-modeled percussion voice: MATERIAL sweeps from an ebony balafon / marimba bar (wood) to a closed-pipe PVC tube (odd harmonics 1:3:5…). Six-mode modal resonator, jet flanger, overdrive/crunch, full per-knob CV, stereo external-audio processing bus.

### Wizar Matrix (`wizarkeyboard`)
Keyboard mapper: 46 assignable keys + spacebar, momentary/toggle, fader/knob with height & velocity (0–60s), Morse and Random modes, 3-unit expander, localized overlay.

## Build

```bash
make -j$(nproc)
```

## Install

Copy `plugin.so`, `plugin.json`, `res/` to `~/.local/share/Rack2/plugins-lin-x64/STJModules/`

## License

GPL-3.0-or-later
