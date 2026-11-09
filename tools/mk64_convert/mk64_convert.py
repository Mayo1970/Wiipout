#!/usr/bin/env python3
"""Convert n64decomp MK64 course geometry into Wipeout's PSX asset formats."""
import argparse
import math
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

SCALE = 16.0
ROAD_WIDTH = 3500.0
SPACING = 1744.0
VERTEX = re.compile(r'\{\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}\s*,\s*\{[^}]*\}\s*,\s*\{\s*MACRO_COLOR_FLAG\(([^)]*)\)\s*,\s*([^}]+)\}')
ARRAY = re.compile(r'\bGfx\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};', re.S)
COMMAND = re.compile(r'\b(gsSPVertex|gsSP1Triangle|gsSP2Triangles)\s*\(([^()]*)\)')
POINT = re.compile(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}')


def number(s):
    return int(s.strip(), 0)


def read_source(root, course):
    folder = root / 'courses' / course
    source = (folder / 'course_vertices.inc.c').read_text()
    vertices = []
    for m in VERTEX.finditer(source):
        x, y, z = map(int, m.group(1, 2, 3))
        r, g, b, flag = map(number, m.group(4).split(','))
        vertices.append(((x, y, z), ((r & ~3) | (flag & 3), (g & ~3) | ((flag >> 2) & 3), b)))
    if not vertices:
        raise ValueError('no CourseVtx entries found')
    triangles = []
    for array in ARRAY.finditer((folder / 'course_displaylists.inc.c').read_text()):
        cache = [None] * 32
        for cmd in COMMAND.finditer(array.group(1)):
            args = [number(x) for x in cmd.group(2).split(',')]
            if cmd.group(1) == 'gsSPVertex':
                address, count, dest = args
                if address >> 24 != 4 or address & 15 or dest < 0 or dest + count > len(cache):
                    raise ValueError(f'invalid vertex command: {cmd.group(0)}')
                start = (address & 0xffffff) // 16
                if start + count > len(vertices):
                    raise ValueError(f'vertex command out of range: {cmd.group(0)}')
                cache[dest:dest + count] = range(start, start + count)
            else:
                for i in (0, 4) if cmd.group(1) == 'gsSP2Triangles' else (0,):
                    indices = args[i:i + 3]
                    if len(indices) != 3 or any(n < 0 or n >= len(cache) or cache[n] is None for n in indices):
                        raise ValueError(f'triangle references unloaded vertices: {cmd.group(0)}')
                    triangles.append(tuple(cache[n] for n in indices))
    if not triangles:
        raise ValueError('no display-list triangles found')
    data = (folder / 'course_data.c').read_text()
    match = re.search(r'\bTrackPathPoint\s+d_course_' + re.escape(course) + r'_track_path\s*\[\s*\]\s*=\s*\{(.*?)\};', data, re.S)
    if not match:
        raise ValueError('main track path not found')
    points = []
    for m in POINT.finditer(match.group(1)):
        p = tuple(map(int, m.groups()))
        if p[0] == -32768:
            break
        points.append(p[:3])
    else:
        raise ValueError('track path has no terminator')
    if len(points) < 3:
        raise ValueError('track path has fewer than three points')
    return vertices, triangles, points


def samples(points, scale):
    segments = [(a, b, math.dist(a, b)) for a, b in zip(points, points[1:] + points[:1])]
    total = sum(s[2] for s in segments)
    count = max(3, round(total * scale / SPACING))
    if count * 5 > 32767 or count * 4 > 32767:
        raise ValueError('track exceeds signed 16-bit vertex/face index limits')
    result = []
    j = 0
    elapsed = 0.0
    for i in range(count):
        target = total * i / count
        while j < len(segments) - 1 and elapsed + segments[j][2] <= target:
            elapsed += segments[j][2]
            j += 1
        a, b, length = segments[j]
        t = (target - elapsed) / length if length else 0
        result.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(3)))
    return result


def heights(points, vertices, triangles):
    grid = defaultdict(list)
    cell = 256
    for tri in triangles:
        a, b, c = (vertices[i][0] for i in tri)
        den = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if not den:
            continue
        record = (a, b, c, den)
        for x in range(math.floor(min(a[0], b[0], c[0]) / cell), math.floor(max(a[0], b[0], c[0]) / cell) + 1):
            for z in range(math.floor(min(a[2], b[2], c[2]) / cell), math.floor(max(a[2], b[2], c[2]) / cell) + 1):
                grid[x, z].append(record)
    result = []
    misses = 0
    for x, y, z in points:
        hits = []
        for a, b, c, den in grid[math.floor(x / cell), math.floor(z / cell)]:
            u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / den
            v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / den
            if u >= -1e-7 and v >= -1e-7 and u + v <= 1.0000001:
                hits.append(u * a[1] + v * b[1] + (1 - u - v) * c[1])
        if hits:
            y = min(hits, key=lambda h: abs(h - y))
        else:
            misses += 1
        result.append((x, y, z))
    return result, misses


def normal(a, b, c):
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
    length = math.sqrt(sum(x*x for x in n))
    if not length:
        raise ValueError('degenerate generated track face')
    return tuple(round(x * 4096 / length) for x in n)


def road(points, scale):
    centers = [(x * scale, -y * scale, -z * scale) for x, y, z in points]
    rows = []
    for i, p in enumerate(centers):
        prev, nxt = centers[i-1], centers[(i+1) % len(centers)]
        dx, dz = nxt[0]-prev[0], nxt[2]-prev[2]
        length = math.hypot(dx, dz)
        if length < 1e-6:
            raise ValueError(f'path direction undefined at section {i}')
        side = (dz / length, -dx / length)
        row = []
        for offset, elevation in [(-ROAD_WIDTH/2, -500), (-ROAD_WIDTH/2, 0), (0, 0), (ROAD_WIDTH/2, 0), (ROAD_WIDTH/2, -500)]:
            row.append(tuple(round(v) for v in (p[0]+side[0]*offset, p[1]+elevation, p[2]+side[1]*offset)))
        rows.append(row)
    trv = b''.join(struct.pack('>iiii', *v, 0) for row in rows for v in row)
    trf = bytearray()
    trs = bytearray()
    for i, row in enumerate(rows):
        nxt = rows[(i+1) % len(rows)]
        for j in range(4):
            ids = ((i+1) % len(rows)*5+j+1, (i+1) % len(rows)*5+j, i*5+j, i*5+j+1)
            n = normal(nxt[j+1], nxt[j], row[j])
            trf.extend(struct.pack('>hhhhhhhBBI', *ids, *n, 0, 1 if j in (1, 2) else 0, 0xffffffff))
        section = bytearray(156)
        struct.pack_into('>iiiiii', section, 0, -1, (i-1) % len(rows), (i+1) % len(rows), *map(round, centers[i]))
        struct.pack_into('>h', section, 24, 8)
        struct.pack_into('>hh', section, 142, i*4, 4)
        trs.extend(section)
    return trv, bytes(trf), bytes(trs)


def object_bytes(name, positions, triangles):
    low = [min(v[k] for v in positions) for k in range(3)]
    high = [max(v[k] for v in positions) for k in range(3)]
    origin = [(a+b)//2 for a, b in zip(low, high)]
    if any(max(abs(a-o), abs(b-o)) > 32767 for a, b, o in zip(low, high, origin)):
        raise ValueError('object exceeds signed 16-bit local coordinate range; reduce --scale')
    h = bytearray(144)
    h[:16] = name.encode('ascii')[:15].ljust(16, b'\0')
    struct.pack_into('>h', h, 16, len(positions))
    struct.pack_into('>h', h, 32, len(triangles))
    struct.pack_into('>iii', h, 84, *origin)
    data = bytearray(h)
    for p in positions:
        data.extend(struct.pack('>hhhh', *(p[k]-origin[k] for k in range(3)), 0))
    for indices, colors in triangles:
        data.extend(struct.pack('>hhhhhxx', 5, 0, *indices))
        for color in colors:
            data.extend(bytes((*color, 0)))
    return bytes(data)


def scenery(vertices, triangles, scale):
    coords = [tuple(round(v * scale * s) for v, s in zip(p, (1, -1, -1))) for p, color in vertices]
    groups = defaultdict(list)
    for tri in triangles:
        center = [sum(coords[i][k] for i in tri) // 3 for k in range(3)]
        groups[tuple(v // 12000 for v in center)].append(tri)
    objects = []
    for group in groups.values():
        for start in range(0, len(group), 800):
            subset = group[start:start+800]
            lookup = {}
            positions = []
            primitives = []
            for tri in subset:
                ids = []
                for i in tri:
                    if i not in lookup:
                        lookup[i] = len(positions)
                        positions.append(coords[i])
                    ids.append(lookup[i])
                primitives.append((ids, [vertices[i][1] for i in tri]))
            objects.append(object_bytes(f'mesh{len(objects)}', positions, primitives))
    return b''.join(objects), len(objects)


def cmp_file():
    tim = struct.pack('<II I HHHH', 0x10, 2, 12+32*32*2, 0, 0, 32, 32) + struct.pack('<H', 0x7fff)*1024
    bits = ''.join('1'+format(b, '08b') for b in tim) + '0' + '0'*13
    bits += '0' * (-len(bits) % 8)
    return struct.pack('<II', 1, len(tim)) + bytes(int(bits[i:i+8], 2) for i in range(0, len(bits), 8))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mk64', type=Path, default=Path(__file__).resolve().parents[3].parent.parent / 'mk64')
    parser.add_argument('--course', required=True)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--scale', type=float, default=SCALE)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9_]+', args.course) or not math.isfinite(args.scale) or args.scale <= 0:
        parser.error('course must be a lowercase identifier and scale must be positive and finite')
    vertices, triangles, path = read_source(args.mk64, args.course)
    points, misses = heights(samples(path, args.scale), vertices, triangles)
    trv, trf, trs = road(points, args.scale)
    scene, objects = scenery(vertices, triangles, args.scale)
    skyverts = [(-12000, 0, -12000), (12000, 0, -12000), (12000, 0, 12000), (-12000, 0, 12000), (0, -12000, 0)]
    skytris = [((i, (i+1)%4, 4), [(110, 160, 220), (110, 160, 220), (30, 60, 150)]) for i in range(4)]
    files = {'track.trv': trv, 'track.trf': trf, 'track.trs': trs, 'track.trc': bytes([1])*(len(trf)//20), 'library.ttf': bytes(42), 'library.cmp': cmp_file(), 'scene.prm': scene, 'scene.cmp': cmp_file(), 'sky.prm': object_bytes('sky', skyverts, skytris), 'sky.cmp': cmp_file()}
    out = args.out / 'mk64' / args.course
    out.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (out / name).write_bytes(data)
    print(f'{args.course}: {len(points)} sections, {len(trf)//20} faces, {len(triangles)} scene triangles, {objects} scene objects, {misses} height fallbacks')
    for name, data in files.items():
        print(f'  {name}: {len(data)} bytes')
    print(f'  total: {sum(map(len, files.values()))} bytes')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, struct.error) as exc:
        sys.exit(f'conversion failed: {exc}')


