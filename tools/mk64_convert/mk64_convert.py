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
OVERLAP_OFFSET = 8.0
CROSS_SLOPE_LIMIT = math.tan(math.radians(20))
PLANE_SAMPLE_OFFSET = 700.0
PLANE_TOLERANCE = 120.0
CEILING_HEIGHT = 1200.0
SLOPE_LOOKAHEAD = 64
VERTEX = re.compile(r'\{\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}\s*,\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}\s*,\s*\{\s*MACRO_COLOR_FLAG\(([^)]*)\)\s*,\s*([^}]+)\}')
ARRAY = re.compile(r'\bGfx\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};', re.S)
COMMAND = re.compile(r'\b(gsSPVertex|gsSP1Triangle|gsSP2Triangles|gsSPDisplayList|gsDPSetTextureImage|gsDPSetTile|gsDPSetTileSize|gsSPTexture|gsDPSetCombineMode)\s*\(([^()]*)\)')
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
        vertices.append(((x, y, z), (r & ~3, g & ~3, b), tuple(map(int, m.group(4, 5)))))
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
            elif op == 'gsDPSetCombineMode':
                state['combine'] = args[0]
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
    scene_triangles = triangles
    triangles, seen = [], set()
    collision_table = re.search(r'TrackSections\s+d_course_' + re.escape(course) + r'_(?:addr|track_sections)\s*\[\s*\]\s*=\s*\{(.*?)\};', data, re.S)
    if not collision_table:
        raise ValueError('collision display-list table not found')
    for name, surface in re.findall(r'\{\s*(\w+)\s*,\s*(\w+)\s*,\s*[^,]+,\s*[^}]+\}', collision_table.group(1)):
        if name in ('0', '0x00000000', 'NULL'):
            break
        if name not in arrays:
            raise ValueError(f'collision display list not found: {name}')
        seen = set()
        walk(name, {'surface': surface}, [None]*32, set())
    collision_triangles = triangles
    triangles = scene_triangles
    if not collision_triangles:
        raise ValueError('no collision triangles found')
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
    match = re.search(r'struct ActorSpawnData\s+d_course_' + re.escape(course) + r'_item_box_spawns\s*\[\s*\]\s*=\s*\{(.*?)\};', data, re.S)
    if not match:
        raise ValueError('item box spawns not found')
    boxes = []
    for m in re.finditer(r'\{\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}\s*,\s*\{\s*(-?\d+)\s*\}\s*\}', match.group(1)):
        x, y, z, group = map(int, m.groups())
        if x == -32768:
            break
        boxes.append((x, y, z, group))
    else:
        raise ValueError('item boxes have no terminator')
    return vertices, triangles, collision_triangles, paths, boxes


def dedupe(vertices, triangles):
    groups = defaultdict(lambda: defaultdict(list))
    for tri, state in triangles:
        positions = tuple(vertices[i][0] for i in tri)
        rotations = [positions[i:] + positions[:i] for i in range(3)]
        shift = rotations.index(min(rotations))
        tri = tri[shift:] + tri[:shift]
        variant = (tuple((vertices[i][1], vertices[i][2]) for i in tri),
                   tuple(sorted((k, str(v)) for k, v in state.items() if k != 'array')))
        groups[min(rotations)][variant].append((tri, state))
    result = []
    for variants in groups.values():
        def rank(items):
            tri = items[0][0]
            shade = sum(sum(c*w for c, w in zip(vertices[i][1], (2126, 7152, 722))) for i in tri)
            return (-len({state['array'] for _, state in items}), shade)
        items = min(variants.values(), key=rank)
        tri, state = items[0]
        result.append((tri, dict(state, source_arrays=frozenset(s['array'] for _, s in items))))
    return result, len(triangles)-len(result)



def overlap_offsets(vertices, triangles):
    planes = defaultdict(list)
    for index, (tri, state) in enumerate(triangles):
        p = [vertices[i][0] for i in tri]
        a = [p[1][i]-p[0][i] for i in range(3)]
        b = [p[2][i]-p[0][i] for i in range(3)]
        n = (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
        div = math.gcd(*n)
        if not div: continue
        n = tuple(x//div for x in n)
        if next(x for x in n if x) < 0: n = tuple(-x for x in n)
        d = sum(n[i]*p[0][i] for i in range(3))
        drop = max(range(3), key=lambda i: abs(n[i]))
        axes = [i for i in range(3) if i != drop]
        q = [tuple(x[i] for i in axes) for x in p]
        variant = (state.get('image'), tuple(sorted(vertices[i][1] for i in tri)))
        planes[n, d].append((index, q, variant))
    parent = list(range(len(triangles)))
    edges = []
    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for plane in planes.values():
        for i, (ia, a, va) in enumerate(plane):
            for ib, b, vb in plane[i+1:]:
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
                if overlap:
                    edges.append((ia, ib))
                    parent[root(ia)] = root(ib)
    groups = defaultdict(set)
    for a, b in edges:
        groups[root(a)].update((a, b))
    offsets = {}
    for group in groups.values():
        variants = defaultdict(list)
        for i in group:
            tri, state = triangles[i]
            key = (state.get('image'), tuple(sorted(vertices[j][1] for j in tri)))
            variants[key].append(i)
        def rank(item):
            key, indices = item
            shade = sum(sum(c*w for c, w in zip(color, (2126, 7152, 722))) for color in key[1])
            arrays = set().union(*(triangles[i][1]['source_arrays'] for i in indices))
            return (shade, -len(arrays), repr(key))
        for level, (_, indices) in enumerate(sorted(variants.items(), key=rank)):
            if level:
                offsets.update((i, level * OVERLAP_OFFSET) for i in indices)
    return offsets, len(groups), edges


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


class CollisionMesh:
    def __init__(self, vertices, triangles, scenery):
        self.floors, self.walls = [], []
        self.grid = defaultdict(list)
        self.overhead = defaultdict(list)
        for indices, _ in scenery + triangles:
            p = tuple(vertices[i][0] for i in indices)
            a, b, c = p
            den = (b[2]-c[2])*(a[0]-c[0]) + (c[0]-b[0])*(a[2]-c[2])
            if not den:
                continue
            for x in range(math.floor(min(q[0] for q in p)/256), math.floor(max(q[0] for q in p)/256)+1):
                for z in range(math.floor(min(q[2] for q in p)/256), math.floor(max(q[2] for q in p)/256)+1):
                    self.overhead[x, z].append((p, den))
        for indices, state in triangles:
            p = tuple(vertices[i][0] for i in indices)
            a, b, c = p
            u = tuple(b[i]-a[i] for i in range(3))
            v = tuple(c[i]-a[i] for i in range(3))
            n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
            if not any(n):
                continue
            # MK64 assigns the collision axis from the largest normal component.
            if abs(n[1]) >= max(abs(n[0]), abs(n[2])):
                if n[1] <= 0:
                    continue
                record = (p, state['surface'])
                self.floors.append(record)
                for x in range(math.floor(min(q[0] for q in p)/256), math.floor(max(q[0] for q in p)/256)+1):
                    for z in range(math.floor(min(q[2] for q in p)/256), math.floor(max(q[2] for q in p)/256)+1):
                        self.grid[x, z].append(record)
            else:
                self.walls.append(p)

    def floor(self, x, y, z):
        hits = []
        for (a, b, c), surface in self.grid[math.floor(x/256), math.floor(z/256)]:
            den = (b[2]-c[2])*(a[0]-c[0]) + (c[0]-b[0])*(a[2]-c[2])
            if not den:
                continue
            u = ((b[2]-c[2])*(x-c[0]) + (c[0]-b[0])*(z-c[2]))/den
            v = ((c[2]-a[2])*(x-c[0]) + (a[0]-c[0])*(z-c[2]))/den
            if u >= -1e-7 and v >= -1e-7 and u+v <= 1.0000001:
                h = u*a[1]+v*b[1]+(1-u-v)*c[1]
                if abs(h-y) <= 80:
                    hits.append((abs(h-y), h, surface))
        if not hits:
            return None
        _, h, surface = min(hits)
        return None if surface == 'OUT_OF_BOUNDS' else h

    def ceiling(self, x, y, z, height):
        ground = self.floor(x, y, z)
        lower = max(y, ground if ground is not None else y) + 2
        for (a, b, c), den in self.overhead[math.floor(x/256), math.floor(z/256)]:
            u = ((b[2]-c[2])*(x-c[0]) + (c[0]-b[0])*(z-c[2]))/den
            v = ((c[2]-a[2])*(x-c[0]) + (a[0]-c[0])*(z-c[2]))/den
            if u >= -1e-7 and v >= -1e-7 and u+v <= 1.0000001:
                h = u*a[1]+v*b[1]+(1-u-v)*c[1]
                if lower < h < y+height:
                    return True
        return False

    def plane(self, origin, direction, scale):
        x, y, z = origin
        center = self.floor(x, y, z)
        center = y if center is None else center
        offset = PLANE_SAMPLE_OFFSET/scale
        heights = [self.floor(x+direction[0]*sign*offset, center, z+direction[1]*sign*offset)
                   for sign in (-1, 1)]
        if all(h is not None for h in heights):
            slope = (heights[1]-heights[0])/(2*offset)
        elif heights[0] is not None:
            slope = (center-heights[0])/offset
        elif heights[1] is not None:
            slope = (heights[1]-center)/offset
        else:
            slope = 0
        return center, max(-CROSS_SLOPE_LIMIT, min(CROSS_SLOPE_LIMIT, slope))

    def edge(self, origin, direction, sign, center, slope, scale):
        x, y, z = origin
        dx, dz = direction[0]*sign, direction[1]*sign
        wall_hits = []
        for tri in self.walls:
            cuts = []
            for a, b in zip(tri, tri[1:]+tri[:1]):
                da = (a[0]-x)*dz-(a[2]-z)*dx
                db = (b[0]-x)*dz-(b[2]-z)*dx
                if (da <= 0 <= db or db <= 0 <= da) and abs(da-db) > 1e-9:
                    t = da/(da-db)
                    q = tuple(a[k]+t*(b[k]-a[k]) for k in range(3))
                    cuts.append(((q[0]-x)*dx+(q[2]-z)*dz, q[1]))
            if len(cuts) < 2:
                continue
            lo, hi = sorted(cuts)[:2]
            if hi[0] < 0 or lo[0] > 400:
                continue
            d = max(0, lo[0])
            ground = self.floor(x+dx*d, y, z+dz*d)
            ground = y if ground is None else ground
            if max(lo[1], hi[1]) > ground+2 and min(lo[1], hi[1]) < ground+60:
                wall_hits.append((d, max(lo[1], hi[1])))
        distance = min((hit[0] for hit in wall_hits), default=math.inf)
        top = max((hit[1] for hit in wall_hits if hit[0] <= distance+1), default=y+500/16)
        wall = distance, top
        last = self.floor(x, y, z)
        if last is None:
            last = y
        for d in range(4, 401, 4):
            if wall[0] <= d:
                return wall[0], 'wall', wall[1]
            h = self.floor(x+dx*d, last, z+dz*d)
            if h is None:
                return d-4, 'floor', last+500/16
            if abs(h-(center+slope*sign*d)) > PLANE_TOLERANCE/scale:
                top = max([h] + [hit[1] for hit in wall_hits if d-4 <= hit[0] <= d+SLOPE_LOOKAHEAD])
                probe = h
                for ahead in range(d+4, min(400, d+SLOPE_LOOKAHEAD)+1, 4):
                    probe = self.floor(x+dx*ahead, probe, z+dz*ahead)
                    if probe is None:
                        break
                    top = max(top, probe)
                return d-4, 'plane', top
            last = h
        return 400, 'cap', last+500/16

    def report(self, samples, scale):
        slopes = sorted(abs(math.degrees(math.atan(row[4]))) for row in samples)
        mid = len(slopes)//2
        median = (slopes[mid]+slopes[~mid])/2
        ceilings = 0
        for origin, direction, edges, center, slope in samples:
            left, right = edges[0][0], edges[1][0]
            count = max(1, math.ceil((left+right)/16))
            if any(self.ceiling(origin[0]+direction[0]*d, center+slope*d,
                                origin[2]+direction[1]*d, CEILING_HEIGHT/scale)
                   for d in (-left+(left+right)*i/count for i in range(count+1))):
                ceilings += 1
        print(f'  cross-slope max/median: {slopes[-1]:.2f}/{median:.2f} degrees; ceiling rows: {ceilings}')
        for j, name in enumerate(('left', 'right')):
            widths = sorted(row[2][j][0]*scale for row in samples)
            counts = defaultdict(int)
            for row in samples:
                counts[row[2][j][1]] += 1
            mid = len(widths)//2
            median = (widths[mid]+widths[~mid])/2
            print(f'  {name} width min/median/max: {widths[0]:.0f}/{median:.0f}/{widths[-1]:.0f}; wall/floor/plane/cap: {counts["wall"]}/{counts["floor"]}/{counts["plane"]}/{counts["cap"]}')

    def debug(self, path, rows, links, scale, samples):
        size = 1024
        points = [p for tri, _ in self.floors for p in tri]
        points += [p for tri in self.walls for p in tri]
        minx, maxx = min(p[0] for p in points), max(p[0] for p in points)
        minz, maxz = min(p[2] for p in points), max(p[2] for p in points)
        factor = (size-20)/max(maxx-minx, maxz-minz, 1)
        def xy(p):
            return (round((p[0]-minx)*factor)+10, round((p[2]-minz)*factor)+10)
        pixels = bytearray([255, 255, 255])*(size*size)
        def line(a, b, color):
            x0, y0 = a
            x1, y1 = b
            steps = max(abs(x1-x0), abs(y1-y0), 1)
            for i in range(steps+1):
                x, y = round(x0+(x1-x0)*i/steps), round(y0+(y1-y0)*i/steps)
                if 0 <= x < size and 0 <= y < size:
                    pixels[(y*size+x)*3:(y*size+x)*3+3] = bytes(color)
        for tri, _ in self.floors:
            p = [xy(q) for q in tri]
            for y in range(max(0, min(q[1] for q in p)), min(size-1, max(q[1] for q in p))+1):
                cuts = []
                for a, b in zip(p, p[1:]+p[:1]):
                    if a[1] != b[1] and min(a[1], b[1]) <= y <= max(a[1], b[1]):
                        cuts.append(round(a[0]+(b[0]-a[0])*(y-a[1])/(b[1]-a[1])))
                if len(cuts) >= 2:
                    line((min(cuts), y), (max(cuts), y), (200, 200, 200))
        for tri in self.walls:
            p = [xy(q) for q in tri]
            for a, b in zip(p, p[1:]+p[:1]): line(a, b, (220, 30, 30))
        for i, row in enumerate(rows):
            for j, color in ((1, (0, 60, 255)), (3, (0, 170, 40))):
                if samples[i][2][0 if j == 1 else 1][1] == 'plane':
                    color = (255, 140, 0)
                a, b = row[j], rows[links[i][2]][j]
                line(xy((a[0]/scale, 0, -a[2]/scale)), xy((b[0]/scale, 0, -b[2]/scale)), color)
        raw = b''.join(b'\0'+pixels[y*size*3:(y+1)*size*3] for y in range(size))
        def chunk(kind, data):
            return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0))+chunk(b'IDAT', zlib.compress(raw))+chunk(b'IEND', b''))


def road(points, scale, mesh, branch_points=(), boxes=()):
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
    rows, samples = [], []
    for i, p in enumerate(centers):
        prev, nxt = centers[links[i][1]], centers[links[i][2]]
        dx, dz = nxt[0]-prev[0], nxt[2]-prev[2]
        length = math.hypot(dx, dz)
        if length < 1e-6:
            raise ValueError(f'path direction undefined at section {i}')
        side = (dz / length, -dx / length)
        origin = (p[0]/scale, -p[1]/scale, -p[2]/scale)
        direction = (side[0], -side[1])
        center_y, slope = mesh.plane(origin, direction, scale)
        samples.append((origin, direction, [mesh.edge(origin, direction, sign, center_y, slope, scale)
                                            for sign in (-1, 1)], center_y, slope))
    widths = [[edge[0] for edge in sample[2]] for sample in samples]
    for i, (origin, direction, edges, center_y, slope) in enumerate(samples):
        row = []
        for j, sign in enumerate((-1, 1)):
            width, reason, top = edges[j]
            neighbours = [widths[k][j] for k in (links[i][1], i, links[i][2])]
            smooth = sorted(neighbours)[1]
            width = min(width, smooth)
            width = max(20, width)
            x, z = origin[0]+direction[0]*sign*width, origin[2]+direction[1]*sign*width
            height = center_y+slope*sign*width
            edge = (round(x*scale), round(-height*scale), round(-z*scale))
            raw_height = mesh.floor(x, center_y, z)
            raw_height = height if raw_height is None else raw_height
            wall_height = max(500, round((top-height)*scale), round((raw_height-height)*scale)+500)
            wall = (edge[0], edge[1]-wall_height, edge[2])
            if j == 0:
                row.extend((wall, edge, (round(origin[0]*scale), round(-center_y*scale), round(-origin[2]*scale))))
            else:
                row.extend((edge, wall))
            edges[j] = (width, reason, top)
        rows.append(row)
    pickup_faces = set()
    for x, y, z, group in boxes:
        p = (x*scale, -y*scale, -z*scale)
        i = min(range(len(centers)), key=lambda k: distance_to_segment(p, centers[k], centers[links[k][2]]))
        a, b = centers[i], centers[links[i][2]]
        cross = (b[2]-a[2])*(p[0]-a[0]) - (b[0]-a[0])*(p[2]-a[2])
        pickup_faces.add((i, 2 if cross >= 0 else 1))
    trv = b''.join(struct.pack('>iiii', *v, 0) for row in rows for v in row)
    trf = bytearray()
    trs = bytearray()
    for i, row in enumerate(rows):
        next_index = links[i][2]
        nxt = rows[next_index]
        for j in range(4):
            ids = (next_index*5+j+1, next_index*5+j, i*5+j, i*5+j+1)
            n = normal(nxt[j+1], nxt[j], row[j])
            pickup = (i, j) in pickup_faces
            flags = (1 if j in (1, 2) else 0) | ((8 if j == 2 else 2) if pickup else 0)
            trf.extend(struct.pack('>hhhhhhhBBI', *ids, *n, 1 if pickup else 0, flags, 0xffffffff))
        section = bytearray(156)
        struct.pack_into('>iiiiii', section, 0, *links[i][:3], *map(round, centers[i]))
        struct.pack_into('>h', section, 24, 8)
        struct.pack_into('>hh', section, 142, i*4, 4)
        struct.pack_into('>h', section, 150, links[i][3])
        trs.extend(section)
    return trv, bytes(trf), bytes(trs), pickup_faces, rows, samples, links


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


def scenery(vertices, triangles, scale, mapping, no_textures, offsets):
    tims, paths, dims, missing = [], {}, {}, set()
    groups = defaultdict(list)
    capped = 0
    untextured = defaultdict(int)
    untextured_arrays = defaultdict(int)
    for index, (tri, state) in enumerate(triangles):
        symbol = state.get('image')
        path = mapping.get(symbol)
        if symbol and (not path or not path.exists()): missing.add(symbol)
        tex = None
        reason = ('textures disabled by option' if no_textures else
                  'no SetTextureImage seen' if not symbol else
                  'texture disabled' if not state.get('enabled', True) or state.get('combine') == 'G_CC_SHADE' else
                  'symbol unmapped' if not path or not path.exists() else None)
        if not no_textures and path and path.exists() and state.get('enabled', True) and state.get('combine') != 'G_CC_SHADE':
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
        if index in offsets:
            a = [coords[1][i]-coords[0][i] for i in range(3)]
            b = [coords[2][i]-coords[0][i] for i in range(3)]
            normal = (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
            length = math.sqrt(sum(x*x for x in normal))
            coords = [tuple(p[i]-offsets[index]*normal[i]/length for i in range(3)) for p in coords]
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
            factors = (0.5, 0.5, 0.5) if tex is not None else (255/264, 255/260, 255/264)
            cols = [tuple(max(0,min(255,round(x*f))) for x, f in zip(p[3:6], factors)) for p in pp]
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
    vertices, triangles, collision_triangles, paths, boxes = read_source(args.mk64, args.course)
    mesh = CollisionMesh(vertices, collision_triangles, triangles)
    triangles, duplicates = dedupe(vertices, triangles)
    offsets, overlap_groups, _ = overlap_offsets(vertices, triangles)
    points, misses = heights(samples(paths[0], args.scale), vertices, triangles)
    branch_points = []
    for start, end, stretch in branches(paths, points, args.scale):
        branch, fallback = heights(open_samples([points[start]] + stretch + [points[end]], args.scale), vertices, triangles)
        misses += fallback
        branch_points.append((start, end, branch))
        print(f'  branch {start}->{end}: {len(branch)} sections')
    trv, trf, trs, pickups, rows, edge_samples, links = road(points, args.scale, mesh, branch_points, boxes)
    mesh.report(edge_samples, args.scale)
    if args.course in ('luigi_raceway', 'koopa_troopa_beach'):
        name = 'luigi' if args.course == 'luigi_raceway' else 'koopa'
        mesh.debug(args.out / 'build' / f'{name}_collision.png', rows, links, args.scale, edge_samples)
    scene, objects, tims, after, capped, missing, untextured, untextured_arrays = scenery(vertices, triangles, args.scale, texture_map(args.mk64, args.course), args.no_textures, offsets)
    skyverts = [(-12000, 0, -12000), (12000, 0, -12000), (12000, 0, 12000), (-12000, 0, 12000), (0, -12000, 0)]
    skytris = [((i, (i+1)%4, 4), [(110, 160, 220), (110, 160, 220), (30, 60, 150)]) for i in range(4)]
    pad = [(245, 245, 245, 255) if x % 8 < 2 or y % 8 < 2 or (abs(x-16) <= (y-8)//2 and 8 <= y < 24) else (55, 55, 55, 255) for y in range(32) for x in range(32)]
    collision = bytes(0 if (i//4, i%4) in pickups else 1 for i in range(len(trf)//20))
    files = {'track.trv': trv, 'track.trf': trf, 'track.trs': trs, 'track.trc': collision, 'library.ttf': bytes(42) + struct.pack('>21h', *([1]*21)), 'library.cmp': cmp_file([tim_image(32, 32, [(255,255,255,255)]*1024), tim_image(32, 32, pad)]), 'scene.prm': scene, 'scene.cmp': cmp_file(tims if tims else None), 'sky.prm': object_bytes('sky', skyverts, skytris), 'sky.cmp': cmp_file()}
    out = args.out / 'mk64' / args.course
    out.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (out / name).write_bytes(data)
    print(f'  pickups: {len(boxes)} boxes -> {len(pickups)} faces; duplicates removed: {duplicates}')
    print(f'  overlaps: {overlap_groups} groups, {len(offsets)} triangles nudged')
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



