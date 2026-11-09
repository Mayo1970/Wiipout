#!/usr/bin/env python3
"""Estimate Luigi texture modulation through the N64 and GX color paths."""
import argparse
from pathlib import Path
from mk64_convert import read_source, dedupe, texture_map, png

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mk64', type=Path, required=True)
args = parser.parse_args()
vertices, triangles, _, _ = read_source(args.mk64, 'luigi_raceway')
triangles, _ = dedupe(vertices, triangles)
mapping = texture_map(args.mk64, 'luigi_raceway')
for label, name in [('road', 'gLRTextureRoad2'), ('sand', 'gLRTexture67BBD8'),
                    ('grass', 'gLRTextureGrass11'), ('checker', 'gLRTextureCheckerboardBlueGreen')]:
    _, _, pixels = png(mapping[name])
    pixels = [p for p in pixels if p[3] >= 128]
    shades = [vertices[i][1] for tri, state in triangles if state.get('image') == name for i in tri]
    tex = [sum(p[c] for p in pixels)/len(pixels) for c in range(3)]
    shade = [sum(p[c] for p in shades)/len(shades) for c in range(3)]
    gxtex = [sum(((p[c] >> 3) << 3) | (p[c] >> (6 if c == 1 else 5)) for p in pixels)/len(pixels) for c in range(3)]
    gxshade = [sum(round(p[c]/2) for p in shades)/len(shades) for c in range(3)]
    expected = [tex[c]*shade[c]/255 for c in range(3)]
    actual = [min(255, gxtex[c]*gxshade[c]*2/255) for c in range(3)]
    fmt = lambda v: '(' + ', '.join(f'{x:.1f}' for x in v) + ')'
    print(f'{label}: tex {fmt(tex)}, shade {fmt(shade)}, N64 {fmt(expected)}, GX {fmt(actual)}')
print('Untextured GX fallback RGB565 expands (128,128,128) to (132,130,132); shade gains 3.5%, 2.0%, 3.5% before converter compensation.')
