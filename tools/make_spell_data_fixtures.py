"""Recover function-name metadata and independent SPEL fixtures from read-only original inputs."""
from pathlib import Path
import json
import struct
import zlib
import pefile

root = Path(__file__).resolve().parents[2]
pe = pefile.PE(str(root / "Sacrifice.exe"))

def identifier(name):
    data = name.encode("ascii") + b"\0"
    words, remainder = divmod(len(data), 4)
    value = sum(struct.unpack_from("<I", data, i * 4)[0] for i in range(words))
    value += sum(byte << (8 * (3 - remainder + i)) for i, byte in enumerate(data[words * 4:]))
    return value & 0xffffffff

functions = []
for address, count, evaluator in ((0x4cbba8, 272, False), (0x4ce1e8, 7, True)):
    for index in range(count):
        raw = pe.get_data(address - pe.OPTIONAL_HEADER.ImageBase + index * 36, 36)
        name = raw[:28].split(b"\0")[0].decode("ascii")
        assert raw[32:] == b"\xff" * 4
        functions.append({"name": name, "evaluator": evaluator, "id": identifier(name)})
assert len({(f["name"], f["evaluator"]) for f in functions}) == 279
source = root / "source/engine/spell_functions.inc"
source.write_text("// Native registration order; function names only, no original addresses/code.\n" +
                  "\n".join('    {"' + f["name"] + '", ' + str(f["evaluator"]).lower() + '},'
                            for f in functions) + "\n", newline="\n")
ids = {(f["name"], f["evaluator"]): f["id"] for f in functions}
spells = []
archive = next(a for a in json.loads((root / "analysis/wad_inventory.json").read_text())
               if a["file"].lower() == "data\\spells.wad")
raw = (root / archive["file"]).read_bytes()
for entry in archive["entries"]:
    if entry["tag"] != "SPEL":
        continue
    data = raw[entry["offset"]:entry["offset"] + entry["stored_size"]]
    if entry["flags"] & 2:
        data = zlib.decompress(data)
    names = [data[offset:offset + 28].split(b"\0")[0].decode("ascii") for offset in (206, 242, 278)]
    assert all(not name or (name, index == 0) in ids for index, name in enumerate(names))
    cooldown, mana_cost = struct.unpack_from("<2i", data, 20)
    distance, targets = struct.unpack_from("<fI", data, 28)
    amount = struct.unpack_from("<i", data, 68)[0]
    flags = struct.unpack_from("<I", data, 88)[0]
    evaluator = names[0]
    if not evaluator:
        if flags & 0x40000000:
            evaluator = "EvalSpell_DamageArea" if flags & 0x400000 else "EvalSpell_Damage"
        elif flags & 0x80000000:
            evaluator = "EvalSpell_Heal"
        elif flags & 0x8000000:
            evaluator = "EvalSpell_Paralyze"
        elif flags & 0x4000000:
            evaluator = "EvalSpell_Protect"
    spells.append({"id": entry["name"], "path": entry["path"], "cooldown": cooldown,
                   "mana_cost": mana_cost, "range": distance, "targets": targets,
                   "amount": amount, "flags": flags,
                   "property_flags": struct.unpack_from("<I",data,92)[0],
                   "radius": struct.unpack_from("<f",data,60)[0],
                   "damage_type": struct.unpack_from("<i",data,64)[0],
                   "speed": struct.unpack_from("<f",data,72)[0],
                   "acceleration": struct.unpack_from("<f",data,76)[0],
                   "duration": struct.unpack_from("<f",data,80)[0],
                   "spread_limit": struct.unpack_from("<f",data,84)[0],
                   "sounds": [list(struct.unpack_from("<BBI",data,112+i*6)) for i in range(5)],
                   "blocked_effects": list(struct.unpack_from("<4I",data,44)),
                   "parameters": [list(struct.unpack_from("<2I", data, 142 + i * 8)) for i in range(8)],
                   "names": names, "ids": [ids.get((evaluator, True), 0),
                       ids[(names[1] or "NullSpellRun", False)], ids.get((names[2], False), 0)]})
assert len(spells) == 141
fixture = root / "source/tests/fixtures/spell_data.json"
fixture.write_text(json.dumps({"functions": functions, "spells": spells}, indent=2) + "\n", newline="\n")
print(f"Wrote {len(functions)} function names and {len(spells)} native spell fixtures")
