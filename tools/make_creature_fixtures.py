"""Independent stock CRE8/WIZD/FSMC regression metadata, from the read-only Python inventory.

Development only; the C++ tests consume the committed JSON without Python or IDA.
"""
from pathlib import Path
import json
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
INVENTORY = json.loads((ROOT / 'analysis/wad_inventory.json').read_text())


def u32(b, p):
    return struct.unpack_from('<I', b, p)[0]


def i32(b, p):
    return struct.unpack_from('<i', b, p)[0]


def fourcc(b, p):
    return b[p:p + 4][::-1].decode('ascii')


def string(b, p, size=32):
    return b[p:p + size].split(b'\0')[0].decode('ascii')


def definition(b):
    count = u32(b, 156)
    fsm = fourcc(b, 144)
    return dict(name=fourcc(b, 0), kind=u32(b, 4), id=fourcc(b, 8), icon=fourcc(b, 12),
                stats=list(struct.unpack_from('<24h', b, 44)),
                spells=[fourcc(b, p) for p in range(92, 124, 4)], run_spell=fourcc(b, 124),
                melee_damage_type=i32(b, 128), melee_damage=i32(b, 132),
                mana=i32(b, 136), behaviour_fsm=fourcc(b, 140),
                animation_fsm='norm' if fsm in ('\0' * 4, '----') else fsm,
                body=fourcc(b, 148), flags=u32(b, 152),
                sounds=[fourcc(b, 160 + i * 4) for i in range(count)],
                animations=[fourcc(b, p) for p in range(176, 432, 4)],
                event_spells=[dict(event=i32(b, p), chance=struct.unpack_from('<H', b, p+4)[0],
                                   flags=struct.unpack_from('<H', b, p+6)[0], spell=fourcc(b, p+8))
                              for p in range(432, 528, 12)])


def machine(b):
    groups = []
    p = 48
    for _ in range(u32(b, 44)):
        end = p + u32(b, p)
        group = dict(id=fourcc(b, p + 4), name=string(b, p + 8), flags=u32(b, p + 40), states=[])
        q = p + 48
        for _ in range(u32(b, p + 44)):
            state = dict(id=fourcc(b, q + 4), animation=i32(b, q + 8), flags=u32(b, q + 12),
                         name=string(b, q + 16), enter=string(b, q + 48), run=string(b, q + 80),
                         exit=string(b, q + 112), events=[])
            assert u32(b, q) == 148 + u32(b, q + 144) * 48
            for e in range(u32(b, q + 144)):
                at = q + 148 + 48 * e
                state['events'].append(dict(id=i32(b, at), animation=i32(b, at + 4),
                                            group=fourcc(b, at + 8), state=fourcc(b, at + 12),
                                            handler=string(b, at + 16)))
            group['states'].append(state)
            q += u32(b, q)
        assert q == end
        groups.append(group)
        p = end
    assert p == len(b)
    return dict(id=fourcc(b, 0), name=string(b, 4), initial_group=fourcc(b, 36),
                initial_state=fourcc(b, 40), groups=groups)


results = []
for pack in INVENTORY:
    for entry in pack['entries']:
        if entry['tag'] not in ('CRE8', 'WIZD', 'FSMC') or entry['flags'] & 1:
            continue
        with (ROOT / pack['file']).open('rb') as source:
            source.seek(entry['offset'])
            b = source.read(entry['stored_size'])
        if entry['flags'] & 2:
            b = zlib.decompress(b)
        results.append(dict(file=pack['file'], path=entry['path'], tag=entry['tag'],
                            expected=machine(b) if entry['tag'] == 'FSMC' else definition(b)))
target = ROOT / 'source/tests/fixtures/creature.json'
target.write_text(json.dumps(results, indent=2) + '\n', encoding='utf8')
print(f'Wrote {len(results)} resources to {target}')
