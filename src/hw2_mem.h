#pragma once
// Read-only Hood Warfare 2 (UE 5.8.3) memory reader.
//
// Everything here was verified live against the shipping exe; the offsets and
// their derivation are written up in HOOD_WARFARE_2.md. Summary:
//
//   GObjects          module_base + 0x0AFB3500 (GOBJ_ES53_1 AOB)
//   FNamePool.Blocks  module_base + 0x0AEE54D0 (GNAM_ES53_1 AOB)
//   FUObjectArray     ObjObjects FIRST: Objects +0x00, NumElements +0x08
//   FUObjectItem      0x18 bytes, UObject* at +0x08 (UE 5.7+ reordered item)
//   FNameEntry        header = (Len << 6) | flags; offset = (idx & 0xFFFF) * 2
//
// Reads go through /proc/<pid>/mem only: no ptrace, no injection, no writes.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace hw2 {

// ---------------------------------------------------------------- constants

constexpr const char *kModule = "HW2-HoodWarfare2-Win64-Shipping.exe";
constexpr uintptr_t kObjectsRva = 0x0AFB3500;   // FUObjectArray
constexpr uintptr_t kNamesBlocksRva = 0x0AEE54D0; // FNamePool.Blocks[0]
constexpr int kFNameStride = 2;
constexpr uint32_t kObjectsCountOff = 0x08;
constexpr uint32_t kItemStride = 0x18;
constexpr uint32_t kItemObjOff = 0x08;
constexpr uint32_t kOffClass = 0x10;
constexpr uint32_t kOffName = 0x18;
constexpr uint32_t kOffSuper = 0x40;      // UStruct::SuperStruct
constexpr uint32_t kOffRootComponent = 0x1B8;
constexpr uint32_t kOffTranslation = 0x110; // USceneComponent world translation (3 doubles)
constexpr uint32_t kOffCamPov = 0x350;    // APlayerCameraManager -> FMinimalViewInfo
constexpr uint32_t kOffCtrlPawn = 0x2E8;
constexpr uint32_t kOffCamOwner = 0x158;
// Recovered via UClass reflection (property names): PawnTeam is the real team
// (0/1), and AC_Health_C carries Health + bDead.
constexpr uint32_t kOffPawnTeam = 0x910;
constexpr uint32_t kOffHealthComp = 0x6C0;
constexpr uint32_t kOffDead = 0xF8;
constexpr uint32_t kOffHealth = 0xC8;     // AC_Health_C::Health    (double)
constexpr uint32_t kOffMaxHealth = 0xD0;  // AC_Health_C::MaxHealth (double)

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

struct Player {
    uintptr_t obj = 0;
    std::string cls;
    std::string name;
    Vec3 pos;
    int team = -1;
    bool dead = false;
    bool local = false;
    double hp = 0.0;
    double maxHp = 0.0;
};

struct Camera {
    bool ok = false;
    Vec3 loc, rot;
    float fov = 90.0f;
};

// ---------------------------------------------------------------- reader

class Reader {
public:
    bool open() {
        _pid = findPid();
        if (_pid < 0) return false;
        if (!readMaps()) return false;
        char p[64];
        std::snprintf(p, sizeof(p), "/proc/%d/mem", _pid);
        _mem = ::open(p, O_RDONLY);
        _objects = _base + kObjectsRva;
        _blocks = _base + kNamesBlocksRva;
        return _mem >= 0;
    }
    bool isOpen() const { return _mem >= 0; }
    int pid() const { return _pid; }
    uintptr_t base() const { return _base; }

    bool read(uintptr_t addr, void *dst, size_t n) const {
        if (_mem < 0 || addr < 0x10000) return false;
        return ::pread(_mem, dst, n, static_cast<off_t>(addr)) == static_cast<ssize_t>(n);
    }
    template <typename T>
    bool peek(uintptr_t addr, T &out) const { return read(addr, &out, sizeof(T)); }

    uint8_t u8(uintptr_t a) const { uint8_t v = 0; peek(a, v); return v; }
    uint16_t u16(uintptr_t a) const { uint16_t v = 0; peek(a, v); return v; }
    uint32_t u32(uintptr_t a) const { uint32_t v = 0; peek(a, v); return v; }
    int32_t i32(uintptr_t a) const { int32_t v = 0; peek(a, v); return v; }
    uintptr_t u64(uintptr_t a) const { uint64_t v = 0; peek(a, v); return v; }
    float f32(uintptr_t a) const { float v = 0; peek(a, v); return v; }

    // ---- FNamePool ----
    std::string fname(uint32_t idx) const {
        if (idx == 0) return "None";
        const uint32_t block = idx >> 16;
        const uint32_t off = (idx & 0xFFFF) * kFNameStride;
        if (block > 256) return "";
        const uintptr_t base = u64(_blocks + block * 8);
        if (!base) return "";
        const uint16_t header = u16(base + off);
        const uint32_t len = header >> 6;
        if (len == 0 || len > 256) return "";
        std::vector<char> buf(len * (header & 1 ? 2 : 1), 0);
        if (!read(base + off + 2, buf.data(), buf.size())) return "";
        if (header & 1) {
            std::string out;
            out.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                uint16_t w = static_cast<uint8_t>(buf[i * 2]) |
                             (static_cast<uint16_t>(static_cast<uint8_t>(buf[i * 2 + 1])) << 8);
                out.push_back(w < 0x80 ? static_cast<char>(w) : '?');
            }
            return out;
        }
        return std::string(buf.begin(), buf.end());
    }

    // ---- UObject / GObjects ----
    int32_t objectCount() const { return i32(_objects + kObjectsCountOff); }
    uintptr_t objectsTable() const { return _objects; }

    uintptr_t objectAt(int32_t index) const {
        const uintptr_t chunkTable = u64(_objects);
        if (!chunkTable) return 0;
        const uintptr_t chunk = u64(chunkTable + static_cast<uintptr_t>(index >> 16) * 8);
        if (!chunk) return 0;
        return u64(chunk + static_cast<uintptr_t>(index & 0xFFFF) * kItemStride + kItemObjOff);
    }

    uintptr_t classOf(uintptr_t obj) const { return u64(obj + kOffClass); }
    std::string nameOf(uintptr_t obj) const { return fname(u32(obj + kOffName)); }
    std::string className(uintptr_t obj) const { return nameOf(classOf(obj)); }

    std::vector<std::string> classChain(uintptr_t obj, int depth = 16) const {
        std::vector<std::string> out;
        uintptr_t c = classOf(obj);
        for (int i = 0; c && i < depth; ++i) {
            out.push_back(nameOf(c));
            c = u64(c + kOffSuper);
            if (std::find(out.begin(), out.end(), nameOf(c)) != out.end()) break;
        }
        return out;
    }
    bool isA(uintptr_t obj, const char *base) const {
        for (const auto &n : classChain(obj))
            if (n == base) return true;
        return false;
    }

    Vec3 translation(uintptr_t actor) const {
        const uintptr_t root = u64(actor + kOffRootComponent);
        Vec3 v;
        if (!root || !read(root + kOffTranslation, &v, sizeof(v))) return Vec3{};
        return v;
    }

    uintptr_t localPawn() const {
        const int32_t n = objectCount();
        for (int32_t i = 0; i < n; ++i) {
            const uintptr_t o = objectAt(i);
            if (!o || className(o) != "PlayerCameraManager" || nameOf(o).rfind("Default__", 0) == 0)
                continue;
            const uintptr_t ctrl = u64(o + kOffCamOwner);
            if (ctrl) return u64(ctrl + kOffCtrlPawn);
        }
        return 0;
    }

    std::vector<Player> players() const {
        std::vector<Player> out;
        const int32_t n = objectCount();
        const uintptr_t lp = localPawn();
        for (int32_t i = 0; i < n; ++i) {
            const uintptr_t o = objectAt(i);
            if (!o) continue;
            const std::string nm = nameOf(o);
            if (nm.rfind("Default__", 0) == 0 || nm.size() > 11 &&
                nm.compare(nm.size() - 11, 11, "_GEN_VARIABLE") == 0)
                continue;
            const auto chain = classChain(o);
            if (std::find(chain.begin(), chain.end(), "Character") == chain.end()) continue;
            if (std::find(chain.begin(), chain.end(), "ActorComponent") != chain.end()) continue;
            if (!u64(o + kOffRootComponent)) continue;
            Player p;
            p.obj = o;
            p.cls = chain.empty() ? "" : chain.front();
            p.name = nm;
            p.pos = translation(o);
            p.local = (o == lp);
            if (p.pos.x == 0 && p.pos.y == 0 && p.pos.z == 0) continue;  // not spawned
            out.push_back(std::move(p));
        }
        return out;
    }

    Camera camera() const {
        const uintptr_t cam = findCameraManager();
        return cam ? readCameraAt(cam) : Camera{};
    }

    // ---- cheap, cache-friendly access (the expensive full scan runs rarely) ----

    // One full pass over GObjects. Expensive (tens of thousands of reads), so
    // callers should run it every second or two and cache the result.
    void scanActors(uintptr_t &camMgr, uintptr_t &localPawn,
                    std::vector<uintptr_t> &actors) const {
        actors.clear();
        camMgr = 0;
        localPawn = 0;
        const int32_t n = objectCount();
        for (int32_t i = 0; i < n; ++i) {
            const uintptr_t o = objectAt(i);
            if (!o) continue;
            const uintptr_t cls = classOf(o);
            if (!cls) continue;
            if (!camMgr && isClassNamed(cls, "PlayerCameraManager")) {
                if (nameOf(o).rfind("Default__", 0) != 0) {
                    camMgr = o;
                    const uintptr_t ctrl = u64(o + kOffCamOwner);
                    if (ctrl) localPawn = u64(ctrl + kOffCtrlPawn);
                }
                continue;
            }
            if (!isCharacterClass(cls)) continue;
            if (nameOf(o).rfind("Default__", 0) == 0) continue;
            if (!u64(o + kOffRootComponent)) continue;   // not spawned yet
            actors.push_back(o);
        }
    }

    Player readPlayer(uintptr_t o) const {
        Player p;
        p.obj = o;
        p.cls = className(o);
        p.name = nameOf(o);
        p.pos = translation(o);
        p.team = i32(o + kOffPawnTeam);
        const uintptr_t hc = u64(o + kOffHealthComp);
        if (hc) {
            p.dead = u8(hc + kOffDead) != 0;
            read(hc + kOffHealth, &p.hp, sizeof(double));
            read(hc + kOffMaxHealth, &p.maxHp, sizeof(double));
        }
        return p;
    }

    Camera readCameraAt(uintptr_t camMgr) const {
        Camera cam;
        const uintptr_t pov = camMgr + kOffCamPov;
        if (read(pov, &cam.loc, sizeof(double) * 3) &&
            read(pov + 0x18, &cam.rot, sizeof(double) * 3) &&
            read(pov + 0x30, &cam.fov, sizeof(float))) {
            cam.ok = std::isfinite(cam.loc.x) && cam.fov > 1.0f && cam.fov < 179.0f;
        }
        return cam;
    }

private:
    bool isClassNamed(uintptr_t cls, const char *name) const { return nameOf(cls) == name; }

    // Cache class-ptr -> "is a Character subclass". The SuperStruct walk costs
    // several reads, and the same handful of classes repeats for every object.
    bool isCharacterClass(uintptr_t cls) const {
        const auto it = _charClass.find(cls);
        if (it != _charClass.end()) return it->second;
        bool is = false;
        uintptr_t c = cls;
        for (int guard = 0; c && guard < 16; ++guard) {
            const std::string n = nameOf(c);
            if (n == "Character") { is = true; break; }
            if (n == "ActorComponent" || n == "SceneComponent") break;
            c = u64(c + kOffSuper);
        }
        _charClass[cls] = is;
        return is;
    }

    uintptr_t findCameraManager() const {
        const int32_t n = objectCount();
        for (int32_t i = 0; i < n; ++i) {
            const uintptr_t o = objectAt(i);
            if (o && className(o) == "PlayerCameraManager" &&
                nameOf(o).rfind("Default__", 0) != 0)
                return o;
        }
        return 0;
    }

public:

private:
    static int findPid() {
        DIR *d = ::opendir("/proc");
        if (!d) return -1;
        int found = -1;
        for (dirent *e; (e = ::readdir(d));) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/%s/maps", e->d_name);
            FILE *f = std::fopen(path, "r");
            if (!f) continue;
            char line[512];
            while (std::fgets(line, sizeof(line), f)) {
                if (std::strstr(line, kModule)) { found = std::atoi(e->d_name); break; }
            }
            std::fclose(f);
            if (found >= 0) break;
        }
        ::closedir(d);
        return found;
    }

    bool readMaps() {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/maps", _pid);
        FILE *f = std::fopen(path, "r");
        if (!f) return false;
        char line[512];
        uintptr_t low = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (!std::strstr(line, kModule)) continue;
            uintptr_t start = std::strtoull(line, nullptr, 16);
            if (!low || start < low) low = start;
        }
        std::fclose(f);
        _base = low;
        return low != 0;
    }

    int _pid = -1;
    int _mem = -1;
    uintptr_t _base = 0;
    uintptr_t _objects = 0;
    uintptr_t _blocks = 0;
    mutable std::unordered_map<uintptr_t, bool> _charClass;
};

}  // namespace hw2
