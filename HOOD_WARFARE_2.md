# Hood Warfare 2 — reverse-engineering notes

Target: **Hood Warfare 2**, Steam appid **5155360**.
Binary: `HW2/Binaries/Win64/HW2-HoodWarfare2-Win64-Shipping.exe`.
Engine: **Unreal Engine 5.8.3** (`++UE5+Release-5.8`).
Platform: Linux, running under Proton/Wayland.

Everything below is read-only, through `/proc/<pid>/mem`. No ptrace, no
injection. Every address is `module_base + RVA` unless stated otherwise.

> Anti-cheat: Hood Warfare 2 ships EOS Anti-Cheat (`EOS_AntiCheatClient_*`,
> `bEnableAntiCheat`). Reads are not what it keys on, but a live-service match
> can disagree. Nothing here bypasses it.

## Module base

`0x140000000` on the observed launch — the PE preferred image base, but treat it
as runtime data: read it from `/proc/<pid>/maps` (the harness does), never
hardcode it.

Status: **verified** by `python3 hw2_mem.py` (reads `MZ` at the base).

| symbol | value | how derived | status |
|---|---|---|---|
| module base | `0x140000000` | `/proc/<pid>/maps` first mapping of the exe | verified |

## Reflection globals

### FNamePool (GNames) — verified

| symbol | value | how derived | status |
|---|---|---|---|
| `FNamePool.Blocks[0]` | `module_base + 0x0AEE54D0` | the only `.data` pointer equal to the pool's block-array base; block 0 decodes to the built-in FName sequence | verified |

UE 5.8 changed the `FNameEntry` header. It is **`(Len << 6) | flags`**, with
`bIsWide` in bit 0 — *not* the classic `(Len << 1) | bIsWide`. Entries are
2-byte aligned with a `0x00` pad after an odd-length name. Decode with
`block = index >> 16`, `offset = index & 0xFFFF`, `header = u16(block_base + offset)`;
the name follows the 2-byte header, `Len` bytes (or `Len` UTF-16 units when wide).

Verified by `python3 hw2_mem.py --names`, which reproduces the built-in sequence
`None / ByteProperty / IntProperty / BoolProperty` at `0x0 / 0x6 / 0x14 / 0x22`.

### FUObjectArray (GObjects) — pending

UE 5.8 also changed the object-array layout: the classic
`Objects`/`NumElements`/`NumChunks` offsets do not hold, and no `.data` pointer
yields the old `FUObjectArray` fingerprint. `UObject` vtables live in **`.rdata`**
(not `.text`); a candidate `FUObjectItem` chunk (stride `0x18`) was observed at
`0x7ff43e490000`. Exact layout still being derived (background Ghidra pass).

## World & actors

_(filled in by Task 3)_

## Players

_(filled in by Task 4)_

## Camera & projection

_(filled in by Task 5)_
