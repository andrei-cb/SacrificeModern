"""Independent LAND property/terrain metadata; reads installed archives, never runs game binaries.

Connectivity uses an unbounded BFS oracle, independent of LAND's bounded scanline implementation.
Hashes cover all samples/cells, rather than a few selected map locations.
"""
from pathlib import Path
from collections import deque
import json
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
inventory = json.loads((ROOT / 'analysis/wad_inventory.json').read_text())

def payload(raw, entry):
    b = raw[entry['offset']:entry['offset'] + entry['stored_size']]
    return zlib.decompress(b) if entry['flags'] & 2 else b

def fnv(data):
    h = 14695981039346656037
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
    return str(h)

tilesets, maps = [], []
for archive in inventory:
    entries = archive['entries']
    if not any(e['tag'] in ('HMAP', 'MAPT') for e in entries):
        continue
    raw = (ROOT / archive['file']).read_bytes()
    for land in (e for e in entries if e['tag'] == 'LAND'):
        descendants = [e for e in entries if e['path'].startswith(land['path'] + '/')]
        if not any(e['tag'] == 'MAPT' for e in descendants):
            continue
        properties = [bytearray(12) for _ in range(256)]
        groups = [bytearray(60) for _ in range(16)]
        for e in descendants:
            if e['tag'] == 'MAPT':
                properties[int(e['name'])][:] = payload(raw, e)[-12:]
        group_order = [e for e in descendants if e['tag'] == 'MAPG']
        for e in group_order:
            g = bytearray(payload(raw, e))
            slot = int(e['name'][-2:])
            if g[32] == 1:
                a, b = (groups[i][48:] for i in struct.unpack_from('<2I', g))
                g[48] = a[0] | b[0]
                g[50] = a[2]
                for i in (1, 3, 8, 9, 10, 11):
                    g[48+i] = (a[i] + b[i]) // 2
            groups[slot] = g
            for tile in g[34:34+g[33]]:
                old = properties[tile]
                preserve = {1: 0x40, 2: 0x10, 3: 0x10, 7: 0x80,
                            8: 8, 9: 8, 10: 8, 11: 8}
                properties[tile] = bytearray(
                    (old[i] | g[48]) if i == 0 else
                    g[48+i] if i in preserve and not old[0] & preserve[i] else old[i]
                    for i in range(12))
        tilesets.append(dict(file=archive['file'], id=land['name'],
                            properties=[list(p) for p in properties],
                            group_properties=[list(g[48:]) for g in groups]))
    hmap = next((e for e in entries if e['tag'] == 'HMAP'), None)
    if not hmap:
        continue
    b = payload(raw, hmap)
    assert struct.unpack_from('<2H', b) == (256, 256)
    h = list(struct.unpack_from('<65536H', b, 4))
    assert all(h[i] & 0x8000 for i in range(65536)
               if i < 256 or i >= 65280 or i % 256 in (0, 255))
    padded = h + h[:256] + [h[0]] + [0] * 259
    regions = bytearray(255 if v < 32768 else 0 for v in padded[:65792]) + bytearray(260)
    label, components = 254, 0
    for start in range(65536):
        if regions[start] != 255 or not label:
            continue
        components += 1
        queue = deque([start])
        regions[start] = label
        while queue:
            at = queue.popleft()
            for n in (at-256, at+256, at-1 if at%256 else -1,
                      at+1 if at%256 != 255 else -1):
                if 0 <= n < 65536 and regions[n] == 255:
                    regions[n] = label
                    queue.append(n)
        label -= 1
    flags = bytearray()
    for i in range(65536):
        holes = sum(padded[j] >> 15 for j in (i, i+1, i+256, i+257))
        flags.append(int(holes > 0) | (int(holes > 1) << 1))
    mips = [flags]
    for width in (128, 64):
        old = mips[-1]
        mip = bytearray()
        for y in range(width):
            for x in range(width):
                i = (2*y)*(2*width) + 2*x
                a, b, c, d = (old[j] for j in (i, i+1, i+2*width, i+2*width+1))
                mip.append(((a | b | c | d) & 1) | ((a & b & c & d) & 2))
        mips.append(mip)
    maps.append(dict(file=archive['file'], regions=fnv(regions),
                     mipmaps=[fnv(m) for m in mips], components=components,
                     height=fnv(struct.pack('<%dH' % len(padded), *padded))))
assert len(maps) == 96
out = ROOT / 'source/tests/fixtures/navigation.json'
out.write_text(json.dumps(dict(tilesets=tilesets, maps=maps), indent=2) + '\n', encoding='utf8')
print(f'Wrote {len(tilesets)} tilesets and {len(maps)} map grids to {out}')
