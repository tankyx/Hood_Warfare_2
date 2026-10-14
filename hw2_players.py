#!/usr/bin/env python3
"""Live player list for Hood Warfare 2.

Walks PlayerControllers in GObjects, reads each one's PlayerState and Pawn, and
prints the pawn's world position. The local player's controller is reached
through the live PlayerCameraManager (PCOwner). Read-only.
"""
import argparse
import sys

import hw2_mem as H


def live_controllers(objs, total):
    out = []
    for i in range(total):
        o = objs.at(i)
        name = objs.name_of(o)
        if name.startswith("Default__"):
            continue
        chain = objs.class_chain(o)
        if "PlayerController" in chain and "PlayerController" not in [objs.name_of(o)]:
            out.append((o, name, chain))
    return out


def find_fstring(proc, obj, limit=0x400):
    for off in range(0, limit, 8):
        num = proc.i32(obj + off + 8)
        if not (1 < num <= 64):
            continue
        data = proc.u64(obj + off)
        if not (0x10000000000 <= data < 0x7FFFFFFFFF00):
            continue
        s = proc.fstring(obj + off)
        if s and all(32 <= ord(c) < 127 for c in s):
            return off, s
    return None, None


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

    # local controller via the camera manager's PCOwner
    local_ctrl = 0
    for i in range(total):
        o = objs.at(i)
        if objs.class_name(o) == "PlayerCameraManager" and not objs.name_of(o).startswith("Default__"):
            local_ctrl = proc.u64(o + 0x158)
            break

    players = []
    for o, name, chain in live_controllers(objs, total):
        state = proc.u64(o + H.PLAYERCONTROLLER_PLAYERSTATE_OFF)
        pawn = proc.u64(o + H.PLAYERCONTROLLER_PAWN_OFF)
        pos = objs.translation(pawn) if pawn else None
        pname = None
        if state:
            _, pname = find_fstring(proc, state)
        players.append({
            "controller": o,
            "class": chain[0],
            "local": (o == local_ctrl),
            "name": pname,
            "pawn": pawn,
            "pos": pos,
        })

    if args.json:
        import json
        print(json.dumps([{k: (list(v) if isinstance(v, tuple) else v) for k, v in p.items()}
                          for p in players], indent=2))
        return 0

    if not players:
        print("no players (in a match?)")
        return 0
    for p in players:
        pos = "(no pawn)" if p["pos"] is None else "(%.0f, %.0f, %.0f)" % p["pos"]
        tag = "local" if p["local"] else "     "
        print(f"{tag} {p['class']:28s} name={p['name']!r} pos={pos}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
