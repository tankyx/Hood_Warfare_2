#pragma once
// Locate the MCC game window and the monitor it lives on, via X11 + RandR.
//
// Xwayland reports its coordinate space in *physical* pixels, while the
// wlr-layer-shell surface we draw on is in *logical* pixels. We never need to
// know the compositor scale: the fraction of the monitor a window covers is
// identical in both spaces. So boxes are emitted in monitor-physical
// coordinates and LinuxOverlay is told the source size is the monitor's
// physical size - it then maps them onto the surface itself.
#include <string>

namespace x11 {

struct GameWindow {
    bool found = false;
    int x = 0, y = 0, w = 0, h = 0;   // window rect, Xwayland pixels
    std::string monitor;              // connector name, e.g. "HDMI-A-1"
    int monX = 0, monY = 0, monW = 0, monH = 0;
};

GameWindow find_game_window(const char *displayName = nullptr);

// Monitor containing the given point (falls back to the first one).
GameWindow monitor_at(int x, int y, const char *displayName = nullptr);

}  // namespace x11

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <cstdlib>
#include <cstring>
#include <functional>

namespace x11 {
namespace {

bool is_game_window(Display *dpy, Window w) {
    bool hit = false;
    XClassHint ch{};
    if (XGetClassHint(dpy, w, &ch)) {
        if (ch.res_name && std::strstr(ch.res_name, "steam_app_5155360")) hit = true;
        if (ch.res_class && std::strstr(ch.res_class, "steam_app_5155360")) hit = true;
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    if (!hit) {
        char *name = nullptr;
        if (XFetchName(dpy, w, &name) && name) {
            if (std::strstr(name, "Hood Warfare") || std::strstr(name, "HW2")) hit = true;
            XFree(name);
        }
    }
    return hit;
}

struct Finder {
    Display *dpy;
    Window best = 0;
    unsigned long area = 0;

    void walk(Window w, int depth) {
        if (depth > 4) return;
        Window r, parent, *kids = nullptr;
        unsigned int n = 0;
        if (!XQueryTree(dpy, w, &r, &parent, &kids, &n)) return;
        for (unsigned int i = 0; i < n; ++i) {
            XWindowAttributes a{};
            if (XGetWindowAttributes(dpy, kids[i], &a) && a.map_state == IsViewable &&
                a.width > 64 && a.height > 64 && is_game_window(dpy, kids[i])) {
                unsigned long ar = static_cast<unsigned long>(a.width) * a.height;
                if (ar > area) { area = ar; best = kids[i]; }
            }
            walk(kids[i], depth + 1);
        }
        if (kids) XFree(kids);
    }
};

void fill_monitor(Display *dpy, Window root, int cx, int cy, GameWindow &out) {
    int count = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True, &count);
    if (!mons || count <= 0) {
        if (mons) XRRFreeMonitors(mons);
        return;
    }
    int pick = 0;
    for (int i = 0; i < count; ++i) {
        if (cx >= mons[i].x && cx < mons[i].x + mons[i].width &&
            cy >= mons[i].y && cy < mons[i].y + mons[i].height) {
            pick = i;
            break;
        }
    }
    out.monX = mons[pick].x;
    out.monY = mons[pick].y;
    out.monW = mons[pick].width;
    out.monH = mons[pick].height;
    if (mons[pick].name) {          // Atom, not a char*
        char *n = XGetAtomName(dpy, mons[pick].name);
        if (n) { out.monitor = n; XFree(n); }
    }
    XRRFreeMonitors(mons);
}

}  // namespace

GameWindow monitor_at(int x, int y, const char *displayName) {
    GameWindow out;
    const char *dname = displayName ? displayName : std::getenv("DISPLAY");
    Display *dpy = XOpenDisplay(dname);
    if (!dpy) return out;
    fill_monitor(dpy, DefaultRootWindow(dpy), x, y, out);
    XCloseDisplay(dpy);
    out.found = !out.monitor.empty();
    return out;
}

GameWindow find_game_window(const char *displayName) {
    GameWindow out;
    const char *dname = displayName ? displayName : std::getenv("DISPLAY");
    Display *dpy = XOpenDisplay(dname);
    if (!dpy) return out;
    Window root = DefaultRootWindow(dpy);

    Finder f{dpy};
    f.walk(root, 0);
    if (!f.best) { XCloseDisplay(dpy); return out; }

    XWindowAttributes a{};
    XGetWindowAttributes(dpy, f.best, &a);
    int rx = 0, ry = 0;
    Window child = 0;
    XTranslateCoordinates(dpy, f.best, root, 0, 0, &rx, &ry, &child);

    out.found = true;
    out.x = rx; out.y = ry;
    out.w = a.width; out.h = a.height;
    fill_monitor(dpy, root, rx + a.width / 2, ry + a.height / 2, out);
    XCloseDisplay(dpy);
    return out;
}

}  // namespace x11
#endif  // HAVE_X11
