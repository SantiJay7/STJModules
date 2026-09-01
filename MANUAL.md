# Wizar Matrix — User Manual

Wizar Matrix is a VCV Rack 2 module that maps computer‑keyboard keys to
module parameters. One key can drive a button (momentary or toggle) or a
fader/knob (with adjustable height and velocity), and a single key can even
control **several controls at once** with the same settings.

> Brand: STJ Modules · License: GPL‑3.0‑or‑later

---

## 1. Installation

1. Quit VCV Rack.
2. Copy the `wizarkeyboard` folder (contains `plugin.so`, `plugin.json` and
   `res/`) into your Rack plugins directory, e.g.:
   - Linux: `~/.local/share/Rack2/plugins-lin-x64/`
   - macOS: `Documents/Rack2/plugins/`
   - Windows: `Documents/Rack2/plugins/`
3. Start VCV Rack. "Wizar Matrix" appears under the "STJ Modules" brand.

The module is **not** in the VCV Library yet; it is shared as a development build.

---

## 2. Module layout

- **CONTROL switch** (top‑left, green light): toggles exclusive keyboard mode.
- **Keys**: a grid of assignable keys (up to 46 across up to 3 units).
- **Extensions**: drag the right edge to expand the module into more units
  (labeled EXT 2, EXT 3). More units = more keys.
- **Logo**: shown under the keys.
- Mapped keys are drawn in **red**; unmapped keys in **blue**.

---

## 3. Quick start — map a key

1. Make sure **CONTROL is OFF** (the green light is dark). While CONTROL is ON,
   Wizar Matrix captures the keyboard and you cannot use Rack's shortcuts
   (including `Ctrl+D`).
2. **Right‑click** a key → choose **Button** or **Fader/Knob** → **Map…**
3. The screen dims and shows the mapping overlay. **Click the control** you
   want to drive (a knob, slider, switch, etc.).
4. The mapping is saved. Turn **CONTROL ON** to play.

To remove a mapping: right‑click the key → **Unmap**.

---

## 4. CONTROL mode (exclusive capture)

When CONTROL is ON, Wizar Matrix takes over the computer keyboard so keypresses
are sent to your mappings instead of Rack:

- All keys except **F1–F12** and **Space** are captured (they will not trigger
  Rack or other modules).
- **Escape** releases CONTROL immediately (handy if you get stuck).
- Only **one** Wizar Matrix instance may use CONTROL at a time (see §8).

Turn CONTROL OFF whenever you need the normal keyboard (e.g. to duplicate the
module, type in fields, or use Rack shortcuts).

---

## 5. Button vs Fader/Knob

- **Button → Momentary**: key down = maximum, key up = minimum.
- **Button → Toggle**: each press flips between minimum and maximum.
- **Fader/Knob**: the key ramps the parameter toward a target.
  - **Height minimum / maximum** set the travel range (0–100%).
  - **Velocity** sets how fast it moves (0.02 s – 1 min).
  - **Fader (reversible)**: each press reverses the direction.

---

## 6. Morse mode

In a Fader/Knob key's menu, enable **Morse** (`~` mark in the submenu).
Short press = +1%, long press = −1%, with a global short/long threshold
(adjustable in the module menu, 50–1500 ms). Height/Velocity sliders are
disabled in Morse mode.

---

## 7. One key → several controls

You can make a single key drive **multiple controls with the same settings**:

1. Open the mapping overlay for a key (right‑click → Map…).
2. **Click** the first control (mapping is created and the overlay closes).
   *or*
3. Hold **Shift** and **click** more controls to add them to the same key; the
   overlay stays open and shows *"N parameters mapped"*.
4. Press **Enter** (or click without Shift) to finish, **Esc** / right‑click to
   cancel.

All targets share the key's settings (button/fader type, height, velocity…).

---

## 8. Copy / Paste settings

Right‑click a key → **Copy settings**, then right‑click another key →
**Paste settings**. This copies the *behaviour* (button/fader type, height,
velocity, Morse) — **not** the mapping itself — so each key keeps its own
control(s). Paste is disabled until something has been copied.

---

## 9. Collapse

Drag the right edge far left to collapse the module to a slim strip (just the
CONTROL switch + a line). Drag right again to expand. You can also collapse/
expand from the module's right‑click menu. The module keeps working while
collapsed.

---

## 10. Multiple instances (singleton)

Wizar Matrix installs one global keyboard hook, so **only one instance may use
CONTROL at a time**. If you end up with two Wizar Matrix modules:

- The instance with the **lowest id** keeps CONTROL; the other is a duplicate.
- A duplicate is marked with an **orange border**. Click it (any key or its
  right edge) and it removes itself automatically.
- If you want two separate modules, give only one of them CONTROL ON.

---

## 11. Tips & troubleshooting

- Mapped keys turn **red**; the small badge on each controlled parameter shows
  the driving key letter.
- "Height minimum" cannot go below 0% (a negative value would make the fader
  slower than intended).
- If keys do nothing, check the CONTROL light is ON and the key is mapped.
- If Rack feels "frozen", press **Escape** to release CONTROL.
- Mappings are saved inside the patch (`.vcv`); presets can be saved via the
  module menu.

---

Enjoy Wizar Matrix!
