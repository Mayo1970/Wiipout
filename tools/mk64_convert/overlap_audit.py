#!/usr/bin/env python3
"""Count coplanar positive-area overlaps between differently rendered triangles."""
import argparse
import math
from collections import defaultdict
from pathlib import Path
from mk64_convert import read_source, dedupe, overlap_offsets

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mk64', type=Path, required=True)
parser.add_argument('courses', nargs='+')
args = parser.parse_args()
for course in args.courses:
    vertices, triangles, _, _ = read_source(args.mk64, course)
    triangles, _ = dedupe(vertices, triangles)
    offsets, groups, _ = overlap_offsets(vertices, triangles)
    planes = defaultdict(list)
    for index, (tri, state) in enumerate(triangles):
        p = [vertices[i][0] for i in tri]
        a = [p[1][i]-p[0][i] for i in range(3)]
        b = [p[2][i]-p[0][i] for i in range(3)]
        n = (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
        div = math.gcd(*n)
        if not div: continue
        n = tuple(x//div for x in n)
        direction = 1 if next(x for x in n if x) > 0 else -1
        if direction < 0: n = tuple(-x for x in n)
        d = sum(n[i]*p[0][i] for i in range(3))
        drop = max(range(3), key=lambda i: abs(n[i]))
        axes = [i for i in range(3) if i != drop]
        q = [tuple(x[i] for i in axes) for x in p]
        variant = (state.get('image'), tuple(sorted(vertices[i][1] for i in tri)))
        planes[n, d].append((q, variant, direction * offsets.get(index, 0)))
    count = 0
    remaining = 0
    for group in planes.values():
        for i, (a, va, da) in enumerate(group):
            for b, vb, db in group[i+1:]:
                if va == vb: continue
                overlap = True
                for poly in (a, b):
                    for j in range(3):
                        u, v = poly[j], poly[(j+1)%3]
                        axis = (u[1]-v[1], v[0]-u[0])
                        pa = [x*axis[0]+y*axis[1] for x,y in a]
                        pb = [x*axis[0]+y*axis[1] for x,y in b]
                        if min(max(pa), max(pb)) <= max(min(pa), min(pb)):
                            overlap = False
                            break
                    if not overlap: break
                count += overlap
                remaining += overlap and da == db
    print(f'{course}: {count} original overlapping pairs, {remaining} at equal depth after offsets', flush=True)
    if remaining: raise SystemExit(1)
