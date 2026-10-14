#pragma once
// Aim output for Hood Warfare 2: a real USB HID mouse (shared PhysicalHidMouse
// library), gated on the physical left mouse button read with EVIOCGKEY.
//
// Nothing is touched inside the game process: no injection, no hooks. The gate
// reads the user's own event stream passively, so their clicks still reach the
// game normally.
#include <physical_hid_mouse.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hw2 {

class ButtonGate {
public:
    ~ButtonGate() { close(); }

    bool open(uint16_t code) {
        _code = code;
        DIR *d = ::opendir("/dev/input");
        if (!d) return false;
        int chosen = -1;
        std::string chosenPath, chosenName;
        for (dirent *e; (e = ::readdir(d));) {
            if (std::strncmp(e->d_name, "event", 5) != 0) continue;
            char path[128];
            std::snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
            int fd = ::open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            char name[256] = {0};
            ::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
            // Skip synthetic devices (uinput clones, libvirtualhid, ...): the
            // gate must read the user's *physical* mouse, or the button state is
            // always released.
            if (std::strstr(name, "uinput") || std::strstr(name, "virtual") ||
                std::strstr(name, "Virtual") || std::strstr(name, "Keyboard") ||
                std::strstr(name, "keyboard")) {
                ::close(fd);
                continue;
            }
            unsigned long rel[8] = {0};
            unsigned long keys[12] = {0};
            if (::ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0 ||
                ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) {
                ::close(fd);
                continue;
            }
            const auto has = [](const unsigned long *bits, int bit) {
                return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
            };
            if (!has(rel, REL_X) || !has(rel, REL_Y) || !has(keys, code)) {
                ::close(fd);
                continue;
            }
            chosen = fd;
            chosenPath = path;
            chosenName = name;
            break;
        }
        ::closedir(d);
        if (chosen < 0) return false;
        _fd = chosen;
        _path = chosenPath;
        _name = chosenName;
        return true;
    }

    void close() {
        if (_fd >= 0) ::close(_fd);
        _fd = -1;
    }
    bool ok() const { return _fd >= 0; }
    const char *path() const { return _path.c_str(); }
    const char *name() const { return _name.c_str(); }

    bool down() const {
        if (_fd < 0) return false;
        unsigned long keys[12] = {0};  // enough for BTN_SIDE / BTN_EXTRA
        if (::ioctl(_fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return false;
        return (keys[_code / (8 * sizeof(long))] >> (_code % (8 * sizeof(long)))) & 1UL;
    }

    // The physical mouse's current button state, as a phm button mask. Injected
    // HID reports replace the whole button byte, so this must be re-sent with
    // every move -- otherwise holding right mouse (ADS) would be released.
    uint8_t mask() const {
        if (_fd < 0) return 0;
        unsigned long keys[12] = {0};
        if (::ioctl(_fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return 0;
        const auto held = [&](int bit) {
            return (keys[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
        };
        uint8_t m = 0;
        if (held(BTN_LEFT)) m |= phm::Left;
        if (held(BTN_RIGHT)) m |= phm::Right;
        if (held(BTN_MIDDLE)) m |= phm::Middle;
        if (held(BTN_SIDE)) m |= phm::X1;
        if (held(BTN_EXTRA)) m |= phm::X2;
        return m;
    }

private:
    int _fd = -1;
    uint16_t _code = BTN_LEFT;
    std::string _path;
    std::string _name;
};

}  // namespace hw2
