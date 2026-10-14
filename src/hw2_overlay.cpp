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
    float aimHeight = 0.40f;    // aim point as a fraction of body height above the capsule centre (0.5 = crown, 0.40 ~ neck)
    bool teams = false;         // false = FFA (everyone hostile); true = compare PawnTeam
    bool fx = true;             // anti-alias + rounded corners + hostile glow
    bool aimDebug = false;
    int hz = 160;
};

std::string argValue(int argc, char **argv, const std::string &key) {
    for (int i = 1; i + 1 < argc; ++i)
        if (key == argv[i]) return argv[i + 1];
    return {};
}

// Faction token from a character class name: the token before the trailing
// "_C" ("BP_Tyreese_TeamAlpha_C" -> "TeamAlpha"). The local player's faction
// defines "ally"; anything else is "hostile".
std::string factionOf(const std::string &cls) {
    std::string s = cls;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "_C") == 0) s.resize(s.size() - 2);
    const auto pos = s.rfind('_');
    return pos == std::string::npos ? s : s.substr(pos + 1);
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
        else if (a == "--height" && i + 1 < argc) opt.height = std::atof(argv[++i]);
        else if (a == "--aim-fov" && i + 1 < argc) opt.aimFov = std::atof(argv[++i]);
        else if (a == "--aim-height" && i + 1 < argc) opt.aimHeight = std::atof(argv[++i]);
        else if (a == "--teams") opt.teams = true;
        else if (a == "--no-fx") opt.fx = false;
        else if (a == "--aim-debug") opt.aimDebug = true;
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
    if (opt.fx) overlay.setAntiAlias(true);   // smoothed lines/ellipses/polygons
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
        if (!lmb.open(BTN_RIGHT))
            std::fprintf(stderr, "aim: no mouse for the RMB gate\n");
        else
            std::fprintf(stderr, "aim gate: %s (%s) — aim engages on RMB\n", lmb.path(), lmb.name());
    }

    hw2::KeyGate killKey;
    std::fprintf(stderr, "kill switch (HOME): %s\n",
                 killKey.open(KEY_HOME) ? "armed" : "keyboard not found");

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
    double lastScanMs = 0.0;

    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        if (killKey.ok() && killKey.down()) {
            std::fprintf(stderr, "HOME pressed - shutting down\n");
            break;
        }
        if (actors.empty() ||
            std::chrono::duration_cast<std::chrono::milliseconds>(t0 - lastScan).count() > 1500) {
            const auto s0 = std::chrono::steady_clock::now();
            rd.scanActors(camMgr, localPawn, actors);
            lastScanMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - s0).count();
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
        const int localTeam = localPawn ? rd.readPlayer(localPawn).team : -1;

        boxes.clear();
        shapes.clear();
        labelNames.clear();
        float bestOx = 0, bestOy = 0, bestMag = 1e30f;
        double aimMaxPx = 0.0;

        if (cam.ok) {
            if (opt.aim) {
                // Cone half-angle in pixels: tan(aimFov/2) * focal.
                const double aspect = static_cast<double>(opt.renderW) / opt.renderH;
                const double fovY = 2.0 * std::atan(std::tan(cam.fov * M_PI / 360.0) / aspect);
                const double focal = (opt.renderH / 2.0) / std::tan(fovY / 2.0);
                aimMaxPx = std::tan(opt.aimFov * M_PI / 360.0) * focal;
            }
            if (opt.aim && aimMaxPx > 0.0) {
                // The aim cone, as a circle around the crosshair (radius = the
                // pixel half-angle the target selection uses).
                WaylandOverlay::Shape ring;
                ring.kind = WaylandOverlay::Shape::Ellipse;
                ring.x0 = opt.renderW / 2.0f;
                ring.y0 = opt.renderH / 2.0f;
                ring.x1 = static_cast<float>(aimMaxPx);
                ring.y1 = static_cast<float>(aimMaxPx);
                ring.outline = true;
                ring.thickness = 1.5f;
                ring.color = 0x99FFEE00u;  // translucent yellow
                shapes.push_back(ring);
            }
            for (const auto &p : players) {
                if (p.local) continue;   // never ESP your own entity
                if (p.dead) continue;    // don't box corpses
                const double d = dist(p.pos, cam.loc);
                if (d > opt.maxDist) continue;
                // The actor origin is the capsule centre (~hip), so the box spans
                // half the height either side of it: head at +h/2, feet at -h/2.
                const hw2::Vec3 head{p.pos.x, p.pos.y, p.pos.z + opt.height * 0.5};
                const hw2::Vec3 feet{p.pos.x, p.pos.y, p.pos.z - opt.height * 0.5};
                float hx, hy, fx, fy, hf, ff;
                if (!project(head, cam, opt.renderW, opt.renderH, hx, hy, hf)) continue;
                if (!project(feet, cam, opt.renderW, opt.renderH, fx, fy, ff)) continue;
                const float bh = std::fabs(fy - hy);
                const float bw = bh * opt.widthRatio;
                const float bx = (hx + fx) / 2.0f - bw / 2.0f;
                const float by = std::min(hy, fy);

                const bool ally = opt.teams && p.team >= 0 && p.team == localTeam;
                const uint32_t color = p.local ? 0xFF00FFFFu
                                     : ally    ? 0xFF33FF55u
                                               : 0xFFFF3030u;

                // Box as an explicit-colour shape, so ally/hostile colouring does
                // not depend on the label palette.
                WaylandOverlay::Shape box;
                box.kind = WaylandOverlay::Shape::Filled;
                box.x0 = bx; box.y0 = by; box.x1 = bx + bw; box.y1 = by + bh;
                box.outline = true;
                box.thickness = 2.0f;
                box.color = color;
                if (opt.fx) {
                    box.radius = 3.0f;                 // rounded corners
                    if (!ally) {                       // hostiles get a soft halo
                        box.glow = true;
                        box.glowRadius = 5.0f;
                    }
                }
                shapes.push_back(box);

                // HP bar to the left of the box: full-height dark track with a
                // bottom-anchored fill, green -> yellow -> red by ratio.
                if (p.maxHp > 0.0) {
                    const float frac = static_cast<float>(std::clamp(p.hp / p.maxHp, 0.0, 1.0));
                    const float barX = bx - 6.0f, barW = 3.0f;
                    WaylandOverlay::Shape track;
                    track.kind = WaylandOverlay::Shape::Filled;
                    track.x0 = barX; track.y0 = by;
                    track.x1 = barX + barW; track.y1 = by + bh;
                    track.color = 0x90000000u;
                    if (opt.fx) track.radius = 1.0f;
                    shapes.push_back(track);
                    WaylandOverlay::Shape fill;
                    fill.kind = WaylandOverlay::Shape::Filled;
                    fill.x0 = barX; fill.y0 = by + bh * (1.0f - frac);
                    fill.x1 = barX + barW; fill.y1 = by + bh;
                    fill.color = frac > 0.6f ? 0xFF33FF33u
                               : frac > 0.3f ? 0xFFFFE000u
                                            : 0xFFFF3030u;
                    if (opt.fx) fill.radius = 1.0f;
                    shapes.push_back(fill);
                }

                if (opt.labels) {
                    WaylandOverlay::DetectionBox t;
                    t.x = hx; t.y = hy; t.w = 0; t.h = 0;   // label only
                    t.label = static_cast<int>(labelNames.size());
                    t.confidence = 1.0f;
                    const char *tag = p.local ? "YOU " : (ally ? "[A] " : "[H] ");
                    labelNames.push_back(std::string(tag) + p.name + " " +
                                         std::to_string(static_cast<int>(d / 100.0f + 0.5f)) + "m " +
                                         std::to_string(static_cast<int>(p.hp + 0.5)) + "hp");
                    boxes.push_back(t);
                }

                if (!ally && aimMaxPx > 0.0) {
                    // Aim point is its own height (neck by default), independent of
                    // the box, which still spans the full body.
                    const hw2::Vec3 aimPt{p.pos.x, p.pos.y,
                                          p.pos.z + opt.height * opt.aimHeight};
                    float nx, ny, nf;
                    if (project(aimPt, cam, opt.renderW, opt.renderH, nx, ny, nf)) {
                        const float cx = nx - opt.renderW / 2.0f;
                        const float cy = ny - opt.renderH / 2.0f;
                        const float mag = std::sqrt(cx * cx + cy * cy);
                        if (mag < aimMaxPx && mag < bestMag) {
                            bestMag = mag;
                            bestOx = cx; bestOy = cy;
                        }
                    }
                }
            }
        }

        if (opt.dump) {
            std::printf("cam=(%8.1f,%8.1f,%8.1f) fov=%.1f players=%zu local-team=%d\n",
                        cam.loc.x, cam.loc.y, cam.loc.z, cam.fov, players.size(), localTeam);
            for (const auto &p : players)
                std::printf("  %-34s team=%d %-12s %s dead=%d pos=(%9.1f,%9.1f,%9.1f)\n", p.cls.c_str(),
                            p.team, factionOf(p.cls).c_str(),
                            p.local ? "YOU" : (p.team == localTeam ? "ALLY" : "HOSTILE"),
                            (int)p.dead, p.pos.x, p.pos.y, p.pos.z);
        } else {
            overlay.setLabelNames(labelNames);
            overlay.setDetections(std::move(boxes));
            overlay.setShapes(std::move(shapes));
            overlay.setStats(0, fps);
        }

        if (opt.aim && cam.ok && mouse.ok() && lmb.ok() && lmb.down() &&
            bestMag < aimMaxPx && bestMag > 2.0f) {
            const float gain = 0.15f * opt.aimScale;
            const int dx = static_cast<int>(std::lround(bestOx * gain));
            const int dy = static_cast<int>(std::lround(bestOy * gain));
            // Re-send the physical button state: a HID report replaces the whole
            // button byte, so dropping it would release a held right mouse (ADS).
            if (dx || dy) mouse.move(dx, dy, lmb.mask());
        }
        if (opt.aimDebug && (frames % 60) == 0) {
            std::fprintf(stderr,
                         "aim: lmb_ok=%d lmb_down=%d rmb=%d mask=0x%02x mouse_ok=%d maxpx=%.0f best=%.0f dx=%.0f dy=%.0f target=%s scan=%.1fms\n",
                         (int)lmb.ok(), (int)lmb.down(), (int)((lmb.mask() & phm::Right) != 0),
                         (unsigned)lmb.mask(), (int)mouse.ok(), aimMaxPx, bestMag,
                         bestOx, bestOy, bestMag < aimMaxPx ? "yes" : "no", lastScanMs);
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
