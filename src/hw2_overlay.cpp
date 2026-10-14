// Hood Warfare 2 read-only ESP overlay.
//
// Draws player boxes on a click-through wlr-layer-shell surface via the shared
// Wayland_Game_Overlay library. Reads the game through /proc/<pid>/mem only --
// no injection, no hooks, no ptrace.
//
//   ./hw2_overlay                 # auto-detect window, draw
//   ./hw2_overlay --dump          # print projections, no overlay
//   ./hw2_overlay --aim           # + external aim (physical USB HID mouse)
//
// Offsets and their derivation are in HOOD_WARFARE_2.md.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <WaylandOverlay.h>

#include "hw2_mem.h"
#include "hw2_aim.h"
#include "x11_window.h"

namespace {

struct Options {
    int renderW = 1920, renderH = 1080;
    float maxDist = 25000.0f;   // world units (cm)
    float height = 180.0f;      // player height, world units
    float widthRatio = 0.42f;   // box width / projected height
    bool dump = false;
    bool labels = true;
    bool aim = false;
    float aimFov = 6.0f;        // aim cone, degrees from crosshair
    float aimScale = 1.0f;
    int hz = 120;
};

std::string argValue(int argc, char **argv, const std::string &key) {
    for (int i = 1; i + 1 < argc; ++i)
        if (key == argv[i]) return argv[i + 1];
    return {};
}

// Team index/name from the character class name.
int teamIndex(const std::string &cls, std::vector<std::string> &names) {
    struct { const char *needle; const char *name; } teams[] = {
        {"TeamAlpha", "Team Alpha"}, {"TeamBeta", "Team Beta"},
        {"Bloods", "Bloods"}, {"Crips", "Crips"},
    };
    for (auto &t : teams) {
        if (cls.find(t.needle) != std::string::npos) {
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == t.name) return static_cast<int>(i);
            names.push_back(t.name);
            return static_cast<int>(names.size() - 1);
        }
    }
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == "Enemy") return static_cast<int>(i);
    names.push_back("Enemy");
    return static_cast<int>(names.size() - 1);
}

// Project a world point to render pixels. Returns false when behind the camera.
bool project(const hw2::Vec3 &p, const hw2::Camera &cam, int w, int h,
             float &sx, float &sy, float &fwd) {
    const double pitch = cam.rot.x * M_PI / 180.0, yaw = cam.rot.y * M_PI / 180.0;
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cy = std::cos(yaw), sy_ = std::sin(yaw);
    const double f[3] = {cp * cy, cp * sy_, sp};
    const double r[3] = {-sy_, cy, 0.0};
    const double u[3] = {-sp * cy, -sp * sy_, cp};
    const double d[3] = {p.x - cam.loc.x, p.y - cam.loc.y, p.z - cam.loc.z};
    const double xd = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
    const double yd = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    const double zd = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    fwd = static_cast<float>(zd);
    if (zd <= 1.0) return false;
    const double aspect = static_cast<double>(w) / h;
    const double fovY = 2.0 * std::atan(std::tan(cam.fov * M_PI / 360.0) / aspect);
    const double focal = (h / 2.0) / std::tan(fovY / 2.0);
    sx = static_cast<float>(w / 2.0 + xd * focal / zd);
    sy = static_cast<float>(h / 2.0 - yd * focal / zd);
    return true;
}

double dist(const hw2::Vec3 &a, const hw2::Vec3 &b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dump") opt.dump = true;
        else if (a == "--aim") opt.aim = true;
        else if (a == "--no-labels") opt.labels = false;
        else if (a == "--render" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &opt.renderW, &opt.renderH);
        else if (a == "--max-dist" && i + 1 < argc) opt.maxDist = std::atof(argv[++i]);
        else if (a == "--aim-fov" && i + 1 < argc) opt.aimFov = std::atof(argv[++i]);
        else if (a == "--hz" && i + 1 < argc) opt.hz = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: %s [--dump] [--aim] [--no-labels] [--render WxH]"
                        " [--max-dist U] [--aim-fov DEG] [--hz N]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    hw2::Reader rd;
    if (!rd.open()) {
        std::fprintf(stderr, "%s is not running\n", hw2::kModule);
        return 1;
    }
    std::fprintf(stderr, "pid %d base=%#lx objects=%d\n", rd.pid(), (unsigned long)rd.base(),
                 rd.objectCount());

    // Window / monitor placement.
    int monW = opt.renderW, monH = opt.renderH, mapX = 0, mapY = 0, winW = opt.renderW, winH = opt.renderH;
    bool haveWindow = false;
#ifdef HAVE_X11
    const x11::GameWindow gw = x11::find_game_window();
    if (gw.found && gw.w > 64 && gw.h > 64) {
        haveWindow = true;
        winW = gw.w; winH = gw.h;
        mapX = gw.x - gw.monX; mapY = gw.y - gw.monY;
        monW = gw.monW; monH = gw.monH;
        opt.renderW = gw.w; opt.renderH = gw.h;
        std::fprintf(stderr, "window %dx%d at monitor+%d,%d (monitor %dx%d)\n",
                     winW, winH, mapX, mapY, monW, monH);
    }
#endif
    if (!haveWindow)
        std::fprintf(stderr, "no game window found; assuming %dx%d fullscreen\n", opt.renderW, opt.renderH);

    WaylandOverlay overlay(haveWindow ? monW : opt.renderW, haveWindow ? monH : opt.renderH);
    overlay.setSourceSize(opt.renderW, opt.renderH);
    if (haveWindow) overlay.setWindowRect(mapX, mapY, winW, winH);
    if (!opt.dump && !overlay.start()) {
        std::fprintf(stderr, "overlay start failed\n");
        return 1;
    }

    hw2::ButtonGate lmb;
    phm::PhysicalHidMouse mouse;
    if (opt.aim) {
        if (!mouse.open())
            std::fprintf(stderr, "aim: no physical USB HID mouse found (install the udev rule)\n");
        else
            std::fprintf(stderr, "aim: %s\n", mouse.path().c_str());
        if (!lmb.open(BTN_LEFT)) std::fprintf(stderr, "aim: no mouse for the LMB gate\n");
    }

    std::vector<std::string> labelNames;
    std::vector<WaylandOverlay::DetectionBox> boxes;
    std::vector<WaylandOverlay::Shape> shapes;
    int frames = 0, fpsFrames = 0, fps = 0;
    auto fpsT0 = std::chrono::steady_clock::now();
    int printed = 0;

    // The full GObjects scan is expensive; run it every ~1.5 s and read only the
    // cached player addresses in between.
    std::vector<uintptr_t> actors;
    uintptr_t camMgr = 0, localPawn = 0;
    auto lastScan = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        if (actors.empty() ||
            std::chrono::duration_cast<std::chrono::milliseconds>(t0 - lastScan).count() > 1500) {
            rd.scanActors(camMgr, localPawn, actors);
            lastScan = t0;
        }
        const hw2::Camera cam = camMgr ? rd.readCameraAt(camMgr) : hw2::Camera{};
        std::vector<hw2::Player> players;
        players.reserve(actors.size());
        for (uintptr_t a : actors) {
            hw2::Player p = rd.readPlayer(a);
            p.local = (a == localPawn);
            players.push_back(std::move(p));
        }

        boxes.clear();
        shapes.clear();
        labelNames.clear();
        float bestOx = 0, bestOy = 0, bestMag = 1e30f;

        if (cam.ok) {
            for (const auto &p : players) {
                const double d = dist(p.pos, cam.loc);
                if (d > opt.maxDist) continue;
                hw2::Vec3 head{p.pos.x, p.pos.y, p.pos.z + opt.height};
                hw2::Vec3 feet = p.pos;
                float hx, hy, fx, fy, hf, ff;
                if (!project(head, cam, opt.renderW, opt.renderH, hx, hy, hf)) continue;
                if (!project(feet, cam, opt.renderW, opt.renderH, fx, fy, ff)) continue;
                const float bh = std::fabs(fy - hy);
                const float bw = bh * opt.widthRatio;
                const float bx = (hx + fx) / 2.0f - bw / 2.0f;
                const float by = std::min(hy, fy);

                const int team = teamIndex(p.cls, labelNames);
                WaylandOverlay::DetectionBox b;
                b.x = bx; b.y = by; b.w = bw; b.h = bh;
                b.label = opt.labels ? static_cast<int>(labelNames.size()) : team;
                b.confidence = 1.0f;
                if (opt.labels) {
                    labelNames.push_back(p.cls + " " + std::to_string(static_cast<int>(d / 100.0f + 0.5f)) + "m");
                }
                boxes.push_back(b);

                if (p.local) continue;  // aim only at others

                const float cx = hx - opt.renderW / 2.0f;
                const float cy = hy - opt.renderH / 2.0f;
                if (std::sqrt(cx * cx + cy * cy) < bestMag) { bestMag = std::sqrt(cx * cx + cy * cy); bestOx = cx; bestOy = cy; }
            }
        }

        if (opt.dump) {
            std::printf("cam=(%8.1f,%8.1f,%8.1f) fov=%.1f players=%zu\n",
                        cam.loc.x, cam.loc.y, cam.loc.z, cam.fov, players.size());
            std::printf("  on-screen boxes: %zu\n", boxes.size());
            for (const auto &b : boxes)
                std::printf("    box x=%.0f y=%.0f w=%.0f h=%.0f\n", b.x, b.y, b.w, b.h);
        } else {
            overlay.setLabelNames(labelNames);
            overlay.setDetections(std::move(boxes));
            overlay.setShapes(std::move(shapes));
            overlay.setStats(0, fps);
        }

        if (opt.aim && mouse.ok() && lmb.ok() && lmb.down() && bestMag < 1e29f) {
            if (bestMag > 3.0f)
                mouse.move(static_cast<int>(bestOx * 0.35 * opt.aimScale),
                           static_cast<int>(bestOy * 0.35 * opt.aimScale));
        }

        if (++fpsFrames >= 30) {
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - fpsT0).count();
            fps = static_cast<int>(fpsFrames / secs);
            fpsFrames = 0;
            fpsT0 = now;
        }
        if (++frames % 600 == 0 && !opt.dump)
            std::fprintf(stderr, "frames=%d players=%zu fps=%d\n", frames, players.size(), fps);

        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(1000 / std::max(1, opt.hz)));
    }

    if (!opt.dump) overlay.stop();
    return 0;
}
