"""Independent SIDS fixture from the read-only installation and Python WAD inventory."""
from pathlib import Path
import json
import struct
import zlib

root = Path(__file__).resolve().parents[2]
rows = []
for archive in json.loads((root / "analysis/wad_inventory.json").read_text()):
    for entry in archive["entries"]:
        if entry["tag"] != "SIDS":
            continue
        payload = (root / archive["file"]).read_bytes()[
            entry["offset"] : entry["offset"] + entry["stored_size"]
        ]
        if entry["flags"] & 2:
            payload = zlib.decompress(payload)
        count = struct.unpack_from("<I", payload)[0]
        assert len(payload) == 4 + count * 20
        table = [list(struct.unpack_from("<5I", payload, 4 + index * 20)) for index in range(count)]
        rows.append({"file": archive["file"], "records": table})
assert len(rows) == 96 and sum(len(row["records"]) for row in rows) == 652
output = root / "source/tests/fixtures/sides.json"
output.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf8")
print(f"Wrote {len(rows)} side tables to {output}")
