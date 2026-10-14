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
| `hw2_mem.py` | attach harness: find the process, map the module, read memory |

## Smoke test

With the game running:

```sh
python3 hw2_mem.py
# pid=<pid> base=0x140000000 header=b'MZ'
```
