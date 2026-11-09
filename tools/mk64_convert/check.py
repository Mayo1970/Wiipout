#!/usr/bin/env python3
"""Check converted files using the field sizes and offsets used by the C loaders."""
import argparse
import struct
from pathlib import Path


def compressed(path):
    data = path.read_bytes()
    count, = struct.unpack_from('<i', data)
    assert 0 <= count <= 10000
    sizes = struct.unpack_from('<' + 'i'*count, data, 4)
    assert all(n >= 0 for n in sizes)
    pos = (4 + 4*count)*8
    window = bytearray(8192)
    cursor = 1
    out = bytearray()
    def bits(n):
        nonlocal pos
        assert pos+n <= len(data)*8
        value = 0
        for _ in range(n):
            value = (value << 1) | ((data[pos//8] >> (7-pos%8)) & 1)
            pos += 1
        return value
    while True:
        if bits(1):
            value = bits(8)
            out.append(value)
            window[cursor] = value
            cursor = (cursor+1) % 8192
        else:
            offset = bits(13)
            if not offset:
                break
            length = bits(4)+3
            for i in range(length):
                value = window[(offset+i) % 8192]
                out.append(value)
                window[cursor] = value
                cursor = (cursor+1) % 8192
        assert len(out) <= sum(sizes)
    assert len(out) == sum(sizes)
    offset = 0
    dimensions = []
    for size in sizes:
        tim = out[offset:offset+size]
        assert len(tim) >= 20
        magic, kind, block, x, y, width, height = struct.unpack_from('<IIIHHHH', tim)
        assert magic == 0x10 and kind == 2 and block == 12+width*height*2
        assert len(tim) == 8+block and 1 <= width <= 256 and 1 <= height <= 256
        assert width & (width-1) == 0 and height & (height-1) == 0
        dimensions.append((width, height))
        offset += size
    return dimensions


def objects(path, textures):
    data = path.read_bytes()
    pos = 0
    count = triangles = 0
    while pos < len(data):
        assert pos+144 <= len(data)
        nv = struct.unpack_from('>h', data, pos+16)[0]
        nn = struct.unpack_from('>h', data, pos+24)[0]
        np = struct.unpack_from('>h', data, pos+32)[0]
        assert nv > 0 and nn == 0 and np > 0
        pos += 144
        assert pos+nv*8 <= len(data)
        pos += nv*8
        for _ in range(np):
            assert pos+24 <= len(data)
            kind, flags, a, b, c = struct.unpack_from('>hhhhh', data, pos)
            assert kind in (5, 6) and all(0 <= i < nv for i in (a, b, c))
            if kind == 6:
                assert pos+36 <= len(data)
                texture, = struct.unpack_from('>h', data, pos+10)
                assert 0 <= texture < len(textures)
                uv = data[pos+16:pos+22]
                assert len(uv) == 6 and all(0 <= value <= 255 for value in uv)
                pos += 36
            else:
                pos += 24
            triangles += 1
        count += 1
    assert pos == len(data) and count
    return count, triangles


def check(folder):
    trv = (folder/'track.trv').read_bytes()
    trf = (folder/'track.trf').read_bytes()
    trs = (folder/'track.trs').read_bytes()
    trc = (folder/'track.trc').read_bytes()
    ttf = (folder/'library.ttf').read_bytes()
    assert len(trv)%16 == len(trf)%20 == len(trs)%156 == 0
    nv, nf, ns = len(trv)//16, len(trf)//20, len(trs)//156
    assert nv and nf and ns and len(trc) == nf and all(x == 1 for x in trc)
    assert len(ttf) == 42
    texcount = len(compressed(folder/'library.cmp'))
    assert all(0 <= i < texcount for i in struct.unpack('>21h', ttf))
    for face in struct.iter_unpack('>hhhhhhhBBI', trf):
        assert all(0 <= i < nv for i in face[:4])
        assert face[7] == 0
    links = []
    for i in range(ns):
        off = i*156
        junction, prev, nxt = struct.unpack_from('>iii', trs, off)
        version, = struct.unpack_from('>h', trs, off+24)
        start, count = struct.unpack_from('>hh', trs, off+142)
        assert junction == -1 and 0 <= prev < ns and 0 <= nxt < ns and version == 8
        assert 0 <= start and start+count <= nf and count == 4
        flags = [trf[(start+j)*20+15] for j in range(count)]
        assert flags == [0, 1, 1, 0]
        links.append((prev, nxt))
    seen = set()
    current = 0
    for _ in range(ns):
        assert current not in seen
        seen.add(current)
        prev, nxt = links[current]
        assert links[nxt][0] == current and links[prev][1] == current
        current = nxt
    assert current == 0 and len(seen) == ns
    for name in ('scene', 'sky'):
        textures = compressed(folder/(name+'.cmp'))
        count, triangles = objects(folder/(name+'.prm'), textures)
        print(f'  {name}: {count} objects, {triangles} triangles')
    print(f'{folder.name}: OK, {ns} sections, {nf} faces, {nv} vertices')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folders', nargs='+', type=Path)
    for folder in parser.parse_args().folders:
        check(folder)
