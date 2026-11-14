"""Independent registry metadata/grant fixtures from the Python WAD inventory; no payload copies."""
from pathlib import Path
import json
import struct
import zlib

root = Path(__file__).resolve().parents[2]
archive = next(a for a in json.loads((root / "analysis/wad_inventory.json").read_text())
               if a["file"].lower() == "data\\spells.wad")
raw = (root / archive["file"]).read_bytes()
groups = []
payloads = {}
for index, entry in enumerate(archive["entries"]):
    if entry["tag"] == "SCAT":
        assert entry["flags"] == 1
        groups.append({"id": entry["name"], "definitions": []})
    elif entry["tag"] in ("SPEL", "CRE8", "WIZD", "STRC"):
        payload = raw[entry["offset"]:entry["offset"] + entry["stored_size"]]
        if entry["flags"] & 2:
            payload = zlib.decompress(payload)
        kind, identifier, icon = struct.unpack_from("<3I", payload, 4)
        payloads[identifier.to_bytes(4, "big").decode("ascii")] = payload
        _old_fast, slow_cast_ticks, priority = struct.unpack_from("<3H", payload, 36)
        slow_cast_ticks = 3 * slow_cast_ticks // 4
        groups[-1]["definitions"].append({
            "id": identifier.to_bytes(4, "big").decode("ascii"), "kind": kind,
            "icon": icon.to_bytes(4, "big").decode("ascii"), "priority": priority,
            "fast_cast_ticks": slow_cast_ticks // 2, "slow_cast_ticks": slow_cast_ticks,
            "cooldown": struct.unpack_from("<i",payload,20)[0],
            "mana_cost": struct.unpack_from("<i",payload,24)[0],
            "range": struct.unpack_from("<f",payload,28)[0],
            "targets": struct.unpack_from("<I",payload,32)[0],
            "required_souls": struct.unpack_from("<h",payload,82)[0] if kind==2 else 0,
            "index": index})
for group in groups:
    group["definitions"].sort(key=lambda d: (d["priority"], -d["index"]))
    for definition in group["definitions"]:
        del definition["index"]
assert len(groups) == 18 and sum(len(g["definitions"]) for g in groups) == 229
by_group = {g["id"]: g["definitions"] for g in groups}
all_definitions = {d["id"]: d for g in groups for d in g["definitions"]}
assert len(all_definitions) == 229
for definition in all_definitions.values():
    if definition["kind"] not in (2,8):
        continue
    capabilities = 6
    for value in struct.unpack_from("<8I",payloads[definition["id"]],92):
        if not value:
            break
        item = all_definitions[value.to_bytes(4,"big").decode("ascii")]
        spell = payloads[item["id"]]
        properties = struct.unpack_from("<I",spell,92)[0] if item["kind"] == 4 else 0
        if properties & 0x800000:
            continue
        if item["targets"] & 0x781f:
            flags = struct.unpack_from("<I",spell,88)[0] if item["kind"] == 4 else 0
            if flags & 0xa4000000: capabilities |= 0x10
            if flags & 0x58000000: capabilities |= 8
        else:
            capabilities |= 0x20
    definition["attack_capabilities"] = capabilities

def trace(gods):
    learned, books, rows = [], [[], [], []], []
    for level in range(1, 15):
        god = gods[level - 1]
        for category in (("Slif", "Clif"), ("Sfir", "Cfir"), ("Sear", "Cear"),
                         ("Sair", "Cair"), ("Sdea", "Cdea"))[god - 1] + ("free",):
            for definition in by_group[category]:
                if definition["priority"] // 100 == level and definition["id"] not in learned:
                    learned.append(definition["id"])
        for identifier in learned:
            definition = all_definitions[identifier]
            book = (0 if definition["kind"] == 2 else
                    2 if definition["kind"] == 16 or identifier in ("sacc", "sacu", "grdn", "tele") else 1)
            if len(books[book]) < 16 and identifier not in books[book]:
                books[book].append(identifier)
                books[book].sort(key=lambda key: all_definitions[key]["priority"])
        rows.append({"level": level, "god": god, "learned": learned[:], "books": [b[:] for b in books]})
    return rows

traces = [trace([god] * 14) for god in range(1, 6)]
traces.append(trace([level % 5 + 1 for level in range(1, 15)]))
output = root / "source/tests/fixtures/spell_registry.json"
output.write_text(json.dumps({"groups": groups, "traces": traces}, indent=2) + "\n", encoding="utf8", newline="\n")
print(f"Wrote {len(groups)} groups, 229 definitions and {len(traces)} god paths to {output}")
