#!/usr/bin/env python3
"""Convert n64decomp MK64 course geometry into Wipeout's PSX asset formats."""
import argparse
import ast
import zlib
import math
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

SCALE = 16.0
ROAD_WIDTH = 3500.0
SPACING = 1744.0
VERTEX = re.compile(r'\{\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}\s*,\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}\s*,\s*\{\s*MACRO_COLOR_FLAG\(([^)]*)\)\s*,\s*([^}]+)\}')
ARRAY = re.compile(r'\bGfx\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};', re.S)
COMMAND = re.compile(r'\b(gsSPVertex|gsSP1Triangle|gsSP2Triangles|gsSPDisplayList|gsDPSetTextureImage|gsDPSetTile|gsDPSetTileSize|gsSPTexture)\s*\(([^()]*)\)')
POINT = re.compile(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}')


def number(s):
    return int(s.strip(), 0)


def read_source(root, course):
    folder = root / 'courses' / course
    source = (folder / 'course_vertices.inc.c').read_text()
    vertices = []
    for m in VERTEX.finditer(source):
        x, y, z = map(int, m.group(1, 2, 3))
        r, g, b, flag = map(number, m.group(6).split(','))
        vertices.append(((x, y, z), ((r & ~3) | (flag & 3), (g & ~3) | ((flag >> 2) & 3), b), tuple(map(int, m.group(4, 5)))))
    if not vertices:
        raise ValueError('no CourseVtx entries found')
    data = (folder / 'course_data.c').read_text()
    source = data + '\n' + (folder / 'course_displaylists.inc.c').read_text()
    arrays = {m.group(1): list(COMMAND.finditer(m.group(2))) for m in ARRAY.finditer(source)}
    referenced = {m.group(2).strip() for cmds in arrays.values() for m in cmds if m.group(1) == 'gsSPDisplayList'}
    triangles = []
    seen = set()
    def walk(name, state, cache, stack):
        if name not in arrays or name in stack:
            return
        stack = stack | {name}
        for cmd in arrays[name]:
            op = cmd.group(1)
            args = [x.strip() for x in cmd.group(2).split(',')]
            if op == 'gsSPDisplayList':
                walk(args[0], state, cache, stack)
            elif op == 'gsDPSetTextureImage':
                state['image'] = args[3]
            elif op == 'gsSPTexture':
                state['scale'] = (number(args[0])/65536, number(args[1])/65536)
                state['enabled'] = args[4] != 'G_OFF'
            elif op == 'gsDPSetTile' and args[4] == 'G_TX_RENDERTILE':
                state['modes'] = (args[9], args[6])
                state['masks'] = tuple(0 if x == 'G_TX_NOMASK' else number(x) for x in (args[10], args[7]))
            elif op == 'gsDPSetTileSize' and args[0] == 'G_TX_RENDERTILE':
                state['tile'] = tuple(number(x)/4 for x in args[1:5])
            elif op == 'gsSPVertex':
                address, count, dest = map(number, args)
                if address >> 24 != 4 or address & 15 or dest < 0 or dest + count > len(cache):
                    raise ValueError(f'invalid vertex command: {cmd.group(0)}')
                start = (address & 0xffffff) // 16
                if start + count > len(vertices):
                    raise ValueError(f'vertex command out of range: {cmd.group(0)}')
                cache[dest:dest + count] = range(start, start + count)
            elif op in ('gsSP1Triangle', 'gsSP2Triangles'):
                nums = list(map(number, args))
                for i in (0, 4) if op == 'gsSP2Triangles' else (0,):
                    indices = nums[i:i + 3]
                    if len(indices) != 3 or any(n < 0 or n >= len(cache) or cache[n] is None for n in indices):
                        raise ValueError(f'triangle references unloaded vertices: {cmd.group(0)}')
                    key = (name, cmd.start(), i)
                    if key not in seen:
                        seen.add(key)
                        triangles.append((tuple(cache[n] for n in indices), dict(state, array=name)))
    packed = {m.group(1) for m in ARRAY.finditer((folder / 'course_displaylists.inc.c').read_text())}
    def reaches_packed(name, stack):
        if name in packed:
            return True
        if name not in arrays or name in stack:
            return False
        return any(reaches_packed(m.group(2).strip(), stack | {name}) for m in arrays[name] if m.group(1) == 'gsSPDisplayList')
    for name in arrays:
        if name not in referenced and reaches_packed(name, set()):
            walk(name, {}, [None]*32, set())
    for name in arrays:
        if name in packed:
            walk(name, {}, [None]*32, set())
    if not triangles:
        raise ValueError('no display-list triangles found')
    table = (root / 'assets/course_metadata/gCoursePathTable.inc.c').read_text()
    prefix = 'd_course_' + course + '_track_path'
    entry = re.search(r'\{\s*' + re.escape(prefix) + r'\s*,([^}]*)\}', table)
    if not entry:
        raise ValueError('course path table entry not found')
    names = [prefix] + re.findall(r'\b' + re.escape(prefix) + r'_\d+\b', entry.group(1))
    paths = []
    for name in names:
        match = re.search(r'\bTrackPathPoint\s+' + re.escape(name) + r'\s*\[\s*\]\s*=\s*\{(.*?)\};', data, re.S)
        if not match:
            raise ValueError(f'track path not found: {name}')
        points = []
        for m in POINT.finditer(match.group(1)):
            point = tuple(map(int, m.groups()))
            if point[0] == -32768:
                break
            points.append(point[:3])
        else:
            raise ValueError(f'track path has no terminator: {name}')
        if len(points) < 3:
            raise ValueError(f'track path has fewer than three points: {name}')
        paths.append(points)
    return vertices, triangles, paths



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
    for tri, state in triangles:
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


def distance_to_segment(p, a, b):
    v = tuple(b[k] - a[k] for k in range(3))
    w = tuple(p[k] - a[k] for k in range(3))
    t = max(0, min(1, sum(x*y for x, y in zip(v, w)) / (sum(x*x for x in v) or 1)))
    return math.sqrt(sum((w[k] - t*v[k])**2 for k in range(3)))


def branches(paths, main, scale):
    if len(paths) == 1:
        return []
    threshold = ROAD_WIDTH / scale / 2
    segments = list(zip(paths[0], paths[0][1:] + paths[0][:1]))
    result = []
    occupied = set()
    intervals = []
    for path_index, path in enumerate(paths[1:], 2):
        distances = [min(distance_to_segment(p, a, b) for a, b in segments) for p in path]
        high = [d > threshold for d in distances]
        low = [d > threshold * 0.6 for d in distances]
        runs = []
        for i in range(len(path)):
            if low[i] and not low[i-1]:
                run = []
                j = i
                while low[j] and len(run) < len(path):
                    run.append(j)
                    j = (j+1) % len(path)
                if any(high[k] for k in run):
                    runs.append(run)
        if all(low):
            print(f'  path {path_index}: skipped: never rejoins main path')
        for run in runs:
            before, after = (run[0]-1) % len(path), (run[-1]+1) % len(path)
            stretch = [path[before]] + [path[k] for k in run] + [path[after]]
            length = sum(math.dist(a, b) for a, b in zip(stretch, stretch[1:]))
            if length * scale < SPACING * 3:
                print(f'  path {path_index}: skipped tiny divergence')
                continue
            start = min(range(len(main)), key=lambda i: math.dist(main[i], stretch[0]))
            end = min(range(len(main)), key=lambda i: math.dist(main[i], stretch[-1]))
            span = (end-start) % len(main)
            if span < 3 or span > len(main)//2 or start == 0 or end == 0:
                print(f'  path {path_index}: skipped divergence {start}->{end}: unsupported main-loop span')
                continue
            if any(max(max(min(distance_to_segment(p, a, b) for a, b in zip(old, old[1:])) for p in stretch),
                       max(min(distance_to_segment(p, a, b) for a, b in zip(stretch, stretch[1:])) for p in old)) < threshold * 0.6 for _, _, old in result):
                print(f'  path {path_index}: skipped duplicate divergence {start}->{end}')
                continue
            used = {(start-1) % len(main), start, (end-1) % len(main), end}
            interval = {(start+i) % len(main) for i in range(span+1)}
            if used & occupied or any(interval & old_interval for old_interval in intervals):
                print(f'  path {path_index}: skipped divergence {start}->{end}: overlapping or nested branch')
                continue
            result.append((start, end, stretch))
            occupied |= used
            intervals.append(interval)
    return result


def open_samples(points, scale):
    segments = [(a, b, math.dist(a, b)) for a, b in zip(points, points[1:])]
    total = sum(s[2] for s in segments)
    count = max(3, round(total * scale / SPACING))
    result = []
    j = 0
    elapsed = 0
    for i in range(1, count):
        target = total * i / count
        while j < len(segments)-1 and elapsed + segments[j][2] <= target:
            elapsed += segments[j][2]
            j += 1
        a, b, length = segments[j]
        t = (target-elapsed)/length if length else 0
        result.append(tuple(a[k] + (b[k]-a[k])*t for k in range(3)))
    return result


def road(points, scale, branch_points=()):
    centers = [(x * scale, -y * scale, -z * scale) for x, y, z in points]
    main_count = len(centers)
    links = [[-1, (i-1) % main_count, (i+1) % main_count, 0] for i in range(main_count)]
    for start, end, branch in branch_points:
        first = len(centers)
        centers.extend((x * scale, -y * scale, -z * scale) for x, y, z in branch)
        last = len(centers)-1
        links.extend([[-1, start if i == first else i-1, end if i == last else i+1,
                       32 | (16 if i == first else 0) | (8 if i == last else 0)] for i in range(first, last+1)])
        for i in (start-1, start): links[i][0] = first
        for i in (end-1, end): links[i][0] = last
    if len(centers) * 5 > 32767 or len(centers) * 4 > 32767:
        raise ValueError('track exceeds signed 16-bit vertex/face index limits')
    rows = []
    for i, p in enumerate(centers):
        prev, nxt = centers[links[i][1]], centers[links[i][2]]
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
        next_index = links[i][2]
        nxt = rows[next_index]
        for j in range(4):
            ids = (next_index*5+j+1, next_index*5+j, i*5+j, i*5+j+1)
            n = normal(nxt[j+1], nxt[j], row[j])
            trf.extend(struct.pack('>hhhhhhhBBI', *ids, *n, 0, 1 if j in (1, 2) else 0, 0xffffffff))
        section = bytearray(156)
        struct.pack_into('>iiiiii', section, 0, *links[i][:3], *map(round, centers[i]))
        struct.pack_into('>h', section, 24, 8)
        struct.pack_into('>hh', section, 142, i*4, 4)
        struct.pack_into('>h', section, 150, links[i][3])
        trs.extend(section)
    return trv, bytes(trf), bytes(trs)


def png(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'bad PNG: {path}')
    pos, packed, palette, alpha = 8, bytearray(), b'', b''
    while pos < len(data):
        n, = struct.unpack_from('>I', data, pos)
        kind, chunk = data[pos+4:pos+8], data[pos+8:pos+8+n]
        pos += n+12
        if kind == b'IHDR':
            w, h, depth, color, comp, filt, interlace = struct.unpack('>IIBBBBB', chunk)
        elif kind == b'IDAT': packed.extend(chunk)
        elif kind == b'PLTE': palette = chunk
        elif kind == b'tRNS': alpha = chunk
        elif kind == b'IEND': break
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color)
    if depth != 8 or not channels or interlace or comp or filt:
        raise ValueError(f'unsupported PNG: {path}')
    raw = zlib.decompress(packed)
    stride = w*channels
    rows, previous = [], bytearray(stride)
    for y in range(h):
        f = raw[y*(stride+1)]
        row = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        for i in range(stride):
            left = row[i-channels] if i >= channels else 0
            up = previous[i]
            ul = previous[i-channels] if i >= channels else 0
            if f == 1: pred = left
            elif f == 2: pred = up
            elif f == 3: pred = (left+up)//2
            elif f == 4:
                q = left+up-ul
                pred = min((left, up, ul), key=lambda v: abs(q-v))
            elif f == 0: pred = 0
            else: raise ValueError(f'bad PNG filter: {path}')
            row[i] = (row[i]+pred) & 255
        rows.append(row)
        previous = row
    pixels = []
    for row in rows:
        for i in range(0, stride, channels):
            p = row[i:i+channels]
            if color == 6: rgba = tuple(p)
            elif color == 2:
                transparent = len(alpha) == 6 and tuple(p) == struct.unpack('>HHH', alpha)
                rgba = (*p, 0 if transparent else 255)
            elif color == 0:
                transparent = len(alpha) == 2 and p[0] == struct.unpack('>H', alpha)[0]
                rgba = (p[0], p[0], p[0], 0 if transparent else 255)
            elif color == 4: rgba = (p[0], p[0], p[0], p[1])
            else:
                j = p[0]
                rgba = (*palette[j*3:j*3+3], alpha[j] if j < len(alpha) else 255)
            pixels.append(rgba)
    return w, h, pixels


def texture_map(root, course):
    tree = ast.parse((root/'tools/linkonly_generator.py').read_text())
    tables = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name) and node.targets[0].id in ('texture_map', 'abbreviations'):
            tables[node.targets[0].id] = ast.literal_eval(node.value)
    names = re.findall(r'\{\s*(gTexture\w+)\s*,', (root/'courses'/course/'course_offsets.c').read_text())
    result = {}
    for name in names:
        entry = tables['texture_map'].get(name)
        if 'MooMooFarm' in name:
            path = root/'assets/courses/moo_moo_farm'/(name+'.png')
        elif entry:
            path = root/'textures/standalone'/('.'.join(entry)+'.png')
        else:
            continue
        result['g'+tables['abbreviations'][course]+name[1:]] = path
    return result


def tim_image(w, h, pixels):
    if not w or not h or w & (w-1) or h & (h-1):
        raise ValueError(f'texture dimensions must be powers of two: {w}x{h}')
    if w > 256 or h > 256:
        raise ValueError(f'texture too large for PRM UVs: {w}x{h}')
    words = []
    for r, g, b, a in pixels:
        c = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
        words.append(0 if a < 128 else (c or 0x8000))
    return struct.pack('<IIIHHHH', 16, 2, 12+w*h*2, 0, 0, w, h) + struct.pack('<'+'H'*len(words), *words)


def cmp_file(tims=None):
    if tims is None:
        tims = [tim_image(32, 32, [(255,255,255,255)]*1024)]
    data = b''.join(tims)
    bits = ''.join('1'+format(b, '08b') for b in data) + '0'*14
    bits += '0' * (-len(bits) % 8)
    return struct.pack('<I', len(tims)) + b''.join(struct.pack('<I', len(t)) for t in tims) + bytes(int(bits[i:i+8], 2) for i in range(0, len(bits), 8))


def clip(poly, axis, edge, keep_above):
    out = []
    for a, b in zip(poly, poly[1:]+poly[:1]):
        da, db = a[axis]-edge, b[axis]-edge
        ina, inb = (da >= 0 if keep_above else da <= 0), (db >= 0 if keep_above else db <= 0)
        if ina: out.append(a)
        if ina != inb:
            t = da/(da-db)
            out.append(tuple(x+(y-x)*t for x, y in zip(a,b)))
    return out


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
    for ids, colors, *texture in triangles:
        data.extend(struct.pack('>hhhhh', 6 if texture else 5, 0, *ids))
        if texture:
            tex, uvs = texture[0]
            data.extend(struct.pack('>hhh', tex, 0, 0))
            data.extend(bytes(v for uv in uvs for v in uv))
        data.extend(b'\0\0')
        for color in colors: data.extend(bytes((*color, 0)))
    return bytes(data)


def scenery(vertices, triangles, scale, mapping, no_textures):
    tims, paths, dims, missing = [], {}, {}, set()
    groups = defaultdict(list)
    capped = 0
    untextured = defaultdict(int)
    untextured_arrays = defaultdict(int)
    for tri, state in triangles:
        symbol = state.get('image')
        path = mapping.get(symbol)
        if symbol and (not path or not path.exists()): missing.add(symbol)
        tex = None
        reason = ('textures disabled by option' if no_textures else
                  'no SetTextureImage seen' if not symbol else
                  'texture disabled' if not state.get('enabled', True) else
                  'symbol unmapped' if not path or not path.exists() else None)
        if not no_textures and path and path.exists() and state.get('enabled', True):
            modes = state.get('modes', ('G_TX_WRAP', 'G_TX_WRAP'))
            mirror = tuple('G_TX_MIRROR' in mode and 'G_TX_CLAMP' not in mode for mode in modes)
            key = (path, *mirror)
            if key not in paths:
                w, h, pixels = png(path)
                nw, nh = w * (1 + mirror[0]), h * (1 + mirror[1])
                pixels = [pixels[(y if y < h else 2*h-1-y)*w + (x if x < w else 2*w-1-x)] for y in range(nh) for x in range(nw)]
                paths[key] = len(tims)
                dims[key] = (nw, nh)
                tims.append(tim_image(nw, nh, pixels))
            tex = paths[key]
        coords = [tuple(v*scale*s for v, s in zip(vertices[i][0], (1,-1,-1))) for i in tri]
        colors = [vertices[i][1] for i in tri]
        pieces = []
        if tex is not None:
            w, h = dims[key]
            modes = state.get('modes', ('G_TX_WRAP', 'G_TX_WRAP'))
            tile = state.get('tile', (0,0,w-1,h-1))
            periods = [(255 // size) * size if size < 256 else 256 for size in (w,h)]
            scales = state.get('scale', (1,1))
            poly = [(*p, *c, *(vertices[i][2][j]/32*scales[j]-tile[j] for j in range(2))) for i,p,c in zip(tri, coords, colors)]
            ranges = []
            origins = [math.floor(min(v[6+j] for v in poly) / (w,h)[j]) * (w,h)[j] for j in range(2)]
            for j in range(2):
                if 'G_TX_CLAMP' in modes[j]: ranges.append([-1, 0, 1])
                else:
                    lo = math.floor((min(v[6+j] for v in poly)-origins[j])/periods[j])
                    hi = math.ceil((max(v[6+j] for v in poly)-origins[j])/periods[j])-1
                    ranges.append(range(lo, max(lo,hi)+1))
            if len(ranges[0])*len(ranges[1]) > 256:
                capped += 1
                reason = 'capped'
                tex = None
            else:
                for u in ranges[0]:
                    for v in ranges[1]:
                        clipped = poly
                        for j, cell in enumerate((u,v)):
                            if 'G_TX_CLAMP' in modes[j]:
                                limit = min((w,h)[j]-1, tile[j+2]-tile[j])
                                if cell == -1: clipped = clip(clipped, 6+j, 0, False)
                                elif cell == 0:
                                    clipped = clip(clipped, 6+j, 0, True)
                                    if clipped: clipped = clip(clipped, 6+j, limit, False)
                                else: clipped = clip(clipped, 6+j, limit, True)
                            else:
                                clipped = clip(clipped, 6+j, origins[j]+cell*periods[j], True)
                                if clipped: clipped = clip(clipped, 6+j, origins[j]+(cell+1)*periods[j], False)
                            if not clipped: break
                        for k in range(1,len(clipped)-1):
                            pp = [clipped[0],clipped[k],clipped[k+1]]
                            uv = []
                            for vertex in pp:
                                values = []
                                for j, cell in enumerate((u,v)):
                                    x = vertex[6+j]
                                    if 'G_TX_CLAMP' not in modes[j]:
                                        x -= origins[j]+cell*periods[j]
                                    limit = min((w,h)[j]-1, tile[j+2]-tile[j]) if 'G_TX_CLAMP' in modes[j] else 255
                                    values.append(max(0,min(int(limit),round(x))))
                                uv.append(tuple(values))
                            pieces.append((pp, uv))
        if tex is None:
            pieces = [([(*p,*c) for p,c in zip(coords,colors)], None)]
        for pp, uv in pieces:
            pos = [tuple(round(x) for x in p[:3]) for p in pp]
            if len(set(pos)) < 3: continue
            ab = [pos[1][k]-pos[0][k] for k in range(3)]
            ac = [pos[2][k]-pos[0][k] for k in range(3)]
            if all(ab[(k+1)%3]*ac[(k+2)%3] == ab[(k+2)%3]*ac[(k+1)%3] for k in range(3)): continue
            # The renderer doubles texture modulation; untextured faces use a half-white texture.
            divisor = 2 if tex is not None else 1
            cols = [tuple(max(0,min(255,round(x/divisor))) for x in p[3:6]) for p in pp]
            center = tuple(sum(p[k] for p in pos)//3//12000 for k in range(3))
            groups[center].append((pos,cols,tex,uv))
            if tex is None:
                untextured[reason] += 1
                untextured_arrays[state['array']] += 1
    objects = []
    count = 0
    for group in groups.values():
        for start in range(0,len(group),800):
            positions, primitives, lookup = [], [], {}
            for pos, colors, tex, uv in group[start:start+800]:
                ids = []
                for p in pos:
                    if p not in lookup:
                        lookup[p] = len(positions)
                        positions.append(p)
                    ids.append(lookup[p])
                primitives.append((ids, colors) if tex is None else (ids, colors, (tex, uv)))
            objects.append(object_bytes(f'mesh{len(objects)}', positions, primitives))
            count += len(primitives)
    return b''.join(objects), len(objects), tims, count, capped, sorted(missing), untextured, untextured_arrays

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mk64', type=Path, default=Path(__file__).resolve().parents[3].parent.parent / 'mk64')
    parser.add_argument('--course', required=True)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--no-textures', action='store_true')
    parser.add_argument('--scale', type=float, default=SCALE)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9_]+', args.course) or not math.isfinite(args.scale) or args.scale <= 0:
        parser.error('course must be a lowercase identifier and scale must be positive and finite')
    vertices, triangles, paths = read_source(args.mk64, args.course)
    points, misses = heights(samples(paths[0], args.scale), vertices, triangles)
    branch_points = []
    for start, end, stretch in branches(paths, points, args.scale):
        branch, fallback = heights(open_samples([points[start]] + stretch + [points[end]], args.scale), vertices, triangles)
        misses += fallback
        branch_points.append((start, end, branch))
        print(f'  branch {start}->{end}: {len(branch)} sections')
    trv, trf, trs = road(points, args.scale, branch_points)
    scene, objects, tims, after, capped, missing, untextured, untextured_arrays = scenery(vertices, triangles, args.scale, texture_map(args.mk64, args.course), args.no_textures)
    skyverts = [(-12000, 0, -12000), (12000, 0, -12000), (12000, 0, 12000), (-12000, 0, 12000), (0, -12000, 0)]
    skytris = [((i, (i+1)%4, 4), [(110, 160, 220), (110, 160, 220), (30, 60, 150)]) for i in range(4)]
    files = {'track.trv': trv, 'track.trf': trf, 'track.trs': trs, 'track.trc': bytes([1])*(len(trf)//20), 'library.ttf': bytes(42), 'library.cmp': cmp_file(), 'scene.prm': scene, 'scene.cmp': cmp_file(tims if tims else None), 'sky.prm': object_bytes('sky', skyverts, skytris), 'sky.cmp': cmp_file()}
    out = args.out / 'mk64' / args.course
    out.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (out / name).write_bytes(data)
    print(f'{args.course}: {len(trs)//156} sections, {len(trf)//20} faces, {len(triangles)} scene triangles, {objects} scene objects, {misses} height fallbacks')
    print(f'  clipping: {len(triangles)} -> {after} triangles, {capped} capped, {len(tims)} textures')
    print(f'  untextured: {sum(untextured.values())} (' + ', '.join(f'{reason}: {untextured[reason]}' for reason in ('no SetTextureImage seen', 'texture disabled', 'symbol unmapped', 'capped', 'textures disabled by option') if untextured[reason]) + ')')
    print('  untextured arrays: ' + (', '.join(f'{name}: {count}' for name, count in sorted(untextured_arrays.items(), key=lambda item: (-item[1], item[0]))[:5]) or 'none'))
    print('  unmapped: ' + (', '.join(missing) if missing else 'none'))
    for name, data in files.items():
        print(f'  {name}: {len(data)} bytes')
    print(f'  total: {sum(map(len, files.values()))} bytes')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, struct.error) as exc:
        sys.exit(f'conversion failed: {exc}')



