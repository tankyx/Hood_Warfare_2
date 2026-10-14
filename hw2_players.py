#!/usr/bin/env python3
"""Live player list for Hood Warfare 2.

Enumerates live Pawn/Character instances (which includes the AI/bot characters)
and reports their world position. The local player is identified through the
live PlayerCameraManager's PCOwner -> Pawn. Read-only.
"""
import argparse
import sys

import hw2_mem as H


def local_pawn(proc, objs, total):
    for i in range(total):
        o = objs.at(i)
        if objs.class_name(o) == "PlayerCameraManager" and not objs.name_of(o).startswith("Default__"):
            ctrl = proc.u64(o + 0x158)
            if ctrl:
                return proc.u64(ctrl + H.PLAYERCONTROLLER_PAWN_OFF)
    return 0


def live_pawns(proc, objs, total):
    out = []
    for i in range(total):
        o = objs.at(i)
        name = objs.name_of(o)
        if name.startswith("Default__") or name.endswith("_GEN_VARIABLE"):
            continue
        chain = objs.class_chain(o)
        if "Character" not in chain:
            continue
        if "ActorComponent" in chain or "SceneComponent" in chain:
            continue
        if not proc.u64(o + H.ACTOR_ROOT_COMPONENT_OFF):
            continue
        pos = objs.translation(o)
        if not pos or pos == (0.0, 0.0, 0.0):
            continue  # not spawned / pending
        out.append({"obj": o, "class": chain[0], "name": name, "pos": pos})
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    proc = H.Process()
    if not proc.open():
        return 1
    names = H.Names(proc)
    objs = H.Objects(proc, names)
    total = objs.count()

    lp = local_pawn(proc, objs, total)
    players = live_pawns(proc, objs, total)
    for p in players:
        p["local"] = (p["obj"] == lp)

    if args.json:
        import json
        print(json.dumps([{**p, "obj": hex(p["obj"]), "pos": list(p["pos"])} for p in players], indent=2))
        return 0

    if not players:
        print("no players (in a match?)")
        return 0
    for p in players:
        tag = "local" if p["local"] else "     "
        print(f"{tag} {p['class']:34s} {p['name']:34s} pos=({p['pos'][0]:.0f}, {p['pos'][1]:.0f}, {p['pos'][2]:.0f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
