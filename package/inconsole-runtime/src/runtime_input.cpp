#include "runtime.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace inconsole {

namespace {

const int kBitsPerLong = static_cast<int>(sizeof(unsigned long) * 8);
const size_t kEvBitsLen = static_cast<size_t>((EV_MAX + 1 + kBitsPerLong - 1) / kBitsPerLong);
const size_t kAbsBitsLen = static_cast<size_t>((ABS_MAX + 1 + kBitsPerLong - 1) / kBitsPerLong);
const size_t kKeyBitsLen = static_cast<size_t>((KEY_MAX + 1 + kBitsPerLong - 1) / kBitsPerLong);

bool test_bit(const unsigned long *bits, size_t len, int bit) {
    if (!bits || bit < 0) return false;
    const size_t idx = static_cast<size_t>(bit / kBitsPerLong);
    if (idx >= len) return false;
    const unsigned long mask = 1UL << (bit % kBitsPerLong);
    return (bits[idx] & mask) != 0;
}

bool starts_with(const std::string &s, const char *prefix) {
    if (!prefix) return false;
    const size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool is_mapped_button_key(int code) {
    switch (code) {
        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
        case BTN_DPAD_UP:
        case BTN_DPAD_DOWN:
        case BTN_DPAD_LEFT:
        case BTN_DPAD_RIGHT:
        case BTN_SOUTH:
        case BTN_EAST:
        case BTN_START:
        case BTN_SELECT:
        case BTN_TL:
        case BTN_TR:
        case BTN_TL2:
        case BTN_TR2:
            return true;
        default:
            return false;
    }
}

std::string evdev_name(int fd) {
    char name[128];
    memset(name, 0, sizeof(name));
    if (fd >= 0 && ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) return std::string(name);
    return std::string("unknown");
}

}  // namespace

Input::AxisCalibration::AxisCalibration()
    : has_absinfo(false), centered_from_event(false), min(-32768), max(32767), center(0), deadzone(12000) {}

Input::Raw::Raw()
    : up(false),
      down(false),
      left(false),
      right(false),
      a(false),
      b(false),
      start(false),
      select(false),
      l(false),
      r(false),
      joy(false),
      axis_x(0),
      axis_y(0) {}

Input::Input()
    : button_fd_(-1),
      axis_fd_(-1),
      evdev_fds_(),
      joysticks_(),
      axis_x_cal_(),
      axis_y_cal_(),
      raw_(),
      prev_raw_(),
      snapshot_(),
      queued_nav_up_(false),
      queued_nav_down_(false),
      queued_nav_left_(false),
      queued_nav_right_(false),
      queued_hold_select_(false),
      queued_hold_l_(false),
      queued_hold_r_(false),
      queued_accept_(false),
      queued_back_(false),
      queued_menu_(false),
      queued_accept_ms_(0),
      queued_back_ms_(0),
      queued_menu_ms_(0),
      last_edge_a_ms_(0),
      last_edge_b_ms_(0),
      last_edge_start_ms_(0),
      last_edge_select_ms_(0),
      last_edge_l_ms_(0),
      last_edge_r_ms_(0) {
    for (int i = 0; i < 4; ++i) repeat_due_[i] = 0;
}

Input::~Input() {
    close_inputs();
}

void Input::init(Logger *logger) {
    axis_x_cal_ = AxisCalibration();
    axis_y_cal_ = AxisCalibration();
    raw_ = Raw();
    prev_raw_ = Raw();
    snapshot_ = InputSnapshot();
    for (int i = 0; i < 4; ++i) repeat_due_[i] = 0;
    queued_nav_up_ = false;
    queued_nav_down_ = false;
    queued_nav_left_ = false;
    queued_nav_right_ = false;
    queued_hold_select_ = false;
    queued_hold_l_ = false;
    queued_hold_r_ = false;
    queued_accept_ = false;
    queued_back_ = false;
    queued_menu_ = false;
    queued_accept_ms_ = 0;
    queued_back_ms_ = 0;
    queued_menu_ms_ = 0;
    last_edge_a_ms_ = 0;
    last_edge_b_ms_ = 0;
    last_edge_start_ms_ = 0;
    last_edge_select_ms_ = 0;
    last_edge_l_ms_ = 0;
    last_edge_r_ms_ = 0;

    open_sdl_inputs(logger);
    open_evdev_inputs(logger);

    if (logger) {
        logger->info("Input mapping: DTS-style buttons + joystick + keyboard fallback");
        logger->info("Input fallback keyboard: arrows/WASD, Enter=Accept, Esc=Back, Tab=Menu");
        logger->info("Input fallback shoulder/select: Q/E and RightShift");
    }
}

void Input::open_sdl_inputs(Logger *logger) {
    SDL_JoystickEventState(SDL_ENABLE);

    const int total = SDL_NumJoysticks();
    if (logger) logger->info("Input: SDL joysticks detected: " + std::to_string(total));

    for (int i = 0; i < total; ++i) {
        SDL_Joystick *joy = SDL_JoystickOpen(i);
        if (!joy) continue;
        const int id = SDL_JoystickIndex(joy);
        bool duplicate = false;
        for (size_t j = 0; j < joysticks_.size(); ++j) {
            if (SDL_JoystickIndex(joysticks_[j]) == id) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            joysticks_.push_back(joy);
            if (logger) {
                const char *n = SDL_JoystickName(i);
                logger->info(std::string("Input: SDL joystick open: ") + (n ? n : "unknown"));
            }
        } else {
            SDL_JoystickClose(joy);
        }
    }
}

void Input::open_evdev_inputs(Logger *logger) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        if (logger) logger->warn("Input: /dev/input not available");
        return;
    }

    std::vector<std::string> paths;
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (!starts_with(ent->d_name, "event")) continue;
        paths.push_back(std::string("/dev/input/") + ent->d_name);
    }
    closedir(dir);
    std::sort(paths.begin(), paths.end());

    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string &path = paths[i];
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long ev_bits[kEvBitsLen];
        memset(ev_bits, 0, sizeof(ev_bits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
            close(fd);
            continue;
        }

        bool useful = false;
        if (test_bit(ev_bits, kEvBitsLen, EV_KEY)) {
            unsigned long key_bits[kKeyBitsLen];
            memset(key_bits, 0, sizeof(key_bits));
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
                for (int code = 0; code <= KEY_MAX; ++code) {
                    if (test_bit(key_bits, kKeyBitsLen, code) && is_mapped_button_key(code)) {
                        useful = true;
                        if (button_fd_ < 0) button_fd_ = fd;
                        break;
                    }
                }
            }
        }

        if (test_bit(ev_bits, kEvBitsLen, EV_ABS)) {
            unsigned long abs_bits[kAbsBitsLen];
            memset(abs_bits, 0, sizeof(abs_bits));
            if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
                if (test_bit(abs_bits, kAbsBitsLen, ABS_X) || test_bit(abs_bits, kAbsBitsLen, ABS_Y) ||
                    test_bit(abs_bits, kAbsBitsLen, ABS_HAT0X) || test_bit(abs_bits, kAbsBitsLen, ABS_HAT0Y)) {
                    useful = true;
                    if (axis_fd_ < 0) axis_fd_ = fd;
                    configure_axis_calibration(fd, ABS_X, &axis_x_cal_, "ABS_X", logger);
                    configure_axis_calibration(fd, ABS_Y, &axis_y_cal_, "ABS_Y", logger);
                }
            }
        }

        if (!useful) {
            close(fd);
            continue;
        }

        evdev_fds_.push_back(fd);
        if (logger) logger->info("Input: evdev open: " + path + " [" + evdev_name(fd) + "]");
    }
}

void Input::close_inputs() {
    for (size_t i = 0; i < evdev_fds_.size(); ++i) {
        if (evdev_fds_[i] >= 0) close(evdev_fds_[i]);
    }
    evdev_fds_.clear();
    button_fd_ = -1;
    axis_fd_ = -1;

    if (SDL_WasInit(SDL_INIT_JOYSTICK) != 0) {
        for (size_t i = 0; i < joysticks_.size(); ++i) {
            if (joysticks_[i]) SDL_JoystickClose(joysticks_[i]);
        }
    }
    joysticks_.clear();
}

void Input::configure_axis_calibration(int fd, int code, AxisCalibration *cal, const char *name, Logger *logger) {
    if (!cal || fd < 0 || cal->has_absinfo) return;

    struct input_absinfo absinfo;
    memset(&absinfo, 0, sizeof(absinfo));
    if (ioctl(fd, EVIOCGABS(code), &absinfo) != 0) return;

    cal->has_absinfo = true;
    cal->min = absinfo.minimum;
    cal->max = absinfo.maximum;
    cal->center = (absinfo.minimum + absinfo.maximum) / 2;
    const int range = std::max(1, absinfo.maximum - absinfo.minimum);
    int deadzone = range / 9;
    deadzone = std::max(2000, deadzone);
    deadzone = std::min(16000, deadzone);
    cal->deadzone = deadzone;

    if (code == ABS_X) raw_.axis_x = absinfo.value;
    if (code == ABS_Y) raw_.axis_y = absinfo.value;

    if (logger) {
        logger->info(std::string("Input axis calib ") + name + ": min=" + std::to_string(absinfo.minimum) +
                     " max=" + std::to_string(absinfo.maximum) + " center=" + std::to_string(cal->center) +
                     " deadzone=" + std::to_string(cal->deadzone));
    }
}

void Input::prime_axis_center(AxisCalibration *cal, int value) {
    if (!cal || cal->centered_from_event) return;
    cal->center = value;
    cal->centered_from_event = true;
}

void Input::set_button_state(bool &field, bool pressed, bool *queued_hold, bool *queued_nav, uint64_t *edge_ms) {
    if (pressed && !field) {
        if (queued_hold) *queued_hold = true;
        if (queued_nav) *queued_nav = true;
        if (edge_ms) *edge_ms = monotonic_ms();
    }
    field = pressed;
}

void Input::map_keycode(SDLKey key, bool pressed) {
    switch (key) {
        case SDLK_UP:
        case SDLK_w:
            set_button_state(raw_.up, pressed, nullptr, &queued_nav_up_, nullptr);
            break;
        case SDLK_DOWN:
        case SDLK_s:
            set_button_state(raw_.down, pressed, nullptr, &queued_nav_down_, nullptr);
            break;
        case SDLK_LEFT:
        case SDLK_a:
            set_button_state(raw_.left, pressed, nullptr, &queued_nav_left_, nullptr);
            break;
        case SDLK_RIGHT:
        case SDLK_d:
            set_button_state(raw_.right, pressed, nullptr, &queued_nav_right_, nullptr);
            break;

        case SDLK_RETURN:
        case SDLK_SPACE:
        case SDLK_z:
        case SDLK_j:
        case SDLK_LCTRL:
            set_button_state(raw_.a, pressed, nullptr, nullptr, &last_edge_a_ms_);
            if (pressed) {
                queued_accept_ = true;
                queued_accept_ms_ = last_edge_a_ms_;
            }
            break;

        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
        case SDLK_x:
        case SDLK_k:
        case SDLK_LALT:
            set_button_state(raw_.b, pressed, nullptr, nullptr, &last_edge_b_ms_);
            if (pressed) {
                queued_back_ = true;
                queued_back_ms_ = last_edge_b_ms_;
            }
            break;

        case SDLK_TAB:
        case SDLK_p:
        case SDLK_F1:
            set_button_state(raw_.start, pressed, nullptr, nullptr, &last_edge_start_ms_);
            if (pressed) {
                queued_menu_ = true;
                queued_menu_ms_ = last_edge_start_ms_;
            }
            break;

        case SDLK_RSHIFT:
        case SDLK_SLASH:
        case SDLK_F2:
            set_button_state(raw_.select, pressed, &queued_hold_select_, nullptr, &last_edge_select_ms_);
            break;

        case SDLK_q:
        case SDLK_LEFTBRACKET:
        case SDLK_PAGEUP:
            set_button_state(raw_.l, pressed, &queued_hold_l_, nullptr, &last_edge_l_ms_);
            break;

        case SDLK_e:
        case SDLK_RIGHTBRACKET:
        case SDLK_PAGEDOWN:
            set_button_state(raw_.r, pressed, &queued_hold_r_, nullptr, &last_edge_r_ms_);
            break;

        default:
            break;
    }
}

void Input::map_joy_button(uint8_t button, bool pressed) {
    switch (button) {
        case 0:
            set_button_state(raw_.a, pressed, nullptr, nullptr, &last_edge_a_ms_);
            if (pressed) {
                queued_accept_ = true;
                queued_accept_ms_ = last_edge_a_ms_;
            }
            break;
        case 1:
            set_button_state(raw_.b, pressed, nullptr, nullptr, &last_edge_b_ms_);
            if (pressed) {
                queued_back_ = true;
                queued_back_ms_ = last_edge_b_ms_;
            }
            break;
        case 4: set_button_state(raw_.l, pressed, &queued_hold_l_, nullptr, &last_edge_l_ms_); break;
        case 5: set_button_state(raw_.r, pressed, &queued_hold_r_, nullptr, &last_edge_r_ms_); break;
        case 6: set_button_state(raw_.select, pressed, &queued_hold_select_, nullptr, &last_edge_select_ms_); break;
        case 7:
            set_button_state(raw_.start, pressed, nullptr, nullptr, &last_edge_start_ms_);
            if (pressed) {
                queued_menu_ = true;
                queued_menu_ms_ = last_edge_start_ms_;
            }
            break;
        case 9:
        case 10:
            set_button_state(raw_.joy, pressed, nullptr, nullptr, nullptr);
            break;
        case 11: set_button_state(raw_.up, pressed, nullptr, &queued_nav_up_, nullptr); break;
        case 12: set_button_state(raw_.down, pressed, nullptr, &queued_nav_down_, nullptr); break;
        case 13: set_button_state(raw_.left, pressed, nullptr, &queued_nav_left_, nullptr); break;
        case 14: set_button_state(raw_.right, pressed, nullptr, &queued_nav_right_, nullptr); break;
        default: break;
    }
}

void Input::map_ev_key(int code, bool pressed) {
    switch (code) {
        case KEY_UP:
        case BTN_DPAD_UP:
            set_button_state(raw_.up, pressed, nullptr, &queued_nav_up_, nullptr);
            break;
        case KEY_DOWN:
        case BTN_DPAD_DOWN:
            set_button_state(raw_.down, pressed, nullptr, &queued_nav_down_, nullptr);
            break;
        case KEY_LEFT:
        case BTN_DPAD_LEFT:
            set_button_state(raw_.left, pressed, nullptr, &queued_nav_left_, nullptr);
            break;
        case KEY_RIGHT:
        case BTN_DPAD_RIGHT:
            set_button_state(raw_.right, pressed, nullptr, &queued_nav_right_, nullptr);
            break;
        case BTN_SOUTH:
        case KEY_ENTER:
            set_button_state(raw_.a, pressed, nullptr, nullptr, &last_edge_a_ms_);
            if (pressed) {
                queued_accept_ = true;
                queued_accept_ms_ = last_edge_a_ms_;
            }
            break;
        case BTN_EAST:
        case KEY_BACKSPACE:
        case KEY_ESC:
            set_button_state(raw_.b, pressed, nullptr, nullptr, &last_edge_b_ms_);
            if (pressed) {
                queued_back_ = true;
                queued_back_ms_ = last_edge_b_ms_;
            }
            break;
        case BTN_START:
            set_button_state(raw_.start, pressed, nullptr, nullptr, &last_edge_start_ms_);
            if (pressed) {
                queued_menu_ = true;
                queued_menu_ms_ = last_edge_start_ms_;
            }
            break;
        case BTN_SELECT:
        case KEY_SELECT:
            set_button_state(raw_.select, pressed, &queued_hold_select_, nullptr, &last_edge_select_ms_);
            break;
        case BTN_TL:
        case BTN_TL2:
            set_button_state(raw_.l, pressed, &queued_hold_l_, nullptr, &last_edge_l_ms_);
            break;
        case BTN_TR:
        case BTN_TR2:
            set_button_state(raw_.r, pressed, &queued_hold_r_, nullptr, &last_edge_r_ms_);
            break;
        case BTN_THUMBL:
        case BTN_THUMBR:
            set_button_state(raw_.joy, pressed, nullptr, nullptr, nullptr);
            break;
        default:
            break;
    }
}

void Input::handle_joy_axis(uint8_t axis, int value) {
    switch (axis) {
        case 0:
            raw_.axis_x = value;
            break;
        case 1:
            raw_.axis_y = value;
            break;
        case 6:
            set_button_state(raw_.left, value < -16000, nullptr, &queued_nav_left_, nullptr);
            set_button_state(raw_.right, value > 16000, nullptr, &queued_nav_right_, nullptr);
            break;
        case 7:
            set_button_state(raw_.up, value < -16000, nullptr, &queued_nav_up_, nullptr);
            set_button_state(raw_.down, value > 16000, nullptr, &queued_nav_down_, nullptr);
            break;
        default:
            break;
    }
}

void Input::handle_joy_hat(uint8_t value) {
    set_button_state(raw_.up, (value & SDL_HAT_UP) != 0, nullptr, &queued_nav_up_, nullptr);
    set_button_state(raw_.down, (value & SDL_HAT_DOWN) != 0, nullptr, &queued_nav_down_, nullptr);
    set_button_state(raw_.left, (value & SDL_HAT_LEFT) != 0, nullptr, &queued_nav_left_, nullptr);
    set_button_state(raw_.right, (value & SDL_HAT_RIGHT) != 0, nullptr, &queued_nav_right_, nullptr);
}

void Input::handle_abs_event(int code, int value) {
    switch (code) {
        case ABS_X:
            raw_.axis_x = value;
            prime_axis_center(&axis_x_cal_, value);
            break;
        case ABS_Y:
            raw_.axis_y = value;
            prime_axis_center(&axis_y_cal_, value);
            break;
        case ABS_HAT0X:
            set_button_state(raw_.left, value < 0, nullptr, &queued_nav_left_, nullptr);
            set_button_state(raw_.right, value > 0, nullptr, &queued_nav_right_, nullptr);
            break;
        case ABS_HAT0Y:
            set_button_state(raw_.up, value < 0, nullptr, &queued_nav_up_, nullptr);
            set_button_state(raw_.down, value > 0, nullptr, &queued_nav_down_, nullptr);
            break;
        default:
            break;
    }
}

bool Input::compute_repeat(int idx, bool held, uint64_t now_ms) {
    if (!held) {
        repeat_due_[idx] = 0;
        return false;
    }

    if (repeat_due_[idx] == 0) {
        repeat_due_[idx] = now_ms + 320;
        return true;
    }

    if (now_ms >= repeat_due_[idx]) {
        repeat_due_[idx] = now_ms + 120;
        return true;
    }

    return false;
}

void Input::update_snapshot(uint64_t now_ms) {
    int axis_x = 0;
    int axis_y = 0;

    if (raw_.left && !raw_.right) axis_x = -28000;
    if (raw_.right && !raw_.left) axis_x = 28000;
    if (raw_.up && !raw_.down) axis_y = -28000;
    if (raw_.down && !raw_.up) axis_y = 28000;

    if (raw_.axis_x != 0) axis_x = raw_.axis_x;
    if (raw_.axis_y != 0) axis_y = raw_.axis_y;

    const int centered_x = axis_x - axis_x_cal_.center;
    const int centered_y = axis_y - axis_y_cal_.center;

    const bool axis_left = centered_x < -axis_x_cal_.deadzone;
    const bool axis_right = centered_x > axis_x_cal_.deadzone;
    const bool axis_up = centered_y < -axis_y_cal_.deadzone;
    const bool axis_down = centered_y > axis_y_cal_.deadzone;

    snapshot_.hold_left = raw_.left;
    snapshot_.hold_right = raw_.right;
    snapshot_.hold_up = raw_.up;
    snapshot_.hold_down = raw_.down;
    snapshot_.hold_a = raw_.a;
    snapshot_.hold_b = raw_.b;
    snapshot_.hold_start = raw_.start;
    snapshot_.hold_select = raw_.select || queued_hold_select_;
    snapshot_.hold_l = raw_.l || queued_hold_l_;
    snapshot_.hold_r = raw_.r || queued_hold_r_;
    snapshot_.hold_joy = raw_.joy;
    snapshot_.axis_x = centered_x;
    snapshot_.axis_y = centered_y;
    const bool select_edge = (raw_.select && !prev_raw_.select) || queued_hold_select_;
    const bool l_edge = (raw_.l && !prev_raw_.l) || queued_hold_l_;
    const bool r_edge = (raw_.r && !prev_raw_.r) || queued_hold_r_;
    snapshot_.edge_select_ms = select_edge ? last_edge_select_ms_ : 0;
    snapshot_.edge_l_ms = l_edge ? last_edge_l_ms_ : 0;
    snapshot_.edge_r_ms = r_edge ? last_edge_r_ms_ : 0;

    const bool nav_left = raw_.left || axis_left;
    const bool nav_right = raw_.right || axis_right;
    const bool nav_up = raw_.up || axis_up;
    const bool nav_down = raw_.down || axis_down;

    snapshot_.nav_left = queued_nav_left_ || compute_repeat(0, nav_left, now_ms);
    snapshot_.nav_right = queued_nav_right_ || compute_repeat(1, nav_right, now_ms);
    snapshot_.nav_up = queued_nav_up_ || compute_repeat(2, nav_up, now_ms);
    snapshot_.nav_down = queued_nav_down_ || compute_repeat(3, nav_down, now_ms);

    const bool accept_edge = raw_.a && !prev_raw_.a;
    const bool back_edge = raw_.b && !prev_raw_.b;
    const bool menu_edge = raw_.start && !prev_raw_.start;
    snapshot_.accept = queued_accept_ || accept_edge;
    snapshot_.back = queued_back_ || back_edge;
    snapshot_.menu = queued_menu_ || menu_edge;
    snapshot_.edge_accept_ms = queued_accept_ ? queued_accept_ms_ : (accept_edge ? last_edge_a_ms_ : 0);
    snapshot_.edge_back_ms = queued_back_ ? queued_back_ms_ : (back_edge ? last_edge_b_ms_ : 0);
    snapshot_.edge_menu_ms = queued_menu_ ? queued_menu_ms_ : (menu_edge ? last_edge_start_ms_ : 0);

    prev_raw_ = raw_;
    queued_nav_up_ = false;
    queued_nav_down_ = false;
    queued_nav_left_ = false;
    queued_nav_right_ = false;
    queued_hold_select_ = false;
    queued_hold_l_ = false;
    queued_hold_r_ = false;
    queued_accept_ = false;
    queued_back_ = false;
    queued_menu_ = false;
}

void Input::poll(int timeout_ms) {
    if (timeout_ms > 0) SDL_Delay(static_cast<uint32_t>(timeout_ms));

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            raise(SIGTERM);
            continue;
        }

        if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            map_keycode(ev.key.keysym.sym, ev.type == SDL_KEYDOWN);
            continue;
        }

        if (ev.type == SDL_JOYAXISMOTION) {
            handle_joy_axis(ev.jaxis.axis, ev.jaxis.value);
            continue;
        }

        if (ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_JOYBUTTONUP) {
            map_joy_button(ev.jbutton.button, ev.type == SDL_JOYBUTTONDOWN);
            continue;
        }

        if (ev.type == SDL_JOYHATMOTION) {
            handle_joy_hat(ev.jhat.value);
            continue;
        }

    }

    for (size_t i = 0; i < evdev_fds_.size(); ++i) {
        const int fd = evdev_fds_[i];
        if (fd < 0) continue;

        struct input_event iev;
        ssize_t n = 0;
        do {
            n = read(fd, &iev, sizeof(iev));
            if (n != static_cast<ssize_t>(sizeof(iev))) break;

            if (iev.type == EV_KEY) {
                map_ev_key(iev.code, iev.value != 0);
            } else if (iev.type == EV_ABS) {
                handle_abs_event(iev.code, iev.value);
            }
        } while (n == static_cast<ssize_t>(sizeof(iev)));

        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            continue;
        }
    }

    update_snapshot(monotonic_ms());
}

InputSnapshot Input::snapshot() const {
    return snapshot_;
}

void Input::clear_edges() {
    snapshot_.accept = false;
    snapshot_.back = false;
    snapshot_.menu = false;
    snapshot_.nav_left = false;
    snapshot_.nav_right = false;
    snapshot_.nav_up = false;
    snapshot_.nav_down = false;
}

}  // namespace inconsole
