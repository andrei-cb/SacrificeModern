"""Development-only stock-asset oracle. Uses the existing Python WAD inventory.

LOD semantics: game3d.dll MRMINSTANCE::UpdateLod, original VA 0x1601cf30.
This reads serialized face lifetimes directly; it does not invoke the C++ decoder.
No original game payloads are copied into the fixture.
"""
from pathlib import Path
import json
import struct
import zlib

source = Path(__file__).resolve().parents[1]
root = source.parent
inventory = json.loads((root / 'analysis/wad_inventory.json').read_text())
selected = {
    ('data\\models.wad', 'roks'), ('data\\models.wad', 'mrph'),
    ('data\\models.wad', 'bess'), ('data\\models.wad', 'bovi'),
    ('data\\Daniel.wad', 'skul'), ('data\\jamesmod.wad', 'jman'),
    ('data\\charlie.wad', 'deth'),
}
fixtures = []
for container in inventory:
    requested = {name for file, name in selected if file == container['file']}
    if not requested:
        continue
    blob = (root / container['file']).read_bytes()
    for resource in container['entries']:
        if resource['tag'] != 'MRMM' or resource['name'] not in requested:
            continue
        payload = blob[resource['offset']:resource['offset'] + resource['stored_size']]
        if resource['flags'] & 2:
            payload = zlib.decompress(payload)
        h = struct.unpack_from('<49I', payload)
        face_offset = 196 + ((h[47] - h[46]) & 0xffffffff)
        level_offset = 196 + ((h[48] - h[46]) & 0xffffffff)
        faces = [struct.unpack_from('<9I', payload, face_offset + 36 * i) for i in range(h[3])]
        levels = [struct.unpack_from('<2I', payload, level_offset + 8 * i)
                  for i in range(h[1] - h[2] + 1)]
        snapshots = []
        for vertices in sorted({h[2], (h[1] + h[2]) // 2, h[1]}):
            # Face birth is its LOD bucket; retirement is the face's last vertex count.
            active = [face for count, first in levels[:vertices - h[2] + 1]
                      for face in faces[first:first + count] if face[8] >= vertices]
            checksum = 14695981039346656037
            for face in active:
                for index in (face[0], face[2], face[1]):
                    for byte in struct.pack('<I', index):
                        checksum = ((checksum ^ byte) * 1099511628211) & 0xffffffffffffffff
            snapshots.append({'vertices': vertices, 'triangles': len(active),
                              'indices_fnv64': f'{checksum:016x}'})
        endpoints = []
        for frame in sorted({0, h[6] - 1}):
            x, y, z = struct.unpack_from('<3f', payload, 196 + frame * h[1] * 40)
            endpoints.append({'frame': frame, 'first_position': [x, z, y]})
        fixtures.append({'file': container['file'], 'resource': resource['path'],
                         'frames': h[6], 'stored_faces': h[3], 'snapshots': snapshots,
                         'frame_endpoints': endpoints})
assert len(fixtures) == len(selected)
destination = source / 'tests/fixtures/mrm.json'
destination.parent.mkdir(exist_ok=True)
destination.write_text(json.dumps({'format': 1, 'fixtures': fixtures}, indent=2) + '\n', encoding='utf8')
print(f'Wrote {len(fixtures)} stock MRM regression fixtures to {destination}')
