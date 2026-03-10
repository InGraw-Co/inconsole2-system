#ifndef INCONSOLE_RUNTIME_HPP
#define INCONSOLE_RUNTIME_HPP

#include <SDL.h>

#include <stdint.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

namespace inconsole {

struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct Theme {
    std::string id;
    Color background_top;
    Color background_bottom;
    Color background_glow;
    Color panel_top;
    Color panel_bottom;
    Color panel_border;
    Color panel_focus_top;
    Color panel_focus_bottom;
    Color panel_focus_border;
    Color text_primary;
    Color text_muted;
    Color text_invert;
    Color accent;
    Color ok;
    Color warn;
    Color danger;
    Color footer_top;
    Color footer_bottom;
    Color footer_border;

    Theme();
};

struct Typography {
    int small_px;
    int large_px;
    int line_small;
    int line_large;
    int hint_chars;

    Typography();
};

struct LayoutRect {
    int x;
    int y;
    int w;
    int h;

    LayoutRect() : x(0), y(0), w(0), h(0) {}
    LayoutRect(int px, int py, int pw, int ph) : x(px), y(py), w(pw), h(ph) {}

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool valid() const { return w > 0 && h > 0; }
    bool intersects(const LayoutRect &other) const {
        return x < other.right() && right() > other.x && y < other.bottom() && bottom() > other.y;
    }
};

struct LayoutMetrics {
    LayoutRect safe;
    LayoutRect top_bar;
    LayoutRect content;
    LayoutRect footer;
    int gap;
    int grid_cols;
    int grid_rows;

    LayoutMetrics();
};

struct AppEntry {
    std::string id;
    std::string type;
    std::string name;
    std::string description;
    std::string icon;
    std::string exec;
    std::vector<std::string> args;
    std::string category;
    int order;
    std::string base_dir;
    bool builtin;
    bool exclusive_runtime;

    AppEntry();
};

struct Settings {
    std::string language;
    std::string theme_id;
    int volume;
    int brightness;
    bool animations;

    Settings();
};

struct Profile {
    std::string username;
    bool pin_enabled;
    std::string pin;

    Profile();
};

struct BatteryInfo {
    bool available;
    bool has_capacity;
    bool has_voltage;
    int capacity;
    int voltage_mv;
    std::string status;
    std::string node;

    BatteryInfo();
};

struct InputSnapshot {
    bool hold_up;
    bool hold_down;
    bool hold_left;
    bool hold_right;
    bool hold_a;
    bool hold_b;
    bool hold_start;
    bool hold_select;
    bool hold_l;
    bool hold_r;
    bool hold_joy;

    bool nav_up;
    bool nav_down;
    bool nav_left;
    bool nav_right;
    bool accept;
    bool back;
    bool menu;

    int axis_x;
    int axis_y;
    uint64_t edge_accept_ms;
    uint64_t edge_back_ms;
    uint64_t edge_menu_ms;
    uint64_t edge_select_ms;
    uint64_t edge_l_ms;
    uint64_t edge_r_ms;

    InputSnapshot();
};

struct RuntimePerfStats {
    uint32_t frame_ms;
    uint32_t update_ms;
    uint32_t render_ms;
    uint32_t present_ms;
    float fps_avg;
    float fps_p95;
    uint32_t slow_frame_count;
    uint32_t last_input_delay_a_ms;
    uint32_t last_input_delay_b_ms;
    uint32_t last_input_delay_start_ms;
    uint32_t last_input_delay_select_ms;
    uint32_t last_input_delay_l_ms;
    uint32_t last_input_delay_r_ms;

    RuntimePerfStats();
};

struct UiRuntimeSnapshot {
    RuntimePerfStats perf;
    BatteryInfo battery;
    int battery_percent;
    int scene_id;
    uint32_t now_ms;

    UiRuntimeSnapshot();
};

struct KeyboardKey {
    std::string id;
    std::string text;
    int row;
    int col;
    int span;
    bool action;

    KeyboardKey();
};

struct KeyboardLayout {
    int cols;
    int rows;
    std::vector<KeyboardKey> letter_keys;
    std::vector<KeyboardKey> digit_keys;

    KeyboardLayout();
};

struct TextInputState {
    std::string value;
    int max_len;
    int key_index;
    float key_cursor_t;
    bool digits_mode;
    float panel_t;
    bool confirm_requested;

    TextInputState();
};

struct FontFace;

class Logger {
public:
    explicit Logger(const std::string &path);

    void info(const std::string &msg);
    void warn(const std::string &msg);
    void error(const std::string &msg);

private:
    std::string path_;
    void log(const char *level, const std::string &msg);
};

class SettingsStore {
public:
    explicit SettingsStore(const std::string &path);

    bool load(Settings *settings, Logger *logger) const;
    bool save(const Settings &settings, Logger *logger) const;

    bool apply_volume(int volume, Logger *logger) const;
    bool apply_brightness(int brightness, Logger *logger) const;

private:
    std::string path_;
};

class ProfileStore {
public:
    explicit ProfileStore(const std::string &path);

    bool load(Profile *profile, Logger *logger) const;
    bool save(const Profile &profile, Logger *logger) const;

private:
    std::string path_;
};

class Registry {
public:
    Registry();

    void scan(const std::string &apps_dir, Logger *logger);
    std::vector<AppEntry> apps_for_category(const std::string &category) const;
    const std::vector<AppEntry> &all_apps() const;

private:
    std::vector<AppEntry> apps_;
};

class ProcessRunner {
public:
    int run_app(const AppEntry &app, Logger *logger, std::string *error_text) const;
    int run_app_exclusive(const AppEntry &app, Logger *logger, std::string *error_text) const;
};

class BatteryMonitor {
public:
    BatteryMonitor();
    const BatteryInfo &update(Logger *logger);

private:
    BatteryInfo info_;
    uint64_t next_update_ms_;
    std::string battery_path_;

    void discover(Logger *logger);
};

class Input {
public:
    Input();
    ~Input();

    void init(Logger *logger);
    void poll(int timeout_ms);
    InputSnapshot snapshot() const;
    void clear_edges();

private:
    struct AxisCalibration {
        bool has_absinfo;
        bool centered_from_event;
        int min;
        int max;
        int center;
        int deadzone;

        AxisCalibration();
    };

    struct Raw {
        bool up;
        bool down;
        bool left;
        bool right;
        bool a;
        bool b;
        bool start;
        bool select;
        bool l;
        bool r;
        bool joy;
        int axis_x;
        int axis_y;

        Raw();
    };

    int button_fd_;
    int axis_fd_;
    std::vector<int> evdev_fds_;
    std::vector<SDL_Joystick *> joysticks_;
    AxisCalibration axis_x_cal_;
    AxisCalibration axis_y_cal_;
    Raw raw_;
    Raw prev_raw_;
    InputSnapshot snapshot_;
    uint64_t repeat_due_[4];
    bool queued_nav_up_;
    bool queued_nav_down_;
    bool queued_nav_left_;
    bool queued_nav_right_;
    bool queued_hold_select_;
    bool queued_hold_l_;
    bool queued_hold_r_;
    bool queued_accept_;
    bool queued_back_;
    bool queued_menu_;
    uint64_t queued_accept_ms_;
    uint64_t queued_back_ms_;
    uint64_t queued_menu_ms_;
    uint64_t last_edge_a_ms_;
    uint64_t last_edge_b_ms_;
    uint64_t last_edge_start_ms_;
    uint64_t last_edge_select_ms_;
    uint64_t last_edge_l_ms_;
    uint64_t last_edge_r_ms_;

    void configure_axis_calibration(int fd, int code, AxisCalibration *cal, const char *name, Logger *logger);
    void prime_axis_center(AxisCalibration *cal, int value);
    void set_button_state(bool &field, bool pressed, bool *queued_hold, bool *queued_nav, uint64_t *edge_ms);
    void handle_abs_event(int code, int value);
    void map_keycode(SDLKey key, bool pressed);
    void map_joy_button(uint8_t button, bool pressed);
    void map_ev_key(int code, bool pressed);
    void handle_joy_axis(uint8_t axis, int value);
    void handle_joy_hat(uint8_t value);
    void open_sdl_inputs(Logger *logger);
    void open_evdev_inputs(Logger *logger);
    void close_inputs();
    bool compute_repeat(int idx, bool held, uint64_t now_ms);
    void update_snapshot(uint64_t now_ms);
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(Logger *logger);
    void begin_frame(const Color &bg);
    void present();
    bool disable_vsync();

    int width() const;
    int height() const;

    void fill_rect(int x, int y, int w, int h, const Color &color);
    void draw_rect_outline(int x, int y, int w, int h, const Color &color);
    void draw_text(const std::string &text, int x, int y, const Color &color, bool large = false);
    void draw_text_centered(const std::string &text, int center_x, int y, const Color &color, bool large = false);
    int text_width(const std::string &text, bool large = false);
    int line_height(bool large = false);
    std::string ellipsize_to_width(const std::string &text, int max_width, bool large = false);
    std::vector<std::string> wrap_text_to_width(const std::string &text, int max_width, size_t max_lines, bool large = false);
    void draw_icon64(const std::string &path, int x, int y);
    void draw_icon(const std::string &path, int x, int y, int w, int h);
    void draw_background_cached(const std::string &theme_key, const Color &top, const Color &bottom, const Color &scanline);
    void draw_panel_cached(const std::string &style_key, int x, int y, int w, int h, const Color &top, const Color &bottom, const Color &border);
    void draw_fade(uint8_t alpha);
    bool has_vsync() const;

private:
    struct TextWidthEntry {
        int width;
        uint64_t last_used;

        TextWidthEntry() : width(0), last_used(0) {}
    };

    struct IconTexture {
        SDL_Surface *surface;
        int w;
        int h;
        size_t bytes;
        uint64_t last_used;

        IconTexture() : surface(nullptr), w(0), h(0), bytes(0), last_used(0) {}
    };

    struct TextTexture {
        SDL_Surface *surface;
        int w;
        int h;
        size_t bytes;
        uint64_t last_used;

        TextTexture() : surface(nullptr), w(0), h(0), bytes(0), last_used(0) {}
    };

    struct StyleTexture {
        SDL_Surface *surface;
        int w;
        int h;
        size_t bytes;
        uint64_t last_used;

        StyleTexture() : surface(nullptr), w(0), h(0), bytes(0), last_used(0) {}
    };

    SDL_Surface *screen_;
    Logger *logger_;
    void *font_lib_;
    FontFace *font_small_;
    FontFace *font_large_;
    std::map<std::string, IconTexture> icon_cache_;
    std::map<std::string, IconTexture> icon_scaled_cache_;
    std::map<std::string, TextTexture> text_cache_;
    std::map<std::string, TextWidthEntry> text_width_cache_;
    std::map<std::string, StyleTexture> style_cache_;
    size_t text_cache_bytes_;
    size_t icon_cache_bytes_;
    size_t icon_scaled_cache_bytes_;
    size_t style_cache_bytes_;
    size_t text_cache_budget_bytes_;
    size_t icon_cache_budget_bytes_;
    size_t icon_scaled_cache_budget_bytes_;
    size_t style_cache_budget_bytes_;
    size_t text_width_cache_limit_;
    uint64_t cache_tick_;
    bool cache_debug_;
    uint64_t cache_next_log_ms_;
    uint64_t text_cache_hits_;
    uint64_t text_cache_misses_;
    uint64_t text_cache_evicts_;
    uint64_t icon_cache_hits_;
    uint64_t icon_cache_misses_;
    uint64_t icon_cache_evicts_;
    uint64_t icon_scaled_cache_hits_;
    uint64_t icon_scaled_cache_misses_;
    uint64_t icon_scaled_cache_evicts_;
    uint64_t text_width_cache_hits_;
    uint64_t text_width_cache_misses_;
    uint64_t text_width_cache_evicts_;
    uint64_t style_cache_hits_;
    uint64_t style_cache_misses_;
    uint64_t style_cache_evicts_;
    bool has_vsync_;
    SDL_Surface *fade_surface_;

    bool create_screen();
    void clear_texture_caches();
    IconTexture *get_icon64(const std::string &path);
    IconTexture *get_scaled_icon(const std::string &path, int w, int h);
    TextTexture *get_text_texture(const std::string &text, const Color &color, bool large);
    StyleTexture *get_style_texture(const std::string &key);
    StyleTexture *cache_style_surface(const std::string &key, SDL_Surface *surface);
    void trim_text_width_cache();
    void trim_text_cache();
    void trim_icon_cache();
    void trim_scaled_icon_cache();
    void trim_style_cache();
    void maybe_log_cache_stats();
};

enum SceneId {
    SCENE_WIZARD = 0,
    SCENE_PIN_LOCK = 1,
    SCENE_LAUNCHER = 2,
    SCENE_SETTINGS = 3,
    SCENE_DIAGNOSTICS = 4,
    SCENE_SYSTEM_INFO = 5,
    SCENE_FILE_MANAGER = 6,
    SCENE_POWER_OFF = 7
};

struct SceneOutput {
    bool request_scene;
    SceneId next_scene;

    bool request_launch;
    AppEntry launch_app;
    bool request_restart;

    SceneOutput();
};

class Scene {
public:
    virtual ~Scene() {}
    virtual void on_enter() = 0;
    virtual SceneOutput update(const InputSnapshot &input, uint32_t now_ms) = 0;
    virtual void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot) = 0;
};

class WizardScene : public Scene {
public:
    WizardScene(Profile *profile,
                ProfileStore *profile_store,
                Settings *settings,
                SettingsStore *settings_store,
                Logger *logger);

    void on_enter();
    SceneOutput update(const InputSnapshot &input, uint32_t now_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    enum Step {
        STEP_WELCOME = 0,
        STEP_LANGUAGE = 1,
        STEP_THEME = 2,
        STEP_NAME = 3,
        STEP_PIN_CHOICE = 4,
        STEP_PIN = 5,
        STEP_INSTALL = 6,
        STEP_DONE = 7
    };

    Profile *profile_;
    ProfileStore *profile_store_;
    Settings *settings_;
    SettingsStore *settings_store_;
    Logger *logger_;

    Step step_;
    TextInputState name_input_;
    KeyboardLayout keyboard_layout_;
    std::vector<std::string> language_codes_;
    int language_selected_;
    float language_cursor_t_;
    int theme_selected_;
    float theme_cursor_t_;
    int pin_choice_selected_;
    float pin_choice_cursor_t_;
    int pin_digits_[4];
    int pin_cursor_;
    uint32_t name_error_until_ms_;
    bool name_reserved_error_;
    uint32_t welcome_cycle_base_ms_;
    std::string power_hint_;
    pid_t install_pid_;
    int install_output_fd_;
    bool install_running_;
    bool install_complete_;
    bool install_success_;
    std::string install_buffer_;
    std::string install_status_;
    std::vector<std::string> install_facts_;
    int install_fact_idx_;
    uint32_t install_fact_due_ms_;

    void init_keyboard_layout();
    void reset_name_input_from_profile();
    void update_name_input(const InputSnapshot &input);
    void render_name_input(Renderer *renderer, const LayoutMetrics &layout, const LayoutRect &body, const Theme &theme, uint32_t now_ms);
    std::string cleaned_username() const;
    void begin_install_phase(uint32_t now_ms);
    void update_install_phase(uint32_t now_ms);
    void stop_install_phase();
};

class PinLockScene : public Scene {
public:
    PinLockScene(const Profile *profile, const Settings *settings, Logger *logger);

    void on_enter();
    SceneOutput update(const InputSnapshot &input, uint32_t now_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    const Profile *profile_;
    const Settings *settings_;
    Logger *logger_;

    int pin_digits_[4];
    int pin_cursor_;
    uint32_t error_until_ms_;
};

class LauncherScene : public Scene {
public:
    LauncherScene(const Profile *profile,
                  const Registry *registry,
                  const Settings *settings);

    void on_enter();
    SceneOutput update(const InputSnapshot &input, uint32_t now_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    const Profile *profile_;
    const Registry *registry_;
    const Settings *settings_;

    int selected_;
    bool in_system_panel_;
    int system_selected_;
    float selection_t_;
    float panel_t_;
    float page_t_;
    float bottom_panel_t_;
    float system_marker_t_;
    bool prev_select_hold_;
    std::vector<AppEntry> items_cache_;
    uint32_t next_items_refresh_ms_;

    void refresh_items_cache(uint32_t now_ms, bool force);
};

class SettingsScene : public Scene {
public:
    SettingsScene(Settings *settings, SettingsStore *store, Profile *profile, ProfileStore *profile_store, Logger *logger);

    void on_enter();
    SceneOutput update(const InputSnapshot &input, uint32_t now_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    enum ResetMode {
        RESET_NONE = 0,
        RESET_CONFIRM = 1,
        RESET_PIN = 2,
        RESET_USERNAME = 3,
        RESET_DONE = 4
    };

    Settings *settings_;
    SettingsStore *store_;
    Profile *profile_;
    ProfileStore *profile_store_;
    Logger *logger_;
    int category_selected_;
    int option_selected_;
    int focus_column_;
    float category_cursor_t_;
    float option_cursor_t_;

    ResetMode reset_mode_;
    uint32_t reset_error_until_ms_;
    TextInputState reset_input_;
    KeyboardLayout reset_keyboard_layout_;
    std::string reset_expected_username_;
    std::vector<std::string> reset_log_lines_;

    void init_reset_keyboard_layout();
    void begin_reset_pin_mode();
    void begin_reset_username_mode();
    void update_reset_keyboard(const InputSnapshot &input, bool pin_mode);
    std::string reset_name_normalized() const;
    bool apply_factory_reset();
};

class DiagnosticsScene : public Scene {
public:
    DiagnosticsScene(Input *input, BatteryMonitor *battery, const Settings *settings, Logger *logger);
    ~DiagnosticsScene();

    void on_enter();
    SceneOutput update(const InputSnapshot &input, uint32_t now_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    Input *input_;
    BatteryMonitor *battery_;
    const Settings *settings_;
    Logger *logger_;
    InputSnapshot last_input_;
};

class SystemInfoScene : public Scene {
public:
    SystemInfoScene(const Registry *registry, const Settings *settings, Logger *logger);

    void on_enter();
    SceneOutput update(const InputSnapshot &input, uint32_t now_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    const Registry *registry_;
    const Settings *settings_;
    Logger *logger_;
    std::string cpu_model_cached_;
    long ram_mib_cached_;
    long free_mib_cached_;
    int app_count_cached_;
    uint32_t next_refresh_ms_;

    void refresh_cached_stats(uint32_t now_ms);
};

class FileManagerScene : public Scene {
public:
    FileManagerScene(const Settings *settings, Logger *logger);

    void on_enter();
    SceneOutput update(const InputSnapshot &input, uint32_t now_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    struct Entry {
        std::string name;
        std::string path;
        bool dir;
        long size;
    };

    const Settings *settings_;
    Logger *logger_;

    std::vector<std::string> roots_;
    std::vector<std::string> root_labels_;
    std::vector<bool> root_editable_;
    int root_selected_;
    int root_offset_;
    std::string current_root_;
    std::string current_path_;
    bool current_root_editable_;
    std::vector<Entry> entries_;
    int selected_;
    int list_offset_;

    bool file_preview_mode_;
    bool edit_mode_;
    std::string file_name_;
    std::string file_path_;
    std::vector<std::string> file_lines_;
    std::vector<std::string> file_backup_lines_;
    int file_scroll_;
    bool file_read_only_;
    bool file_text_;
    bool file_dirty_;
    int cursor_row_;
    int cursor_col_;
    std::string keyset_letters_;
    std::string keyset_digits_;
    int keyset_index_;
    bool keyset_digits_mode_;
    bool prev_l_hold_;
    bool prev_r_hold_;
    bool prev_select_hold_;
    std::string status_line_;
    uint32_t status_until_ms_;

    void refresh_roots();
    void open_root(int index);
    void refresh_entries();
    void open_file_preview(const std::string &path, const std::string &name);
    void ensure_root_visible();
    void ensure_entry_visible();
    void ensure_line_visible();
    bool is_path_under(const std::string &base, const std::string &child) const;
    bool is_forbidden_edit_path(const std::string &path) const;
    bool has_image_extension(const std::string &path) const;
    bool is_probably_text_file(const std::string &path) const;
    void begin_edit();
    void discard_edit();
    bool save_edit();
    void normalize_cursor();
    void move_cursor_h(int dx);
    void move_cursor_v(int dy);
    void insert_char(char c);
    void backspace_char();
    void newline_char();
    void set_status(const std::string &msg, uint32_t now_ms, uint32_t duration_ms = 1400);
};

class PowerOffScene : public Scene {
public:
    explicit PowerOffScene(const Settings *settings);

    void on_enter();
    SceneOutput update(const InputSnapshot &input, uint32_t now_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    const Settings *settings_;
};

class SceneManager {
public:
    SceneManager();

    void register_scene(SceneId id, Scene *scene);
    void set_initial(SceneId id);
    void request(SceneId id);

    Scene *current_scene();
    SceneId current_id() const;
    void update(uint32_t delta_ms);
    void render(Renderer *renderer, uint32_t now_ms, const UiRuntimeSnapshot &snapshot);

private:
    std::map<int, Scene *> scenes_;
    SceneId current_;
    SceneId target_;
    bool has_current_;

    enum TransitionPhase {
        TRANSITION_NONE = 0,
        TRANSITION_OUT = 1,
        TRANSITION_IN = 2
    };

    TransitionPhase phase_;
    uint32_t phase_elapsed_ms_;
};

class App {
public:
    App();
    int run();

private:
    Logger logger_;
    SettingsStore settings_store_;
    ProfileStore profile_store_;
    Registry registry_;
    ProcessRunner process_runner_;
    BatteryMonitor battery_;
    Input input_;
    Renderer renderer_;

    Settings settings_;
    Profile profile_;

    WizardScene wizard_scene_;
    PinLockScene pin_lock_scene_;
    LauncherScene launcher_scene_;
    SettingsScene settings_scene_;
    DiagnosticsScene diagnostics_scene_;
    SystemInfoScene system_info_scene_;
    FileManagerScene file_manager_scene_;
    PowerOffScene power_off_scene_;
    SceneManager scene_manager_;

    std::string overlay_message_;
    uint32_t overlay_until_ms_;
    uint32_t volume_overlay_until_ms_;
    int volume_overlay_value_;
    int volume_before_mute_;
    bool mute_active_;
    bool prev_l_hold_;
    bool prev_r_hold_;
    bool prev_lr_combo_hold_;
    RuntimePerfStats perf_stats_;
    UiRuntimeSnapshot ui_snapshot_;

    void show_message(const std::string &msg, uint32_t duration_ms);
    void render_overlay(uint32_t now_ms);
};

bool ensure_directory(const std::string &path);
uint64_t monotonic_ms();
std::string read_text_file(const std::string &path);
bool write_text_file(const std::string &path, const std::string &content);
std::string normalize_language(const std::string &language);
std::vector<std::string> available_languages();
std::string language_label(const std::string &language);
std::string translate_ui_text(const std::string &language, const std::string &key, const std::string &fallback);
std::string normalize_theme_id(const std::string &theme_id);
const Theme &theme_by_id(const std::string &theme_id);
const Typography &default_typography();
LayoutMetrics build_layout_metrics(int screen_w, int screen_h);

}  // namespace inconsole

#endif
