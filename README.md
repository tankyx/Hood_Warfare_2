# Hood Warfare 2

Read-only memory tooling for **Hood Warfare 2** (Steam appid **5155360**), in the
spirit of [Halo-Infinite](https://github.com/tankyx/Halo-Infinite).

Unreal Engine **5.8.3**, Proton/Wayland on Linux. No injection, no hooks, no
patching: the game is read through `/proc/<pid>/mem`.

> **Status: reverse engineering.** What exists is the memory layer and its
> probes. See [`HOOD_WARFARE_2.md`](HOOD_WARFARE_2.md) for every offset and how
> it was derived.

## Probes

| file | what |
|---|---|
| `hw2_mem.py` | memory layer: attach, GObjects, FNamePool, object/class reads |
| `hw2_players.py` | live player list: name, pawn, world position |
| `hw2_w2s.py` | camera POV + world->screen projection |

## Smoke test

With the game running:

```sh
python3 hw2_mem.py              # pid=<pid> base=0x140000000 header=b'MZ'
python3 hw2_mem.py --names      # FNamePool sanity check
python3 hw2_mem.py --census     # GObjects: count + first objects
python3 hw2_mem.py --find World # live UWorld instance(s)

# in a match:
python3 hw2_players.py          # local player name + position
python3 hw2_w2s.py              # camera POV, projection self-check
```
