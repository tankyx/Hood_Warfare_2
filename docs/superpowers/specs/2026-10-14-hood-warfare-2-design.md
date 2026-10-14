# Hood Warfare 2 — read-only ESP overlay (design)

Date: 2026-10-14
Status: awaiting review
Author: pair (tanguy + agent)

## Context

Three sibling repos establish the pattern this project copies:

| repo | target | engine | output |
|---|---|---|---|
| `Halo-MCC` | Halo 3 in MCC | in-house | full overlay: boxes, bones, bars, aim |
| `Halo-Infinite` | Halo Infinite MP | in-house | RE map + probes + player-box overlay |
| `CS2Miam` | CS2 | Source 2 | overlay + aim (Arduino HID) |
| `YOLOv8-TensorRT-CPP` | CS2 / Valorant | — | overlay + aim (`/dev/uinput`) |

All share one contract, and this project keeps it:

> **Read the game through `/proc/<pid>/mem`; never inject, hook, or patch the
> game process. Output input through an external device** — `/dev/uinput`
> virtual mouse (YOLO) or a serial HID microcontroller (CS2Miam) — **never
> into the game.**

Hood Warfare 2 is the first **Unreal Engine 5.8.3** target. Halo's data arrays
are hand-rolled and fingerprinted; UE ships the same engine reflection in every
build, so the discovery work is different and mostly *static*.

## Target

- Game: **Hood Warfare 2**, Steam appid **5155360**
- Install: `~/.local/share/Steam/steamapps/common/Hood Warfare 2`
- Binary: `HW2/Binaries/Win64/HW2-HoodWarfare2-Win64-Shipping.exe` (193 MB, PE32+)
- Engine: **UE 5.8.3** (`++UE5+Release-5.8`)
- Content: IoStore (`HW2-Windows.ucas` 13.7 GB + `.utoc`, `global.ucas`)
- Runtime: Proton/Wayland on Linux
- **Anti-cheat: EOS Anti-Cheat** (`EOS_AntiCheatClient_*`, `bEnableAntiCheat`,
  `EHW2AntiCheatLocalState`). No EAC/BattlEye binaries on disk; EOS AC is
  SDK-integrated.
- Not yet launched on this machine (`LastPlayed 0`) — the Proton prefix is
  created on first run.

## Goal

A click-through Wayland overlay that draws player ESP (boxes, name, distance,
team colour, health) and provides external aim assist, driven by a read-only
memory reader. Plus the RE write-up that documents every address and offset.

## Non-goals

- No injection, hooks, trampolines, DLL sideloading, or process patching.
- No asset extraction / pak unpacking.
- No bypass of EOS Anti-Cheat.
- No game-file modification.

## Approach (approved: A — static-signature-first, live-verified)

1. **Static.** Ghidra the shipping exe to locate the two engine globals every
   UE build has:
   - `FUObjectArray` (**GObjects**) — the global object array
   - `FNamePool` (**GNames**) — the name pool
   Found by string xref (`"Object"`, `"UWorld"`) and AOB signature, expressed
   as `module_base + RVA` so they survive ASLR and restarts.
2. **Live verify.** A Python probe attaches to the Proton process and confirms
   each RVA resolves, then enumerates objects and resolves names.
3. **Reflection walk.** Use the live object array to identify the concrete
   classes (`UWorld`, `AGameStateBase`, `APlayerState`, `APawn`/`ACharacter`,
   `APlayerCameraManager`) and derive the field offsets from `UClass`
   reflection; verify each against live values.
4. **Reader + overlay.** Port the verified constants to `src/hw2_mem.h`,
   draw with the shared `Wayland_Game_Overlay`, aim via `/dev/uinput`.

Rejected: live-only fingerprinting (UE's chunked object/name arrays are far
less fingerprintable than Halo's self-describing arrays) and off-the-shelf
dumpers (UE4SS/Dumper-7 target ≤5.5 and inject).

### UE 5.x layout we expect to rely on

| concept | shape |
|---|---|
| `FUObjectArray` | `ObjObjects` = chunked ptr array; chunk size 0x10000; `FUObjectItem` 0x18 bytes |
| `FNamePool` | blocks of 0x20000 bytes; 2-byte entry header; index = block << 16 \| offset |
| world→screen | `ULocalPlayer → PlayerController → PlayerCameraManager → CameraCachePrivate.POV` (`FMinimalViewInfo`: Location, Rotation, FOV, AspectRatio); project ourselves |
| players | `UWorld → GameState → PlayerArray` (`TArray<APlayerState*>`) → `PawnPrivate`, `PlayerNamePrivate`, `TeamId` |

These are the *expected* shapes; the spec is not authoritative until each is
verified live and written up in `HOOD_WARFARE_2.md`.

## Deliverables

```
HOOD_WARFARE_2.md      RE write-up: every address, offset, and how it was derived
hw2_probe.py           attach harness + GObjects/FNamePool discovery
hw2_objects.py         live reflection census / actor dump
hw2_players.py         PlayerArray: team, name, pawn, position, health
hw2_w2s.py             camera POV + world→screen projection
src/hw2_mem.h          the C++ reader (base+RVA, no injection)
src/hw2_overlay.cpp    the overlay: boxes, labels, health, aim
src/hw2_aim.h          VirtualMouse (/dev/uinput) + MouseButtons (evdev gate)
src/x11_window.h       game window / monitor detection (copied from Halo)
third_party/Wayland_Game_Overlay   submodule, shared with both Halo repos
CMakeLists.txt         mirrors Halo-Infinite
README.md              usage + anti-cheat note
```

## Phases

Each phase ends with one runnable check.

1. **Scaffold** — `git init`, add the `Wayland_Game_Overlay` submodule, CMake
   skeleton, copy `x11_window.h`.
2. **Attach harness** (`hw2_probe.py`) — find the Proton process, map module
   base, verify `/proc/<pid>/mem` reads. *Check: reads a known module header.*
3. **GObjects + FNamePool** — Ghidra pass, then live verify. *Check: enumerate
   >100k objects and resolve class names.*
4. **Reflection census** (`hw2_objects.py`) — find `UWorld` and the player
   classes; derive field offsets. *Check: dumps a class with sane fields.*
5. **Players** (`hw2_players.py`) — team, name, pawn, world position, health
   (health only if it is replicated client-side; else documented as absent).
   *Check: prints the local player's own name/position in a match.*
6. **World→screen** (`hw2_w2s.py`) — camera POV → projection. *Check: a known
   world point projects to a stable screen pixel while moving the camera.*
7. **C++ reader** (`src/hw2_mem.h`) — port verified constants. *Check: matches
   the Python output for the same frame.*
8. **Overlay + aim** (`src/hw2_overlay.cpp`, `src/hw2_aim.h`) — boxes, labels,
   health, uinput aim gated on the physical mouse. *Check: boxes track players;
   aim only fires while the gate button is held.*
9. **Write-up + README** — `HOOD_WARFARE_2.md`, README.

## Verification

- Phases 2–6: Python probes print values that can be checked by eye against
  the running game.
- Phase 7: C++ constants must reproduce the Python projection to <1 px.
- Phase 8: overlay boxes must sit on players in a live match; toggle aim off
  and confirm zero synthetic input.
- Every memory claim in `HOOD_WARFARE_2.md` must cite the phase/probe that
  produced it; unverified guesses are labelled as such.

## Anti-cheat and safety

- Reads are `/proc/<pid>/mem` only. EOS AC keys on client integrity and
  process/module tampering, not arbitrary reads; even so, a live-service match
  can disagree — the README says so, as both Halo repos do.
- Input never enters the game process. Aim uses `/dev/uinput` (YOLO/Halo-MCC
  method), gated on the physical mouse via `EVIOCGKEY`; an Arduino/serial HID
  backend (CS2Miam method) is a documented alternative.
- Aim assist defaults **off**; ESP is the default.
- No anti-cheat bypass is implemented or attempted.

## Risks / open questions

- **UE 5.8 is very new.** Public signatures don't exist; ours come from the
  Ghidra pass. If `FUObjectArray` is found only via an indirect pointer, an
  extra dereference may be needed — resolved in phase 3.
- **Health may not be replicated** to the client. If not, ESP shows boxes +
  name + team only; documented, not faked.
- **EOS AC under Proton.** Behaviour is untested here; the design assumes
  reads are tolerated and input is safe, and the README states the risk.
- The game must be launched manually when phase 3 starts (never run before).

## Execution

Architectural path: this spec → user review → `writing-plans` → implementation
plan → user selects execution method.
