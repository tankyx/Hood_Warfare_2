#!/usr/bin/env python3
"""Read-only memory layer for Hood Warfare 2 (UE 5.8.3, Steam appid 5155360).

Everything here reads the game's memory through /proc/<pid>/mem. No ptrace, no
injection, no writing. Addresses are always expressed as module_base + RVA.
"""
import glob
import os
import struct
import sys

MODULE_NAME = "HW2-HoodWarfare2-Win64-Shipping.exe"

# FNamePool: address of `Blocks[0]`. UE 5.8 FNameEntry header is
# `(Len << 6) | flags` (bit0 = bIsWide) — NOT the classic `Len << 1` — and the
# entry offset is `(index & 0xFFFF) * FNAME_STRIDE` (stride 2 on this build).
# Verified live: 0x3/0xA/0x11 -> ByteProperty/IntProperty/BoolProperty.
GNAMES_BLOCKS_RVA = 0x0AEE54D0
FNAME_STRIDE = 2

# FUObjectArray (`GObjects`) — located via the GOBJ_ES53_1 AOB signature
# (`sub rsp,28; lea rcx,[GUObjectArray]; call FUObjectArray::FUObjectArray`).
# UE 5.8 geometry: ObjObjects FIRST — {Objects +0x00, Num +0x08, Max +0x0C,
# NumChunks +0x10, MaxChunks +0x14}; FUObjectItem stride 0x18 with the UObject*
# at +0x08 (UE 5.7+ reordered item). Verified live: 81,496 objects, first ones
# Package /Script/CoreUObject, Class Object, Package /Script/Engine, Class Actor.
GOBJECTS_RVA = 0x0AFB3500
ITEM_STRIDE = 0x18
ITEM_OBJ_OFF = 0x08


class Process:
    """A handle on the running game: find the pid, map the module, read memory."""

    def __init__(self, pid=None):
        self.pid = pid if pid is not None else self.find_pid(MODULE_NAME)
        self._fd = -1

    @staticmethod
    def find_pid(module_name=MODULE_NAME):
        """Return the pid whose /proc/<pid>/maps maps `module_name`.

        Matches the mapped file path, not /proc/<pid>/comm: the Wine thread
        running the game is named "GameThread", not after the executable.
        """
        for maps in glob.glob("/proc/[0-9]*/maps"):
            try:
                with open(maps, "r", errors="replace") as fh:
                    for line in fh:
                        path = line.rstrip("\n").split(None, 5)[-1]
                        if path.endswith(module_name):
                            return int(maps.split("/")[2])
            except OSError:
                continue
        return None

    def _maps(self):
        try:
            with open(f"/proc/{self.pid}/maps", "r", errors="replace") as fh:
                for line in fh:
                    parts = line.rstrip("\n").split(None, 5)
                    if len(parts) == 6:
                        yield parts[0], parts[1], parts[2], parts[3], parts[4], parts[5]
        except OSError:
            return

    def open(self):
        if self.pid is None:
            print(f"game not running: {MODULE_NAME} not found in any /proc/<pid>/maps",
                  file=sys.stderr)
            return False
        try:
            self._fd = os.open(f"/proc/{self.pid}/mem", os.O_RDONLY)
        except PermissionError:
            print(f"cannot read /proc/{self.pid}/mem: permission denied. "
                  "Allow attaches with `sudo sysctl kernel.yama.ptrace_scope=0` "
                  "(or run under a process that is the game's parent).", file=sys.stderr)
            return False
        except OSError as exc:
            print(f"cannot open /proc/{self.pid}/mem: {exc}", file=sys.stderr)
            return False
        return True

    def close(self):
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1

    def base(self):
        """Lowest mapping address of the game module (the PE preferred base)."""
        low = None
        for rng, _perms, _off, _dev, _inode, path in self._maps():
            if not path.endswith(MODULE_NAME):
                continue
            start = int(rng.split("-")[0], 16)
            if low is None or start < low:
                low = start
        return low or 0

    def read(self, addr, n):
        if self._fd < 0 or addr <= 0 or n <= 0:
            return b""
        try:
            data = os.pread(self._fd, n, addr)
        except OSError:
            return b""
        return data if len(data) == n else b""

    def _unpack(self, fmt, addr):
        size = struct.calcsize(fmt)
        data = self.read(addr, size)
        return struct.unpack(fmt, data)[0] if data else 0

    def u8(self, addr):
        return self._unpack("<B", addr)

    def u16(self, addr):
        return self._unpack("<H", addr)

    def u32(self, addr):
        return self._unpack("<I", addr)

    def u64(self, addr):
        return self._unpack("<Q", addr)

    def i32(self, addr):
        return self._unpack("<i", addr)

    def f32(self, addr):
        return self._unpack("<f", addr)

    def cstr(self, addr, maxlen=64):
        out = bytearray()
        while len(out) < maxlen:
            ch = self.read(addr + len(out), 1)
            if not ch or ch == b"\x00":
                break
            out += ch
        return out.decode("latin-1")

    def fstring(self, addr):
        """A UE FString: TArray<wchar_t> — data ptr at +0x0, num i32 at +0x8."""
        data = self.u64(addr)
        num = self.i32(addr + 8)
        if not data or num <= 1 or num > 4096:
            return ""
        raw = self.read(data, num * 2)
        try:
            return raw.decode("utf-16-le").rstrip("\x00")
        except UnicodeDecodeError:
            return ""


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--names", action="store_true", help="resolve a few built-in FNames")
    ap.add_argument("--census", action="store_true", help="enumerate GObjects")
    ap.add_argument("--find", metavar="CLASS", help="count objects of a class")
    args = ap.parse_args()

    proc = Process()
    if not proc.open():
        return 1
    base = proc.base()
    header = proc.read(base, 2)
    print(f"pid={proc.pid} base=0x{base:x} header={header!r}")
    if header != b"MZ":
        print(f"unexpected header at module base: {header!r}", file=sys.stderr)
        return 1

    if args.names:
        names = Names(proc)
        expected = {0x0: "None", 0x3: "ByteProperty", 0xA: "IntProperty", 0x11: "BoolProperty"}
        for idx, want in expected.items():
            got = names.resolve(idx)
            status = "ok" if got == want else "BAD"
            print(f"  FName[0x{idx:04x}] = {got!r} ({status})")
        if any(names.resolve(i) != w for i, w in expected.items()):
            return 1

    if args.census or args.find:
        names = Names(proc)
        objects = Objects(proc, names)
        total = objects.count()
        print(f"GObjects=0x{objects.addr:x} count={total} max={objects.max()}")
        if args.find:
            hits = [objects.name_of(objects.at(i))
                    for i in range(total)
                    if objects.class_name(objects.at(i)) == args.find]
            print(f"  class {args.find!r}: {len(hits)} objects")
            for nm in hits[:10]:
                print(f"    {nm!r}")
        else:
            for i in range(min(total, 8)):
                print(f"  [{i}] {objects.name_of(objects.at(i))!r} ({objects.class_name(objects.at(i))})")
    return 0


class Names:
    """FNamePool resolver. UE 5.8: header = (Len << 6) | flags, bit0 = bIsWide."""

    def __init__(self, proc: "Process", blocks_rva: int = GNAMES_BLOCKS_RVA):
        self.proc = proc
        self.blocks = proc.base() + blocks_rva

    def resolve(self, index: int) -> str:
        if index <= 0:
            return "None" if index == 0 else ""
        block, offset = index >> 16, (index & 0xFFFF) * FNAME_STRIDE
        if block > 256:  # bounds check (Review Focus 5)
            return ""
        base = self.proc.u64(self.blocks + block * 8)
        if not base:
            return ""
        header = self.proc.u16(base + offset)
        length = header >> 6
        if length <= 0 or length > 256:
            return ""
        raw = self.proc.read(base + offset + 2, length * 2 if header & 1 else length)
        if not raw:
            return ""
        try:
            return raw.decode("utf-16-le" if header & 1 else "latin1")
        except UnicodeDecodeError:
            return ""


class Objects:
    """FUObjectArray (UE 5.8): ObjObjects first; item stride 0x18, object at +0x08."""

    def __init__(self, proc: "Process", names: "Names", array_rva: int = GOBJECTS_RVA):
        self.proc = proc
        self.names = names
        self.addr = proc.base() + array_rva

    def count(self) -> int:
        return self.proc.i32(self.addr + 0x08)

    def max(self) -> int:
        return self.proc.i32(self.addr + 0x0C)

    def at(self, index: int) -> int:
        chunk_table = self.proc.u64(self.addr)
        chunk = self.proc.u64(chunk_table + (index >> 16) * 8)
        return self.proc.u64(chunk + (index & 0xFFFF) * ITEM_STRIDE + ITEM_OBJ_OFF)

    def name_of(self, obj: int) -> str:
        return self.names.resolve(self.proc.u32(obj + 0x18))

    def class_of(self, obj: int) -> int:
        return self.proc.u64(obj + 0x10)

    def class_name(self, obj: int) -> str:
        cls = self.class_of(obj)
        return self.names.resolve(self.proc.u32(cls + 0x18)) if cls else ""


if __name__ == "__main__":
    sys.exit(main())
