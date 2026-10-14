# Hood Warfare 2 — Reverse Engineering and Live Probes: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Locate the Unreal Engine 5.8 reflection globals (`FUObjectArray`, `FNamePool`) inside the Hood Warfare 2 shipping binary and expose live class, player and camera data through read-only Python probes, documenting every offset in `HOOD_WARFARE_2.md`.

**Architecture:** A stdlib-only Python memory layer (`hw2_mem.py`) attaches to the Proton-hosted game via `/proc/<pid>/mem` — no ptrace, no injection — resolves `FUObjectArray` and `FNamePool` from `module_base + RVA` values found with Ghidra, and walks UE reflection to enumerate `UObject`s and resolve `FName`s. Three thin probes (`hw2_objects.py`, `hw2_players.py`, `hw2_w2s.py`) build on it to census classes, list players with world positions, and project world→screen.

**Tech Stack:** Python 3 (stdlib only), `/proc/<pid>/mem`, Ghidra (headless, via the `ghidra` tool) for the static pass, Unreal Engine 5.8.3.

**Spec:** `docs/superpowers/specs/2026-10-14-hood-warfare-2-design.md`

## Global Constraints

- **Read-only.** Only `os.pread` on `/proc/<pid>/mem`. Never write to the game process, never `ptrace`, never inject, never patch files.
- **Python 3.12+, stdlib only.** No pip dependencies (`struct`, `os`, `sys`, `argparse`, `re`, `glob` are enough).
- **Target:** UE 5.8.3, Steam appid 5155360, module `HW2-HoodWarfare2-Win64-Shipping.exe`.
- **Every address is documented as `module_base + RVA`** (ASLR-safe) in `HOOD_WARFARE_2.md`, together with the probe/task that produced it.
- **Unverified values are labelled `UNVERIFIED`** in the write-up and never presented as fact.
- **The game must be running for live steps.** Probes exit non-zero with actionable guidance when it is not.
- **No hardcoded absolute addresses.** Only RVAs relative to the module base.

## Review Focus

The input classes and failure modes the spec implies but no task's happy-path test exercises — each has a test added to the task that owns the code, in that task's step style.

1. **Game not running, or `/proc/<pid>/mem` denied** (Yama `ptrace_scope`, or a non-descendant process) — the probe must print the exact fix (`sysctl kernel.yama.ptrace_scope=0`, or run as a descendant) and exit 1, never dump a traceback.
2. **Process name is truncated** to 15 chars in `/proc/<pid>/comm` (`HW2-HoodWarfare`) — discovery must match on `/proc/<pid>/maps` / `cmdline`, never on the process name.
3. **Wrong UE layout assumptions** (a 32-bit `FUObjectItem`, an 8-byte stride, an off-by-8-chunks index) — sanity checks must fail loudly (object count out of range, first object name not printable ASCII) rather than silently returning garbage.
4. **No world loaded** (main menu / loading screen) — the player walk must report `no world` or `no players` and exit 0, not crash on a null `GameState`.
5. **Corrupted / out-of-range `FName` index** — the name pool decoder must bounds-check the block and offset and return a placeholder, never index past the pool.

---

### Task 1: Attach harness — find the process, map the module, read memory

**Files:**
- Create: `hw2_mem.py`
- Create: `.gitignore`
- Create: `HOOD_WARFARE_2.md`
- Create: `README.md` (stub — expanded in Task 6)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `MODULE_NAME: str = "HW2-HoodWarfare2-Win64-Shipping.exe"`
  - `class Process` with `__init__(self, pid: int | None = None)`, `find_pid(module_name: str) -> int | None`, `open(self) -> bool`, `base(self) -> int`, `read(self, addr: int, n: int) -> bytes`, `u8/u16/u32/u64/i32/f32(self, addr: int) -> int|float`, `cstr(self, addr: int, maxlen: int = 64) -> str`, `fstring(self, addr: int) -> str`
  - `main()` self-test invoked as `python3 hw2_mem.py`

- [ ] **Step 1: Write the failing self-test behaviour**

In `hw2_mem.py`, `main()` must: call `Process.find_pid(MODULE_NAME)`; if `None`, print `game not running: <MODULE_NAME> not found in any /proc/<pid>/maps` to stderr and `return 1`; else open the process, read 2 bytes at `base()`, and print `pid={pid} base=0x{base:x} header={header!r}`. Assert the header equals `b"MZ"` and `return 0` on success.

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 hw2_mem.py`
Expected: fails (module has no `find_pid` / `Process` yet) with `NameError`/`AttributeError`.

- [ ] **Step 3: Implement `Process`**

`find_pid` scans `glob.glob("/proc/[0-9]*/maps")`, reads each, and returns the pid whose maps contain a line ending in `MODULE_NAME` (match the file path, not `comm` — see Review Focus 2). `base()` returns the start address of the lowest mapping whose path ends in `MODULE_NAME`. `open()` does `os.open(f"/proc/{pid}/mem", os.O_RDONLY)` and, on `PermissionError`, prints the Yama guidance from Review Focus 1 and returns `False`. `read()` is `os.pread(fd, n, addr)`; return `b""` on short/failed reads. Typed readers unpack little-endian (`<B`, `<H`, `<I`, `<Q`, `<i`, `<f`). `fstring()` reads a UE `FString` (`TArray<wchar_t>`: `data` ptr at `+0x0`, `num` i32 at `+0x8`) and decodes UTF-16LE.

- [ ] **Step 4: Run the self-test against a stopped game, then a running one**

Run: `python3 hw2_mem.py` (game closed)
Expected: `game not running: ...`, exit code 1.
Run: `python3 hw2_mem.py` (game launched, at the main menu)
Expected: `pid=... base=0x... header=b'MZ'`, exit code 0. Record `base` in `HOOD_WARFARE_2.md` as `UNVERIFIED` (it is ASLR-random per launch; only the RVA is stable).

- [ ] **Step 5: Add `.gitignore` and a README stub**

`.gitignore`: `__pycache__/`, `*.pyc`. `README.md`: title, the appid, "status: reverse engineering", and the `python3 hw2_mem.py` smoke test.

- [ ] **Step 6: Commit**

```bash
git add hw2_mem.py .gitignore README.md HOOD_WARFARE_2.md
git commit -m "feat: attach harness for the Hood Warfare 2 process"
```

---

### Task 2: Locate `FUObjectArray` and `FNamePool`, enumerate objects and names

**Files:**
- Modify: `hw2_mem.py`
- Modify: `HOOD_WARFARE_2.md`

**Interfaces:**
- Consumes: `Process` from Task 1.
- Produces:
  - `GOBJECTS_RVA: int`, `GNAMES_RVA: int` (module-relative, filled from the Ghidra pass)
  - `class Names` with `__init__(self, proc: Process, pool_rva: int)`, `resolve(self, index: int) -> str`
  - `class Objects` with `__init__(self, proc: Process, names: Names, array_rva: int)`, `count(self) -> int`, `at(self, index: int) -> int`, `name_of(self, obj: int) -> str`, `class_of(self, obj: int) -> int`, `class_name(self, obj: int) -> str`, `find_all(self, class_name: str) -> list[int]`
  - CLI: `python3 hw2_mem.py --census [--find NAME]`, plus a `--scan` fallback

- [ ] **Step 1: Static pass — find the two RVAs with Ghidra**

Run: `ghidra action=analyze binary="/home/tanguy/.local/share/Steam/steamapps/common/Hood Warfare 2/HW2/Binaries/Win64/HW2-HoodWarfare2-Win64-Shipping.exe"`.

Locate `FNamePool` first (easier to confirm): search the listing for the `FName` construction path that references the `NAME_None` string, or byte-search for the pool's block array. Then locate `FUObjectArray`: search for `UObject::StaticAllocateObject` / `FUObjectArray::AllocateUObjectIndex`, or xref the string `"Object"` used by the object array. Record both as `base + RVA` (RVA = VA − preferred image base).

If neither yields a single unambiguous RVA, record the candidates and fall back to a live scan: in `hw2_mem.py`, add `--scan` that walks the module's writable data for a pointer-to-pointer array whose first entry is a live object (vtable inside the module `.text`, `NamePrivate` index resolvable). Whichever path succeeds, the value that goes into `GOBJECTS_RVA`/`GNAMES_RVA` is the one the next step verifies.

- [ ] **Step 2: Write the failing census test**

`python3 hw2_mem.py --census` must print `objects=<N> names=<M> first=<name>` and exit 0 only when `N > 100_000` and `first` is non-empty printable ASCII.

- [ ] **Step 3: Run it to verify it fails**

Run: `python3 hw2_mem.py --census`
Expected: fails because `Objects`/`Names` do not exist yet.

- [ ] **Step 4: Implement `Names` and `Objects`**

`Names.resolve`: the `FNamePool` holds 64 KiB blocks addressed by a `Block` array. `index` encodes `block = index >> 16`, `offset = index & 0xFFFF`; the entry at `pool[block] + offset` starts with a 2-byte header (length + flags) followed by the UTF-8 bytes. Bounds-check `block` and `offset` (Review Focus 5) and return `"<bad:FName:%d>" % index` on any violation.

`Objects.__init__` takes the `Names` instance it needs for `name_of`/`class_name`. The array itself is a chunked structure: `chunk = index >> 16`, `within = index & 0xFFFF`, `item_stride = 0x18`, `FUObjectItem` is at `chunks[chunk] + within*0x18`, and its first `u64` is the `UObject*`. `name_of` reads the object's `FName` index (UE 5.8: `UObject.NamePrivate` — confirm the offset live against `first`, see Review Focus 3) and resolves it via `Names`. `class_of` reads `UObject.ClassPrivate`. `count` reads `ObjObjects.NumElements`.

- [ ] **Step 5: Run the census to verify it passes**

Run: `python3 hw2_mem.py --census`
Expected: `objects=<N>` with `N > 100000`, `first` a real name (e.g. contains `Object`/`Package`), exit 0.
Also: `python3 hw2_mem.py --census --find UWorld` prints at least one object whose class is `World`.

- [ ] **Step 6: Document and commit**

Add a `## Reflection globals` section to `HOOD_WARFARE_2.md`: `GOBJECTS_RVA`, `GNAMES_RVA`, the Ghidra command, the layout assumptions, and the verifying output. Mark anything still guessed `UNVERIFIED`.

```bash
git add hw2_mem.py HOOD_WARFARE_2.md
git commit -m "feat: locate FUObjectArray and FNamePool, enumerate UObjects"
```

---

### Task 3: Reflection census — find the world and the player classes

**Files:**
- Create: `hw2_objects.py`
- Modify: `HOOD_WARFARE_2.md`

**Interfaces:**
- Consumes: `Process`, `Names`, `Objects` from Tasks 1–2.
- Produces: CLI `python3 hw2_objects.py [--class NAME] [--actor]`.

- [ ] **Step 1: Write the failing check**

`python3 hw2_objects.py` must print, in order: the number of `World` objects, the `GameStateBase` object, and `actors=<N>`; exit 0 when at least one `World` exists. With `--actor`, print the first 20 actors as `class @ 0xADDR`.

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 hw2_objects.py`
Expected: `No such file or directory`.

- [ ] **Step 3: Implement the world walk**

Find the live `UWorld` via `Objects.find_all("World")` (skip the class object whose own class is `Class`). From `UWorld`, read `PersistentLevel` and `GameState`; from `PersistentLevel`, read `Actors` (a `TArray<AActor*>`: `data` `+0x0`, `num` `+0x8`). Confirm, live, the offsets of `UWorld.GameState` and `ULevel.Actors` for 5.8 — if they differ from the expected values, record the observed ones. Group actors by class name and print a histogram with `--class`.

- [ ] **Step 4: Run it against a loaded map**

Run: `python3 hw2_objects.py --actor` (game in a match or range)
Expected: `worlds=1 ... actors=<N>` with `N > 0` and a histogram containing the expected player/pawn class names (e.g. `PlayerState`, `Character`/`Pawn`, `PlayerController`).
Run with the game at the main menu
Expected: prints `no world loaded` and exits 0 (Review Focus 4).

- [ ] **Step 5: Document and commit**

`HOOD_WARFARE_2.md`: `## World & actors` — found class names, the derived offsets for `PersistentLevel`/`GameState`/`Actors`, and the probe output.

```bash
git add hw2_objects.py HOOD_WARFARE_2.md
git commit -m "feat: census the UWorld, its game state, and its actors"
```

---

### Task 4: Players — team, name, pawn, world position, health

**Files:**
- Create: `hw2_players.py`
- Modify: `HOOD_WARFARE_2.md`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: CLI `python3 hw2_players.py [--json]`; a `Player` record `{index, name, team, local, pawn, pos: (x,y,z), health}`.

- [ ] **Step 1: Write the failing check**

`python3 hw2_players.py` must print one line per player: `[local] name=... team=... pos=(x,y,z) hp=...`, and exit 0 when at least one player is found.

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 hw2_players.py`
Expected: `No such file or directory`.

- [ ] **Step 3: Implement the player walk**

From `UWorld.GameState`, read `PlayerArray` (a `TArray<APlayerState*>`). For each `APlayerState`, read `PlayerNamePrivate` (an `FString`, resolved via `Process.fstring`), `TeamId`, and `PawnPrivate` (or `Pawn`). For the pawn, read its world position from its root component transform (locate `RootComponent` → `RelativeLocation`, or the component-to-world translation), and mark the local player via the `PlayerController`/`LocalPlayer` chain or the owning connection. Health is on the pawn/character only if it is replicated client-side — if no health field can be confirmed, set `health=None` and document "not client-replicated" rather than guessing.

- [ ] **Step 4: Run it in a match**

Run: `python3 hw2_players.py`
Expected: at least one line, the local player's own name present, positions plausible (finite floats, within the map).
Also run `python3 hw2_players.py --json` and confirm it is valid JSON (`python3 -c "import json,sys; json.load(sys.stdin)"`).

- [ ] **Step 5: Document and commit**

`HOOD_WARFARE_2.md`: `## Players` — the `PlayerArray` offset, the `PlayerState` field offsets, the pawn-position path (one line each, with the probe output that verified them). Record health as present or absent.

```bash
git add hw2_players.py HOOD_WARFARE_2.md
git commit -m "feat: read the player list with names, teams and positions"
```

---

### Task 5: Camera and world→screen projection

**Files:**
- Create: `hw2_w2s.py`
- Modify: `HOOD_WARFARE_2.md`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: CLI `python3 hw2_w2s.py [--point x,y,z] [--render WxH]`; functions `camera_pov() -> (loc, rot, fov, aspect)` and `world_to_screen(p, pov, w, h) -> (x, y, on_screen)`.

- [ ] **Step 1: Write the failing check**

`python3 hw2_w2s.py --point 0,0,0` must print `screen=(x,y) on_screen=...` and exit 0. With `--point` set to a player's world position from Task 4, the projected pixel must land inside the render rect.

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 hw2_w2s.py --point 0,0,0`
Expected: `No such file or directory`.

- [ ] **Step 3: Implement the camera read and the projection**

Read the local `PlayerController` → `PlayerCameraManager` → `CameraCachePrivate` / `CameraCache` (an `FCameraCacheEntry` whose `POV` is an `FMinimalViewInfo`: `Location` f32[3], `Rotation` f32[3] (pitch, yaw, roll), `FOV` f32). Build the view matrix from `Location`/`Rotation` and project with the vertical FOV and the render aspect. Return `on_screen=False` when the point is behind the camera (view-space `z <= 0`).

- [ ] **Step 4: Run and verify stability**

Run: `python3 hw2_w2s.py --point <a player's position from Task 4>`
Expected: a pixel inside the render rect whose box tracks the player.
Then nudge the camera (move the mouse) and re-run: the projected pixel must move in the opposite screen direction to the camera pan and stay inside the rect while the player is visible.

- [ ] **Step 5: Document and commit**

`HOOD_WARFARE_2.md`: `## Camera & projection` — the `PlayerCameraManager` path, the `FMinimalViewInfo` offsets, the FOV source, and the projection formula. Note that the horizontal/vertical FOV convention must be recorded exactly (the Halo-Infinite overlay spent its whole rework on this).

```bash
git add hw2_w2s.py HOOD_WARFARE_2.md
git commit -m "feat: read the camera POV and project world to screen"
```

---

### Task 6: Write-up and README

**Files:**
- Modify: `HOOD_WARFARE_2.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: every offset documented in Tasks 1–5.
- Produces: a complete `HOOD_WARFARE_2.md` and a usable `README.md`.

- [ ] **Step 1: Assemble the write-up**

`HOOD_WARFARE_2.md` must contain: target/version, the anti-cheat note, a summary table of every `base + RVA` and field offset with the task that produced it, per-section derivations, the projection convention, and a "not yet known / UNVERIFIED" list. Cross-check that every RVA in the code appears in the document and vice versa.

- [ ] **Step 2: Write the README**

`README.md` must list the probes with one-line descriptions, the attach smoke test, the prerequisite that the game be running, and the anti-cheat caveat copied from the spec.

- [ ] **Step 3: Verify**

Run: `python3 hw2_mem.py && python3 hw2_players.py && python3 hw2_w2s.py --point 0,0,0` with the game running.
Expected: all three exit 0.
Run: grep each `*_RVA`/offset constant in `hw2_mem.py` against `HOOD_WARFARE_2.md`.
Expected: every constant documented, every documented constant present.

- [ ] **Step 4: Commit**

```bash
git add HOOD_WARFARE_2.md README.md
git commit -m "docs: Hood Warfare 2 reverse-engineering write-up"
```

---

## What this plan does not contain

The C++ overlay (`src/hw2_mem.h`, `src/hw2_overlay.cpp`, `src/hw2_aim.h`, `CMakeLists.txt`, the `Wayland_Game_Overlay` + `PhysicalHidMouse` submodules) is **Plan 2**, written after Task 5 produces the constants it hardcodes. Its inputs are exactly the `##` sections of `HOOD_WARFARE_2.md` this plan writes.
