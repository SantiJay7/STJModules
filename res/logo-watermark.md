# Logo watermark (Membrane, reutilizable para Blank Panel)

- Fuente: `res/Logo.png` (260x258; disco dorado #C99700 + tambor #141414).
- Disco = `<circle>` vectorial; glifo trazado con `potrace` (25 paths).
- Colocación: grupo `translate(-10,248.4) scale(0.423)` → disco Ø110px centrado en (45, 303); el borde inferior pasa los centros de L/R; los laterales los recorta el viewBox (90px).
- Opacidad del grupo: 0.28 (marca de agua; rótulos y jacks van encima).
- Pipeline: `res/Membrane-text.svg` (master editable) → `inkscape --export-text-to-path --export-plain-svg` → `res/Membrane.svg` (final, sin `<text>` porque el renderizador de Rack no lo dibuja).
- Panel 6HP 90x380.
