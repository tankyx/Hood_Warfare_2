#!/usr/bin/env python3
"""Camera POV and world->screen projection for Hood Warfare 2.

Reads the live APlayerCameraManager's FMinimalViewInfo (Location/Rotation/FOV as
UE5 Large-World doubles) and projects world points to render pixels.
"""
import argparse
import math
import sys

import hw2_mem as H

POV_OFF = 0x350          # APlayerCameraManager -> FMinimalViewInfo
LOC_OFF = 0x00           # FVector Location (3 doubles)
ROT_OFF = 0x18           # FRotator Rotation (3 doubles: pitch, yaw, roll)
FOV_OFF = 0x30           # float FOV


def camera_pov(proc, objs, total):
    for i in range(total):
        o = objs.at(i)
        if objs.class_name(o) == "PlayerCameraManager" and not objs.name_of(o).startswith("Default__"):
            pov = o + POV_OFF
            raw = proc.read(pov, 0x34)
            if len(raw) < 0x34:
                return None
            import struct
            loc = struct.unpack_from("<ddd", raw, LOC_OFF)
            rot = struct.unpack_from("<ddd", raw, ROT_OFF)
            fov = struct.unpack_from("<f", raw, FOV_OFF)[0]
            if all(v == v for v in loc) and 1.0 < fov < 179.0:
                return loc, rot, fov
    return None


def world_to_screen(p, loc, rot, fov, w, h, fov_is_horizontal=True):
    pitch, yaw = math.radians(rot[0]), math.radians(rot[1])
    cp, sp, cy, sy = math.cos(pitch), math.sin(pitch), math.cos(yaw), math.sin(yaw)
    fwd = (cp * cy, cp * sy, sp)
    right = (-sy, cy, 0.0)
    up = (-sp * cy, -sp * sy, cp)
    d = (p[0] - loc[0], p[1] - loc[1], p[2] - loc[2])
    xd = sum(a * b for a, b in zip(d, right))
    yd = sum(a * b for a, b in zip(d, up))
    zd = sum(a * b for a, b in zip(d, fwd))
    if zd <= 0.001:
        return None
    aspect = w / h
    fov_y = math.radians(fov)
    if fov_is_horizontal:
        fov_y = 2.0 * math.atan(math.tan(fov_y / 2.0) / aspect)
    f = (h / 2.0) / math.tan(fov_y / 2.0)
    return (w / 2.0 + xd * f / zd, h / 2.0 - yd * f / zd)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--point", help="x,y,z world point")
    ap.add_argument("--render", default="1920x1080", help="WxH (default 1920x1080)")
    ap.add_argument("--fov-mode", choices=("h", "v"), default="h")
    args = ap.parse_args()

    proc = H.Process()
    if not proc.open():
        return 1
    names = H.Names(proc)
    objs = H.Objects(proc, names)
    total = objs.count()

    cam = camera_pov(proc, objs, total)
    if not cam:
        print("no camera POV (in a match?)", file=sys.stderr)
        return 1
    loc, rot, fov = cam
    w, h = (int(v) for v in args.render.lower().split("x"))
    print(f"pov loc=({loc[0]:.1f}, {loc[1]:.1f}, {loc[2]:.1f}) "
          f"rot=(pitch {rot[0]:.2f}, yaw {rot[1]:.2f}, roll {rot[2]:.2f}) fov={fov:.2f}")

    if args.point:
        p = tuple(float(v) for v in args.point.split(","))
    else:
        # default: 500 units straight ahead of the camera
        pitch, yaw = math.radians(rot[0]), math.radians(rot[1])
        fwd = (math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch))
        p = tuple(loc[i] + fwd[i] * 500 for i in range(3))
        print(f"using point 500 ahead: ({p[0]:.1f}, {p[1]:.1f}, {p[2]:.1f})")

    r = world_to_screen(p, loc, rot, fov, w, h, args.fov_mode == "h")
    print("screen=" + (f"({r[0]:.1f}, {r[1]:.1f})" if r else "offscreen/behind"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
