# Hood Warfare 2 — Overlay Implementation Plan (Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A read-only C++ ESP overlay for Hood Warfare 2 that draws player boxes, using the constants produced by the reverse-engineering plan.

**Architecture:** A C++ port of the Python reader (`src/hw2_mem.h`) reads GObjects/FNames through `/proc/<pid>/mem`. `src/hw2_overlay.cpp` projects the live player list and paints via the shared `Wayland_Game_Overlay`; aim output goes through the shared `PhysicalHidMouse`.

**Tech Stack:** C++17, CMake, `Wayland_Game_Overlay` + `PhysicalHidMouse` (sibling checkouts), X11/RandR for window placement.

**Spec:** `docs/superpowers/specs/2026-10-14-hood-warfare-2-design.md`
**Inputs:** the `##` sections of `HOOD_WARFARE_2.md` (Plan 1 output).

## Global Constraints

- Read-only: `/proc/<pid>/mem` only; no injection, no hooks, no writes.
- Aim output only through a real USB HID mouse (PhysicalHidMouse); ESP is the default, aim is opt-in.
- Every constant mirrors `HOOD_WARFARE_2.md`; a change there changes `src/hw2_mem.h`.

## Files

| file | role |
|---|---|
| `src/hw2_mem.h` | reader: GObjects, FNamePool, class chain, players, camera |
| `src/hw2_overlay.cpp` | ESP: boxes, labels, stats; optional aim |
| `src/hw2_aim.h` | `phm::PhysicalHidMouse` + evdev LMB gate |
| `src/x11_window.h` | game window / monitor detection (appid 5155360) |
| `CMakeLists.txt` | links `../Wayland_Game_Overlay`, `../PhysicalHidMouse` |

## Tasks

- [x] **Task 1: C++ reader.** Port the verified constants and read paths.
      Check: `./build/hw2_overlay --dump` prints the camera and the live player list.
- [x] **Task 2: ESP overlay.** Project each Character instance to a box; team
      colour from the class-name suffix; labels + stats.
      Check: boxes appear on the game window and track players.
- [x] **Task 3: Aim (opt-in).** `--aim` moves a real USB HID mouse toward the
      nearest enemy, gated on the physical LMB.
      Check: pointer moves only while LMB is held; no uinput.
