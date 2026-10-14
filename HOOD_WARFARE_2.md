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
as runtime data: read it from `/proc/<pid>/maps`, never hardcode it. Verified by
`python3 hw2_mem.py` (reads `MZ` at the base).

## Reflection globals

| symbol | value | how derived | status |
|---|---|---|---|
| `FNamePool.Blocks[0]` | `module_base + 0x0AEE54D0` | pointer to a block whose first entries are the built-in FNames; also the `GNAM_ES53_1` AOB target (`lea rcx,[FNamePool]`) | verified |
| `GObjects` (`FUObjectArray`) | `module_base + 0x0AFB3500` | `GOBJ_ES53_1` AOB (`sub rsp,28; lea rcx,[GUObjectArray]; call FUObjectArray::FUObjectArray`), then live-scored over 96 candidates | verified |

### FNamePool (GNames)

UE 5.8 changed the `FNameEntry` header **and** the index encoding:

- Header = **`(Len << 6) | flags`**, bit 0 = `bIsWide` — *not* the classic `(Len << 1)`.
- Entry byte offset = **`(index & 0xFFFF) * 2`** (stride 2), block = `index >> 16`.
- Entries are 2-byte aligned with a `0x00` pad after an odd-length name.

Verified by `python3 hw2_mem.py --names`: `0x0/0x3/0xA/0x11` →
`None / ByteProperty / IntProperty / BoolProperty`.

### FUObjectArray (GObjects) and FUObjectItem

UE 5.8 also reordered the object array. Unlike UE 5.0–5.7 (`Objects` at `+0x10`,
`NumElements` at `+0x24`), **UE 5.8 puts `ObjObjects` first**:

| field | offset |
|---|---|
| `Objects` (chunk table ptr) | `+0x00` |
| `NumElements` | `+0x08` |
| `MaxElements` | `+0x0C` |
| `NumChunks` | `+0x10` |
| `MaxChunks` | `+0x14` |

And the `FUObjectItem` (UE 5.7+ reordered) is **0x18 bytes with the `UObject*`
at `+0x08`** (flags/refcount first), not `+0x00`.

Read path: `chunk = *(Objects + (index >> 16) * 8)`;
`item = chunk + (index & 0xFFFF) * 0x18`; `obj = *(item + 0x08)`.

`UObject` fields used: `ClassPrivate` at `+0x10`, `NamePrivate` (FName
comparison index) at `+0x18`, `OuterPrivate` at `+0x20`.

Verified by `python3 hw2_mem.py --census`: **81,496 objects**, the first ones
being `Package /Script/CoreUObject`, `Class Object`, `Package /Script/Engine`,
`Class Actor`.

## World & actors

_(filled in by Task 3)_

## Players

_(filled in by Task 4)_

## Camera & projection

_(filled in by Task 5)_
