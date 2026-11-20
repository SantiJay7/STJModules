# STJ

Collection of modules for VCV Rack 2 by STJ (Santi Jay) — https://github.com/SantiJay7

## Modules

### Balatube (`Balatube`)
Physically-modeled percussion voice: MATERIAL sweeps from balafon / marimba bar (wood) to a closed-pipe PVC tube (odd harmonics 1:3:5…). Six-mode modal resonator, jet flanger, overdrive/crunch, full per-knob CV, stereo external-audio processing bus.

### Wizar Matrix (`wizarkeyboard`)
Keyboard mapper: 46 assignable keys, momentary/toggle, fader/knob with height & velocity (0–60s), Morse and Random modes, 3-unit expander, localized overlay.

### Membrane (`Membrane`)
Morphing modal drum: MATERIAL slides from a double-membrane taiko through a goblet djembe to a plastic darbuka. 3 voices with a per-channel rotor, stereo out, Exciter for the attack and Harmonics for saturation, full per-knob CV.

## Build

```bash
make -j$(nproc)
```

## Install

Copy `plugin.so`, `plugin.json`, `res/` to `~/.local/share/Rack2/plugins-lin-x64/STJ/`

## License

GPL-3.0-or-later
