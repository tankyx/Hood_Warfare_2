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
        for (dirent *e; (e = ::readdir(d));) {
            if (std::strncmp(e->d_name, "event", 5) != 0) continue;
            char path[128];
            std::snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
            const int fd = ::open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            unsigned long rel[8] = {0};
            if (::ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0) {
                ::close(fd);
                continue;
            }
            const auto has = [&](int bit) {
                return (rel[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
            };
            if (!has(REL_X) || !has(REL_Y)) {  // not a mouse
                ::close(fd);
                continue;
            }
            _fd = fd;
            _path = path;
            break;
        }
        ::closedir(d);
        return _fd >= 0;
    }

    void close() {
        if (_fd >= 0) ::close(_fd);
        _fd = -1;
    }
    bool ok() const { return _fd >= 0; }
    const char *path() const { return _path.c_str(); }

    bool down() const {
        if (_fd < 0) return false;
        unsigned long keys[12] = {0};  // enough for BTN_SIDE / BTN_EXTRA
        if (::ioctl(_fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return false;
        return (keys[_code / (8 * sizeof(long))] >> (_code % (8 * sizeof(long)))) & 1UL;
    }

private:
    int _fd = -1;
    uint16_t _code = BTN_LEFT;
    std::string _path;
};

}  // namespace hw2
