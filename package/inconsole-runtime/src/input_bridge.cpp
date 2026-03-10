#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

void log_line(const std::string &msg) {
    fprintf(stderr, "input-bridge: %s\n", msg.c_str());
    fflush(stderr);
}

bool starts_with(const std::string &s, const char *prefix) {
    const size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

int emit_uinput(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev)) ? 0 : -1;
}

int emit_key(int fd, unsigned short code, bool pressed) {
    if (emit_uinput(fd, EV_KEY, code, pressed ? 1 : 0) != 0) return -1;
    if (emit_uinput(fd, EV_SYN, SYN_REPORT, 0) != 0) return -1;
    return 0;
}

int open_uinput() {
    const char *paths[] = {"/dev/uinput", "/dev/input/uinput"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        int fd = open(paths[i], O_WRONLY | O_NONBLOCK);
        if (fd >= 0) return fd;
    }
    return -1;
}

int setup_virtual_keyboard(int ufd) {
    if (ioctl(ufd, UI_SET_EVBIT, EV_KEY) < 0) return -1;
    if (ioctl(ufd, UI_SET_EVBIT, EV_SYN) < 0) return -1;

    const int keys[] = {
        KEY_UP,       KEY_DOWN,     KEY_LEFT,    KEY_RIGHT, KEY_LEFTCTRL, KEY_LEFTALT,
        KEY_LEFTSHIFT, KEY_ENTER,   KEY_ESC,     KEY_TAB,   KEY_SPACE,    KEY_Q,
        KEY_E,        KEY_Z,        KEY_X
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (ioctl(ufd, UI_SET_KEYBIT, keys[i]) < 0) return -1;
    }

    struct uinput_user_dev uud;
    memset(&uud, 0, sizeof(uud));
    snprintf(uud.name, sizeof(uud.name), "inconsole-doom-bridge");
    uud.id.bustype = BUS_USB;
    uud.id.vendor = 0x1f3a;
    uud.id.product = 0x1133;
    uud.id.version = 1;

    if (write(ufd, &uud, sizeof(uud)) != static_cast<ssize_t>(sizeof(uud))) return -1;
    if (ioctl(ufd, UI_DEV_CREATE) < 0) return -1;
    return 0;
}

struct ButtonState {
    bool up;
    bool down;
    bool left;
    bool right;
};

void set_virtual_key(int ufd, bool *state, bool next, unsigned short keycode) {
    if (!state) return;
    if (*state == next) return;
    if (emit_key(ufd, keycode, next) == 0) *state = next;
}

void map_button_event(int ufd, int code, bool pressed, ButtonState *st) {
    switch (code) {
        case BTN_DPAD_UP:
        case KEY_UP:
            set_virtual_key(ufd, &st->up, pressed, KEY_UP);
            break;
        case BTN_DPAD_DOWN:
        case KEY_DOWN:
            set_virtual_key(ufd, &st->down, pressed, KEY_DOWN);
            break;
        case BTN_DPAD_LEFT:
        case KEY_LEFT:
            set_virtual_key(ufd, &st->left, pressed, KEY_LEFT);
            break;
        case BTN_DPAD_RIGHT:
        case KEY_RIGHT:
            set_virtual_key(ufd, &st->right, pressed, KEY_RIGHT);
            break;
        case BTN_SOUTH: emit_key(ufd, KEY_LEFTCTRL, pressed); break;
        case BTN_EAST: emit_key(ufd, KEY_SPACE, pressed); break;
        case BTN_NORTH: emit_key(ufd, KEY_LEFTSHIFT, pressed); break;
        case BTN_WEST: emit_key(ufd, KEY_LEFTALT, pressed); break;
        case BTN_START: emit_key(ufd, KEY_ESC, pressed); break;
        case BTN_SELECT: emit_key(ufd, KEY_TAB, pressed); break;
        case BTN_TL: emit_key(ufd, KEY_Q, pressed); break;
        case BTN_TR: emit_key(ufd, KEY_E, pressed); break;
        case BTN_THUMBL: emit_key(ufd, KEY_ENTER, pressed); break;
        default: break;
    }
}

struct AxisState {
    bool left;
    bool right;
    bool up;
    bool down;
    int center_x;
    int center_y;
    bool center_x_set;
    bool center_y_set;
    int deadzone;
};

void map_abs_event(int ufd, int code, int value, AxisState *a, ButtonState *st) {
    if (!a || !st) return;
    if (code == ABS_X) {
        if (!a->center_x_set) {
            a->center_x = value;
            a->center_x_set = true;
        }
        const int dv = value - a->center_x;
        const bool left = dv < -a->deadzone;
        const bool right = dv > a->deadzone;
        set_virtual_key(ufd, &st->left, left, KEY_LEFT);
        set_virtual_key(ufd, &st->right, right, KEY_RIGHT);
    } else if (code == ABS_Y) {
        if (!a->center_y_set) {
            a->center_y = value;
            a->center_y_set = true;
        }
        const int dv = value - a->center_y;
        const bool up = dv < -a->deadzone;
        const bool down = dv > a->deadzone;
        set_virtual_key(ufd, &st->up, up, KEY_UP);
        set_virtual_key(ufd, &st->down, down, KEY_DOWN);
    } else if (code == ABS_HAT0X) {
        set_virtual_key(ufd, &st->left, value < 0, KEY_LEFT);
        set_virtual_key(ufd, &st->right, value > 0, KEY_RIGHT);
    } else if (code == ABS_HAT0Y) {
        set_virtual_key(ufd, &st->up, value < 0, KEY_UP);
        set_virtual_key(ufd, &st->down, value > 0, KEY_DOWN);
    }
}

void release_all_keys(int ufd, ButtonState *st) {
    if (!st) return;
    set_virtual_key(ufd, &st->up, false, KEY_UP);
    set_virtual_key(ufd, &st->down, false, KEY_DOWN);
    set_virtual_key(ufd, &st->left, false, KEY_LEFT);
    set_virtual_key(ufd, &st->right, false, KEY_RIGHT);
    emit_key(ufd, KEY_LEFTCTRL, false);
    emit_key(ufd, KEY_SPACE, false);
    emit_key(ufd, KEY_LEFTSHIFT, false);
    emit_key(ufd, KEY_LEFTALT, false);
    emit_key(ufd, KEY_TAB, false);
    emit_key(ufd, KEY_ESC, false);
    emit_key(ufd, KEY_Q, false);
    emit_key(ufd, KEY_E, false);
    emit_key(ufd, KEY_ENTER, false);
}

}  // namespace

int main() {
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    std::vector<std::string> paths;
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        log_line("cannot open /dev/input");
        return 1;
    }
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (starts_with(ent->d_name, "event")) paths.push_back(std::string("/dev/input/") + ent->d_name);
    }
    closedir(dir);
    std::sort(paths.begin(), paths.end());

    std::vector<int> fds;
    for (size_t i = 0; i < paths.size(); ++i) {
        int fd = open(paths[i].c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0) fds.push_back(fd);
    }
    if (fds.empty()) {
        log_line("no input event devices");
        return 1;
    }

    const int ufd = open_uinput();
    if (ufd < 0) {
        log_line("cannot open /dev/uinput");
        for (size_t i = 0; i < fds.size(); ++i) close(fds[i]);
        return 1;
    }
    if (setup_virtual_keyboard(ufd) != 0) {
        log_line("failed to create uinput keyboard");
        close(ufd);
        for (size_t i = 0; i < fds.size(); ++i) close(fds[i]);
        return 1;
    }
    log_line("uinput keyboard ready");

    AxisState axis;
    memset(&axis, 0, sizeof(axis));
    axis.deadzone = 2500;
    ButtonState buttons;
    memset(&buttons, 0, sizeof(buttons));

    std::vector<struct pollfd> polls;
    polls.reserve(fds.size());
    for (size_t i = 0; i < fds.size(); ++i) {
        struct pollfd p;
        memset(&p, 0, sizeof(p));
        p.fd = fds[i];
        p.events = POLLIN;
        polls.push_back(p);
    }

    while (!g_stop) {
        int pr = poll(polls.data(), polls.size(), 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        for (size_t i = 0; i < polls.size(); ++i) {
            if ((polls[i].revents & POLLIN) == 0) continue;
            while (true) {
                struct input_event ev;
                ssize_t rd = read(polls[i].fd, &ev, sizeof(ev));
                if (rd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (rd != static_cast<ssize_t>(sizeof(ev))) break;

                if (ev.type == EV_KEY) map_button_event(ufd, ev.code, ev.value != 0, &buttons);
                if (ev.type == EV_ABS) map_abs_event(ufd, ev.code, ev.value, &axis, &buttons);
            }
        }
    }

    release_all_keys(ufd, &buttons);
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    for (size_t i = 0; i < fds.size(); ++i) close(fds[i]);
    log_line("exit");
    return 0;
}
