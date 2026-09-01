#!/usr/bin/env python3
"""Panel base de WizarKeyboard (1 unidad, 12HP). Sin letras de teclas:
los caracteres los dibuja el modulo segun la distribucion elegida."""

W, H = 180, 380          # 12 HP x altura estandar
KEY_X0, KEY_Y0 = 10, 68  # origen de la matriz
KW, KH = 37, 58
PX, PY = 41, 70          # paso entre teclas

svg = []
svg.append('<?xml version="1.0" encoding="UTF-8"?>')
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
svg.append('  <defs>')
svg.append('    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">')
svg.append('      <stop offset="0" stop-color="#1a1a24"/><stop offset="1" stop-color="#0d0d12"/>')
svg.append('    </linearGradient>')
svg.append('    <linearGradient id="keyBlue" x1="0" y1="0" x2="0" y2="1">')
svg.append('      <stop offset="0" stop-color="#4a90e2"/><stop offset="0.3" stop-color="#2d5a9e"/><stop offset="1" stop-color="#1a3a7e"/>')
svg.append('    </linearGradient>')
svg.append('  </defs>')

svg.append(f'  <rect x="0" y="0" width="{W}" height="{H}" fill="url(#bg)"/>')
svg.append(f'  <rect x="4" y="4" width="{W-8}" height="{H-8}" rx="6" fill="none" stroke="#3a3a4a" stroke-width="1.5"/>')

# Titulo
svg.append(f'  <text x="{W//2}" y="17" text-anchor="middle" font-family="Arial" font-size="10" letter-spacing="1.5" fill="#e0e0e0">WIZAR KEYBOARD</text>')

# Zona boton de modo control (arriba izquierda; el widget real va encima)
svg.append('  <circle cx="26" cy="38" r="11" fill="#22222b" stroke="#55555f" stroke-width="1.5"/>')
svg.append('  <text x="26" y="62" text-anchor="middle" font-family="Arial" font-size="7" fill="#8888a0">CONTROL</text>')

# Indicador de distribucion (texto lo pinta el modulo)
svg.append(f'  <text x="{W-12}" y="60" text-anchor="end" font-family="Arial" font-size="7" fill="#8888a0" id="layout-label"></text>')

# Teclas vacias (los caracteres los dibuja el modulo)
for r in range(4):
    for c in range(4):
        slot = r * 4 + c
        # Unidad 1 completa (16 teclas); la unidad 3 tiene huecos abajo-derecha,
        # pero este SVG es solo la unidad base.
        x = KEY_X0 + c * PX
        y = KEY_Y0 + r * PY
        svg.append(f'  <rect x="{x}" y="{y}" width="{KW}" height="{KH}" rx="3" fill="url(#keyBlue)" stroke="#152c54" stroke-width="0.75"/>')

# Agarre de expansion (borde derecho)
gx = W - 7
for i in range(6):
    gy = 140 + i * 20
    svg.append(f'  <circle cx="{gx}" cy="{gy}" r="1.8" fill="#55555f"/>')
svg.append(f'  <path d="M {gx-3} 130 l 5 6 l -5 6" fill="none" stroke="#55555f" stroke-width="1.5"/>')
svg.append(f'  <path d="M {gx-3} 250 l 5 6 l -5 6" fill="none" stroke="#55555f" stroke-width="1.5"/>')

svg.append('</svg>')

with open("res/WizarKeyboard.svg", "w") as f:
    f.write("\n".join(svg) + "\n")
print("Panel base escrito:", W, "x", H)
