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

_(filled in by Task 2)_

## World & actors

_(filled in by Task 3)_

## Players

_(filled in by Task 4)_

## Camera & projection

_(filled in by Task 5)_
