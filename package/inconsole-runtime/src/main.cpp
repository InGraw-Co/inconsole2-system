#include "runtime.hpp"

#include <algorithm>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace inconsole {

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

std::string data_root() {
    static std::string root;
    if (!root.empty()) return root;

    const char *env = getenv("INCONSOLE_DATA_ROOT");
    if (env && env[0] != '\0') {
        root = env;
    } else {
        root = "/userdata";
    }
    return root;
}

std::string app_dir() { return data_root() + "/apps"; }
std::string system_dir() { return data_root() + "/system"; }
std::string logs_dir() { return system_dir() + "/logs"; }
std::string core_log_path() { return logs_dir() + "/runtime-core.log"; }

const char *signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        default: return "SIGNAL";
    }
}

void on_fatal_signal(int sig) {
    const char *name = signal_name(sig);
    const char *prefix = "[FATAL] ";
    const char *suffix = " in inconsole-runtime\n";
    const std::string log_path = core_log_path();
    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)write(fd, prefix, strlen(prefix));
        (void)write(fd, name, strlen(name));
        (void)write(fd, suffix, strlen(suffix));
        close(fd);
    }

    int tty = open("/dev/tty0", O_WRONLY);
    if (tty >= 0) {
        static const char kHead[] = "\033[2J\033[HInConsole runtime fatal error\n";
        static const char kTail[] = "\nCheck /userdata/system/logs/runtime-core.log\n";
        (void)write(tty, kHead, sizeof(kHead) - 1);
        (void)write(tty, "Signal: ", 8);
        (void)write(tty, name, strlen(name));
        (void)write(tty, kTail, sizeof(kTail) - 1);
        close(tty);
    }

    execl("/proc/self/exe", "inconsole-runtime", "--recovery", name, static_cast<char *>(nullptr));

    _exit(128 + sig);
}

const char *env_or_unset(const char *name) {
    const char *value = getenv(name);
    return value ? value : "(unset)";
}

void log_device_state(Logger *logger, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        logger->warn(std::string("Device missing: ") + path + " errno=" + std::to_string(errno));
        return;
    }

    errno = 0;
    if (access(path, R_OK | W_OK) == 0) {
        logger->info(std::string("Device ready: ") + path);
        return;
    }

    logger->warn(std::string("Device not writable: ") + path + " errno=" + std::to_string(errno));
}

bool restart_self(Logger *logger) {
    char exe_path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (n <= 0) {
        if (logger) logger->error("Restart failed: cannot resolve /proc/self/exe errno=" + std::to_string(errno));
        return false;
    }
    exe_path[n] = '\0';
    if (logger) logger->warn(std::string("Restarting runtime: ") + exe_path);
    execl(exe_path, exe_path, static_cast<char *>(nullptr));
    if (logger) logger->error("Restart failed: exec errno=" + std::to_string(errno));
    return false;
}

bool truthy_env(const char *v) {
    if (!v || !v[0]) return false;
    return !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "FALSE") == 0);
}

int estimate_battery_percent_from_mv(int mv) {
    struct Point {
        int mv;
        int pct;
    };
    static const Point curve[] = {
        {3300, 0}, {3500, 6}, {3600, 12}, {3700, 22}, {3750, 32}, {3800, 45},
        {3850, 56}, {3900, 66}, {3950, 76}, {4000, 86}, {4100, 95}, {4200, 100},
    };

    if (mv <= curve[0].mv) return 0;
    if (mv >= curve[sizeof(curve) / sizeof(curve[0]) - 1].mv) return 100;

    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (mv > curve[i].mv) continue;
        const int x0 = curve[i - 1].mv;
        const int y0 = curve[i - 1].pct;
        const int x1 = curve[i].mv;
        const int y1 = curve[i].pct;
        const int dx = std::max(1, x1 - x0);
        const int dy = y1 - y0;
        return std::max(0, std::min(100, y0 + ((mv - x0) * dy) / dx));
    }

    return 100;
}

bool consume_marker(const std::string &path, Logger *logger) {
    if (access(path.c_str(), F_OK) != 0) return false;
    if (unlink(path.c_str()) == 0) {
        if (logger) logger->warn("Consumed marker: " + path);
    } else if (logger) {
        logger->warn("Marker present but failed to remove: " + path + " errno=" + std::to_string(errno));
    }
    return true;
}

bool force_first_login_requested(Logger *logger) {
    const bool env_force = truthy_env(getenv("INCONSOLE_FORCE_FIRST_LOGIN"));
    const bool marker_force = consume_marker(system_dir() + "/force-first-login", logger);
    if (env_force && logger) logger->warn("Force first login requested by env");
    return env_force || marker_force;
}

uint32_t edge_delay_ms(uint64_t now_ms, uint64_t edge_ms) {
    if (edge_ms == 0 || now_ms < edge_ms) return 0;
    const uint64_t delta = now_ms - edge_ms;
    if (delta > 0xFFFFFFFFULL) return 0xFFFFFFFFU;
    return static_cast<uint32_t>(delta);
}

float fps_from_frame_ms(float frame_ms) {
    if (frame_ms <= 0.0f) return 0.0f;
    return 1000.0f / frame_ms;
}

int run_recovery_window(const std::string &message) {
    Logger logger(core_log_path());
    logger.error("Entering recovery mode: " + message);

    Renderer renderer;
    if (!renderer.init(&logger)) {
        int tty = open("/dev/tty0", O_WRONLY);
        if (tty >= 0) {
            const std::string text = std::string("RECOVERY MODE\n") + message +
                                     "\nPower off device manually.\n";
            (void)write(tty, text.c_str(), text.size());
            close(tty);
        }
        return 3;
    }

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_SPACE) {
                    quit = true;
                }
            }
        }

        renderer.begin_frame(Color{10, 14, 20});
        renderer.fill_rect(26, 30, renderer.width() - 52, renderer.height() - 60, Color{24, 34, 48});
        renderer.draw_rect_outline(26, 30, renderer.width() - 52, renderer.height() - 60, Color{176, 86, 86});
        renderer.draw_text_centered("RECOVERY MODE", renderer.width() / 2, 48, Color{240, 210, 210}, true);
        std::vector<std::string> lines = renderer.wrap_text_to_width(message, renderer.width() - 80, 5, false);
        for (size_t i = 0; i < lines.size(); ++i) {
            renderer.draw_text_centered(lines[i], renderer.width() / 2, 82 + static_cast<int>(i) * 16, Color{225, 232, 242});
        }
        renderer.draw_text_centered("Power off device or press ENTER to close recovery.", renderer.width() / 2, renderer.height() - 54,
                                    Color{190, 198, 210});
        renderer.present();
        SDL_Delay(16);
    }
    return 0;
}

}  // namespace

int run_recovery_mode_entry(const std::string &message) {
    return run_recovery_window(message);
}

App::App()
    : logger_(core_log_path()),
      settings_store_(system_dir() + "/settings.json"),
      profile_store_(system_dir() + "/profile.json"),
      registry_(),
      process_runner_(),
      battery_(),
      input_(),
      renderer_(),
      settings_(),
      profile_(),
      wizard_scene_(&profile_, &profile_store_, &settings_, &settings_store_, &logger_),
      pin_lock_scene_(&profile_, &settings_, &logger_),
      launcher_scene_(&profile_, &registry_, &settings_),
      settings_scene_(&settings_, &settings_store_, &profile_, &profile_store_, &logger_),
      diagnostics_scene_(&input_, &battery_, &settings_, &logger_),
      system_info_scene_(&registry_, &settings_, &logger_),
      file_manager_scene_(&settings_, &logger_),
      power_off_scene_(&settings_),
      scene_manager_(),
      overlay_message_(),
      overlay_until_ms_(0),
      volume_overlay_until_ms_(0),
      volume_overlay_value_(0),
      volume_before_mute_(70),
      mute_active_(false),
      prev_l_hold_(false),
      prev_r_hold_(false),
      prev_lr_combo_hold_(false),
      perf_stats_(),
      ui_snapshot_() {}

void App::show_message(const std::string &msg, uint32_t duration_ms) {
    overlay_message_ = msg;
    overlay_until_ms_ = SDL_GetTicks() + duration_ms;
    logger_.warn(msg);
}

void App::render_overlay(uint32_t now_ms) {
    if (!overlay_message_.empty()) {
        if (now_ms > overlay_until_ms_) {
            overlay_message_.clear();
        } else {
            renderer_.fill_rect(30, 210, renderer_.width() - 60, 36, Color{25, 30, 38});
            renderer_.draw_rect_outline(30, 210, renderer_.width() - 60, 36, Color{190, 110, 80});
            renderer_.draw_text_centered(overlay_message_, renderer_.width() / 2, 220, Color{240, 220, 210});
        }
    }

    if (now_ms < volume_overlay_until_ms_) {
        const int w = 178;
        const int h = 26;
        const int x = (renderer_.width() - w) / 2;
        const int y = 10;
        renderer_.fill_rect(x, y, w, h, Color{18, 24, 34});
        renderer_.draw_rect_outline(x, y, w, h, Color{88, 120, 168});
        const std::string label = translate_ui_text(normalize_language(settings_.language), "Volume", "Volume");
        renderer_.draw_text(label, x + 8, y + 5, Color{210, 220, 236});
        const int bw = 90;
        const int bx = x + w - bw - 10;
        const int by = y + 9;
        renderer_.fill_rect(bx, by, bw, 8, Color{30, 38, 52});
        renderer_.draw_rect_outline(bx, by, bw, 8, Color{110, 138, 178});
        const int fill = ((bw - 2) * std::max(0, std::min(100, volume_overlay_value_))) / 100;
        if (fill > 0) renderer_.fill_rect(bx + 1, by + 1, fill, 6, Color{92, 194, 255});
    }
}

int App::run() {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGSEGV, on_fatal_signal);
    signal(SIGABRT, on_fatal_signal);
    signal(SIGBUS, on_fatal_signal);
    signal(SIGILL, on_fatal_signal);
    signal(SIGFPE, on_fatal_signal);

    ensure_directory(data_root());
    ensure_directory(app_dir());
    ensure_directory(system_dir());
    ensure_directory(logs_dir());

    logger_.info("Starting InConsole Runtime V2");
    logger_.info("Data root: " + data_root());
    logger_.info("Process info: pid=" + std::to_string(getpid()) + " uid=" + std::to_string(getuid()) +
                 " euid=" + std::to_string(geteuid()));
    logger_.info(std::string("ENV SDL_VIDEODRIVER=") + env_or_unset("SDL_VIDEODRIVER"));
    logger_.info(std::string("ENV SDL_FBDEV=") + env_or_unset("SDL_FBDEV"));
    logger_.info(std::string("ENV SDL_NOMOUSE=") + env_or_unset("SDL_NOMOUSE"));
    log_device_state(&logger_, "/dev/fb0");
    log_device_state(&logger_, "/dev/tty0");
    log_device_state(&logger_, "/dev/console");
    log_device_state(&logger_, "/dev/input");
    log_device_state(&logger_, "/dev/input/event0");
    log_device_state(&logger_, "/dev/input/event1");
    log_device_state(&logger_, "/dev/input/event2");
    log_device_state(&logger_, "/dev/snd/controlC0");
    log_device_state(&logger_, "/dev/snd/pcmC0D0p");
    log_device_state(&logger_, "/dev/bus/usb");

    const bool force_first_login = force_first_login_requested(&logger_);
    if (force_first_login) {
        const std::string profile_path = system_dir() + "/profile.json";
        const std::string settings_path = system_dir() + "/settings.json";
        if (unlink(profile_path.c_str()) == 0) logger_.warn("Removed profile for forced first login");
        if (unlink(settings_path.c_str()) == 0) logger_.warn("Removed settings for forced first login");
        profile_ = Profile();
        settings_ = Settings();
    }

    settings_store_.load(&settings_, &logger_);
    settings_store_.apply_volume(settings_.volume, &logger_);
    settings_store_.apply_brightness(settings_.brightness, &logger_);

    bool has_profile = false;
    if (!force_first_login) has_profile = profile_store_.load(&profile_, &logger_);

    registry_.scan(app_dir(), &logger_);

    logger_.info("Initializing renderer");
    if (!renderer_.init(&logger_)) {
        logger_.error("Renderer initialization failed");
        return run_recovery_window("Critical runtime error: renderer initialization failed.");
    }
    logger_.info("Renderer ready");

    logger_.info("Initializing input");
    input_.init(&logger_);
    logger_.info("Input ready");

    scene_manager_.register_scene(SCENE_WIZARD, &wizard_scene_);
    scene_manager_.register_scene(SCENE_PIN_LOCK, &pin_lock_scene_);
    scene_manager_.register_scene(SCENE_LAUNCHER, &launcher_scene_);
    scene_manager_.register_scene(SCENE_SETTINGS, &settings_scene_);
    scene_manager_.register_scene(SCENE_DIAGNOSTICS, &diagnostics_scene_);
    scene_manager_.register_scene(SCENE_SYSTEM_INFO, &system_info_scene_);
    scene_manager_.register_scene(SCENE_FILE_MANAGER, &file_manager_scene_);
    scene_manager_.register_scene(SCENE_POWER_OFF, &power_off_scene_);

    SceneId initial_scene = SCENE_WIZARD;
    if (has_profile) {
        initial_scene = (profile_.pin_enabled && profile_.pin.size() == 4) ? SCENE_PIN_LOCK : SCENE_LAUNCHER;
    }
    scene_manager_.set_initial(initial_scene);

    uint32_t prev_ms = SDL_GetTicks();
    int target_fps = 120;
    const char *target_fps_env = getenv("INCONSOLE_TARGET_FPS");
    if (target_fps_env && target_fps_env[0] != '\0') {
        const int parsed = atoi(target_fps_env);
        if (parsed >= 0 && parsed <= 500) target_fps = parsed;
    }
    const uint32_t target_frame_ms = (target_fps > 0) ? static_cast<uint32_t>(std::max(1, 1000 / target_fps)) : 0;
    logger_.info(target_fps > 0 ? ("Frame limiter: " + std::to_string(target_fps) + " FPS")
                                : std::string("Frame limiter: OFF"));
    uint32_t next_perf_log_ms = 0;
    bool prev_select_hold_ = false;
    std::deque<uint32_t> frame_samples_ms;
    const size_t kFrameSampleLimit = 240;
    int pending_volume_delta = 0;
    uint32_t pending_volume_due_ms = 0;
    const uint32_t combo_guard_ms = 120;

    while (!g_stop) {
        uint32_t frame_start = SDL_GetTicks();
        uint32_t delta_ms = frame_start - prev_ms;
        prev_ms = frame_start;
        const uint64_t frame_mono_ms = monotonic_ms();
        const uint32_t update_start_ms = SDL_GetTicks();

        input_.poll(0);
        InputSnapshot snap = input_.snapshot();

        const bool l_edge = snap.hold_l && !prev_l_hold_;
        const bool r_edge = snap.hold_r && !prev_r_hold_;
        const bool select_edge = snap.hold_select && !prev_select_hold_;
        const bool lr_combo = snap.hold_l && snap.hold_r;
        const bool lr_combo_edge = lr_combo && !prev_lr_combo_hold_;
        prev_l_hold_ = snap.hold_l;
        prev_r_hold_ = snap.hold_r;
        prev_select_hold_ = snap.hold_select;
        prev_lr_combo_hold_ = lr_combo;

        if (snap.accept && snap.edge_accept_ms != 0) {
            perf_stats_.last_input_delay_a_ms = edge_delay_ms(frame_mono_ms, snap.edge_accept_ms);
        }
        if (snap.back && snap.edge_back_ms != 0) {
            perf_stats_.last_input_delay_b_ms = edge_delay_ms(frame_mono_ms, snap.edge_back_ms);
        }
        if (snap.menu && snap.edge_menu_ms != 0) {
            perf_stats_.last_input_delay_start_ms = edge_delay_ms(frame_mono_ms, snap.edge_menu_ms);
        }
        if (select_edge && snap.edge_select_ms != 0) {
            perf_stats_.last_input_delay_select_ms = edge_delay_ms(frame_mono_ms, snap.edge_select_ms);
        }
        if (l_edge && snap.edge_l_ms != 0) {
            perf_stats_.last_input_delay_l_ms = edge_delay_ms(frame_mono_ms, snap.edge_l_ms);
        }
        if (r_edge && snap.edge_r_ms != 0) {
            perf_stats_.last_input_delay_r_ms = edge_delay_ms(frame_mono_ms, snap.edge_r_ms);
        }

        if (lr_combo_edge) {
            pending_volume_delta = 0;
            pending_volume_due_ms = 0;
            if (!mute_active_) {
                if (settings_.volume > 0) volume_before_mute_ = settings_.volume;
                settings_.volume = 0;
                mute_active_ = true;
                show_message(translate_ui_text(normalize_language(settings_.language), "Muted", "Muted"), 900);
            } else {
                const int restore = std::max(0, std::min(100, volume_before_mute_ > 0 ? volume_before_mute_ : 70));
                settings_.volume = restore;
                mute_active_ = false;
                show_message(translate_ui_text(normalize_language(settings_.language), "Unmuted", "Unmuted"), 900);
            }
            settings_store_.apply_volume(settings_.volume, &logger_);
            settings_store_.save(settings_, &logger_);
            volume_overlay_value_ = settings_.volume;
            volume_overlay_until_ms_ = frame_start + 1000;
        }

        if (!lr_combo && (l_edge || r_edge)) {
            pending_volume_delta = l_edge ? -5 : 5;
            pending_volume_due_ms = frame_start + combo_guard_ms;
        }

        if (lr_combo) {
            pending_volume_delta = 0;
            pending_volume_due_ms = 0;
        } else if (pending_volume_delta != 0 && frame_start >= pending_volume_due_ms) {
            settings_.volume = std::max(0, std::min(100, settings_.volume + pending_volume_delta));
            pending_volume_delta = 0;
            pending_volume_due_ms = 0;
            mute_active_ = (settings_.volume == 0);
            if (settings_.volume > 0) volume_before_mute_ = settings_.volume;
            settings_store_.apply_volume(settings_.volume, &logger_);
            settings_store_.save(settings_, &logger_);
            volume_overlay_value_ = settings_.volume;
            volume_overlay_until_ms_ = frame_start + 1000;
        }

        Scene *scene = scene_manager_.current_scene();
        SceneOutput out;
        if (scene) out = scene->update(snap, frame_start);

        if (out.request_scene) {
            scene_manager_.request(out.next_scene);
        }

        if (out.request_restart) {
            logger_.warn("Restart requested by scene");
            restart_self(&logger_);
            return 0;
        }

        if (out.request_launch) {
            renderer_.begin_frame(Color{0, 0, 0});
            renderer_.draw_text_centered("Launching " + out.launch_app.name + "...", renderer_.width() / 2, renderer_.height() / 2 - 8,
                                         Color{220, 228, 236}, true);
            renderer_.present();

            std::string error_text;
            int rc = 0;
            if (out.launch_app.exclusive_runtime) {
                logger_.info("Exclusive app launch requested, shutting down SDL before exec");
                SDL_Quit();
                rc = process_runner_.run_app_exclusive(out.launch_app, &logger_, &error_text);
            } else {
                rc = process_runner_.run_app(out.launch_app, &logger_, &error_text);
            }
            input_.clear_edges();

            if (rc != 0) {
                if (error_text.empty()) error_text = "Launch failed";
                show_message(error_text, 2200);
            }

            registry_.scan(app_dir(), &logger_);
        }

        scene_manager_.update(delta_ms);
        const BatteryInfo &battery = battery_.update(&logger_);
        ui_snapshot_.battery = battery;
        if (battery.has_capacity) {
            ui_snapshot_.battery_percent = std::max(0, std::min(100, battery.capacity));
        } else if (battery.has_voltage) {
            ui_snapshot_.battery_percent = estimate_battery_percent_from_mv(battery.voltage_mv);
        } else {
            ui_snapshot_.battery_percent = -1;
        }
        ui_snapshot_.scene_id = static_cast<int>(scene_manager_.current_id());
        ui_snapshot_.now_ms = frame_start;

        const uint32_t update_end_ms = SDL_GetTicks();
        perf_stats_.update_ms = update_end_ms - update_start_ms;
        ui_snapshot_.perf = perf_stats_;

        const uint32_t render_start_ms = SDL_GetTicks();
        scene_manager_.render(&renderer_, frame_start, ui_snapshot_);
        render_overlay(frame_start);
        const uint32_t render_end_ms = SDL_GetTicks();
        perf_stats_.render_ms = render_end_ms - render_start_ms;

        const uint32_t present_start_ms = SDL_GetTicks();
        renderer_.present();
        const uint32_t present_end_ms = SDL_GetTicks();
        perf_stats_.present_ms = present_end_ms - present_start_ms;

        uint32_t frame_time = SDL_GetTicks() - frame_start;
        if (target_frame_ms > 0 && frame_time < target_frame_ms) SDL_Delay(target_frame_ms - frame_time);

        const uint32_t frame_end_ms = SDL_GetTicks();
        perf_stats_.frame_ms = frame_end_ms - frame_start;
        frame_samples_ms.push_back(perf_stats_.frame_ms);
        while (frame_samples_ms.size() > kFrameSampleLimit) frame_samples_ms.pop_front();

        if (perf_stats_.frame_ms > 100) {
            ++perf_stats_.slow_frame_count;
            logger_.warn("Slow frame: frame=" + std::to_string(perf_stats_.frame_ms) + "ms update=" +
                         std::to_string(perf_stats_.update_ms) + "ms render=" + std::to_string(perf_stats_.render_ms) +
                         "ms present=" + std::to_string(perf_stats_.present_ms) + "ms scene=" +
                         std::to_string(static_cast<int>(scene_manager_.current_id())));
        }

        if (frame_start >= next_perf_log_ms) {
            next_perf_log_ms = frame_start + 1000;
            if (!frame_samples_ms.empty()) {
                uint64_t sum_ms = 0;
                for (size_t i = 0; i < frame_samples_ms.size(); ++i) sum_ms += frame_samples_ms[i];
                const float avg_frame_ms = static_cast<float>(sum_ms) / static_cast<float>(frame_samples_ms.size());
                perf_stats_.fps_avg = fps_from_frame_ms(avg_frame_ms);

                std::vector<uint32_t> sorted(frame_samples_ms.begin(), frame_samples_ms.end());
                std::sort(sorted.begin(), sorted.end());
                size_t p95_idx = (sorted.size() * 95) / 100;
                if (p95_idx >= sorted.size()) p95_idx = sorted.size() - 1;
                perf_stats_.fps_p95 = fps_from_frame_ms(static_cast<float>(sorted[p95_idx]));
            }

            logger_.info("Perf: fps_avg=" + std::to_string(perf_stats_.fps_avg) + " fps_p95=" + std::to_string(perf_stats_.fps_p95) +
                         " frame_ms=" + std::to_string(perf_stats_.frame_ms) + " update_ms=" + std::to_string(perf_stats_.update_ms) +
                         " render_ms=" + std::to_string(perf_stats_.render_ms) + " present_ms=" +
                         std::to_string(perf_stats_.present_ms) + " slow_frames=" +
                         std::to_string(perf_stats_.slow_frame_count) + " input_delay_ms[A/B/START/SELECT/L/R]=" +
                         std::to_string(perf_stats_.last_input_delay_a_ms) + "/" +
                         std::to_string(perf_stats_.last_input_delay_b_ms) + "/" +
                         std::to_string(perf_stats_.last_input_delay_start_ms) + "/" +
                         std::to_string(perf_stats_.last_input_delay_select_ms) + "/" +
                         std::to_string(perf_stats_.last_input_delay_l_ms) + "/" +
                         std::to_string(perf_stats_.last_input_delay_r_ms) + " scene=" +
                         std::to_string(static_cast<int>(scene_manager_.current_id())));
        }

        ui_snapshot_.perf = perf_stats_;
    }

    logger_.info("Runtime stopped");
    return 0;
}

}  // namespace inconsole

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--recovery") {
        const std::string msg = (argc > 2) ? std::string(argv[2]) : std::string("Critical runtime error");
        return inconsole::run_recovery_mode_entry(msg);
    }
    inconsole::App app;
    return app.run();
}
