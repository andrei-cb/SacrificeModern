"""Decode every stored trigger branch without executing a mission.

The grammar describes the original serialized format. Opcode names/IDs live in
trigger_codes.hpp. The tool reads only the installed maps; it needs no original
executable.
"""
from pathlib import Path
import argparse
import collections
import json
import re
import struct
import zlib

SOURCE = Path(__file__).resolve().parents[1]
HEADER = (SOURCE / "include/sacrifice/trigger_codes.hpp").read_text(encoding="utf8")
CODES = {name: int(value, 0) for name, value in re.findall(r"\b(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)u?\s*,", HEADER)}
NAMES = {group: {value: name for name, value in CODES.items() if name.startswith(group)}
         for group in ("Condition", "Action", "Order")}

# Grammar atoms: object/type/text/etc. are typed two-word values, not opcodes.
CONDITIONS = {
    "Always": "", "IfSideSpecCommandsNtts": "side filter", "IfSideSpecCommandsNttsAtLocation": "side filter location",
    "IfSideSpecCommandsNttsWithinMetersOfNtt": "side filter quantity object",
    "IfSideSpecDoesNotCommandNtts": "side filter", "IfSideSpecDoesNotCommandNttsAtLocation": "side filter location",
    "IfElapsedGameTimeIsFrames": "constraint", "IfGameTimerIsFrames": "constraint", "IfSideSpecSeesNtts": "side filter",
    "IfNttSeesNtts": "object filter", "IfSideSpecIsAttackedBySide": "side side_id",
    "IfCreatureIsAttackedBySideSpec": "object side", "IfVariableCondition": "variable constraint",
    "IfNttHasResource": "object constraint token", "IfCreatureHasStat": "object constraint token",
    "IfPlayerHasCreaturesSelected": "list", "IfCreatureHasOrders": "object order",
    "IfStructureActiveValueCondition": "object constraint", "IfCurrentSideIsAnAiSide": "",
    "IfCurrentSideIsAPlayerSide": "", "IfNttExists": "object", "IfNttExistsOnTheCurrentSide": "object",
    "IfCreatureIsAlive": "object", "Or": "condition condition", "And": "condition condition", "Not": "condition",
}
ACTIONS = {
    "Order": "object order", "OrderList": "list order", "OrderAllOfTypeAtLocation": "type location order",
    "OrderAllOfType": "type order", "Pause": "integer", "EndMissionInDefeat": "", "EndMissionInVictory": "",
    "EndMission": "", "DeclareStanceToSide": "token side_id", "ChangeSide": "object side_id",
    "ChangeSideAllOfType": "type side_id", "ChangeSideAllOfTypeAtLocation": "type location side_id",
    "DisplayVariable": "variable", "DisplayText": "text", "HideVariable": "variable", "ClearText": "",
    "PlaySample": "sample", "CreateCreatureTypeAtLocation": "type location",
    "CreateCreatureTypeOnSideAtLocation": "type side_id location",
    "CreateMultipleCreatureTypeOnSideAtLocation": "quantity type side_id location", "SetVariable": "variable quantity",
    "SetVariableToNumberOfNtts": "variable quantified", "SetVariableToResourceOfNtt": "variable token object",
    "SetVariableToResourceOfNtts": "variable token quantified", "SetVariableToRandomNumberInRange": "variable quantity quantity",
    "SpeechWithText": "actor speaks text timing sample_spec", "SpeechCreatureSpeaksSample": "object speaks sample",
    "SpeechCreatureSaysTextWithSample": "object text sample", "SpeechCreatureAsksTextWithSample": "object text sample",
    "SpeechCreatureExclaimsTextWithSample": "object text sample", "SpeechGodSaysTextWithSample": "token text sample",
    "SpeechNarratorNarratesTextWithSample": "token text sample", "SetObjectiveComplete": "objective",
    "EnableObjective": "objective", "DisableObjective": "objective",
    "TeleportAllOfTypeAtLocationToLocation": "type location location", "TeleportAllOfTypeAtLocationAway": "type location",
    "TeleportCreatureToLocation": "object location", "TeleportCreatureAway": "object", "SetGameTimer": "quantity",
    "DisplayGameTimer": "", "HideGameTimer": "", "SetAiSetting": "ai", "TurnBeaconAtLocation": "token location",
    "CinematicsStart": "", "CinematicsEnd": "", "SetViewFocusToLocation": "location",
    "SetViewOriginFromLocation": "location", "SetViewOriginFromLocationFocusToLocation": "location location",
    "SetViewZoomLevel": "token", "SetViewSpeed": "quantity", "GiveCreatureResource": "object quantity token",
    "GiveAllOfTypeAtLocationResource": "type location quantity token", "GiveWizardSpell": "object spell",
    "CastSpellAtLocation": "spell location", "SetNttState": "object token token", "SetAllOfTypeState": "type token token",
    "SetAllOfTypeAtLocationState": "type location token token", "SetCreatureStat": "object token quantity",
    "SetWeather": "token token", "SetNextMissionFilename": "filename", "SetNextMissionId": "token",
    "FadeToColorOverFrames": "token quantity", "ScreenShakeAtLocation": "location", "TurnInterfaceElement": "token token",
    "SetStructureActiveLevel": "object quantity", "AskText": "text text text variable",
    "AskTextWithSample": "text sample_spec text text variable", "ShowRandomTip": "", "ShowTipText": "text",
    "SuppressTipbox": "", "AddTextWithSampleToSpeechHistory": "text sample_spec", "AddIntroSampleToSpeechHistory": "sample",
    "MusicReset": "", "MusicAddToMood": "quantity token", "MusicTurn": "token",
}
ORDERS = {
    "Attack": "object", "GoToLocation": "location", "AttackLocation": "location", "GuardLocation": "location",
    "GoToLocationInFormation": "location token", "AttackLocationInFormation": "location token",
    "GuardLocationInFormation": "location token", "Cast": "cast", "Notify": "object",
    "TurnToFaceLocation": "location", "PassiveNotify": "", "CancelOrders": "", "Die": "",
}


class Decoder:
    def __init__(self, words):
        self.words, self.at, self.instructions = words, 0, []

    def take(self):
        if self.at == len(self.words):
            raise ValueError("truncated operand")
        value = self.words[self.at]
        self.at += 1
        return value

    def token(self, *allowed):
        value = self.take()
        if allowed and value not in [CODES[name] for name in allowed]:
            raise ValueError(f"unexpected token {value}; expected {'/'.join(allowed)}")
        return value

    def typed(self, *allowed):
        self.token(*allowed)
        return self.take()

    def sequence(self, spec):
        for atom in spec.split():
            self.parse(atom)

    def parse(self, atom):
        if atom in ("condition", "action", "order"):
            group = atom.title()
            at, code = self.at, self.take()
            name = NAMES[group].get(code)
            if name is None:
                raise ValueError(f"unknown {atom} {code}")
            self.instructions.append({"kind": group, "code": code, "name": name, "word": at})
            spec = {"condition": CONDITIONS, "action": ACTIONS, "order": ORDERS}[atom][name.removeprefix(group)]
            self.sequence(spec)
        elif atom == "object":
            self.typed("ValueCreatureId", "ValueNttId", "ValueStructureId", "ValueWizardId")
        elif atom == "type":
            self.typed("ValueCreatureType", "ValueStructureType", "ValueNttType", "ValueText", "ValueFilename")
        elif atom in ("text", "sample", "variable", "side_id", "integer", "objective", "filename"):
            name = {"side_id": "ValueSideId", "objective": "ValueMissionObjective"}.get(atom, "Value" + atom.title())
            self.typed(name)
        elif atom == "spell":
            self.typed("ValueSpell", "ValueAutoSpell")
        elif atom == "side":
            if self.token("SideSpecCurrentSide", "SideSpecSide", "SideSpecEnemySides", "SideSpecFriendlySides",
                          "SideSpecNeutralSides") == CODES["SideSpecSide"]:
                self.parse("side_id")
        elif atom == "constraint":
            if self.token("ConstraintNone", "ConstraintAtLeast", "ConstraintAtMost", "ConstraintExactly",
                          "ConstraintAll", "ConstraintTrue", "ConstraintFalse") in [CODES[n] for n in
                          ("ConstraintAtLeast", "ConstraintAtMost", "ConstraintExactly")]:
                self.parse("quantity")
        elif atom == "filter":
            code = self.token("NttsQuantityCreatureType", "NttsQuantityStructureType", "NttsCreature", "NttsStructure")
            if code in (CODES["NttsCreature"], CODES["NttsStructure"]):
                self.parse("object")
            else:
                self.sequence("constraint type")
        elif atom == "location":
            code = self.token("LocationMarker", "LocationMarkerAtHeight", "LocationStructure", "LocationCreature")
            if code in (CODES["LocationMarker"], CODES["LocationMarkerAtHeight"]):
                self.typed("ValueMarkerId")
                if code == CODES["LocationMarkerAtHeight"]:
                    self.parse("quantity")
            else:
                self.parse("object")
        elif atom == "quantity":
            code = self.token("QuantityTrue", "QuantityFalse", "QuantityInteger", "QuantityVariable",
                              "QuantityAdd", "QuantitySub", "QuantityMul", "QuantityDiv", "QuantityStat")
            if code == CODES["QuantityInteger"]:
                self.parse("integer")
            elif code == CODES["QuantityVariable"]:
                self.parse("variable")
            elif code == CODES["QuantityStat"]:
                self.sequence("token object")
            elif code not in (CODES["QuantityTrue"], CODES["QuantityFalse"]):
                self.sequence("quantity quantity")
        elif atom == "quantified":
            code = self.token("QuantifiedNttCreatureType", "QuantifiedNttStructureType", "QuantifiedNttCreatureTypeAtLocation",
                              "QuantifiedNttStructureTypeAtLocation")
            self.parse("type")
            if code in (CODES["QuantifiedNttCreatureTypeAtLocation"], CODES["QuantifiedNttStructureTypeAtLocation"]):
                self.parse("location")
        elif atom == "list":
            while True:
                code = self.token("ListOneElement", "ListMoreElements")
                self.parse("object")
                if code == CODES["ListOneElement"]:
                    break
        elif atom == "cast":
            code = self.token("SpellSpecSpellAtNtt", "SpellSpecSpellAtSoulOfNtt", "SpellSpecSpellAtLocation",
                              "SpellSpecSpell", "SpellSpecCreatureSpell")
            self.parse("spell")
            if code in (CODES["SpellSpecSpellAtNtt"], CODES["SpellSpecSpellAtSoulOfNtt"]):
                self.parse("object")
            elif code == CODES["SpellSpecSpellAtLocation"]:
                self.parse("location")
        elif atom == "actor":
            code = self.token("ActorCreature", "ActorGod", "ActorName")
            if code == CODES["ActorCreature"]:
                self.parse("object")
            elif code == CODES["ActorGod"]:
                self.parse("token")
            elif self.token("ActorNameText", "ActorNamePlayerWizard") == CODES["ActorNameText"]:
                self.parse("text")
        elif atom == "speaks":
            code = self.token("SpeaksSays", "SpeaksAsks", "SpeaksExclaims", "SpeaksNarrates", "SpeaksSaysWithEmotion",
                              "SpeaksAsksWithEmotion", "SpeaksExclaimsWithEmotion")
            if code in (CODES["SpeaksSaysWithEmotion"], CODES["SpeaksAsksWithEmotion"], CODES["SpeaksExclaimsWithEmotion"]):
                self.parse("token")
        elif atom == "timing":
            code = self.token("TimingAutoTimed", "TimingSampleLengthMinusFrames", "TimingPersistent", "TimingPauseForInput", "TimingForFrames")
            if code in (CODES["TimingSampleLengthMinusFrames"], CODES["TimingForFrames"]):
                self.parse("quantity")
        elif atom == "sample_spec":
            if self.token("SampleSpecWithSampleNone", "SampleSpecWithSample") == CODES["SampleSpecWithSample"]:
                self.parse("sample")
        elif atom == "ai":
            if self.token("AiSettingPaused", "AiSettingUnpaused", "AiSettingAggression") == CODES["AiSettingAggression"]:
                self.parse("quantity")
        elif atom == "token":
            self.take()
        else:
            raise ValueError(f"unknown grammar atom {atom}")


def trigger_chunks(path):
    data = path.read_bytes()
    magic, directory, length, _, _ = struct.unpack_from("<4s4I", data)
    if magic != b"WAD>":
        raise ValueError("invalid archive")
    index = zlib.decompress(data[directory:])
    if len(index) != length:
        raise ValueError("invalid directory length")
    offset = 20
    for at in range(0, length, 28):
        _, tag, stored, _, children, flags, _ = struct.unpack_from("<4s4s5I", index, at)
        if not children:
            if tag == b"GIRT":
                chunk = data[offset:offset + stored]
                yield zlib.decompress(chunk) if flags & 2 else chunk
            offset += stored


# Runtime dispatch coverage is mechanically checked. Presentation parity is reviewed separately:
# these entries have working hosts, but their native rendering has not been compared frame for frame.
PRESENTATION_REVIEW = {
    "ActionTurnBeaconAtLocation": "Native texture, fade, light and tapered beam; exact tilted primitive transform remains to compare.",
    "ActionSetWeather": "Native drop table, motion, rain/thunder and fog; atmospheric light tint and draw-detail parity remain to compare.",
    "ActionDisplayVariable": "Live side counter host; native value-list typography/format layout remains to compare.",
    "ActionDisplayGameTimer": "Countdown hides itself at zero; native value-list typography remains to compare.",
}
EDITOR_ONLY = {"ActionMusicReset", "ActionMusicAddToMood", "ActionMusicTurn", "OrderGoToLocationInFormation",
               "OrderAttackLocationInFormation", "OrderGuardLocationInFormation"}

def audit(root):
    source = (SOURCE / "engine/trigger.cpp").read_text(encoding="utf8")
    implemented = set(re.findall(r"case tc::((?:Condition|Action|Order)\w+)\s*:", source))
    counts = collections.Counter()
    uses = collections.defaultdict(list)
    failures, triggers, maps = [], 0, 0
    for path in sorted(root.glob("maps/**/*.scp")):
        for chunk in trigger_chunks(path):
            maps += 1
            at = 4
            for index in range(struct.unpack_from("<I", chunk)[0]):
                flags, sides, conditions, actions = struct.unpack_from("<4I", chunk, at)
                at += 16
                triggers += 1
                for kind, size in (("condition", conditions), ("action", actions)):
                    decoder = Decoder(struct.unpack_from(f"<{size // 4}I", chunk, at))
                    at += size
                    try:
                        while decoder.at != len(decoder.words):
                            decoder.parse(kind)
                    except (ValueError, KeyError) as error:
                        failures.append({"map": path.name, "trigger": index, "stream": kind, "word": decoder.at,
                                         "error": str(error)})
                    for instruction in decoder.instructions:
                        name = instruction["name"]
                        counts[name] += 1
                        uses[name].append({"map": path.name, "trigger": index, "stream": kind, "word": instruction["word"]})
            if at != len(chunk):
                failures.append({"map": path.name, "error": "trailing trigger bytes"})
    rows = [{"kind": kind, "code": code, "name": name, "implemented": name in implemented,
             "uses": counts[name], "locations": uses[name],
             "operands": {"Condition": CONDITIONS, "Action": ACTIONS, "Order": ORDERS}[kind][name.removeprefix(kind)],
             "presentation_review": PRESENTATION_REVIEW.get(name), "editor_only": name in EDITOR_ONLY}
            for kind, names in NAMES.items() for code, name in sorted(names.items())]
    return {"maps": maps, "triggers": triggers, "instructions": sum(counts.values()), "decode_errors": failures,
            "vocabulary": rows}


def markdown(report):
    lines = ["# Trigger instruction inventory", "",
             "Generated with `tools/trigger_inventory.py`; reads every stored branch without playing a mission.", "",
             f"{report['maps']} installed maps; {report['triggers']:,} triggers; {report['instructions']:,} condition/action/order instructions; "
             f"{len(report['decode_errors'])} decode errors.", "",
             "The JSON output includes every occurrence (map, trigger index, stream and word offset). The table includes unused named instructions too. "
             "`Handler` means the runtime has a dispatch case; it does not assert full engine or visual parity.", "",
             "Enum vocabulary: [trigger_codes.hpp](../include/sacrifice/trigger_codes.hpp); names follow the original ScapeX editor's trigger phrases. "
             "Runtime semantics were checked against Sacrifice.exe and game3d.dll in the installed game.", ""]
    for kind in ("Condition", "Action", "Order"):
        rows = [r for r in report['vocabulary'] if r['kind'] == kind]
        lines += [f"## {kind}s ({len(rows)})", "", "| ID | Enum | Operands | Uses | Handler |", "|---:|---|---|---:|---|"]
        for r in rows:
            state = "yes" if r['implemented'] else "**missing**"
            if r['presentation_review']: state += "; presentation review"
            if r['editor_only']: state += "; editor only"
            lines.append(f"| {r['code']} | `{r['name']}` | `{r['operands'] or '(none)'}` | {r['uses']} | {state} |")
        lines.append("")
    lines += ["## Native details and remaining presentation work", "",
              "- Resource 132 grants souls, while native resource reads/comparisons use wizard experience. The enum exposes both contextual names.",
              "- Action 49 is obsolete in the original handler: consume a TEXT operand and yield; it deliberately creates no text widget.",
              "- Actions 260/261 only append speech-history recordings; they never start narration on entering a map.",
              "- Building active level (+1276) is an integer, not a completion boolean.",
              "- Constraint All compares matching count with the side's full creature/wizard or primary-building population; resource comparisons use the maximum, stat comparisons use the stat itself.",
              "- Special entity selectors have their own little-endian character convention and named EntityType constants; do not create them with FourCC::from_text.",
              "- AI pause/unpause and aggression use the existing side controller's flag 8 and scale.",
              "- Editor-only codes are named and dispatched, but are absent from the installed original executable's instruction tables and unused by all installed maps."]
    lines += [f"- `{name}`: {note}" for name, note in PRESENTATION_REVIEW.items()]
    lines += ["", "Operand grammar is defined beside the decoder. Typed IDs, counts, flags and FourCC values are never counted as instructions. "
              "The audit requires no original executable. Unknown/truncated instructions are errors; `--strict` also rejects a missing runtime handler.", ""]
    return "\n".join(lines)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    parser.add_argument("--strict", action="store_true", help="fail for any named instruction without a runtime handler")
    args = parser.parse_args()
    report = audit(args.game_dir)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf8")
    if args.markdown:
        args.markdown.write_text(markdown(report), encoding="utf8")
    print(f"{report['maps']} maps, {report['triggers']} triggers, {report['instructions']} instructions; "
          f"{len(report['decode_errors'])} decode errors")
    for row in report["vocabulary"]:
        if not row["implemented"]:
            print(f"MISSING {row['code']:3} {row['name']}: {row['uses']} uses")
    for error in report["decode_errors"][:12]:
        print(error)
    missing = [row for row in report["vocabulary"] if not row["implemented"]]
    print(f"Handlers: {len(report['vocabulary']) - len(missing)}/{len(report['vocabulary'])}; "
          f"presentation reviews: {len(PRESENTATION_REVIEW)}")
    raise SystemExit(bool(report["decode_errors"] or (args.strict and missing)))
