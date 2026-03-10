#include "runtime.hpp"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace inconsole {

namespace {

float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

Color mix_color(const Color &a, const Color &b, float t) {
    const float k = clamp01(t);
    Color out;
    out.r = static_cast<uint8_t>(static_cast<float>(a.r) + (static_cast<float>(b.r) - static_cast<float>(a.r)) * k);
    out.g = static_cast<uint8_t>(static_cast<float>(a.g) + (static_cast<float>(b.g) - static_cast<float>(a.g)) * k);
    out.b = static_cast<uint8_t>(static_cast<float>(a.b) + (static_cast<float>(b.b) - static_cast<float>(a.b)) * k);
    return out;
}

LayoutRect inset(const LayoutRect &r, int p) {
    LayoutRect out(r.x + p, r.y + p, r.w - p * 2, r.h - p * 2);
    if (out.w < 1) out.w = 1;
    if (out.h < 1) out.h = 1;
    return out;
}

std::string tr(const Settings *settings, const char *pl, const char *en) {
    const std::string lang = settings ? normalize_language(settings->language) : std::string("pl");
    (void)pl;
    return translate_ui_text(lang, std::string(en), std::string(en));
}

const Theme &ui_theme(const Settings *settings) {
    if (!settings) return theme_by_id("tech_noir");
    return theme_by_id(settings->theme_id);
}

void draw_card(Renderer *renderer, const LayoutRect &r, const Theme &theme, bool focused) {
    const Color top = focused ? mix_color(theme.panel_focus_top, theme.accent, 0.10f) : theme.panel_top;
    const Color bottom = focused ? mix_color(theme.panel_focus_bottom, Color{0, 0, 0}, 0.10f) : theme.panel_bottom;
    const Color border = focused ? theme.panel_focus_border : theme.panel_border;
    renderer->draw_panel_cached(focused ? "focus" : "normal", r.x, r.y, r.w, r.h, top, bottom, border);
}

void draw_soft_separator(Renderer *renderer, int x, int y, int w, const Theme &theme) {
    renderer->fill_rect(x, y, w, 1, mix_color(theme.panel_border, theme.background_bottom, 0.45f));
}

void draw_vertical_scrollbar(Renderer *renderer,
                             const LayoutRect &area,
                             int total_items,
                             int visible_items,
                             int offset,
                             const Theme &theme) {
    if (total_items <= visible_items || visible_items <= 0 || area.h <= 0 || area.w <= 0) return;
    const int max_offset = std::max(1, total_items - visible_items);
    const int clamped_offset = std::max(0, std::min(offset, max_offset));
    const int track_x = area.right() - 4;
    const int track_y = area.y;
    const int track_h = area.h;
    renderer->fill_rect(track_x, track_y, 2, track_h, mix_color(theme.panel_border, theme.background_bottom, 0.35f));

    int thumb_h = (track_h * visible_items) / std::max(visible_items, total_items);
    if (thumb_h < 10) thumb_h = 10;
    if (thumb_h > track_h) thumb_h = track_h;
    const int travel = std::max(0, track_h - thumb_h);
    const int thumb_y = track_y + (travel * clamped_offset) / max_offset;
    renderer->fill_rect(track_x, thumb_y, 2, thumb_h, mix_color(theme.accent, Color{255, 255, 255}, 0.20f));
}

void draw_battery_icon(Renderer *renderer, int x, int y, int w, int h, int percent, const Theme &theme) {
    const int p = std::max(0, std::min(100, percent));
    renderer->draw_rect_outline(x, y, w, h, theme.text_muted);
    renderer->fill_rect(x + w, y + h / 3, 2, h / 3, theme.text_muted);
    const int fill_w = ((w - 4) * p) / 100;
    if (fill_w > 0) {
        Color fill = p < 20 ? theme.danger : (p < 40 ? theme.warn : theme.ok);
        renderer->fill_rect(x + 2, y + 2, fill_w, h - 4, fill);
    }
}

void draw_top_bar(Renderer *renderer,
                  const LayoutMetrics &layout,
                  const Theme &theme,
                  const std::string &left_title,
                  const std::string &right_info,
                  int battery_percent) {
    const LayoutRect bar = layout.top_bar;
    draw_card(renderer, bar, theme, false);
    renderer->draw_text(left_title, bar.x + 10, bar.y + 8, theme.text_primary, true);
    draw_battery_icon(renderer, bar.right() - 86, bar.y + 11, 20, 10, battery_percent, theme);
    renderer->draw_text(std::to_string(std::max(0, std::min(100, battery_percent))) + "%", bar.right() - 60, bar.y + 8, theme.text_muted);
    if (!right_info.empty()) {
        const int w = renderer->text_width(right_info, false);
        renderer->draw_text(right_info, bar.right() - 92 - w - 10, bar.y + 8, theme.text_muted);
    }
}

void draw_footer(Renderer *renderer, const LayoutMetrics &layout, const Theme &theme, const std::string &hint) {
    draw_card(renderer, layout.footer, theme, false);
    const std::string text = renderer->ellipsize_to_width(hint, layout.footer.w - 10, false);
    renderer->draw_text(text, layout.footer.x + 6, layout.footer.y + 5, theme.text_muted);
}

void render_static_background(Renderer *renderer, const Theme &theme) {
    renderer->draw_background_cached(theme.id,
                                     mix_color(theme.background_top, Color{255, 255, 255}, 0.04f),
                                     mix_color(theme.background_bottom, Color{0, 0, 0}, 0.05f),
                                     mix_color(theme.background_bottom, Color{0, 0, 0}, 0.18f));
}

std::string safe_username(const Profile *profile) {
    if (!profile || profile->username.empty()) return "Player";
    return profile->username;
}

std::string trim_copy(const std::string &in) {
    size_t b = 0;
    while (b < in.size() && (in[b] == ' ' || in[b] == '\t' || in[b] == '\n' || in[b] == '\r')) ++b;
    size_t e = in.size();
    while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\n' || in[e - 1] == '\r')) --e;
    return in.substr(b, e - b);
}

std::string lower_copy(const std::string &in) {
    std::string out = in;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string normalize_username_for_id(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == ' ') c = '_';
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc > 127) continue;
        if (std::islower(uc)) c = static_cast<char>(std::toupper(uc));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        }
    }
    return out;
}

std::string localize_taxonomy_token(const std::string &token, const Settings *settings) {
    const std::string lang = settings ? normalize_language(settings->language) : std::string("pl");
    const std::string raw = trim_copy(token);
    const std::string norm = lower_copy(raw);
    std::string key;
    if (norm == "game") key = "cat.game";
    else if (norm == "games") key = "cat.games";
    else if (norm == "tool") key = "cat.tool";
    else if (norm == "tools") key = "cat.tools";
    else if (norm == "app") key = "cat.app";
    else if (norm == "apps") key = "cat.apps";
    else if (norm == "system") key = "cat.system";
    else if (norm == "demo") key = "cat.demo";
    else if (norm == "diagnostics") key = "cat.diagnostics";
    if (key.empty()) return raw;
    return translate_ui_text(lang, key, raw);
}

std::string localized_csv_value(const std::string &raw, const Settings *settings) {
    size_t sep = raw.find('|');
    if (sep == std::string::npos) {
        const size_t c1 = raw.find(',');
        const size_t c2 = (c1 == std::string::npos) ? std::string::npos : raw.find(',', c1 + 1);
        if (c1 != std::string::npos && c2 == std::string::npos) sep = c1;
    }
    if (sep == std::string::npos) return localize_taxonomy_token(raw, settings);
    const std::string first = trim_copy(raw.substr(0, sep));
    const std::string second = trim_copy(raw.substr(sep + 1));
    const std::string lang = settings ? normalize_language(settings->language) : std::string("pl");
    const std::string selected = (lang == "pl") ? (second.empty() ? first : second) : first;
    return localize_taxonomy_token(selected, settings);
}

std::string scene_data_root() {
    const char *env = getenv("INCONSOLE_DATA_ROOT");
    if (env && env[0] != '\0') return std::string(env);
    return "/userdata";
}

void push_log_line(std::vector<std::string> *lines, const std::string &line, size_t limit = 6) {
    if (!lines) return;
    if (line.empty()) return;
    lines->push_back(line);
    if (lines->size() > limit) lines->erase(lines->begin(), lines->begin() + static_cast<long>(lines->size() - limit));
}

void parse_script_output(std::string *buffer, const char *data, size_t n, std::string *last_line, std::vector<std::string> *log_lines) {
    if (!buffer || !data || n == 0) return;
    buffer->append(data, n);
    size_t nl = buffer->find('\n');
    while (nl != std::string::npos) {
        std::string line = trim_copy(buffer->substr(0, nl));
        if (!line.empty()) {
            if (last_line) *last_line = line;
            push_log_line(log_lines, line);
        }
        buffer->erase(0, nl + 1);
        nl = buffer->find('\n');
    }
}

void flush_script_output_buffer(std::string *buffer, std::string *last_line, std::vector<std::string> *log_lines) {
    if (!buffer) return;
    std::string line = trim_copy(*buffer);
    if (!line.empty()) {
        if (last_line) *last_line = line;
        push_log_line(log_lines, line);
    }
    buffer->clear();
}

bool spawn_script_process(const std::string &script_path, pid_t *pid, int *read_fd) {
    if (!pid || !read_fd) return false;
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) return false;

    pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    if (child == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", script_path.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    close(fds[1]);
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    *pid = child;
    *read_fd = fds[0];
    return true;
}

void poll_script_process_output(int fd, std::string *buffer, std::string *last_line, std::vector<std::string> *log_lines) {
    if (fd < 0) return;
    char chunk[256];
    while (true) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            parse_script_output(buffer, chunk, static_cast<size_t>(n), last_line, log_lines);
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        break;
    }
}

bool run_script_blocking(const std::string &script_path, std::vector<std::string> *log_lines) {
    pid_t pid = -1;
    int read_fd = -1;
    std::string buffer;
    std::string latest;
    if (!spawn_script_process(script_path, &pid, &read_fd)) return false;

    bool done = false;
    bool ok = false;
    while (!done) {
        poll_script_process_output(read_fd, &buffer, &latest, log_lines);
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            done = true;
            ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        } else if (r < 0) {
            done = true;
            ok = false;
        } else {
            usleep(15000);
        }
    }
    poll_script_process_output(read_fd, &buffer, &latest, log_lines);
    flush_script_output_buffer(&buffer, &latest, log_lines);
    if (read_fd >= 0) close(read_fd);
    return ok;
}

bool remove_path_recursive(const std::string &path) {
    if (path.empty() || path == "/") return false;
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path.c_str()) == 0;
    }

    DIR *dir = opendir(path.c_str());
    if (!dir) return false;
    struct dirent *ent = nullptr;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) continue;
        const std::string child = path + "/" + ent->d_name;
        if (!remove_path_recursive(child)) ok = false;
    }
    closedir(dir);
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) ok = false;
    return ok;
}

std::string proc_value_after_colon(const std::string &path, const char *prefix) {
    std::string text = read_text_file(path);
    if (text.empty()) return std::string();
    const std::string p(prefix);
    size_t at = text.find(p);
    if (at == std::string::npos) return std::string();
    size_t colon = text.find(':', at + p.size());
    if (colon == std::string::npos) return std::string();
    size_t line_end = text.find('\n', colon + 1);
    if (line_end == std::string::npos) line_end = text.size();
    return trim_copy(text.substr(colon + 1, line_end - (colon + 1)));
}

long parse_first_long(const std::string &s) {
    if (s.empty()) return 0;
    return strtol(s.c_str(), nullptr, 10);
}

long free_disk_mib_for_scene() {
    struct statvfs st;
    if (statvfs(scene_data_root().c_str(), &st) != 0) return 0;
    unsigned long long bytes = static_cast<unsigned long long>(st.f_bavail) * static_cast<unsigned long long>(st.f_frsize);
    return static_cast<long>(bytes / (1024ULL * 1024ULL));
}

bool file_readable_cached(const std::string &path) {
    if (path.empty()) return false;
    static std::map<std::string, bool> cache;
    std::map<std::string, bool>::const_iterator it = cache.find(path);
    if (it != cache.end()) return it->second;
    const bool ok = access(path.c_str(), R_OK) == 0;
    if (cache.size() > 1024) cache.clear();
    cache[path] = ok;
    return ok;
}

int env_int_clamped(const char *name, int fallback, int min_value, int max_value) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    const int parsed = atoi(v);
    if (parsed < min_value) return min_value;
    if (parsed > max_value) return max_value;
    return parsed;
}

uint32_t scene_fade_phase_ms() {
    static const uint32_t value =
        static_cast<uint32_t>(env_int_clamped("INCONSOLE_SCENE_FADE_MS", 120, 60, 600));
    return value;
}

uint8_t scene_fade_max_alpha() {
    static const uint8_t value =
        static_cast<uint8_t>(env_int_clamped("INCONSOLE_SCENE_FADE_ALPHA", 120, 40, 220));
    return value;
}

class LayoutGuard {
public:
    LayoutGuard(const char *scene_name, int screen_w, int screen_h)
        : scene_name_(scene_name), screen_w_(screen_w), screen_h_(screen_h), enabled_(false) {
        const char *env = getenv("INCONSOLE_LAYOUT_GUARD");
        enabled_ = env && env[0] != '\0' && env[0] != '0';
    }

    void add(const char *name, const LayoutRect &rect, bool allow_overlap = false) {
        if (!enabled_) return;
        if (!rect.valid()) {
            report(std::string(name) + " invalid rect");
            return;
        }
        if (rect.x < 0 || rect.y < 0 || rect.right() > screen_w_ || rect.bottom() > screen_h_) {
            std::ostringstream ss;
            ss << name << " out of bounds: x=" << rect.x << " y=" << rect.y << " w=" << rect.w << " h=" << rect.h;
            report(ss.str());
        }
        if (!allow_overlap) {
            for (size_t i = 0; i < entries_count_; ++i) {
                if (entries_[i].allow_overlap) continue;
                if (rect.intersects(entries_[i].rect)) {
                    report(std::string(name) + " overlaps " + entries_[i].name);
                    break;
                }
            }
        }
        if (entries_count_ < kMaxEntries) {
            entries_[entries_count_].name = name;
            entries_[entries_count_].rect = rect;
            entries_[entries_count_].allow_overlap = allow_overlap;
            ++entries_count_;
        }
    }

private:
    struct Entry {
        std::string name;
        LayoutRect rect;
        bool allow_overlap;
    };

    static const size_t kMaxEntries = 96;
    Entry entries_[kMaxEntries];
    size_t entries_count_ = 0;
    const char *scene_name_;
    int screen_w_;
    int screen_h_;
    bool enabled_;

    void report(const std::string &msg) {
        std::fprintf(stderr, "[layout-guard][%s] %s\n", scene_name_, msg.c_str());
    }
};

void draw_system_chip(Renderer *renderer, const LayoutRect &r, const Theme &theme, bool focused, int kind) {
    draw_card(renderer, r, theme, focused);
    const char *icons_env = getenv("INCONSOLE_SYSTEM_ICONS");
    const std::string icon_root = (icons_env && icons_env[0] != '\0') ? std::string(icons_env) : std::string("/usr/share/inconsole/system-icons");
    const std::string data_root = scene_data_root();

    std::string path = icon_root + "/info.png";
    std::string fallback = data_root + "/apps/info/icon.png";
    if (kind == 0) {
        path = icon_root + "/settings.png";
        fallback = data_root + "/system/icons/settings.png";
    } else if (kind == 1) {
        path = icon_root + "/diagnostics.png";
        fallback = data_root + "/system/icons/diagnostics.png";
    } else if (kind == 2) {
        path = icon_root + "/info.png";
        fallback = data_root + "/apps/info/icon.png";
    } else if (kind == 3) {
        path = icon_root + "/power.png";
        fallback = data_root + "/system/icons/power.png";
    } else if (kind == 4) {
        path = icon_root + "/files.png";
        fallback = data_root + "/system/icons/files.png";
    }
    const int icon_size = r.w - 8;
    const int icon_x = r.x + (r.w - icon_size) / 2;
    const int icon_y = r.y + (r.h - icon_size) / 2;
    if (file_readable_cached(path)) {
        renderer->draw_icon(path, icon_x, icon_y, icon_size, icon_size);
    } else if (file_readable_cached(fallback)) {
        renderer->draw_icon(fallback, icon_x, icon_y, icon_size, icon_size);
    } else {
        renderer->draw_text_centered(kind == 0 ? "S" : (kind == 1 ? "D" : (kind == 2 ? "I" : (kind == 3 ? "P" : "F"))),
                                     r.x + r.w / 2,
                                     r.y + 6,
                                     focused ? theme.text_primary : theme.text_muted,
                                     true);
    }
}

int key_center_twice(const KeyboardKey &key) {
    return key.col * 2 + key.span;
}

int nearest_key_index(const std::vector<KeyboardKey> &keys, int target_row, int target_center2) {
    if (keys.empty()) return 0;
    int best_idx = 0;
    int best_score = std::numeric_limits<int>::max();
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        const int row_penalty = std::abs(keys[i].row - target_row) * 100;
        const int col_penalty = std::abs(key_center_twice(keys[i]) - target_center2);
        const int score = row_penalty + col_penalty;
        if (score < best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    return best_idx;
}

int move_keyboard_focus(const std::vector<KeyboardKey> &keys, int current_idx, int dx, int dy) {
    if (keys.empty()) return 0;
    current_idx = std::max(0, std::min(current_idx, static_cast<int>(keys.size()) - 1));
    const KeyboardKey &current = keys[current_idx];
    const int current_center = key_center_twice(current);

    if (dx != 0) {
        int best_idx = -1;
        int best_center = (dx < 0) ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max();
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            if (keys[i].row != current.row) continue;
            const int c = key_center_twice(keys[i]);
            if (dx < 0) {
                if (c < current_center && c > best_center) {
                    best_center = c;
                    best_idx = i;
                }
            } else {
                if (c > current_center && c < best_center) {
                    best_center = c;
                    best_idx = i;
                }
            }
        }
        if (best_idx >= 0) return best_idx;
        return current_idx;
    }

    if (dy != 0) {
        int target_row = -1;
        int row_delta_best = std::numeric_limits<int>::max();
        for (size_t i = 0; i < keys.size(); ++i) {
            const int delta = keys[i].row - current.row;
            if ((dy < 0 && delta >= 0) || (dy > 0 && delta <= 0)) continue;
            const int ad = std::abs(delta);
            if (ad < row_delta_best) {
                row_delta_best = ad;
                target_row = keys[i].row;
            }
        }
        if (target_row < 0) return current_idx;
        return nearest_key_index(keys, target_row, current_center);
    }

    return current_idx;
}

}  // namespace

SceneOutput::SceneOutput() : request_scene(false), next_scene(SCENE_LAUNCHER), request_launch(false), launch_app(), request_restart(false) {}

WizardScene::WizardScene(Profile *profile,
                         ProfileStore *profile_store,
                         Settings *settings,
                         SettingsStore *settings_store,
                         Logger *logger)
    : profile_(profile),
      profile_store_(profile_store),
      settings_(settings),
      settings_store_(settings_store),
      logger_(logger),
      step_(STEP_WELCOME),
      name_input_(),
      keyboard_layout_(),
      language_selected_(0),
      language_cursor_t_(0.0f),
      theme_selected_(0),
      theme_cursor_t_(0.0f),
      pin_choice_selected_(0),
      pin_choice_cursor_t_(0.0f),
      pin_cursor_(0),
      name_error_until_ms_(0),
      name_reserved_error_(false),
      welcome_cycle_base_ms_(0),
      install_pid_(-1),
      install_output_fd_(-1),
      install_running_(false),
      install_complete_(false),
      install_success_(true),
      install_buffer_(),
      install_status_(),
      install_facts_(),
      install_fact_idx_(0),
      install_fact_due_ms_(0) {
    init_keyboard_layout();
    pin_digits_[0] = 0;
    pin_digits_[1] = 0;
    pin_digits_[2] = 0;
    pin_digits_[3] = 0;
    install_facts_.push_back("Tip: You can change language later in Settings.");
    install_facts_.push_back("Tip: L/R changes system volume anywhere.");
    install_facts_.push_back("Tip: SELECT opens app details in launcher.");
    install_facts_.push_back("Tip: You can switch light and dark themes.");
}

void WizardScene::on_enter() {
    stop_install_phase();
    step_ = STEP_WELCOME;
    name_input_.panel_t = 0.0f;
    language_codes_ = available_languages();
    if (language_codes_.empty()) language_codes_.push_back("pl");
    {
        std::vector<std::string> ordered;
        auto push_if_found = [&](const char *code) {
            for (size_t i = 0; i < language_codes_.size(); ++i) {
                if (language_codes_[i] == code) {
                    ordered.push_back(language_codes_[i]);
                    break;
                }
            }
        };
        push_if_found("pl");
        push_if_found("en");
        for (size_t i = 0; i < language_codes_.size(); ++i) {
            bool exists = false;
            for (size_t j = 0; j < ordered.size(); ++j) {
                if (ordered[j] == language_codes_[i]) {
                    exists = true;
                    break;
                }
            }
            if (!exists) ordered.push_back(language_codes_[i]);
        }
        language_codes_ = ordered;
    }
    const std::string curr_lang = normalize_language(settings_->language);
    language_selected_ = 0;
    for (size_t i = 0; i < language_codes_.size(); ++i) {
        if (language_codes_[i] == curr_lang) {
            language_selected_ = static_cast<int>(i);
            break;
        }
    }
    language_cursor_t_ = static_cast<float>(language_selected_);
    theme_selected_ = normalize_theme_id(settings_->theme_id) == "graphite_light" ? 1 : 0;
    theme_cursor_t_ = static_cast<float>(theme_selected_);
    pin_choice_selected_ = 0;
    pin_choice_cursor_t_ = 0.0f;
    pin_cursor_ = 0;
    name_error_until_ms_ = 0;
    name_reserved_error_ = false;
    welcome_cycle_base_ms_ = 0;
    power_hint_.clear();
    reset_name_input_from_profile();

    pin_digits_[0] = 0;
    pin_digits_[1] = 0;
    pin_digits_[2] = 0;
    pin_digits_[3] = 0;
}

void WizardScene::init_keyboard_layout() {
    keyboard_layout_ = KeyboardLayout();
    keyboard_layout_.cols = 10;
    keyboard_layout_.rows = 4;
    keyboard_layout_.letter_keys.clear();
    keyboard_layout_.digit_keys.clear();

    auto add_key =
        [](std::vector<KeyboardKey> *list, const std::string &id, const std::string &text, int row, int col, int span, bool action) {
            KeyboardKey key;
            key.id = id;
            key.text = text;
            key.row = row;
            key.col = col;
            key.span = std::max(1, span);
            key.action = action;
            list->push_back(key);
        };

    const std::string row0 = "QWERTYUIOP";
    for (int i = 0; i < static_cast<int>(row0.size()); ++i) {
        add_key(&keyboard_layout_.letter_keys, std::string(1, row0[i]), std::string(1, row0[i]), 0, i, 1, false);
    }
    const std::string row1 = "ASDFGHJKL";
    for (int i = 0; i < static_cast<int>(row1.size()); ++i) {
        add_key(&keyboard_layout_.letter_keys, std::string(1, row1[i]), std::string(1, row1[i]), 1, i, 1, false);
    }
    const std::string row2 = "ZXCVBNM";
    for (int i = 0; i < static_cast<int>(row2.size()); ++i) {
        add_key(&keyboard_layout_.letter_keys, std::string(1, row2[i]), std::string(1, row2[i]), 2, 1 + i, 1, false);
    }
    add_key(&keyboard_layout_.letter_keys, "KEY_UNDERSCORE", "", 3, 0, 3, true);
    add_key(&keyboard_layout_.letter_keys, "KEY_BACKSPACE", "", 3, 3, 3, true);
    add_key(&keyboard_layout_.letter_keys, "KEY_CLEAR", "", 3, 6, 2, true);
    add_key(&keyboard_layout_.letter_keys, "KEY_MODE_123", "", 3, 8, 1, true);
    add_key(&keyboard_layout_.letter_keys, "KEY_DONE", "", 3, 9, 1, true);

    const std::string d0 = "12345";
    for (int i = 0; i < static_cast<int>(d0.size()); ++i) {
        add_key(&keyboard_layout_.digit_keys, std::string(1, d0[i]), std::string(1, d0[i]), 0, i * 2, 2, false);
    }
    const std::string d1 = "67890";
    for (int i = 0; i < static_cast<int>(d1.size()); ++i) {
        add_key(&keyboard_layout_.digit_keys, std::string(1, d1[i]), std::string(1, d1[i]), 1, i * 2, 2, false);
    }
    add_key(&keyboard_layout_.digit_keys, "KEY_UNDERSCORE", "", 2, 0, 4, true);
    add_key(&keyboard_layout_.digit_keys, "KEY_BACKSPACE", "", 2, 4, 3, true);
    add_key(&keyboard_layout_.digit_keys, "KEY_CLEAR", "", 2, 7, 3, true);
    add_key(&keyboard_layout_.digit_keys, "KEY_MODE_ABC", "", 3, 0, 5, true);
    add_key(&keyboard_layout_.digit_keys, "KEY_DONE", "", 3, 5, 5, true);
}

void WizardScene::reset_name_input_from_profile() {
    name_input_ = TextInputState();
    name_input_.max_len = 12;
    std::string source = profile_ ? profile_->username : "Player1";
    if (source.empty()) source = "Player1";
    name_input_.value = normalize_username_for_id(source);
    if (static_cast<int>(name_input_.value.size()) > name_input_.max_len) {
        name_input_.value.resize(static_cast<size_t>(name_input_.max_len));
    }

    if (name_input_.value.empty()) name_input_.value = "PLAYER1";
    name_input_.key_index = 0;
    name_input_.key_cursor_t = 0.0f;
    name_input_.digits_mode = false;
    name_input_.confirm_requested = false;
}

void WizardScene::update_name_input(const InputSnapshot &input) {
    const std::vector<KeyboardKey> &keys = name_input_.digits_mode ? keyboard_layout_.digit_keys : keyboard_layout_.letter_keys;
    if (keys.empty()) {
        name_input_.confirm_requested = false;
        return;
    }

    name_input_.key_index = std::max(0, std::min(name_input_.key_index, static_cast<int>(keys.size()) - 1));
    name_input_.confirm_requested = false;

    if (input.nav_left) name_input_.key_index = move_keyboard_focus(keys, name_input_.key_index, -1, 0);
    if (input.nav_right) name_input_.key_index = move_keyboard_focus(keys, name_input_.key_index, +1, 0);
    if (input.nav_up) name_input_.key_index = move_keyboard_focus(keys, name_input_.key_index, 0, -1);
    if (input.nav_down) name_input_.key_index = move_keyboard_focus(keys, name_input_.key_index, 0, +1);

    if (!input.accept) return;
    const KeyboardKey &key = keys[name_input_.key_index];
    if (!key.action) {
        if (static_cast<int>(name_input_.value.size()) < name_input_.max_len) {
            name_input_.value += key.text;
        }
        return;
    }

    if (key.id == "KEY_UNDERSCORE") {
        if (static_cast<int>(name_input_.value.size()) < name_input_.max_len) {
            name_input_.value.push_back('_');
        }
        return;
    }
    if (key.id == "KEY_BACKSPACE") {
        if (!name_input_.value.empty()) name_input_.value.pop_back();
        return;
    }
    if (key.id == "KEY_CLEAR") {
        name_input_.value.clear();
        return;
    }
    if (key.id == "KEY_DONE") {
        name_input_.confirm_requested = true;
        return;
    }
    if (key.id == "KEY_MODE_123" || key.id == "KEY_MODE_ABC") {
        const int old_row = key.row;
        const int old_center = key_center_twice(key);
        name_input_.digits_mode = (key.id == "KEY_MODE_123");
        const std::vector<KeyboardKey> &new_keys = name_input_.digits_mode ? keyboard_layout_.digit_keys : keyboard_layout_.letter_keys;
        name_input_.key_index = nearest_key_index(new_keys, old_row, old_center);
    }
}

std::string WizardScene::cleaned_username() const {
    std::string cleaned = name_input_.value;
    if (cleaned.empty()) return "Player1";
    if (lower_copy(cleaned) == "root") return "Player1";
    return cleaned;
}

void WizardScene::render_name_input(Renderer *renderer,
                                    const LayoutMetrics &layout,
                                    const LayoutRect &body,
                                    const Theme &theme,
                                    uint32_t now_ms) {
    (void)layout;
    const std::vector<KeyboardKey> &keys = name_input_.digits_mode ? keyboard_layout_.digit_keys : keyboard_layout_.letter_keys;
    if (keys.empty()) return;

    const int lift_px = static_cast<int>(22.0f * name_input_.panel_t);
    renderer->draw_text_centered(tr(settings_, "Nazwa użytkownika", "Username"), body.x + body.w / 2, body.y + 8 - lift_px,
                                 theme.text_primary, true);

    const LayoutRect input_box(body.x + 26, body.y + 34 - lift_px, body.w - 52, 34);
    draw_card(renderer, input_box, theme, true);
    const std::string shown_name = name_input_.value.empty()
                                       ? "_"
                                       : renderer->ellipsize_to_width(name_input_.value, input_box.w - 72, true);
    renderer->draw_text_centered(shown_name, input_box.x + input_box.w / 2, input_box.y + 7, theme.text_primary, true);
    const std::string count =
        std::to_string(static_cast<int>(name_input_.value.size())) + "/" + std::to_string(std::max(0, name_input_.max_len));
    const Color count_color = static_cast<int>(name_input_.value.size()) >= 5 ? theme.text_muted : theme.warn;
    renderer->draw_text(count, input_box.right() - 8 - renderer->text_width(count, false), input_box.y + 9, count_color);

    if (now_ms < name_error_until_ms_) {
        renderer->draw_text_centered(name_reserved_error_
                                         ? tr(settings_, "Nazwa root jest zarezerwowana", "Username root is reserved")
                                         : tr(settings_, "Minimum 5 znaków", "Minimum 5 characters"),
                                     body.x + body.w / 2,
                                     input_box.bottom() + 4,
                                     theme.warn);
    }

    const int panel_h = 108;
    const int slide = static_cast<int>((1.0f - name_input_.panel_t) * static_cast<float>(panel_h + 6));
    const LayoutRect kb_panel(body.x + 8, body.bottom() - panel_h + slide, body.w - 16, panel_h);
    if (name_input_.panel_t < 0.01f) return;
    draw_card(renderer, kb_panel, theme, true);

    const LayoutRect kb_inner = inset(kb_panel, 6);
    const int cols = std::max(1, keyboard_layout_.cols);
    const int rows = std::max(1, keyboard_layout_.rows);
    const int col_gap = 3;
    const int row_gap = 3;
    const int total_gap_w = col_gap * (cols - 1);
    const int unit_w = std::max(1, (kb_inner.w - total_gap_w) / cols);
    const int extra_w = std::max(0, kb_inner.w - total_gap_w - unit_w * cols);
    const int key_h = std::max(14, (kb_inner.h - row_gap * (rows - 1)) / rows);

    auto col_start = [&](int col) {
        int x = kb_inner.x;
        for (int i = 0; i < col; ++i) {
            x += unit_w;
            if (i < extra_w) ++x;
            x += col_gap;
        }
        return x;
    };
    auto span_width = [&](int col, int span) {
        int w = 0;
        const int safe_span = std::max(1, span);
        for (int i = 0; i < safe_span; ++i) {
            const int c = col + i;
            w += unit_w;
            if (c < extra_w) ++w;
            if (i + 1 < safe_span) w += col_gap;
        }
        return w;
    };
    auto key_rect = [&](const KeyboardKey &key) {
        return LayoutRect(col_start(std::max(0, key.col)),
                          kb_inner.y + std::max(0, key.row) * (key_h + row_gap),
                          span_width(std::max(0, key.col), std::max(1, key.span)),
                          key_h);
    };
    auto key_label = [&](const KeyboardKey &key) {
        if (!key.action) return key.text;
        if (key.id == "KEY_UNDERSCORE") return tr(settings_, "Podkreślenie", "Keyboard Underscore");
        if (key.id == "KEY_BACKSPACE") return tr(settings_, "Usuń", "Keyboard Backspace");
        if (key.id == "KEY_CLEAR") return tr(settings_, "Wyczyść", "Keyboard Clear");
        if (key.id == "KEY_DONE") return tr(settings_, "Gotowe", "Keyboard Done");
        if (key.id == "KEY_MODE_123") return tr(settings_, "123", "Keyboard 123");
        if (key.id == "KEY_MODE_ABC") return tr(settings_, "ABC", "Keyboard ABC");
        return key.text;
    };

    name_input_.key_index = std::max(0, std::min(name_input_.key_index, static_cast<int>(keys.size()) - 1));
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        const LayoutRect r = key_rect(keys[i]);
        const bool focused = i == name_input_.key_index;
        draw_card(renderer, r, theme, focused);
        const std::string lbl = renderer->ellipsize_to_width(key_label(keys[i]), r.w - 6, false);
        renderer->draw_text_centered(lbl, r.x + r.w / 2, r.y + std::max(1, (r.h - renderer->line_height(false)) / 2), theme.text_primary);
    }

    const float cursor = std::max(0.0f, std::min(name_input_.key_cursor_t, static_cast<float>(std::max(0, static_cast<int>(keys.size()) - 1))));
    const int idx0 = std::max(0, std::min(static_cast<int>(cursor), static_cast<int>(keys.size()) - 1));
    const int idx1 = std::max(0, std::min(idx0 + 1, static_cast<int>(keys.size()) - 1));
    const float frac = cursor - static_cast<float>(idx0);
    const LayoutRect r0 = key_rect(keys[idx0]);
    const LayoutRect r1 = key_rect(keys[idx1]);
    const int marker_x =
        static_cast<int>(static_cast<float>(r0.x) + (static_cast<float>(r1.x) - static_cast<float>(r0.x)) * frac);
    const int marker_w =
        static_cast<int>(static_cast<float>(r0.w) + (static_cast<float>(r1.w) - static_cast<float>(r0.w)) * frac);
    const int marker_y =
        static_cast<int>(static_cast<float>(r0.bottom() + 1) + (static_cast<float>(r1.bottom() - r0.bottom())) * frac);
    renderer->fill_rect(marker_x, marker_y, marker_w, 2, theme.accent);
}

SceneOutput WizardScene::update(const InputSnapshot &input, uint32_t now_ms) {
    SceneOutput out;
    auto tick_name_animation = [&]() {
        const bool smooth = !settings_ || settings_->animations;
        const float blend = smooth ? 0.22f : 1.0f;

        const float panel_target = (step_ == STEP_NAME) ? 1.0f : 0.0f;
        name_input_.panel_t += (panel_target - name_input_.panel_t) * blend;
        if (!smooth || std::fabs(name_input_.panel_t - panel_target) < 0.001f) name_input_.panel_t = panel_target;

        const float key_target = static_cast<float>(name_input_.key_index);
        name_input_.key_cursor_t += (key_target - name_input_.key_cursor_t) * blend;
        if (!smooth || std::fabs(name_input_.key_cursor_t - key_target) < 0.001f) name_input_.key_cursor_t = key_target;
    };

    if (step_ == STEP_WELCOME) {
        if (input.accept) step_ = STEP_LANGUAGE;
        tick_name_animation();
        return out;
    }

    if (step_ == STEP_LANGUAGE) {
        const int max_lang = std::max(0, static_cast<int>(language_codes_.size()) - 1);
        if (input.nav_left) language_selected_ = std::max(0, language_selected_ - 1);
        if (input.nav_right) language_selected_ = std::min(max_lang, language_selected_ + 1);
        settings_->language = language_codes_[language_selected_];
        language_cursor_t_ += (static_cast<float>(language_selected_) - language_cursor_t_) * 0.22f;
        if (input.accept || input.menu) step_ = STEP_THEME;
        if (input.back) step_ = STEP_WELCOME;
        tick_name_animation();
        return out;
    }

    if (step_ == STEP_THEME) {
        if (input.nav_left) theme_selected_ = std::max(0, theme_selected_ - 1);
        if (input.nav_right) theme_selected_ = std::min(1, theme_selected_ + 1);
        settings_->theme_id = theme_selected_ == 1 ? "graphite_light" : "tech_noir";
        theme_cursor_t_ += (static_cast<float>(theme_selected_) - theme_cursor_t_) * 0.22f;
        if (input.accept || input.menu) step_ = STEP_NAME;
        if (input.back) step_ = STEP_LANGUAGE;
        tick_name_animation();
        return out;
    }

    if (step_ == STEP_NAME) {
        update_name_input(input);
        if (name_input_.confirm_requested) {
            name_input_.confirm_requested = false;
            if (static_cast<int>(name_input_.value.size()) < 5) {
                name_reserved_error_ = false;
                name_error_until_ms_ = now_ms + 1400;
            } else if (lower_copy(name_input_.value) == "root") {
                name_reserved_error_ = true;
                name_error_until_ms_ = now_ms + 1600;
            } else {
                name_reserved_error_ = false;
                step_ = STEP_PIN_CHOICE;
            }
        }
        if (input.back) step_ = STEP_THEME;
        tick_name_animation();
        return out;
    }

    if (step_ == STEP_PIN_CHOICE) {
        if (input.nav_left) pin_choice_selected_ = std::max(0, pin_choice_selected_ - 1);
        if (input.nav_right) pin_choice_selected_ = std::min(1, pin_choice_selected_ + 1);
        pin_choice_cursor_t_ += (static_cast<float>(pin_choice_selected_) - pin_choice_cursor_t_) * 0.24f;
        if (input.accept && pin_choice_selected_ == 0) {
            step_ = STEP_PIN;
            pin_cursor_ = 0;
            tick_name_animation();
            return out;
        }
        if ((input.accept && pin_choice_selected_ == 1) || input.menu) {
            profile_->username = cleaned_username();
            profile_->pin_enabled = false;
            profile_->pin = "0000";

            profile_store_->save(*profile_, logger_);
            settings_store_->save(*settings_, logger_);
            begin_install_phase(now_ms);
            power_hint_ = tr(settings_,
                             "OSTRZEŻENIE: Jak wyłączyć urządzenie później: na baterii kliknij szybko 2x POWER. "
                             "Przy zasilaniu USB odłącz kabel USB.",
                             "WARNING: How to power off later: on battery quickly press POWER twice. "
                             "When powered by USB, unplug the USB cable.");
        }
        if (input.back) step_ = STEP_NAME;
        tick_name_animation();
        return out;
    }

    if (step_ == STEP_PIN) {
        if (input.nav_left) pin_cursor_ = std::max(0, pin_cursor_ - 1);
        if (input.nav_right) pin_cursor_ = std::min(3, pin_cursor_ + 1);
        if (input.nav_up) pin_digits_[pin_cursor_] = (pin_digits_[pin_cursor_] + 1) % 10;
        if (input.nav_down) pin_digits_[pin_cursor_] = (pin_digits_[pin_cursor_] + 9) % 10;

        if (input.back) {
            step_ = STEP_PIN_CHOICE;
            tick_name_animation();
            return out;
        }

        if (input.accept) {
            profile_->username = cleaned_username();
            profile_->pin_enabled = true;
            profile_->pin.clear();
            for (int i = 0; i < 4; ++i) profile_->pin.push_back(static_cast<char>('0' + pin_digits_[i]));

            profile_store_->save(*profile_, logger_);
            settings_store_->save(*settings_, logger_);
            begin_install_phase(now_ms);
            power_hint_ = tr(settings_,
                             "OSTRZEŻENIE: Jak wyłączyć urządzenie później: na baterii kliknij szybko 2x POWER. "
                             "Przy zasilaniu USB odłącz kabel USB.",
                             "WARNING: How to power off later: on battery quickly press POWER twice. "
                             "When powered by USB, unplug the USB cable.");
            tick_name_animation();
            return out;
        }
    }

    if (step_ == STEP_INSTALL) {
        update_install_phase(now_ms);
        if (install_complete_) step_ = STEP_DONE;
        tick_name_animation();
        return out;
    }

    if (step_ == STEP_DONE) {
        if (input.accept || input.back || input.menu) {
            out.request_scene = true;
            out.next_scene = SCENE_LAUNCHER;
        }
    }

    tick_name_animation();
    return out;
}

void WizardScene::stop_install_phase() {
    if (install_output_fd_ >= 0) {
        close(install_output_fd_);
        install_output_fd_ = -1;
    }
    if (install_pid_ > 0) {
        int status = 0;
        if (waitpid(install_pid_, &status, WNOHANG) == 0) {
            kill(install_pid_, SIGTERM);
            (void)waitpid(install_pid_, &status, 0);
        }
        install_pid_ = -1;
    }
    install_running_ = false;
}

void WizardScene::begin_install_phase(uint32_t now_ms) {
    stop_install_phase();
    step_ = STEP_INSTALL;
    install_running_ = false;
    install_complete_ = false;
    install_success_ = true;
    install_buffer_.clear();
    install_status_ = tr(settings_, "Przygotowanie instalatora...", "Preparing installer...");
    install_fact_idx_ = 0;
    install_fact_due_ms_ = now_ms + 3000;
}

void WizardScene::update_install_phase(uint32_t now_ms) {
    if (!install_complete_ && !install_running_) {
        const std::string script_path = scene_data_root() + "/system/postscript.sh";
        if (access(script_path.c_str(), R_OK) != 0) {
            install_status_ = tr(settings_, "Brak postscript.sh, pomijam etap instalacji.", "No postscript.sh found, skipping install stage.");
            install_complete_ = true;
            install_success_ = true;
            return;
        }
        if (!spawn_script_process(script_path, &install_pid_, &install_output_fd_)) {
            install_status_ = tr(settings_, "Nie udało się uruchomić postscript.sh", "Unable to start postscript.sh");
            install_complete_ = true;
            install_success_ = false;
            return;
        }
        install_running_ = true;
        install_status_ = tr(settings_, "Uruchamiam postscript.sh...", "Running postscript.sh...");
    }

    if (install_running_) {
        poll_script_process_output(install_output_fd_, &install_buffer_, &install_status_, nullptr);
        int status = 0;
        pid_t r = waitpid(install_pid_, &status, WNOHANG);
        if (r == install_pid_) {
            install_running_ = false;
            flush_script_output_buffer(&install_buffer_, &install_status_, nullptr);
            install_success_ = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            if (!install_success_ && install_status_.empty()) {
                install_status_ = tr(settings_, "Postscript zakończony błędem", "Postscript finished with error");
            }
            if (install_output_fd_ >= 0) {
                close(install_output_fd_);
                install_output_fd_ = -1;
            }
            install_pid_ = -1;
            install_complete_ = true;
        }
    }

    if (now_ms >= install_fact_due_ms_) {
        install_fact_due_ms_ = now_ms + 3000;
        if (!install_facts_.empty()) {
            install_fact_idx_ = (install_fact_idx_ + 1) % static_cast<int>(install_facts_.size());
        }
    }
}

void WizardScene::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    const Theme &theme = ui_theme(settings_);
    render_static_background(renderer, theme);

    const LayoutMetrics layout = build_layout_metrics(renderer->width(), renderer->height());
    LayoutGuard guard("wizard", renderer->width(), renderer->height());
    guard.add("top", layout.top_bar);
    guard.add("content", layout.content);
    guard.add("footer", layout.footer);

    if (welcome_cycle_base_ms_ == 0) welcome_cycle_base_ms_ = now_ms;
    const uint32_t welcome_elapsed_ms = now_ms - welcome_cycle_base_ms_;

    auto welcome_lang = [&](uint32_t t_ms) {
        const uint32_t cycle_ms = 2000;
        const uint32_t phase = t_ms % cycle_ms;
        const size_t lang_count = language_codes_.empty() ? 1u : language_codes_.size();
        const size_t current_lang = (t_ms / cycle_ms) % lang_count;
        if (phase < 1500) return current_lang;
        return (current_lang + 1u) % lang_count;
    };
    const std::string top_lang =
        (step_ == STEP_WELCOME) ? (language_codes_.empty() ? std::string("pl") : language_codes_[welcome_lang(welcome_elapsed_ms)])
                                : normalize_language(settings_->language);
    draw_top_bar(renderer, layout, theme, translate_ui_text(top_lang, "First setup", "First setup"), "", snapshot.battery_percent);

    draw_card(renderer, layout.content, theme, false);
    const LayoutRect body = inset(layout.content, 10);

    int step_index = 0;
    if (step_ == STEP_LANGUAGE) step_index = 1;
    else if (step_ == STEP_THEME) step_index = 2;
    else if (step_ == STEP_NAME) step_index = 3;
    else if (step_ == STEP_PIN_CHOICE) step_index = 4;
    else if (step_ == STEP_PIN) step_index = 5;
    else if (step_ == STEP_INSTALL) step_index = 6;
    for (int i = 0; i < 7; ++i) {
        const bool active = i == step_index;
        const bool done = i < step_index;
        Color c = done ? theme.ok : theme.panel_border;
        if (active) c = theme.accent;
        renderer->fill_rect(body.right() - 116 + i * 16, body.y + 2, 10, 4, c);
    }

    if (step_ == STEP_WELCOME) {
        const uint32_t cycle_ms = 2000;  // 1.0s hold + 0.5s out + 0.5s in (no overlap)
        const uint32_t phase = welcome_elapsed_ms % cycle_ms;
        const size_t lang_count = language_codes_.empty() ? 1u : language_codes_.size();
        const size_t current_lang = (welcome_elapsed_ms / cycle_ms) % lang_count;
        const size_t next_lang = (current_lang + 1u) % lang_count;

        size_t shown_lang = current_lang;
        float alpha = 1.0f;
        if (phase < 1000) {
            shown_lang = current_lang;
            alpha = 1.0f;
        } else if (phase < 1500) {
            shown_lang = current_lang;
            alpha = 1.0f - static_cast<float>(phase - 1000) / 500.0f;
        } else {
            shown_lang = next_lang;
            alpha = static_cast<float>(phase - 1500) / 500.0f;
        }

        const std::string lang = language_codes_.empty() ? std::string("pl") : language_codes_[shown_lang];
        const std::string ttl = translate_ui_text(lang, "Welcome to InConsole", "Welcome to InConsole");
        const std::string sub = translate_ui_text(lang, "Quick setup shown only once.", "Quick setup shown only once.");
        const std::string next_lbl = translate_ui_text(lang, "A next", "A NEXT");
        const std::string next_hint = translate_ui_text(lang, "A next", "A next");
        const Color ttl_c = mix_color(theme.panel_border, theme.text_primary, alpha);
        const Color sub_c = mix_color(theme.panel_border, theme.text_muted, alpha);
        renderer->draw_text_centered(ttl, body.x + body.w / 2, body.y + 24, ttl_c, true);
        renderer->draw_text_centered(sub, body.x + body.w / 2, body.y + 56, sub_c);
        draw_card(renderer, LayoutRect(body.x + body.w / 2 - 92, body.bottom() - 54, 184, 34), theme, true);
        renderer->draw_text_centered(next_lbl, body.x + body.w / 2, body.bottom() - 50, theme.text_primary, true);
        draw_footer(renderer, layout, theme, next_hint);
        return;
    }

    if (step_ == STEP_LANGUAGE) {
        renderer->draw_text_centered(tr(settings_, "Wybór języka", "Language"), body.x + body.w / 2, body.y + 20, theme.text_primary, true);
        const int box_w = 170;
        const int box_h = 60;
        const int center_x = body.x + body.w / 2;
        const int y = body.y + 58;
        const float spacing = static_cast<float>(box_w + 18);
        for (int i = 0; i < static_cast<int>(language_codes_.size()); ++i) {
            const float rel = static_cast<float>(i) - language_cursor_t_;
            if (std::fabs(rel) > 1.6f) continue;
            const int x = center_x + static_cast<int>(rel * spacing) - box_w / 2;
            LayoutRect r(x, y, box_w, box_h);
            const bool focused = std::fabs(rel) < 0.4f;
            draw_card(renderer, r, theme, focused);
            const std::string lbl = language_label(language_codes_[i]);
            renderer->draw_text_centered(renderer->ellipsize_to_width(lbl, box_w - 16, true), r.x + r.w / 2, r.y + 12, theme.text_primary, true);
            renderer->draw_text_centered(language_codes_[i], r.x + r.w / 2, r.y + 34, theme.text_muted);
        }
        renderer->fill_rect(center_x - box_w / 2, y + box_h + 4, box_w, 2, theme.accent);
        draw_footer(renderer, layout, theme, tr(settings_, "Lewo/Prawo zmiana  A dalej", "Left/Right change  A next"));
        return;
    }

    if (step_ == STEP_THEME) {
        renderer->draw_text_centered(tr(settings_, "Wybór motywu", "Theme"), body.x + body.w / 2, body.y + 20, theme.text_primary, true);
        LayoutRect dark(body.x + 46, body.y + 58, 160, 60);
        LayoutRect light(body.right() - 206, body.y + 58, 160, 60);
        const bool is_light = theme_selected_ == 1;
        draw_card(renderer, dark, theme, !is_light);
        draw_card(renderer, light, theme, is_light);
        renderer->draw_text_centered(tr(settings_, "Ciemny", "Dark"), dark.x + dark.w / 2, dark.y + 20, theme.text_primary);
        renderer->draw_text_centered(tr(settings_, "Jasny", "Light"), light.x + light.w / 2, light.y + 20, theme.text_primary);
        const int marker_x = static_cast<int>((static_cast<float>(dark.x) * (1.0f - theme_cursor_t_)) + (static_cast<float>(light.x) * theme_cursor_t_));
        renderer->fill_rect(marker_x, dark.bottom() + 4, dark.w, 2, theme.accent);
        draw_footer(renderer, layout, theme, tr(settings_, "Lewo/Prawo zmiana  A dalej", "Left/Right change  A next"));
        return;
    }

    if (step_ == STEP_NAME) {
        const int lift_px = static_cast<int>(22.0f * name_input_.panel_t);
        const LayoutRect input_box(body.x + 26, body.y + 34 - lift_px, body.w - 52, 34);
        const int panel_h = 108;
        const int slide = static_cast<int>((1.0f - name_input_.panel_t) * static_cast<float>(panel_h + 6));
        const LayoutRect kb_panel(body.x + 8, body.bottom() - panel_h + slide, body.w - 16, panel_h);
        guard.add("wizard-name-input", input_box);
        if (name_input_.panel_t > 0.01f) guard.add("wizard-name-keyboard", kb_panel);

        render_name_input(renderer, layout, body, theme, now_ms);
        draw_footer(renderer,
                    layout,
                    theme,
                    tr(settings_, "Strzałki ruch  A wybierz  _ zamiast spacji  min. 5", "Arrows move  A select key  _ no spaces  min 5"));
        return;
    }

    if (step_ == STEP_PIN_CHOICE) {
        renderer->draw_text_centered(tr(settings_, "Włączyć PIN?", "Enable PIN lock?"), body.x + body.w / 2, body.y + 20, theme.text_primary, true);
        LayoutRect yes(body.x + 40, body.y + 72, 160, 44);
        LayoutRect no(body.right() - 200, body.y + 72, 160, 44);
        draw_card(renderer, yes, theme, pin_choice_selected_ == 0);
        draw_card(renderer, no, theme, pin_choice_selected_ == 1);
        renderer->draw_text_centered(tr(settings_, "Ustaw PIN", "Set PIN"), yes.x + yes.w / 2, yes.y + 13, theme.text_primary);
        renderer->draw_text_centered(tr(settings_, "Pomiń", "Skip"), no.x + no.w / 2, no.y + 13, theme.text_primary);
        const int marker_x = static_cast<int>((static_cast<float>(yes.x) * (1.0f - pin_choice_cursor_t_)) + (static_cast<float>(no.x) * pin_choice_cursor_t_));
        renderer->fill_rect(marker_x, yes.bottom() + 4, yes.w, 2, theme.accent);
        draw_footer(renderer, layout, theme, tr(settings_, "Lewo/Prawo wybór  A zatwierdź", "Left/Right choose  A confirm"));
        return;
    }

    if (step_ == STEP_DONE) {
        renderer->draw_text_centered(tr(settings_, "Konfiguracja zakończona", "Setup complete"), body.x + body.w / 2, body.y + 28, theme.ok, true);
        std::vector<std::string> lines = renderer->wrap_text_to_width(
            power_hint_.empty() ? tr(settings_, "Gotowe.", "Done.") : power_hint_, body.w - 28, 3, false);
        for (size_t i = 0; i < lines.size(); ++i) {
            renderer->draw_text_centered(lines[i], body.x + body.w / 2, body.y + 62 + static_cast<int>(i) * 16, theme.text_muted);
        }
        draw_card(renderer, LayoutRect(body.x + body.w / 2 - 92, body.bottom() - 54, 184, 34), theme, true);
        renderer->draw_text_centered(tr(settings_, "A PRZEJDŹ DALEJ", "A CONTINUE"), body.x + body.w / 2, body.bottom() - 44, theme.text_primary);
        draw_footer(renderer, layout, theme, tr(settings_, "A/B/START dalej", "A/B/START continue"));
        return;
    }

    if (step_ == STEP_INSTALL) {
        renderer->draw_text_centered(tr(settings_, "Instalacja startowa", "Initial installer"), body.x + body.w / 2, body.y + 18, theme.text_primary,
                                     true);

        LayoutRect fact_card(body.x + 18, body.y + 44, body.w - 36, 56);
        draw_card(renderer, fact_card, theme, true);
        const std::string fact_key =
            install_facts_.empty() ? std::string("Tip: You can change language later in Settings.")
                                   : install_facts_[std::max(0, std::min(install_fact_idx_, static_cast<int>(install_facts_.size()) - 1))];
        std::vector<std::string> fact_lines = renderer->wrap_text_to_width(translate_ui_text(normalize_language(settings_->language),
                                                                                              fact_key,
                                                                                              fact_key),
                                                                            fact_card.w - 16,
                                                                            3,
                                                                            false);
        for (size_t i = 0; i < fact_lines.size(); ++i) {
            renderer->draw_text_centered(fact_lines[i], fact_card.x + fact_card.w / 2, fact_card.y + 10 + static_cast<int>(i) * 14,
                                         theme.text_primary);
        }

        LayoutRect status_card(body.x + 18, body.y + 108, body.w - 36, 38);
        draw_card(renderer, status_card, theme, false);
        renderer->draw_text(tr(settings_, "Bieżące zadanie:", "Current task:"), status_card.x + 8, status_card.y + 8, theme.text_muted);
        const std::string status_text =
            renderer->ellipsize_to_width(install_status_.empty() ? tr(settings_, "Uruchamianie...", "Starting...") : install_status_,
                                         status_card.w - 16,
                                         false);
        renderer->draw_text(status_text, status_card.x + 8, status_card.y + 21, theme.text_primary);

        const int bar_y = body.bottom() - 16;
        renderer->fill_rect(body.x + 18, bar_y, body.w - 36, 3, mix_color(theme.panel_border, theme.panel_bottom, 0.4f));
        if (install_running_) {
            const int segment = 48;
            const int travel = std::max(1, body.w - 36 - segment);
            const int x = body.x + 18 + static_cast<int>((now_ms / 6) % travel);
            renderer->fill_rect(x, bar_y, segment, 3, theme.accent);
        } else {
            renderer->fill_rect(body.x + 18, bar_y, body.w - 36, 3, install_success_ ? theme.ok : theme.warn);
        }
        draw_footer(renderer, layout, theme, tr(settings_, "Proszę czekać...", "Please wait..."));
        return;
    }

    renderer->draw_text_centered(tr(settings_, "Ustaw 4-cyfrowy PIN", "Create 4-digit PIN"), body.x + body.w / 2, body.y + 20,
                                 theme.text_primary, true);
    const int slot_w = 56;
    const int slot_h = 64;
    const int gap = 10;
    const int total_w = slot_w * 4 + gap * 3;
    const int start_x = body.x + (body.w - total_w) / 2;
    const int y = body.y + 62;
    for (int i = 0; i < 4; ++i) {
        const bool selected = i == pin_cursor_;
        LayoutRect slot(start_x + i * (slot_w + gap), y, slot_w, slot_h);
        draw_card(renderer, slot, theme, selected);
        renderer->draw_text_centered(std::string(1, static_cast<char>('0' + pin_digits_[i])), slot.x + slot.w / 2, slot.y + 20,
                                     theme.text_primary, true);
    }
    draw_footer(renderer, layout, theme, tr(settings_, "Góra/Dół cyfra  Lewo/Prawo pozycja  A zapisz", "Up/Down digit  Left/Right position  A save"));
}

PinLockScene::PinLockScene(const Profile *profile, const Settings *settings, Logger *logger)
    : profile_(profile), settings_(settings), logger_(logger), pin_cursor_(0), error_until_ms_(0) {
    pin_digits_[0] = 0;
    pin_digits_[1] = 0;
    pin_digits_[2] = 0;
    pin_digits_[3] = 0;
}

void PinLockScene::on_enter() {
    pin_cursor_ = 0;
    error_until_ms_ = 0;
    pin_digits_[0] = 0;
    pin_digits_[1] = 0;
    pin_digits_[2] = 0;
    pin_digits_[3] = 0;
}

SceneOutput PinLockScene::update(const InputSnapshot &input, uint32_t now_ms) {
    SceneOutput out;

    if (!profile_ || !profile_->pin_enabled || profile_->pin.size() != 4) {
        out.request_scene = true;
        out.next_scene = SCENE_LAUNCHER;
        return out;
    }

    if (input.nav_left) pin_cursor_ = std::max(0, pin_cursor_ - 1);
    if (input.nav_right) pin_cursor_ = std::min(3, pin_cursor_ + 1);
    if (input.nav_up) pin_digits_[pin_cursor_] = (pin_digits_[pin_cursor_] + 1) % 10;
    if (input.nav_down) pin_digits_[pin_cursor_] = (pin_digits_[pin_cursor_] + 9) % 10;

    if (input.accept) {
        std::string entered;
        entered.reserve(4);
        for (int i = 0; i < 4; ++i) entered.push_back(static_cast<char>('0' + pin_digits_[i]));

        if (entered == profile_->pin) {
            if (logger_) logger_->info("PIN lock: unlock success");
            out.request_scene = true;
            out.next_scene = SCENE_LAUNCHER;
            return out;
        }

        if (logger_) logger_->warn("PIN lock: wrong PIN");
        pin_cursor_ = 0;
        pin_digits_[0] = 0;
        pin_digits_[1] = 0;
        pin_digits_[2] = 0;
        pin_digits_[3] = 0;
        error_until_ms_ = now_ms + 1500;
    }

    return out;
}

void PinLockScene::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    (void)now_ms;
    (void)settings_;
    const Theme &theme = ui_theme(settings_);
    render_static_background(renderer, theme);

    const LayoutMetrics layout = build_layout_metrics(renderer->width(), renderer->height());
    LayoutGuard guard("pin", renderer->width(), renderer->height());
    guard.add("top", layout.top_bar);
    guard.add("content", layout.content);
    guard.add("footer", layout.footer);

    draw_top_bar(renderer, layout, theme, tr(settings_, "Blokada PIN", "PIN Lock"), "", snapshot.battery_percent);

    draw_card(renderer, layout.content, theme, false);
    const LayoutRect body = inset(layout.content, 14);

    renderer->draw_text_centered(tr(settings_, "Wprowadź PIN", "Enter PIN"), body.x + body.w / 2, body.y + 20, theme.text_primary, true);

    const int slot_w = 60;
    const int slot_h = 72;
    const int gap = 10;
    const int total_w = slot_w * 4 + gap * 3;
    const int start_x = body.x + (body.w - total_w) / 2;
    const int y = body.y + 58;

    for (int i = 0; i < 4; ++i) {
        LayoutRect slot(start_x + i * (slot_w + gap), y, slot_w, slot_h);
        draw_card(renderer, slot, theme, i == pin_cursor_);
        renderer->draw_text_centered(std::string(1, static_cast<char>('0' + pin_digits_[i])), slot.x + slot.w / 2, slot.y + 23,
                                     theme.text_primary, true);
    }

    if (now_ms < error_until_ms_) {
        renderer->draw_text_centered(tr(settings_, "Błędny PIN", "Wrong PIN"), body.x + body.w / 2, body.bottom() - 24, theme.danger, true);
    }

    draw_footer(renderer,
                layout,
                theme,
                tr(settings_, "Góra/Dół cyfra  Lewo/Prawo pozycja  A zatwierdź", "Up/Down digit  Left/Right move  A confirm"));
}

LauncherScene::LauncherScene(const Profile *profile, const Registry *registry, const Settings *settings)
    : profile_(profile),
      registry_(registry),
      settings_(settings),
      selected_(0),
      in_system_panel_(false),
      system_selected_(0),
      selection_t_(0.0f),
      panel_t_(0.0f),
      page_t_(0.0f),
      bottom_panel_t_(0.0f),
      system_marker_t_(-1.0f),
      prev_select_hold_(false),
      items_cache_(),
      next_items_refresh_ms_(0) {}

void LauncherScene::on_enter() {
    refresh_items_cache(SDL_GetTicks(), true);
    selection_t_ = static_cast<float>(selected_);
    panel_t_ = 0.0f;
    page_t_ = 0.0f;
    bottom_panel_t_ = 0.0f;
    system_marker_t_ = -1.0f;
    in_system_panel_ = false;
    prev_select_hold_ = false;
}

void LauncherScene::refresh_items_cache(uint32_t now_ms, bool force) {
    if (!registry_) {
        items_cache_.clear();
        selected_ = 0;
        return;
    }
    if (!force && now_ms < next_items_refresh_ms_) return;
    next_items_refresh_ms_ = now_ms + 1200;

    std::vector<AppEntry> out;
    const std::vector<AppEntry> all = registry_->all_apps();
    for (size_t i = 0; i < all.size(); ++i) {
        if (!all[i].builtin) out.push_back(all[i]);
    }

    std::sort(out.begin(), out.end(), [](const AppEntry &a, const AppEntry &b) {
        if (a.order != b.order) return a.order < b.order;
        return a.name < b.name;
    });
    items_cache_.swap(out);
    if (items_cache_.empty()) {
        selected_ = 0;
    } else {
        selected_ = std::max(0, std::min(selected_, static_cast<int>(items_cache_.size()) - 1));
    }
}

SceneOutput LauncherScene::update(const InputSnapshot &input, uint32_t now_ms) {
    SceneOutput out;
    refresh_items_cache(now_ms, false);
    const std::vector<AppEntry> &entries = items_cache_;

    if (!entries.empty()) {
        const int max_idx = static_cast<int>(entries.size()) - 1;
        selected_ = std::max(0, std::min(selected_, max_idx));
    } else {
        selected_ = 0;
    }

    const bool select_pressed = input.hold_select && !prev_select_hold_;
    prev_select_hold_ = input.hold_select;

    if (input.menu) in_system_panel_ = !in_system_panel_;

    if (in_system_panel_) {
        if (input.nav_left) system_selected_ = std::max(0, system_selected_ - 1);
        if (input.nav_right) system_selected_ = std::min(4, system_selected_ + 1);
        if (input.nav_up || input.back) in_system_panel_ = false;
        if (input.accept) {
            out.request_scene = true;
            if (system_selected_ == 0) out.next_scene = SCENE_SETTINGS;
            else if (system_selected_ == 1) out.next_scene = SCENE_DIAGNOSTICS;
            else if (system_selected_ == 2) out.next_scene = SCENE_SYSTEM_INFO;
            else if (system_selected_ == 3) out.next_scene = SCENE_FILE_MANAGER;
            else out.next_scene = SCENE_POWER_OFF;
            return out;
        }
    } else {
        if (!entries.empty()) {
            const int max_idx = static_cast<int>(entries.size()) - 1;
            if (input.nav_left) selected_ = std::max(0, selected_ - 1);
            if (input.nav_right) selected_ = std::min(max_idx, selected_ + 1);
            if (input.nav_down) in_system_panel_ = true;
            if (input.accept) {
                out.request_launch = true;
                out.launch_app = entries[selected_];
                return out;
            }
        } else if (input.nav_down) {
            in_system_panel_ = true;
        }
    }

    if (select_pressed && !in_system_panel_ && !entries.empty()) {
        page_t_ = page_t_ < 0.5f ? 1.0f : 0.0f;
    }
    if (input.back && !in_system_panel_) page_t_ = 0.0f;
    if (in_system_panel_) page_t_ = 0.0f;

    const bool smooth = !settings_ || settings_->animations;
    const float blend = smooth ? 0.20f : 1.0f;
    selection_t_ += (static_cast<float>(selected_) - selection_t_) * blend;
    panel_t_ += (page_t_ - panel_t_) * blend;
    bottom_panel_t_ += ((in_system_panel_ ? 1.0f : 0.0f) - bottom_panel_t_) * blend;

    if (std::fabs(selection_t_ - static_cast<float>(selected_)) < 0.001f) selection_t_ = static_cast<float>(selected_);
    if (std::fabs(panel_t_ - page_t_) < 0.001f) panel_t_ = page_t_;
    if (std::fabs(bottom_panel_t_ - (in_system_panel_ ? 1.0f : 0.0f)) < 0.001f) bottom_panel_t_ = in_system_panel_ ? 1.0f : 0.0f;

    return out;
}

void LauncherScene::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    const Theme &theme = ui_theme(settings_);
    render_static_background(renderer, theme);
    refresh_items_cache(now_ms, false);

    const LayoutMetrics layout = build_layout_metrics(renderer->width(), renderer->height());
    LayoutGuard guard("launcher", renderer->width(), renderer->height());
    guard.add("top", layout.top_bar);
    guard.add("content", layout.content);
    guard.add("footer", layout.footer);

    const std::vector<AppEntry> &entries = items_cache_;
    const int total = static_cast<int>(items_cache_.size());
    if (total > 0) selected_ = std::max(0, std::min(selected_, total - 1));

    draw_top_bar(renderer, layout, theme, safe_username(profile_), "", snapshot.battery_percent);

    draw_card(renderer, layout.content, theme, false);
    const LayoutRect content = inset(layout.content, 6);

    const int details_w = static_cast<int>(176.0f * panel_t_);
    const int bottom_panel_h = 52;
    const int lift_px = static_cast<int>(42.0f * bottom_panel_t_);
    const LayoutRect carousel_area(content.x + 6, content.y + 8 - lift_px, content.w - 12 - details_w, 118);
    const LayoutRect info_area(content.x + 6, carousel_area.bottom() + 6, content.w - 12, 30);
    guard.add("carousel", carousel_area);
    guard.add("info", info_area);

    draw_soft_separator(renderer, content.x + 6, carousel_area.bottom() + 2, content.w - 12, theme);
    const int panel_top_y = content.bottom() + 6 - static_cast<int>(static_cast<float>(bottom_panel_h + 6) * bottom_panel_t_);
    if (bottom_panel_t_ > 0.01f) {
        draw_soft_separator(renderer, content.x + 6, panel_top_y - 2, content.w - 12, theme);
    }

    const int center_x = carousel_area.x + carousel_area.w / 2;
    const int base_y = carousel_area.y + 8;
    const float spacing = 92.0f;

    int title_center_x = center_x;
    int title_y = std::max(info_area.y + 2, std::min(info_area.bottom() - 18, base_y + 98));
    std::string title_text;

    if (total == 0) {
        renderer->draw_text_centered(tr(settings_, "Brak aplikacji", "No applications"), center_x, base_y + 40, theme.warn, true);
        renderer->draw_text_centered(tr(settings_, "Dodaj app.json + icon.png + launch.sh", "Add app.json + icon.png + launch.sh"),
                                     center_x,
                                     base_y + 66,
                                     theme.text_muted);
    } else {
        for (int i = 0; i < total; ++i) {
            const float rel = static_cast<float>(i) - selection_t_;
            const float dist = std::fabs(rel);
            if (dist > 3.2f) continue;

            const bool focused = (std::fabs(rel) < 0.42f) && !in_system_panel_;
            const int tile = focused ? 94 : std::max(58, 82 - static_cast<int>(dist * 16.0f));
            const int x = center_x + static_cast<int>(rel * spacing) - tile / 2;
            const int y = base_y + static_cast<int>(dist * 10.0f);
            const LayoutRect tile_r(x, y, tile, tile);
            if (tile_r.right() < carousel_area.x || tile_r.x > carousel_area.right()) continue;

            draw_card(renderer, tile_r, theme, focused);

            const int icon_size = std::max(30, tile - 22);
            const int icon_x = tile_r.x + (tile_r.w - icon_size) / 2;
            const int icon_y = tile_r.y + (tile_r.h - icon_size) / 2;

            if (!entries[i].icon.empty() && file_readable_cached(entries[i].icon)) {
                renderer->draw_icon(entries[i].icon, icon_x, icon_y, icon_size, icon_size);
            } else {
                draw_card(renderer, LayoutRect(tile_r.x + tile_r.w / 2 - 14, tile_r.y + tile_r.h / 2 - 12, 28, 24), theme, focused);
                renderer->draw_text_centered("APP", tile_r.x + tile_r.w / 2, tile_r.y + tile_r.h / 2 - 4,
                                             focused ? theme.text_primary : theme.text_muted);
            }
        }

        const AppEntry &focused = entries[selected_];
        const float rel = static_cast<float>(selected_) - selection_t_;
        const float dist = std::fabs(rel);
        const int tile = std::max(58, 94 - static_cast<int>(dist * 16.0f));
        const int tile_x = center_x + static_cast<int>(rel * spacing) - tile / 2;
        title_center_x = tile_x + tile / 2;
        title_y = std::max(info_area.y + 2, std::min(info_area.bottom() - 18, base_y + tile + static_cast<int>(dist * 10.0f) + 4));
        title_text = localized_csv_value(focused.name, settings_);
    }

    if (in_system_panel_) {
        title_center_x = center_x;
        if (system_selected_ == 0) title_text = tr(settings_, "Ustawienia", "Settings");
        else if (system_selected_ == 1) title_text = tr(settings_, "Diagnostyka", "Diagnostics");
        else if (system_selected_ == 2) title_text = tr(settings_, "Informacje", "System Info");
        else if (system_selected_ == 3) title_text = tr(settings_, "Menedżer plików", "File Manager");
        else title_text = tr(settings_, "Wyłączanie", "Power Off");
    }
    if (!title_text.empty()) {
        const int label_max_w = std::max(120, info_area.w - 20);
        const std::string clipped = renderer->ellipsize_to_width(title_text, label_max_w, true);
        renderer->draw_text_centered(clipped, title_center_x, title_y, theme.text_primary, true);
    }

    if (bottom_panel_t_ > 0.01f) {
        const LayoutRect bottom_panel(content.x + 6, panel_top_y, content.w - 12, bottom_panel_h);
        draw_card(renderer, bottom_panel, theme, in_system_panel_);
        guard.add("system-panel", bottom_panel, true);

        const int chip = 24;
        const int gap = 8;
        const int start_x = bottom_panel.x + (bottom_panel.w - (chip * 5 + gap * 4)) / 2;
        const LayoutRect set_r(start_x, bottom_panel.y + 12, chip, chip);
        const LayoutRect dia_r(start_x + chip + gap, bottom_panel.y + 12, chip, chip);
        const LayoutRect inf_r(start_x + (chip + gap) * 2, bottom_panel.y + 12, chip, chip);
        const LayoutRect fil_r(start_x + (chip + gap) * 3, bottom_panel.y + 12, chip, chip);
        const LayoutRect pwr_r(start_x + (chip + gap) * 4, bottom_panel.y + 12, chip, chip);
        draw_system_chip(renderer, set_r, theme, in_system_panel_ && system_selected_ == 0, 0);
        draw_system_chip(renderer, dia_r, theme, in_system_panel_ && system_selected_ == 1, 1);
        draw_system_chip(renderer, inf_r, theme, in_system_panel_ && system_selected_ == 2, 2);
        draw_system_chip(renderer, fil_r, theme, in_system_panel_ && system_selected_ == 3, 4);
        draw_system_chip(renderer, pwr_r, theme, in_system_panel_ && system_selected_ == 4, 3);

        if (in_system_panel_) {
            const int target_x =
                (system_selected_ == 0
                     ? set_r.x
                     : (system_selected_ == 1 ? dia_r.x : (system_selected_ == 2 ? inf_r.x : (system_selected_ == 3 ? fil_r.x : pwr_r.x))));
            const int target_w =
                (system_selected_ == 0
                     ? set_r.w
                     : (system_selected_ == 1 ? dia_r.w : (system_selected_ == 2 ? inf_r.w : (system_selected_ == 3 ? fil_r.w : pwr_r.w))));
            const float follow = 0.28f;
            if (system_marker_t_ < 0.0f) system_marker_t_ = static_cast<float>(target_x);
            system_marker_t_ += (static_cast<float>(target_x) - system_marker_t_) * follow;
            const int marker_x = static_cast<int>(system_marker_t_);
            renderer->fill_rect(marker_x, set_r.bottom() + 3, target_w, 2, theme.accent);
        } else {
            system_marker_t_ = -1.0f;
        }

    } else {
        system_marker_t_ = -1.0f;
    }

    if (panel_t_ > 0.01f && total > 0 && !in_system_panel_ && bottom_panel_t_ < 0.01f) {
        const int panel_w = 176;
        const int shown_x = content.right() - panel_w;
        const int hidden_x = content.right() + 8;
        const int x = hidden_x - static_cast<int>(static_cast<float>(hidden_x - shown_x) * panel_t_);
        const LayoutRect panel(x, content.y + 4, panel_w, content.h - 8);
        draw_card(renderer, panel, theme, true);
        guard.add("details", panel, true);

        const AppEntry &a = entries[selected_];
        renderer->draw_text(tr(settings_, "Szczegóły", "Details"), panel.x + 8, panel.y + 8, theme.text_primary, true);
        draw_soft_separator(renderer, panel.x + 8, panel.y + 28, panel.w - 16, theme);

        const int details_text_w = panel.w - 16;
        const std::string name = renderer->ellipsize_to_width(localized_csv_value(a.name, settings_), details_text_w, true);
        renderer->draw_text(name, panel.x + 8, panel.y + 36, theme.text_primary, true);

        const std::string line1 = renderer->ellipsize_to_width(tr(settings_, "ID: ", "ID: ") + a.id, details_text_w, false);
        const std::string line2 =
            renderer->ellipsize_to_width(tr(settings_, "Typ: ", "Type: ") + localized_csv_value(a.type, settings_), details_text_w, false);
        const std::string line3 = renderer->ellipsize_to_width(tr(settings_, "Kat: ", "Cat: ") + localized_csv_value(a.category, settings_),
                                                               details_text_w,
                                                               false);
        renderer->draw_text(line1, panel.x + 8, panel.y + 62, theme.text_muted);
        renderer->draw_text(line2, panel.x + 8, panel.y + 76, theme.text_muted);
        renderer->draw_text(line3, panel.x + 8, panel.y + 90, theme.text_muted);

        renderer->draw_text(tr(settings_, "Opis", "Description"), panel.x + 8, panel.y + 112, theme.text_primary);
        std::string desc =
            a.description.empty() ? tr(settings_, "Brak opisu", "No description") : localized_csv_value(a.description, settings_);
        const int desc_y = panel.y + 126;
        const int available_h = std::max(0, panel.bottom() - 6 - desc_y);
        const int line_h = std::max(1, renderer->line_height(false));
        const size_t max_desc_lines = static_cast<size_t>(std::max(1, available_h / line_h));
        std::vector<std::string> lines = renderer->wrap_text_to_width(desc, details_text_w, max_desc_lines, false);
        for (size_t i = 0; i < lines.size(); ++i) {
            renderer->draw_text(lines[i], panel.x + 8, desc_y + static_cast<int>(i) * line_h, theme.text_muted);
        }
    }

    const std::string hints = in_system_panel_
                                  ? tr(settings_, "A otwórz  Góra wróć  B zamknij", "A open  Up return  B close")
                                  : tr(settings_, "Lewo/Prawo aplikacje  Dół system  SELECT szczegóły  A start",
                                       "Left/Right apps  Down system  SELECT details  A launch");
    draw_footer(renderer, layout, theme, hints);
}

SettingsScene::SettingsScene(Settings *settings, SettingsStore *store, Profile *profile, ProfileStore *profile_store, Logger *logger)
    : settings_(settings),
      store_(store),
      profile_(profile),
      profile_store_(profile_store),
      logger_(logger),
      category_selected_(0),
      option_selected_(0),
      focus_column_(0),
      category_cursor_t_(0.0f),
      option_cursor_t_(0.0f),
      reset_mode_(RESET_NONE),
      reset_error_until_ms_(0),
      reset_input_(),
      reset_keyboard_layout_(),
      reset_expected_username_() {
    init_reset_keyboard_layout();
}

void SettingsScene::on_enter() {
    category_selected_ = 0;
    option_selected_ = 0;
    focus_column_ = 0;
    category_cursor_t_ = 0.0f;
    option_cursor_t_ = 0.0f;
    reset_mode_ = RESET_NONE;
    reset_error_until_ms_ = 0;
    reset_input_ = TextInputState();
    reset_expected_username_.clear();
    reset_log_lines_.clear();
}

void SettingsScene::init_reset_keyboard_layout() {
    reset_keyboard_layout_ = KeyboardLayout();
    reset_keyboard_layout_.cols = 10;
    reset_keyboard_layout_.rows = 4;
    reset_keyboard_layout_.letter_keys.clear();
    reset_keyboard_layout_.digit_keys.clear();

    auto add_key =
        [](std::vector<KeyboardKey> *list, const std::string &id, const std::string &text, int row, int col, int span, bool action) {
            KeyboardKey key;
            key.id = id;
            key.text = text;
            key.row = row;
            key.col = col;
            key.span = std::max(1, span);
            key.action = action;
            list->push_back(key);
        };

    const std::string row0 = "QWERTYUIOP";
    for (int i = 0; i < static_cast<int>(row0.size()); ++i) {
        add_key(&reset_keyboard_layout_.letter_keys, std::string(1, row0[i]), std::string(1, row0[i]), 0, i, 1, false);
    }
    const std::string row1 = "ASDFGHJKL";
    for (int i = 0; i < static_cast<int>(row1.size()); ++i) {
        add_key(&reset_keyboard_layout_.letter_keys, std::string(1, row1[i]), std::string(1, row1[i]), 1, i, 1, false);
    }
    const std::string row2 = "ZXCVBNM";
    for (int i = 0; i < static_cast<int>(row2.size()); ++i) {
        add_key(&reset_keyboard_layout_.letter_keys, std::string(1, row2[i]), std::string(1, row2[i]), 2, 1 + i, 1, false);
    }
    add_key(&reset_keyboard_layout_.letter_keys, "KEY_UNDERSCORE", "", 3, 0, 3, true);
    add_key(&reset_keyboard_layout_.letter_keys, "KEY_BACKSPACE", "", 3, 3, 3, true);
    add_key(&reset_keyboard_layout_.letter_keys, "KEY_CLEAR", "", 3, 6, 2, true);
    add_key(&reset_keyboard_layout_.letter_keys, "KEY_MODE_123", "", 3, 8, 1, true);
    add_key(&reset_keyboard_layout_.letter_keys, "KEY_DONE", "", 3, 9, 1, true);

    const std::string d0 = "12345";
    for (int i = 0; i < static_cast<int>(d0.size()); ++i) {
        add_key(&reset_keyboard_layout_.digit_keys, std::string(1, d0[i]), std::string(1, d0[i]), 0, i * 2, 2, false);
    }
    const std::string d1 = "67890";
    for (int i = 0; i < static_cast<int>(d1.size()); ++i) {
        add_key(&reset_keyboard_layout_.digit_keys, std::string(1, d1[i]), std::string(1, d1[i]), 1, i * 2, 2, false);
    }
    add_key(&reset_keyboard_layout_.digit_keys, "KEY_BACKSPACE", "", 2, 0, 3, true);
    add_key(&reset_keyboard_layout_.digit_keys, "KEY_CLEAR", "", 2, 3, 3, true);
    add_key(&reset_keyboard_layout_.digit_keys, "KEY_DONE", "", 2, 6, 4, true);
}

void SettingsScene::begin_reset_pin_mode() {
    reset_mode_ = RESET_PIN;
    reset_error_until_ms_ = 0;
    reset_input_ = TextInputState();
    reset_input_.max_len = 4;
    reset_input_.digits_mode = true;
    reset_input_.panel_t = 1.0f;
    reset_input_.key_index = 0;
    reset_input_.key_cursor_t = 0.0f;
}

void SettingsScene::begin_reset_username_mode() {
    reset_mode_ = RESET_USERNAME;
    reset_error_until_ms_ = 0;
    reset_input_ = TextInputState();
    reset_input_.max_len = 12;
    reset_input_.digits_mode = false;
    reset_input_.panel_t = 1.0f;
    reset_input_.key_index = 0;
    reset_input_.key_cursor_t = 0.0f;
    reset_expected_username_ = normalize_username_for_id(profile_ ? profile_->username : std::string("Player1"));
    if (reset_expected_username_.empty()) reset_expected_username_ = "PLAYER1";
}

void SettingsScene::update_reset_keyboard(const InputSnapshot &input, bool pin_mode) {
    const std::vector<KeyboardKey> &keys =
        pin_mode ? reset_keyboard_layout_.digit_keys
                 : (reset_input_.digits_mode ? reset_keyboard_layout_.digit_keys : reset_keyboard_layout_.letter_keys);
    if (keys.empty()) return;

    reset_input_.key_index = std::max(0, std::min(reset_input_.key_index, static_cast<int>(keys.size()) - 1));
    reset_input_.confirm_requested = false;

    if (input.nav_left) reset_input_.key_index = move_keyboard_focus(keys, reset_input_.key_index, -1, 0);
    if (input.nav_right) reset_input_.key_index = move_keyboard_focus(keys, reset_input_.key_index, +1, 0);
    if (input.nav_up) reset_input_.key_index = move_keyboard_focus(keys, reset_input_.key_index, 0, -1);
    if (input.nav_down) reset_input_.key_index = move_keyboard_focus(keys, reset_input_.key_index, 0, +1);

    if (input.accept) {
        const KeyboardKey &key = keys[reset_input_.key_index];
        if (!key.action) {
            if (static_cast<int>(reset_input_.value.size()) < reset_input_.max_len) {
                reset_input_.value += key.text;
            }
        } else if (key.id == "KEY_UNDERSCORE") {
            if (!pin_mode && static_cast<int>(reset_input_.value.size()) < reset_input_.max_len) {
                reset_input_.value.push_back('_');
            }
        } else if (key.id == "KEY_BACKSPACE") {
            if (!reset_input_.value.empty()) reset_input_.value.pop_back();
        } else if (key.id == "KEY_CLEAR") {
            reset_input_.value.clear();
        } else if (key.id == "KEY_DONE") {
            reset_input_.confirm_requested = true;
        } else if (!pin_mode && (key.id == "KEY_MODE_123" || key.id == "KEY_MODE_ABC")) {
            const int old_row = key.row;
            const int old_center = key_center_twice(key);
            reset_input_.digits_mode = (key.id == "KEY_MODE_123");
            const std::vector<KeyboardKey> &new_keys =
                reset_input_.digits_mode ? reset_keyboard_layout_.digit_keys : reset_keyboard_layout_.letter_keys;
            reset_input_.key_index = nearest_key_index(new_keys, old_row, old_center);
        }
    }

    const bool smooth = !settings_ || settings_->animations;
    const float blend = smooth ? 0.24f : 1.0f;
    const float key_target = static_cast<float>(reset_input_.key_index);
    reset_input_.key_cursor_t += (key_target - reset_input_.key_cursor_t) * blend;
    if (!smooth || std::fabs(reset_input_.key_cursor_t - key_target) < 0.001f) reset_input_.key_cursor_t = key_target;
}

std::string SettingsScene::reset_name_normalized() const {
    return normalize_username_for_id(reset_input_.value);
}

bool SettingsScene::apply_factory_reset() {
    if (!settings_ || !store_ || !profile_) return false;

    reset_log_lines_.clear();
    const std::string root = scene_data_root();
    const std::string lang = normalize_language(settings_->language);
    const std::string removing_prefix = translate_ui_text(lang, "Removing: ", "Removing: ");
    bool all_ok = true;

    auto remove_with_log = [&](const std::string &label, const std::string &path) {
        push_log_line(&reset_log_lines_, removing_prefix + label);
        errno = 0;
        const bool ok = remove_path_recursive(path);
        if (!ok && errno != ENOENT) {
            all_ok = false;
            push_log_line(&reset_log_lines_, translate_ui_text(lang, "Remove failed: ", "Remove failed: ") + label);
            if (logger_) logger_->warn("Factory reset remove failed: " + path + " errno=" + std::to_string(errno));
        }
    };

    const std::string remove_list = root + "/system/reset-remove.list";
    const std::string list_body = read_text_file(remove_list);
    if (!list_body.empty()) {
        std::istringstream ls(list_body);
        std::string line;
        while (std::getline(ls, line)) {
            const std::string item = trim_copy(line);
            if (item.empty() || item[0] == '#') continue;
            const std::string abs = (item[0] == '/') ? item : (root + "/" + item);
            remove_with_log(item, abs);
        }
    }

    const std::string undo_script = root + "/system/undo_postscript.sh";
    if (access(undo_script.c_str(), R_OK) == 0) {
        push_log_line(&reset_log_lines_, translate_ui_text(lang, "Running undo script...", "Running undo script..."));
        if (!run_script_blocking(undo_script, &reset_log_lines_)) {
            all_ok = false;
            push_log_line(&reset_log_lines_, translate_ui_text(lang, "Undo script failed.", "Undo script failed."));
        }
    } else {
        push_log_line(&reset_log_lines_, translate_ui_text(lang, "No undo_postscript.sh found.", "No undo_postscript.sh found."));
    }

    remove_with_log("system/settings.json", root + "/system/settings.json");
    remove_with_log("system/profile.json", root + "/system/profile.json");

    *settings_ = Settings();
    *profile_ = Profile();
    store_->apply_volume(settings_->volume, logger_);
    store_->apply_brightness(settings_->brightness, logger_);
    push_log_line(&reset_log_lines_, translate_ui_text(lang, "Reset cleanup completed.", "Reset cleanup completed."));
    return all_ok;
}

SceneOutput SettingsScene::update(const InputSnapshot &input, uint32_t now_ms) {
    SceneOutput out;
    const std::vector<std::string> langs = available_languages();
    int lang_idx = 0;
    const std::string curr_lang = normalize_language(settings_->language);
    for (size_t i = 0; i < langs.size(); ++i) {
        if (langs[i] == curr_lang) {
            lang_idx = static_cast<int>(i);
            break;
        }
    }

    const int category_count = 2;
    category_selected_ = std::max(0, std::min(category_selected_, category_count - 1));
    const int option_count = (category_selected_ == 0) ? 2 : 1;
    option_selected_ = std::max(0, std::min(option_selected_, option_count - 1));

    if (reset_mode_ == RESET_DONE) {
        if (input.accept || input.back || input.menu || now_ms >= reset_error_until_ms_) {
            out.request_restart = true;
        }
    } else if (reset_mode_ == RESET_CONFIRM) {
        if (input.accept) {
            if (profile_ && profile_->pin_enabled && profile_->pin.size() == 4) {
                begin_reset_pin_mode();
            } else {
                begin_reset_username_mode();
            }
        }
        if (input.back || input.menu) reset_mode_ = RESET_NONE;
    } else if (reset_mode_ == RESET_PIN) {
        update_reset_keyboard(input, true);
        if (reset_input_.confirm_requested) {
            reset_input_.confirm_requested = false;
            if (profile_ && reset_input_.value == profile_->pin) {
                if (apply_factory_reset()) {
                    reset_mode_ = RESET_DONE;
                    reset_error_until_ms_ = now_ms + 2600;
                } else {
                    reset_mode_ = RESET_NONE;
                    reset_error_until_ms_ = now_ms + 1600;
                }
            } else {
                reset_error_until_ms_ = now_ms + 1600;
            }
        }
        if (input.back || input.menu) reset_mode_ = RESET_CONFIRM;
    } else if (reset_mode_ == RESET_USERNAME) {
        update_reset_keyboard(input, false);
        if (reset_input_.confirm_requested) {
            reset_input_.confirm_requested = false;
            if (!reset_expected_username_.empty() && reset_name_normalized() == reset_expected_username_) {
                if (apply_factory_reset()) {
                    reset_mode_ = RESET_DONE;
                    reset_error_until_ms_ = now_ms + 2600;
                } else {
                    reset_mode_ = RESET_NONE;
                    reset_error_until_ms_ = now_ms + 1600;
                }
            } else {
                reset_error_until_ms_ = now_ms + 1600;
            }
        }
        if (input.back || input.menu) reset_mode_ = RESET_CONFIRM;
    } else {
        bool changed = false;

        if (focus_column_ == 0) {
            if (input.nav_up) category_selected_ = std::max(0, category_selected_ - 1);
            if (input.nav_down) category_selected_ = std::min(category_count - 1, category_selected_ + 1);
            if (input.accept) focus_column_ = 1;
            if (input.back || input.menu) {
                out.request_scene = true;
                out.next_scene = SCENE_LAUNCHER;
            }
            option_selected_ = 0;
        } else {
            if (input.nav_up) option_selected_ = std::max(0, option_selected_ - 1);
            if (input.nav_down) option_selected_ = std::min(option_count - 1, option_selected_ + 1);
            if (input.back || input.menu) {
                focus_column_ = 0;
            } else if (category_selected_ == 0) {
                if (option_selected_ == 0) {
                    const int before_idx = lang_idx;
                    const int max_idx = std::max(0, static_cast<int>(langs.size()) - 1);
                    if (input.nav_left) lang_idx = std::max(0, lang_idx - 1);
                    if (input.nav_right) lang_idx = std::min(max_idx, lang_idx + 1);
                    if (lang_idx != before_idx) {
                        settings_->language = langs.empty() ? std::string("pl") : langs[lang_idx];
                        changed = true;
                    }
                }

                if (option_selected_ == 1) {
                    if (input.nav_left || input.nav_right || input.accept) {
                        const std::string curr = normalize_theme_id(settings_->theme_id);
                        settings_->theme_id = (curr == "graphite_light") ? "tech_noir" : "graphite_light";
                        changed = true;
                    }
                }
            } else if (category_selected_ == 1) {
                if (option_selected_ == 0 && input.accept) {
                    reset_mode_ = RESET_CONFIRM;
                }
            }
        }

        if (changed) {
            settings_->language = normalize_language(settings_->language);
            settings_->theme_id = normalize_theme_id(settings_->theme_id);
            store_->save(*settings_, logger_);
        }
    }

    const bool smooth = !settings_ || settings_->animations;
    const float blend = smooth ? 0.24f : 1.0f;
    category_cursor_t_ += (static_cast<float>(category_selected_) - category_cursor_t_) * blend;
    option_cursor_t_ += (static_cast<float>(option_selected_) - option_cursor_t_) * blend;
    if (!smooth || std::fabs(category_cursor_t_ - static_cast<float>(category_selected_)) < 0.001f) {
        category_cursor_t_ = static_cast<float>(category_selected_);
    }
    if (!smooth || std::fabs(option_cursor_t_ - static_cast<float>(option_selected_)) < 0.001f) {
        option_cursor_t_ = static_cast<float>(option_selected_);
    }
    return out;
}

void SettingsScene::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    const Theme &theme = ui_theme(settings_);
    render_static_background(renderer, theme);

    const LayoutMetrics layout = build_layout_metrics(renderer->width(), renderer->height());
    draw_top_bar(renderer, layout, theme, tr(settings_, "Ustawienia", "Settings"), "", snapshot.battery_percent);

    draw_card(renderer, layout.content, theme, false);
    const LayoutRect inner = inset(layout.content, 8);
    const LayoutRect categories(inner.x, inner.y, 130, inner.h);
    const LayoutRect options(categories.right() + 8, inner.y, inner.w - categories.w - 8, inner.h);
    draw_card(renderer, categories, theme, focus_column_ == 0 && reset_mode_ == RESET_NONE);
    draw_card(renderer, options, theme, focus_column_ == 1 && reset_mode_ == RESET_NONE);

    const std::string cat_names[2] = {
        tr(settings_, "Ogólne", "General"),
        tr(settings_, "System", "System")
    };
    const int cat_row_h = 34;
    for (int i = 0; i < 2; ++i) {
        LayoutRect r(categories.x + 6, categories.y + 8 + i * (cat_row_h + 4), categories.w - 12, cat_row_h);
        const bool selected = i == category_selected_;
        draw_card(renderer, r, theme, selected && reset_mode_ == RESET_NONE);
        renderer->draw_text_centered(renderer->ellipsize_to_width(cat_names[i], r.w - 8, false),
                                     r.x + r.w / 2,
                                     r.y + 9,
                                     selected ? theme.text_primary : theme.text_muted);
    }
    const int cat_marker_y = categories.y + 8 + static_cast<int>(category_cursor_t_ * static_cast<float>(cat_row_h + 4));
    renderer->fill_rect(categories.x + 6, cat_marker_y + cat_row_h + 1, categories.w - 12, 2, theme.accent);

    const bool option_focus = focus_column_ == 1 && reset_mode_ == RESET_NONE;
    const int row_h = 30;
    auto draw_option = [&](int idx, const std::string &label, const std::string &value, bool meter, int meter_value) {
        LayoutRect r(options.x + 6, options.y + 8 + idx * (row_h + 4), options.w - 12, row_h);
        const bool selected = idx == option_selected_ && option_focus;
        draw_card(renderer, r, theme, selected);
        renderer->draw_text(label, r.x + 8, r.y + 8, selected ? theme.text_primary : theme.text_muted);
        if (!value.empty()) {
            const int vw = renderer->text_width(value, false);
            renderer->draw_text(value, r.right() - 8 - vw, r.y + 8, selected ? theme.text_primary : theme.text_muted);
        }
        if (meter) {
            const int bw = 98;
            LayoutRect bar(r.right() - bw - 50, r.y + 18, bw, 8);
            draw_card(renderer, bar, theme, false);
            const int fill = ((bw - 4) * std::max(0, std::min(100, meter_value))) / 100;
            if (fill > 0) renderer->fill_rect(bar.x + 2, bar.y + 2, fill, bar.h - 4, theme.accent);
        }
    };

    if (category_selected_ == 0) {
        draw_option(0, tr(settings_, "Język", "Language"), language_label(settings_->language), false, 0);
        const std::string theme_name =
            normalize_theme_id(settings_->theme_id) == "graphite_light" ? tr(settings_, "Jasny", "Light") : tr(settings_, "Ciemny", "Dark");
        draw_option(1, tr(settings_, "Motyw", "Theme"), theme_name, false, 0);
    } else {
        draw_option(0, tr(settings_, "Reset ustawień", "Factory reset"), "", false, 0);
    }
    const int opt_marker_y = options.y + 8 + static_cast<int>(option_cursor_t_ * static_cast<float>(row_h + 4));
    renderer->fill_rect(options.x + 6, opt_marker_y + row_h + 1, options.w - 12, 2, theme.accent);

    if (reset_mode_ != RESET_NONE) {
        const LayoutRect modal(inner.x + 14, inner.y + 6, inner.w - 28, inner.h - 12);
        draw_card(renderer, modal, theme, true);
        if (reset_mode_ == RESET_CONFIRM) {
            renderer->draw_text_centered(tr(settings_, "Reset urządzenia", "Reset device"), modal.x + modal.w / 2, modal.y + 14, theme.warn, true);
            std::vector<std::string> lines =
                renderer->wrap_text_to_width(tr(settings_, "To usunie profil użytkownika i ustawienia.", "This will erase user profile and settings."),
                                             modal.w - 20,
                                             3,
                                             false);
            for (size_t i = 0; i < lines.size(); ++i) {
                renderer->draw_text_centered(lines[i], modal.x + modal.w / 2, modal.y + 44 + static_cast<int>(i) * 16, theme.text_muted);
            }
            renderer->draw_text_centered(tr(settings_, "A potwierdź reset  B anuluj", "A confirm reset  B cancel"),
                                         modal.x + modal.w / 2,
                                         modal.bottom() - 26,
                                         theme.text_primary);
        } else if (reset_mode_ == RESET_PIN || reset_mode_ == RESET_USERNAME) {
            const bool pin_mode = reset_mode_ == RESET_PIN;
            const std::vector<KeyboardKey> &keys =
                pin_mode ? reset_keyboard_layout_.digit_keys
                         : (reset_input_.digits_mode ? reset_keyboard_layout_.digit_keys : reset_keyboard_layout_.letter_keys);
            auto key_label = [&](const KeyboardKey &key) {
                if (!key.action) return key.text;
                if (key.id == "KEY_UNDERSCORE") return tr(settings_, "Podkreślenie", "Keyboard Underscore");
                if (key.id == "KEY_BACKSPACE") return tr(settings_, "Usuń", "Keyboard Backspace");
                if (key.id == "KEY_CLEAR") return tr(settings_, "Wyczyść", "Keyboard Clear");
                if (key.id == "KEY_DONE") return tr(settings_, "Gotowe", "Keyboard Done");
                if (key.id == "KEY_MODE_123") return tr(settings_, "123", "Keyboard 123");
                if (key.id == "KEY_MODE_ABC") return tr(settings_, "ABC", "Keyboard ABC");
                return key.text;
            };

            if (pin_mode) {
                renderer->draw_text_centered(tr(settings_, "PIN do resetu", "Enter PIN to reset"), modal.x + modal.w / 2, modal.y + 10,
                                             theme.text_primary, true);
            } else {
                renderer->draw_text_centered(tr(settings_, "Nazwa do resetu", "Enter username to reset"), modal.x + modal.w / 2, modal.y + 10,
                                             theme.text_primary, true);
                draw_card(renderer, LayoutRect(modal.x + 20, modal.y + 34, modal.w - 40, 22), theme, false);
                renderer->draw_text_centered(reset_expected_username_, modal.x + modal.w / 2, modal.y + 39, theme.warn, false);
            }

            const LayoutRect input_box(modal.x + 20, modal.y + (pin_mode ? 36 : 48), modal.w - 40, 26);
            draw_card(renderer, input_box, theme, true);
            const std::string input_value =
                renderer->ellipsize_to_width(reset_input_.value.empty() ? std::string("_") : reset_input_.value, input_box.w - 16, true);
            renderer->draw_text_centered(input_value, input_box.x + input_box.w / 2, input_box.y + 4, theme.text_primary, true);

            const int panel_h = 72;
            const LayoutRect kb_panel(modal.x + 14, modal.bottom() - panel_h - 8, modal.w - 28, panel_h);
            draw_card(renderer, kb_panel, theme, true);
            const LayoutRect kb_inner = inset(kb_panel, 6);
            const int cols = std::max(1, reset_keyboard_layout_.cols);
            const int rows = std::max(1, pin_mode ? 3 : reset_keyboard_layout_.rows);
            const int col_gap = 3;
            const int row_gap = 3;
            const int total_gap_w = col_gap * (cols - 1);
            const int unit_w = std::max(1, (kb_inner.w - total_gap_w) / cols);
            const int extra_w = std::max(0, kb_inner.w - total_gap_w - unit_w * cols);
            const int key_h = std::max(14, (kb_inner.h - row_gap * (rows - 1)) / rows);
            auto col_start = [&](int col) {
                int x = kb_inner.x;
                for (int i = 0; i < col; ++i) {
                    x += unit_w;
                    if (i < extra_w) ++x;
                    x += col_gap;
                }
                return x;
            };
            auto span_width = [&](int col, int span) {
                int w = 0;
                for (int i = 0; i < std::max(1, span); ++i) {
                    const int c = col + i;
                    w += unit_w;
                    if (c < extra_w) ++w;
                    if (i + 1 < std::max(1, span)) w += col_gap;
                }
                return w;
            };
            auto key_rect = [&](const KeyboardKey &key) {
                return LayoutRect(col_start(key.col), kb_inner.y + key.row * (key_h + row_gap), span_width(key.col, key.span), key_h);
            };

            reset_input_.key_index = std::max(0, std::min(reset_input_.key_index, static_cast<int>(keys.size()) - 1));
            for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
                const LayoutRect r = key_rect(keys[i]);
                const bool focused = i == reset_input_.key_index;
                draw_card(renderer, r, theme, focused);
                const std::string lbl = renderer->ellipsize_to_width(key_label(keys[i]), r.w - 6, false);
                renderer->draw_text_centered(lbl, r.x + r.w / 2, r.y + std::max(1, (r.h - renderer->line_height(false)) / 2),
                                             theme.text_primary);
            }

            const float cursor =
                std::max(0.0f, std::min(reset_input_.key_cursor_t, static_cast<float>(std::max(0, static_cast<int>(keys.size()) - 1))));
            const int idx0 = std::max(0, std::min(static_cast<int>(cursor), static_cast<int>(keys.size()) - 1));
            const int idx1 = std::max(0, std::min(idx0 + 1, static_cast<int>(keys.size()) - 1));
            const float frac = cursor - static_cast<float>(idx0);
            const LayoutRect r0 = key_rect(keys[idx0]);
            const LayoutRect r1 = key_rect(keys[idx1]);
            const int marker_x =
                static_cast<int>(static_cast<float>(r0.x) + (static_cast<float>(r1.x) - static_cast<float>(r0.x)) * frac);
            const int marker_w =
                static_cast<int>(static_cast<float>(r0.w) + (static_cast<float>(r1.w) - static_cast<float>(r0.w)) * frac);
            const int marker_y =
                static_cast<int>(static_cast<float>(r0.bottom() + 1) + (static_cast<float>(r1.bottom() - r0.bottom())) * frac);
            renderer->fill_rect(marker_x, marker_y, marker_w, 2, theme.accent);

            if (now_ms < reset_error_until_ms_) {
                renderer->draw_text_centered(tr(settings_, "Błędne potwierdzenie", "Wrong confirmation"),
                                             modal.x + modal.w / 2,
                                             kb_panel.y - 14,
                                             theme.danger);
            }
        } else if (reset_mode_ == RESET_DONE) {
            const std::vector<std::string> done_lines =
                renderer->wrap_text_to_width(tr(settings_, "Reset zakończony", "Reset complete. Open wizard again."),
                                             modal.w - 22,
                                             3,
                                             true);
            for (size_t i = 0; i < done_lines.size(); ++i) {
                renderer->draw_text_centered(done_lines[i], modal.x + modal.w / 2, modal.y + 34 + static_cast<int>(i) * 20, theme.ok, true);
            }
            static const char *kResetFacts[] = {
                "Tip: You can change language later in Settings.",
                "Tip: SELECT opens app details in launcher.",
                "Tip: L/R changes system volume anywhere."
            };
            const int fact_idx = static_cast<int>((now_ms / 3000) % (sizeof(kResetFacts) / sizeof(kResetFacts[0])));
            const std::string fact_text = translate_ui_text(normalize_language(settings_->language), kResetFacts[fact_idx], kResetFacts[fact_idx]);
            renderer->draw_text_centered(renderer->ellipsize_to_width(fact_text, modal.w - 22, false), modal.x + modal.w / 2, modal.y + 74,
                                         theme.text_muted);
            const int log_y = modal.y + 86;
            const int max_logs = 3;
            const int total = static_cast<int>(reset_log_lines_.size());
            const int first = std::max(0, total - max_logs);
            for (int i = first; i < total; ++i) {
                const std::string log_line = renderer->ellipsize_to_width(reset_log_lines_[i], modal.w - 24, false);
                renderer->draw_text(log_line, modal.x + 12, log_y + (i - first) * 14, theme.text_muted);
            }
            renderer->draw_text_centered(tr(settings_, "Restart...", "Restarting..."), modal.x + modal.w / 2, modal.bottom() - 30,
                                         theme.text_primary);
        }
    }

    const std::string hints =
        (reset_mode_ == RESET_NONE)
            ? (focus_column_ == 0 ? tr(settings_, "↕ Kategoria || A Opcje || B Wstecz", "Up/Down category  A options  B back")
                                  : tr(settings_, "↕ Opcja || ←/→ Zmień || B Kategorie", "Up/Down option  Left/Right change  B categories"))
            : tr(settings_, "A Potwierdź || B Anuluj", "A confirm  B cancel");
    draw_footer(renderer, layout, theme, hints);
}

DiagnosticsScene::DiagnosticsScene(Input *input, BatteryMonitor *battery, const Settings *settings, Logger *logger)
    : input_(input), battery_(battery), settings_(settings), logger_(logger), last_input_() {}

DiagnosticsScene::~DiagnosticsScene() {}

void DiagnosticsScene::on_enter() { last_input_ = input_->snapshot(); }

SceneOutput DiagnosticsScene::update(const InputSnapshot &input, uint32_t) {
    last_input_ = input;

    SceneOutput out;
    if (input.back || input.menu) {
        out.request_scene = true;
        out.next_scene = SCENE_LAUNCHER;
    }
    return out;
}

void DiagnosticsScene::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    (void)now_ms;
    (void)settings_;
    (void)battery_;
    (void)logger_;
    const Theme &theme = ui_theme(settings_);
    render_static_background(renderer, theme);

    const LayoutMetrics layout = build_layout_metrics(renderer->width(), renderer->height());
    draw_top_bar(renderer, layout, theme, tr(settings_, "Diagnostyka", "Diagnostics"), "", snapshot.battery_percent);
    const BatteryInfo &bat = snapshot.battery;

    draw_card(renderer, layout.content, theme, false);
    const LayoutRect inner = inset(layout.content, 8);

    LayoutRect left(inner.x, inner.y, (inner.w - 6) / 2, inner.h);
    LayoutRect right(left.right() + 6, inner.y, inner.w - left.w - 6, inner.h);

    draw_card(renderer, left, theme, false);
    draw_card(renderer, right, theme, false);

    renderer->draw_text(tr(settings_, "Przyciski", "Buttons"), left.x + 8, left.y + 6, theme.text_primary, true);

    struct BtnRow {
        const char *name;
        bool state;
    } rows[] = { {"UP", last_input_.hold_up},      {"DOWN", last_input_.hold_down}, {"LEFT", last_input_.hold_left},
                 {"RIGHT", last_input_.hold_right}, {"A", last_input_.hold_a},         {"B", last_input_.hold_b},
                 {"START", last_input_.hold_start}, {"SELECT", last_input_.hold_select}, {"L", last_input_.hold_l},
                 {"R", last_input_.hold_r},         {"JOY", last_input_.hold_joy} };

    const int count = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
    const int rows_per_col = 6;
    for (int i = 0; i < count; ++i) {
        const int col = i / rows_per_col;
        const int row = i % rows_per_col;
        const int col_w = left.w / 2;
        const int base_x = left.x + 8 + col * col_w;
        const int y = left.y + 24 + row * 16;
        renderer->draw_text(rows[i].name, base_x, y, theme.text_muted);
        const Color st = rows[i].state ? theme.ok : theme.warn;
        renderer->draw_text(rows[i].state ? "ON" : "OFF", base_x + 56, y, st);
    }

    renderer->draw_text(tr(settings_, "Analog / Bateria", "Analog / Battery"), right.x + 8, right.y + 6, theme.text_primary, true);
    const int axis_x = std::max(-32768, std::min(32767, last_input_.axis_x));
    const int axis_y = std::max(-32768, std::min(32767, last_input_.axis_y));
    renderer->draw_text("X: " + std::to_string(axis_x), right.x + 8, right.y + 26, theme.text_muted);
    renderer->draw_text("Y: " + std::to_string(axis_y), right.x + 8, right.y + 40, theme.text_muted);

    const int cx = right.x + right.w / 2;
    const int cy = right.y + 80;
    const int r = 26;
    for (int yy = -r; yy <= r; ++yy) {
        for (int xx = -r; xx <= r; ++xx) {
            const int d2 = xx * xx + yy * yy;
            if (d2 <= r * r && d2 >= (r - 2) * (r - 2)) {
                renderer->fill_rect(cx + xx, cy + yy, 1, 1, theme.panel_focus_border);
            }
        }
    }
    renderer->fill_rect(cx - 1, cy - r, 2, r * 2 + 1, mix_color(theme.panel_border, theme.text_muted, 0.4f));
    renderer->fill_rect(cx - r, cy - 1, r * 2 + 1, 2, mix_color(theme.panel_border, theme.text_muted, 0.4f));
    const int knob_r = 5;
    const int kx = cx + (axis_x * (r - knob_r - 2)) / 32768;
    const int ky = cy + (axis_y * (r - knob_r - 2)) / 32768;
    for (int yy = -knob_r; yy <= knob_r; ++yy) {
        for (int xx = -knob_r; xx <= knob_r; ++xx) {
            if (xx * xx + yy * yy <= knob_r * knob_r) renderer->fill_rect(kx + xx, ky + yy, 1, 1, theme.accent);
        }
    }

    renderer->draw_text(tr(settings_, "Bateria", "Battery") + std::string(": ") + (bat.available ? "OK" : "N/A"), right.x + 8, right.y + 114,
                        bat.available ? theme.ok : theme.warn);
    if (bat.available) {
        renderer->draw_text(tr(settings_, "Pojemność", "Capacity") + std::string(": ") +
                                (bat.has_capacity ? std::to_string(bat.capacity) + "%" : "--"),
                            right.x + 8,
                            right.y + 128,
                            theme.text_muted);
        renderer->draw_text(tr(settings_, "Napięcie", "Voltage") + std::string(": ") +
                                (bat.has_voltage ? std::to_string(bat.voltage_mv) + " mV" : "--"),
                            right.x + 8,
                            right.y + 142,
                            theme.text_muted);
    }
    draw_footer(renderer, layout, theme, tr(settings_, "B/START powrót", "B/START back"));
}

SystemInfoScene::SystemInfoScene(const Registry *registry, const Settings *settings, Logger *logger)
    : registry_(registry),
      settings_(settings),
      logger_(logger),
      cpu_model_cached_(),
      ram_mib_cached_(0),
      free_mib_cached_(0),
      app_count_cached_(0),
      next_refresh_ms_(0) {}

void SystemInfoScene::on_enter() {
    refresh_cached_stats(SDL_GetTicks());
}

void SystemInfoScene::refresh_cached_stats(uint32_t now_ms) {
    if (now_ms < next_refresh_ms_) return;
    next_refresh_ms_ = now_ms + 1000;

    app_count_cached_ = registry_ ? static_cast<int>(registry_->all_apps().size()) : 0;

    const std::string cpu_model =
        trim_copy(proc_value_after_colon("/proc/cpuinfo", "model name").empty()
                      ? proc_value_after_colon("/proc/cpuinfo", "Hardware")
                      : proc_value_after_colon("/proc/cpuinfo", "model name"));
    cpu_model_cached_ = cpu_model.empty() ? std::string("Unknown") : cpu_model;
    ram_mib_cached_ = parse_first_long(proc_value_after_colon("/proc/meminfo", "MemTotal")) / 1024;
    free_mib_cached_ = free_disk_mib_for_scene();
}

SceneOutput SystemInfoScene::update(const InputSnapshot &input, uint32_t now_ms) {
    refresh_cached_stats(now_ms);

    SceneOutput out;
    if (input.back || input.menu || input.accept) {
        out.request_scene = true;
        out.next_scene = SCENE_LAUNCHER;
    }
    return out;
}

void SystemInfoScene::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    (void)now_ms;
    const Theme &theme = ui_theme(settings_);
    render_static_background(renderer, theme);

    const LayoutMetrics layout = build_layout_metrics(renderer->width(), renderer->height());
    draw_top_bar(renderer, layout, theme, tr(settings_, "Informacje systemowe", "System Info"), "", snapshot.battery_percent);
    draw_card(renderer, layout.content, theme, false);
    const LayoutRect inner = inset(layout.content, 8);

    const std::string rows[] = {
        tr(settings_, "Runtime", "Runtime"),
        tr(settings_, "CPU", "CPU"),
        tr(settings_, "RAM", "RAM"),
        tr(settings_, "Wolne miejsce", "Free storage"),
        tr(settings_, "Liczba aplikacji", "Apps count")
    };
    const std::string vals[] = {
        "inconsole-runtime",
        cpu_model_cached_.empty() ? std::string("Unknown") : cpu_model_cached_,
        std::to_string(std::max(0L, ram_mib_cached_)) + " MiB",
        std::to_string(std::max(0L, free_mib_cached_)) + " MiB",
        std::to_string(std::max(0, app_count_cached_))
    };

    const int row_h = 28;
    for (int i = 0; i < 5; ++i) {
        LayoutRect r(inner.x, inner.y + i * (row_h + 4), inner.w, row_h);
        draw_card(renderer, r, theme, false);
        renderer->draw_text(rows[i], r.x + 8, r.y + 7, theme.text_muted);
        const std::string clipped = renderer->ellipsize_to_width(vals[i], r.w - 150, false);
        const int w = renderer->text_width(clipped, false);
        renderer->draw_text(clipped, r.right() - 8 - w, r.y + 7, theme.text_primary);
    }

    draw_footer(renderer, layout, theme, tr(settings_, "A/B/START powrót", "A/B/START back"));
}

FileManagerScene::FileManagerScene(const Settings *settings, Logger *logger)
    : settings_(settings),
      logger_(logger),
      roots_(),
      root_labels_(),
      root_editable_(),
      root_selected_(0),
      root_offset_(0),
      current_root_(),
      current_path_(),
      current_root_editable_(false),
      entries_(),
      selected_(0),
      list_offset_(0),
      file_preview_mode_(false),
      edit_mode_(false),
      file_name_(),
      file_path_(),
      file_lines_(),
      file_backup_lines_(),
      file_scroll_(0),
      file_read_only_(true),
      file_text_(false),
      file_dirty_(false),
      cursor_row_(0),
      cursor_col_(0),
      keyset_letters_("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-./:,;!?@#"),
      keyset_digits_(),
      keyset_index_(0),
      keyset_digits_mode_(false),
      prev_l_hold_(false),
      prev_r_hold_(false),
      prev_select_hold_(false),
      status_line_(),
      status_until_ms_(0) {}

void FileManagerScene::set_status(const std::string &msg, uint32_t now_ms, uint32_t duration_ms) {
    status_line_ = msg;
    status_until_ms_ = now_ms + duration_ms;
}

bool FileManagerScene::is_path_under(const std::string &base, const std::string &child) const {
    if (base.empty() || child.empty()) return false;
    if (child == base) return true;
    if (child.size() <= base.size()) return false;
    if (child.compare(0, base.size(), base) != 0) return false;
    return child[base.size()] == '/';
}

bool FileManagerScene::is_forbidden_edit_path(const std::string &path) const {
    const std::string root = scene_data_root();
    if (is_path_under(root + "/apps", path)) return true;
    if (is_path_under(root + "/system/languages", path)) return true;
    if (is_path_under(root + "/system", path)) return true;
    return false;
}

bool FileManagerScene::has_image_extension(const std::string &path) const {
    const std::string low = lower_copy(path);
    return low.size() > 4 &&
           (low.rfind(".png") == low.size() - 4 || low.rfind(".jpg") == low.size() - 4 || low.rfind(".bmp") == low.size() - 4 ||
            low.rfind(".gif") == low.size() - 4 || low.rfind(".svg") == low.size() - 4 ||
            (low.size() > 5 && (low.rfind(".jpeg") == low.size() - 5 || low.rfind(".webp") == low.size() - 5)));
}

bool FileManagerScene::is_probably_text_file(const std::string &path) const {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return true;
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = buf[i];
        if (c == 0) return false;
        if ((c < 9) || (c > 13 && c < 32)) ++bad;
    }
    return bad * 100 <= n * 8;
}

void FileManagerScene::ensure_root_visible() {
    const int visible = 5;
    if (root_selected_ < root_offset_) root_offset_ = root_selected_;
    if (root_selected_ >= root_offset_ + visible) root_offset_ = root_selected_ - visible + 1;
    if (root_offset_ < 0) root_offset_ = 0;
}

void FileManagerScene::ensure_entry_visible() {
    const int visible = 5;
    if (selected_ < list_offset_) list_offset_ = selected_;
    if (selected_ >= list_offset_ + visible) list_offset_ = selected_ - visible + 1;
    if (list_offset_ < 0) list_offset_ = 0;
}

void FileManagerScene::ensure_line_visible() {
    const int visible = 5;
    if (cursor_row_ < file_scroll_) file_scroll_ = cursor_row_;
    if (cursor_row_ >= file_scroll_ + visible) file_scroll_ = cursor_row_ - visible + 1;
    if (file_scroll_ < 0) file_scroll_ = 0;
}

void FileManagerScene::refresh_roots() {
    roots_.clear();
    root_labels_.clear();
    root_editable_.clear();

    auto add_root = [&](const std::string &path, const std::string &label, bool editable) {
        if (path.empty()) return;
        for (size_t i = 0; i < roots_.size(); ++i) {
            if (roots_[i] == path) return;
        }
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
        roots_.push_back(path);
        root_labels_.push_back(label);
        root_editable_.push_back(editable);
    };

    const std::string root = scene_data_root();
    add_root(root + "/apps", "apps", false);
    add_root(root + "/system/logs", "logs", false);
    add_root(root + "/system/languages", "languages", false);

    std::istringstream mounts(read_text_file("/proc/mounts"));
    std::string dev;
    std::string mnt;
    std::string fs;
    std::string opts;
    int a = 0;
    int b = 0;
    while (mounts >> dev >> mnt >> fs >> opts >> a >> b) {
        if (mnt.find("/media/") != 0 && mnt.find("/mnt/") != 0 && mnt.find("/run/media/") != 0) continue;
        std::string label = mnt;
        const size_t slash = label.find_last_of('/');
        if (slash != std::string::npos && slash + 1 < label.size()) label = label.substr(slash + 1);
        add_root(mnt, std::string("usb:") + label, access(mnt.c_str(), W_OK) == 0);
    }
    if (root_selected_ < 0) root_selected_ = 0;
    if (root_selected_ >= static_cast<int>(roots_.size())) root_selected_ = std::max(0, static_cast<int>(roots_.size()) - 1);
    ensure_root_visible();
}

void FileManagerScene::open_root(int index) {
    if (index < 0 || index >= static_cast<int>(roots_.size())) return;
    current_root_ = roots_[index];
    current_path_ = current_root_;
    current_root_editable_ = root_editable_[index];
    selected_ = 0;
    list_offset_ = 0;
    file_preview_mode_ = false;
    edit_mode_ = false;
    file_name_.clear();
    file_path_.clear();
    file_lines_.clear();
    file_backup_lines_.clear();
    refresh_entries();
}

void FileManagerScene::refresh_entries() {
    entries_.clear();
    if (current_path_.empty()) return;
    if (current_path_ != current_root_) entries_.push_back(Entry{"..", current_path_, true, 0});

    DIR *dir = opendir(current_path_.c_str());
    if (!dir) return;
    struct dirent *ent = nullptr;
    std::vector<Entry> rows;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        const std::string name = ent->d_name;
        const std::string path = current_path_ + "/" + name;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0) continue;
        rows.push_back(Entry{name, path, S_ISDIR(st.st_mode), static_cast<long>(st.st_size)});
    }
    closedir(dir);
    std::sort(rows.begin(), rows.end(), [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir) return a.dir > b.dir;
        return a.name < b.name;
    });
    entries_.insert(entries_.end(), rows.begin(), rows.end());
    if (selected_ < 0) selected_ = 0;
    if (selected_ >= static_cast<int>(entries_.size())) selected_ = std::max(0, static_cast<int>(entries_.size()) - 1);
    ensure_entry_visible();
}

void FileManagerScene::open_file_preview(const std::string &path, const std::string &name) {
    file_preview_mode_ = true;
    edit_mode_ = false;
    file_name_ = name;
    file_path_ = path;
    file_lines_.clear();
    file_backup_lines_.clear();
    file_scroll_ = 0;
    file_dirty_ = false;
    cursor_row_ = 0;
    cursor_col_ = 0;
    keyset_index_ = 0;
    keyset_digits_mode_ = false;

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        file_text_ = false;
        file_read_only_ = true;
        file_lines_.push_back(tr(settings_, "Nie można odczytać pliku.", "Unable to read file."));
        return;
    }

    file_read_only_ = !current_root_editable_ || is_forbidden_edit_path(path);

    if (has_image_extension(path)) {
        file_text_ = false;
        file_read_only_ = true;
        file_lines_.push_back(tr(settings_, "Podgląd obrazów jest wyłączony.", "Image preview is disabled."));
        return;
    }
    if (st.st_size > 1024 * 1024) {
        file_text_ = false;
        file_read_only_ = true;
        file_lines_.push_back(tr(settings_, "Plik jest za duży do podglądu.", "File too large to preview."));
        return;
    }
    if (!is_probably_text_file(path)) {
        file_text_ = false;
        file_read_only_ = true;
        file_lines_.push_back(tr(settings_, "Brak danych lub plik binarny.", "No data or binary file."));
        return;
    }

    file_text_ = true;
    const std::string text = read_text_file(path);
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        file_lines_.push_back(line);
    }
    if (file_lines_.empty()) file_lines_.push_back(std::string());
}

void FileManagerScene::normalize_cursor() {
    if (file_lines_.empty()) file_lines_.push_back(std::string());
    cursor_row_ = std::max(0, std::min(cursor_row_, static_cast<int>(file_lines_.size()) - 1));
    cursor_col_ = std::max(0, std::min(cursor_col_, static_cast<int>(file_lines_[cursor_row_].size())));
}

void FileManagerScene::move_cursor_h(int dx) {
    normalize_cursor();
    if (dx < 0) {
        if (cursor_col_ > 0) {
            --cursor_col_;
        } else if (cursor_row_ > 0) {
            --cursor_row_;
            cursor_col_ = static_cast<int>(file_lines_[cursor_row_].size());
        }
    } else if (dx > 0) {
        if (cursor_col_ < static_cast<int>(file_lines_[cursor_row_].size())) {
            ++cursor_col_;
        } else if (cursor_row_ + 1 < static_cast<int>(file_lines_.size())) {
            ++cursor_row_;
            cursor_col_ = 0;
        }
    }
    ensure_line_visible();
}

void FileManagerScene::move_cursor_v(int dy) {
    normalize_cursor();
    cursor_row_ += dy;
    normalize_cursor();
    ensure_line_visible();
}

void FileManagerScene::insert_char(char c) {
    normalize_cursor();
    file_lines_[cursor_row_].insert(static_cast<size_t>(cursor_col_), 1, c);
    ++cursor_col_;
    file_dirty_ = true;
    ensure_line_visible();
}

void FileManagerScene::backspace_char() {
    normalize_cursor();
    if (cursor_col_ > 0) {
        file_lines_[cursor_row_].erase(static_cast<size_t>(cursor_col_ - 1), 1);
        --cursor_col_;
        file_dirty_ = true;
    } else if (cursor_row_ > 0) {
        const int prev_len = static_cast<int>(file_lines_[cursor_row_ - 1].size());
        file_lines_[cursor_row_ - 1] += file_lines_[cursor_row_];
        file_lines_.erase(file_lines_.begin() + cursor_row_);
        --cursor_row_;
        cursor_col_ = prev_len;
        file_dirty_ = true;
    }
    ensure_line_visible();
}

void FileManagerScene::newline_char() {
    normalize_cursor();
    std::string tail = file_lines_[cursor_row_].substr(static_cast<size_t>(cursor_col_));
    file_lines_[cursor_row_].erase(static_cast<size_t>(cursor_col_));
    file_lines_.insert(file_lines_.begin() + cursor_row_ + 1, tail);
    ++cursor_row_;
    cursor_col_ = 0;
    file_dirty_ = true;
    ensure_line_visible();
}

void FileManagerScene::begin_edit() {
    if (!file_text_ || file_read_only_) return;
    edit_mode_ = true;
    file_backup_lines_ = file_lines_;
    cursor_row_ = 0;
    cursor_col_ = 0;
    keyset_index_ = 0;
    keyset_digits_mode_ = false;
    file_dirty_ = false;
    prev_l_hold_ = false;
    prev_r_hold_ = false;
    prev_select_hold_ = false;
}

void FileManagerScene::discard_edit() {
    if (edit_mode_) file_lines_ = file_backup_lines_;
    edit_mode_ = false;
    file_dirty_ = false;
    prev_l_hold_ = false;
    prev_r_hold_ = false;
    prev_select_hold_ = false;
}

bool FileManagerScene::save_edit() {
    if (!edit_mode_ || file_read_only_) return false;
    std::string content;
    for (size_t i = 0; i < file_lines_.size(); ++i) {
        content += file_lines_[i];
        if (i + 1 < file_lines_.size()) content.push_back('\n');
    }
    if (!write_text_file(file_path_, content)) return false;
    file_backup_lines_ = file_lines_;
    file_dirty_ = false;
    return true;
}

void FileManagerScene::on_enter() {
    root_selected_ = 0;
    root_offset_ = 0;
    current_root_.clear();
    current_path_.clear();
    current_root_editable_ = false;
    entries_.clear();
    selected_ = 0;
    list_offset_ = 0;
    file_preview_mode_ = false;
    edit_mode_ = false;
    file_name_.clear();
    file_path_.clear();
    file_lines_.clear();
    file_backup_lines_.clear();
    file_scroll_ = 0;
    status_line_.clear();
    status_until_ms_ = 0;
    refresh_roots();
}

SceneOutput FileManagerScene::update(const InputSnapshot &input, uint32_t now_ms) {
    SceneOutput out;
    if (status_until_ms_ > 0 && now_ms >= status_until_ms_) {
        status_until_ms_ = 0;
        status_line_.clear();
    }

    if (current_path_.empty()) {
        const int max_root = std::max(0, static_cast<int>(roots_.size()) - 1);
        if (input.nav_up) root_selected_ = std::max(0, root_selected_ - 1);
        if (input.nav_down) root_selected_ = std::min(max_root, root_selected_ + 1);
        if (input.nav_left) root_selected_ = std::max(0, root_selected_ - 5);
        if (input.nav_right) root_selected_ = std::min(max_root, root_selected_ + 5);
        ensure_root_visible();
        if (input.accept) open_root(root_selected_);
        if (input.back || input.menu) {
            out.request_scene = true;
            out.next_scene = SCENE_LAUNCHER;
        }
        return out;
    }

    file_preview_mode_ = false;
    edit_mode_ = false;

    if (input.nav_up) selected_ = std::max(0, selected_ - 1);
    if (input.nav_down) selected_ = std::min(std::max(0, static_cast<int>(entries_.size()) - 1), selected_ + 1);
    if (input.nav_left) selected_ = std::max(0, selected_ - 5);
    if (input.nav_right) selected_ = std::min(std::max(0, static_cast<int>(entries_.size()) - 1), selected_ + 5);
    ensure_entry_visible();

    auto go_parent = [&]() {
        if (current_path_ == current_root_) {
            current_path_.clear();
            current_root_.clear();
            current_root_editable_ = false;
            entries_.clear();
            selected_ = 0;
            list_offset_ = 0;
            return;
        }
        const size_t slash = current_path_.find_last_of('/');
        if (slash == std::string::npos || slash <= current_root_.size()) {
            current_path_ = current_root_;
        } else {
            current_path_ = current_path_.substr(0, slash);
        }
        selected_ = 0;
        list_offset_ = 0;
        refresh_entries();
    };

    if (input.back || input.menu) {
        go_parent();
        return out;
    }

    if (input.accept && !entries_.empty()) {
        const Entry e = entries_[selected_];
        if (e.dir) {
            if (e.name == "..") {
                go_parent();
                return out;
            }
            const std::string child = current_path_ + "/" + e.name;
            if (is_path_under(current_root_, child)) {
                current_path_ = child;
                selected_ = 0;
                list_offset_ = 0;
                refresh_entries();
            }
        } else {
            set_status(tr(settings_, "Podgląd zawartości plików jest wyłączony.", "File content preview is disabled."), now_ms, 1200);
        }
    }

    return out;
}

void FileManagerScene::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    const Theme &theme = ui_theme(settings_);
    render_static_background(renderer, theme);

    const LayoutMetrics layout = build_layout_metrics(renderer->width(), renderer->height());
    draw_top_bar(renderer, layout, theme, tr(settings_, "Menedżer plików", "File Manager"), "", snapshot.battery_percent);
    draw_card(renderer, layout.content, theme, true);
    const LayoutRect body = inset(layout.content, 8);

    auto draw_status = [&]() {
        if (status_until_ms_ == 0 || now_ms >= status_until_ms_ || status_line_.empty()) return;
        LayoutRect st(body.x + 6, body.bottom() - 24, body.w - 12, 16);
        draw_card(renderer, st, theme, true);
        renderer->draw_text(renderer->ellipsize_to_width(status_line_, st.w - 8, false), st.x + 4, st.y + 2, theme.warn);
    };

    if (current_path_.empty()) {
        renderer->draw_text_centered(tr(settings_, "Wybierz lokalizację", "Select location"), body.x + body.w / 2, body.y + 8, theme.text_primary,
                                     true);
        if (roots_.empty()) {
            renderer->draw_text_centered(tr(settings_, "Brak dostępnych lokalizacji", "No locations available"), body.x + body.w / 2, body.y + 42,
                                         theme.warn);
        } else {
            const int visible = 5;
            const int row_h = 22;
            const int start = root_offset_;
            const int end = std::min(static_cast<int>(roots_.size()), start + visible);
            for (int i = start; i < end; ++i) {
                const int row_i = i - start;
                LayoutRect row(body.x + 10, body.y + 34 + row_i * row_h, body.w - 20, 20);
                draw_card(renderer, row, theme, i == root_selected_);
                const std::string mode = root_editable_[i] ? tr(settings_, "edytowalny", "editable") : tr(settings_, "read only", "read only");
                const std::string label = renderer->ellipsize_to_width(root_labels_[i] + " [" + mode + "]", row.w - 8, false);
                renderer->draw_text(label, row.x + 4, row.y + 2, i == root_selected_ ? theme.text_primary : theme.text_muted);
            }
            LayoutRect root_list(body.x + 10, body.y + 34, body.w - 20, visible * row_h);
            draw_vertical_scrollbar(renderer, root_list, static_cast<int>(roots_.size()), visible, root_offset_, theme);

            const std::string pos = std::to_string(root_selected_ + 1) + "/" + std::to_string(roots_.size());
            const int pos_w = renderer->text_width(pos, false);
            renderer->draw_text(pos, body.right() - 10 - pos_w, body.y + 8, theme.text_muted);
        }
        draw_footer(renderer,
                    layout,
                    theme,
                    tr(settings_, "↕ Wybór || ←/→ Strona || A Otwórz || B Wróć", "Up/Down choose  Left/Right page  A open  B back"));
        draw_status();
        return;
    }

    renderer->draw_text(renderer->ellipsize_to_width(current_path_, body.w - 12, false), body.x + 4, body.y + 2, theme.text_muted);
    renderer->draw_text(current_root_editable_ ? tr(settings_, "USB: tekst można edytować", "USB: text editable")
                                               : tr(settings_, "READ ONLY: pliki systemowe", "READ ONLY: system files"),
                        body.x + 4,
                        body.y + 16,
                        current_root_editable_ ? theme.ok : theme.warn);

    file_preview_mode_ = false;
    edit_mode_ = false;

    LayoutRect list(body.x + 4, body.y + 30, body.w - 8, body.h - 56);
    draw_card(renderer, list, theme, false);
    if (entries_.empty()) {
        renderer->draw_text_centered(tr(settings_, "Brak czytelnych pozycji", "No readable entries"), list.x + list.w / 2, list.y + 20, theme.warn);
    } else {
        const int visible = 5;
        const int row_h = 22;
        const int start = list_offset_;
        const int end = std::min(static_cast<int>(entries_.size()), start + visible);
        const std::string pos = std::to_string(selected_ + 1) + "/" + std::to_string(entries_.size());
        const int pos_w = renderer->text_width(pos, false);
        renderer->draw_text(pos, list.right() - 8 - pos_w, list.y + 2, theme.text_muted);
        for (int i = start; i < end; ++i) {
            const int row_i = i - start;
            LayoutRect row(list.x + 4, list.y + 6 + row_i * row_h, list.w - 8, 20);
            const bool focused = i == selected_;
            draw_card(renderer, row, theme, focused);
            const Entry &e = entries_[static_cast<size_t>(i)];
            const std::string label = e.dir ? (e.name + "/") : e.name;
            renderer->draw_text(renderer->ellipsize_to_width(label, row.w - 12, false), row.x + 4, row.y + 2,
                                focused ? theme.text_primary : theme.text_muted);
        }
        draw_vertical_scrollbar(renderer, LayoutRect(list.right() - 5, list.y + 6, 3, visible * row_h), static_cast<int>(entries_.size()), visible,
                                list_offset_, theme);
    }

    draw_footer(renderer,
                layout,
                theme,
                tr(settings_, "↕ Wybór || ←/→ Strona || A Otwórz || B Wróć", "Up/Down choose  Left/Right page  A open  B back"));
    draw_status();
}

PowerOffScene::PowerOffScene(const Settings *settings) : settings_(settings) {}

void PowerOffScene::on_enter() {}

SceneOutput PowerOffScene::update(const InputSnapshot &input, uint32_t) {
    (void)input;
    SceneOutput out;
    return out;
}

void PowerOffScene::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    (void)now_ms;
    const Theme &theme = ui_theme(settings_);
    render_static_background(renderer, theme);

    const LayoutMetrics layout = build_layout_metrics(renderer->width(), renderer->height());
    draw_top_bar(renderer, layout, theme, tr(settings_, "Wyłączanie", "Power Off"), "", snapshot.battery_percent);
    draw_card(renderer, layout.content, theme, true);
    const LayoutRect body = inset(layout.content, 12);
    renderer->draw_text_centered(tr(settings_, "Możesz teraz bezpiecznie wyłączyć urządzenie.", "Safe to power off now."),
                                 body.x + body.w / 2,
                                 body.y + 26,
                                 theme.ok,
                                 true);
    std::vector<std::string> lines = renderer->wrap_text_to_width(
        tr(settings_,
           "Kliknij szybko 2x POWER albo odłącz zasilanie USB. W tym trybie nic więcej nie uruchamiaj.",
           "Press POWER twice quickly or unplug USB power. Do not run anything else in this mode."),
        body.w - 20,
        4,
        false);
    for (size_t i = 0; i < lines.size(); ++i) {
        renderer->draw_text_centered(lines[i], body.x + body.w / 2, body.y + 58 + static_cast<int>(i) * 16, theme.text_muted);
    }
    draw_footer(renderer, layout, theme, tr(settings_, "Tryb wyłączania: odłącz zasilanie", "Power-off mode: disconnect power"));
}

SceneManager::SceneManager()
    : current_(SCENE_LAUNCHER), target_(SCENE_LAUNCHER), has_current_(false), phase_(TRANSITION_NONE), phase_elapsed_ms_(0) {}

void SceneManager::register_scene(SceneId id, Scene *scene) {
    scenes_[static_cast<int>(id)] = scene;
}

void SceneManager::set_initial(SceneId id) {
    current_ = id;
    target_ = id;
    has_current_ = true;
    phase_ = TRANSITION_NONE;
    phase_elapsed_ms_ = 0;

    Scene *scene = current_scene();
    if (scene) scene->on_enter();
}

void SceneManager::request(SceneId id) {
    if (!has_current_) return;
    if (id == current_) return;
    target_ = id;
    phase_ = TRANSITION_OUT;
    phase_elapsed_ms_ = 0;
}

Scene *SceneManager::current_scene() {
    std::map<int, Scene *>::iterator it = scenes_.find(static_cast<int>(current_));
    if (it == scenes_.end()) return nullptr;
    return it->second;
}

SceneId SceneManager::current_id() const {
    return current_;
}

void SceneManager::update(uint32_t delta_ms) {
    if (phase_ == TRANSITION_NONE) return;

    const uint32_t kPhaseDuration = scene_fade_phase_ms();
    phase_elapsed_ms_ += delta_ms;

    if (phase_ == TRANSITION_OUT && phase_elapsed_ms_ >= kPhaseDuration) {
        current_ = target_;
        phase_ = TRANSITION_IN;
        phase_elapsed_ms_ = 0;
        Scene *scene = current_scene();
        if (scene) scene->on_enter();
        return;
    }

    if (phase_ == TRANSITION_IN && phase_elapsed_ms_ >= kPhaseDuration) {
        phase_ = TRANSITION_NONE;
        phase_elapsed_ms_ = 0;
    }
}

void SceneManager::render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) {
    Scene *scene = current_scene();
    if (scene) scene->render(renderer, now_ms, snapshot);

    if (phase_ == TRANSITION_NONE) return;

    const uint32_t kPhaseDuration = scene_fade_phase_ms();
    const uint8_t kMaxAlpha = scene_fade_max_alpha();
    const uint32_t elapsed = std::min(phase_elapsed_ms_, kPhaseDuration);
    const float t = static_cast<float>(elapsed) / static_cast<float>(kPhaseDuration);
    const float eased = t * t * (3.0f - 2.0f * t);
    uint8_t alpha = 0;
    if (phase_ == TRANSITION_OUT) {
        alpha = static_cast<uint8_t>(eased * static_cast<float>(kMaxAlpha));
    } else {
        alpha = static_cast<uint8_t>((1.0f - eased) * static_cast<float>(kMaxAlpha));
    }
    renderer->draw_fade(alpha);
}

}  // namespace inconsole
