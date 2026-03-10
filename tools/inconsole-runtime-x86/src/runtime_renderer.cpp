#include "runtime.hpp"

#include <png.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

namespace inconsole {

struct FontFace {
    FT_Face face;
    int pixel_size;

    FontFace() : face(nullptr), pixel_size(0) {}
};

namespace {

const char *kFontCandidates[] = {
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf"
};

void rgba_masks(Uint32 *rmask, Uint32 *gmask, Uint32 *bmask, Uint32 *amask) {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    *rmask = 0xff000000U;
    *gmask = 0x00ff0000U;
    *bmask = 0x0000ff00U;
    *amask = 0x000000ffU;
#else
    *rmask = 0x000000ffU;
    *gmask = 0x0000ff00U;
    *bmask = 0x00ff0000U;
    *amask = 0xff000000U;
#endif
}

SDL_Surface *create_rgba_surface(int w, int h) {
    Uint32 rmask = 0;
    Uint32 gmask = 0;
    Uint32 bmask = 0;
    Uint32 amask = 0;
    rgba_masks(&rmask, &gmask, &bmask, &amask);
    return SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 32, rmask, gmask, bmask, amask);
}

Color color_mix(const Color &a, const Color &b, int num, int den) {
    Color c;
    c.r = static_cast<uint8_t>((a.r * (den - num) + b.r * num) / den);
    c.g = static_cast<uint8_t>((a.g * (den - num) + b.g * num) / den);
    c.b = static_cast<uint8_t>((a.b * (den - num) + b.b * num) / den);
    return c;
}

Color color_mixf(const Color &a, const Color &b, float t) {
    const float k = std::max(0.0f, std::min(1.0f, t));
    Color c;
    c.r = static_cast<uint8_t>(static_cast<float>(a.r) + (static_cast<float>(b.r) - static_cast<float>(a.r)) * k);
    c.g = static_cast<uint8_t>(static_cast<float>(a.g) + (static_cast<float>(b.g) - static_cast<float>(a.g)) * k);
    c.b = static_cast<uint8_t>(static_cast<float>(a.b) + (static_cast<float>(b.b) - static_cast<float>(a.b)) * k);
    return c;
}

int env_int_or_default(const char *name, int fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    const int x = atoi(v);
    return x > 0 ? x : fallback;
}

size_t env_megabytes_or_default(const char *name, size_t fallback_mb) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback_mb * 1024U * 1024U;
    const int mb = atoi(v);
    if (mb <= 0) return fallback_mb * 1024U * 1024U;
    return static_cast<size_t>(mb) * 1024U * 1024U;
}

bool env_truthy(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return false;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "FALSE") == 0) return false;
    return true;
}

FT_Library as_ftlib(void *p) {
    return reinterpret_cast<FT_Library>(p);
}

bool load_png_rgba(const std::string &path, std::vector<uint8_t> *pixels, int *out_w, int *out_h) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        fclose(fp);
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    pixels->assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    std::vector<png_bytep> rows(height);
    for (size_t y = 0; y < height; ++y) {
        rows[y] = reinterpret_cast<png_bytep>(&(*pixels)[y * static_cast<size_t>(width) * 4]);
    }
    png_read_image(png, rows.data());

    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);

    *out_w = static_cast<int>(width);
    *out_h = static_cast<int>(height);
    return true;
}

FontFace *open_font_face(FT_Library ft_lib, int px_size, Logger *logger) {
    for (size_t i = 0; i < sizeof(kFontCandidates) / sizeof(kFontCandidates[0]); ++i) {
        FT_Face face = nullptr;
        if (FT_New_Face(ft_lib, kFontCandidates[i], 0, &face) != 0) continue;
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px_size)) != 0) {
            FT_Done_Face(face);
            continue;
        }

        FontFace *out = new FontFace();
        out->face = face;
        out->pixel_size = px_size;
        return out;
    }

    if (logger) logger->error("Renderer: cannot load font face");
    return nullptr;
}

int measure_text_width(FontFace *font, const std::string &text) {
    if (!font || !font->face) return 0;

    std::vector<uint32_t> codepoints;
    codepoints.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const uint8_t b0 = static_cast<uint8_t>(text[i]);
        uint32_t cp = 0;
        size_t n = 1;
        if ((b0 & 0x80u) == 0) {
            cp = b0;
            n = 1;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            cp = static_cast<uint32_t>(b0 & 0x1Fu) << 6;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            cp = static_cast<uint32_t>(b0 & 0x0Fu) << 12;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 1]) & 0x3Fu) << 6;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            cp = static_cast<uint32_t>(b0 & 0x07u) << 18;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 1]) & 0x3Fu) << 12;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 2]) & 0x3Fu) << 6;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 3]) & 0x3Fu);
            n = 4;
        } else {
            cp = '?';
            n = 1;
        }
        codepoints.push_back(cp);
        i += n;
    }

    int width = 0;
    for (size_t i = 0; i < codepoints.size(); ++i) {
        if (FT_Load_Char(font->face, codepoints[i], FT_LOAD_DEFAULT) != 0) continue;
        width += static_cast<int>(font->face->glyph->advance.x >> 6);
    }
    return width;
}

int line_height(FontFace *font) {
    if (!font || !font->face) return 16;
    const int h = static_cast<int>(font->face->size->metrics.height >> 6);
    if (h > 0) return h;
    return font->pixel_size + 2;
}

SDL_Surface *optimize_alpha_surface(SDL_Surface *surface) {
    if (!surface) return nullptr;
    SDL_Surface *optimized = SDL_DisplayFormatAlpha(surface);
    if (optimized) return optimized;
    optimized = SDL_ConvertSurface(surface, surface->format, surface->flags);
    return optimized;
}

SDL_Surface *build_text_surface(FontFace *font, const std::string &text, const Color &color) {
    if (!font || !font->face || text.empty()) return nullptr;

    const int width = std::max(1, measure_text_width(font, text));
    const int ascent = static_cast<int>(font->face->size->metrics.ascender >> 6);
    const int line_h = std::max(1, line_height(font));

    SDL_Surface *surface = create_rgba_surface(width, line_h + 4);
    if (!surface) return nullptr;
    SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 0, 0, 0, 0));

    uint32_t map_cache[256];
    for (int i = 0; i < 256; ++i) {
        map_cache[i] = SDL_MapRGBA(surface->format, color.r, color.g, color.b, static_cast<uint8_t>(i));
    }

    SDL_LockSurface(surface);
    uint8_t *pixels = static_cast<uint8_t *>(surface->pixels);

    std::vector<uint32_t> codepoints;
    codepoints.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const uint8_t b0 = static_cast<uint8_t>(text[i]);
        uint32_t cp = 0;
        size_t n = 1;
        if ((b0 & 0x80u) == 0) {
            cp = b0;
            n = 1;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            cp = static_cast<uint32_t>(b0 & 0x1Fu) << 6;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            cp = static_cast<uint32_t>(b0 & 0x0Fu) << 12;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 1]) & 0x3Fu) << 6;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            cp = static_cast<uint32_t>(b0 & 0x07u) << 18;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 1]) & 0x3Fu) << 12;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 2]) & 0x3Fu) << 6;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(text[i + 3]) & 0x3Fu);
            n = 4;
        } else {
            cp = '?';
            n = 1;
        }
        codepoints.push_back(cp);
        i += n;
    }

    int pen_x = 0;
    for (size_t i = 0; i < codepoints.size(); ++i) {
        if (FT_Load_Char(font->face, codepoints[i], FT_LOAD_RENDER) != 0) continue;

        FT_GlyphSlot g = font->face->glyph;
        const int glyph_x = pen_x + g->bitmap_left;
        const int glyph_y = ascent - g->bitmap_top;

        for (int row = 0; row < static_cast<int>(g->bitmap.rows); ++row) {
            for (int col = 0; col < static_cast<int>(g->bitmap.width); ++col) {
                const int dst_x = glyph_x + col;
                const int dst_y = glyph_y + row;
                if (dst_x < 0 || dst_x >= surface->w || dst_y < 0 || dst_y >= surface->h) continue;

                const uint8_t alpha = g->bitmap.buffer[row * g->bitmap.pitch + col];
                if (alpha == 0) continue;

                uint32_t *dst = reinterpret_cast<uint32_t *>(pixels + dst_y * surface->pitch + dst_x * 4);
                *dst = map_cache[alpha];
            }
        }

        pen_x += static_cast<int>(g->advance.x >> 6);
    }

    SDL_UnlockSurface(surface);
    SDL_SetAlpha(surface, SDL_SRCALPHA, 255);
    return surface;
}

std::string text_cache_key(const std::string &text, const Color &color, bool large) {
    std::ostringstream ss;
    ss << (large ? 'L' : 'S') << '#' << static_cast<int>(color.r) << ',' << static_cast<int>(color.g) << ','
       << static_cast<int>(color.b) << ':' << text;
    return ss.str();
}

std::string text_width_cache_key(const std::string &text, bool large) {
    std::ostringstream ss;
    ss << (large ? 'L' : 'S') << ':' << text;
    return ss.str();
}

std::string scaled_icon_cache_key(const std::string &path, int w, int h) {
    std::ostringstream ss;
    ss << path << ':' << w << 'x' << h;
    return ss.str();
}

}  // namespace

Renderer::Renderer()
    : screen_(nullptr),
      logger_(nullptr),
      font_lib_(nullptr),
      font_small_(nullptr),
      font_large_(nullptr),
      icon_cache_(),
      icon_scaled_cache_(),
      text_cache_(),
      text_width_cache_(),
      style_cache_(),
      text_cache_bytes_(0),
      icon_cache_bytes_(0),
      icon_scaled_cache_bytes_(0),
      style_cache_bytes_(0),
      text_cache_budget_bytes_(6U * 1024U * 1024U),
      icon_cache_budget_bytes_(6U * 1024U * 1024U),
      icon_scaled_cache_budget_bytes_(4U * 1024U * 1024U),
      style_cache_budget_bytes_(4U * 1024U * 1024U),
      text_width_cache_limit_(2048),
      cache_tick_(0),
      cache_debug_(false),
      cache_next_log_ms_(0),
      text_cache_hits_(0),
      text_cache_misses_(0),
      text_cache_evicts_(0),
      icon_cache_hits_(0),
      icon_cache_misses_(0),
      icon_cache_evicts_(0),
      icon_scaled_cache_hits_(0),
      icon_scaled_cache_misses_(0),
      icon_scaled_cache_evicts_(0),
      text_width_cache_hits_(0),
      text_width_cache_misses_(0),
      text_width_cache_evicts_(0),
      style_cache_hits_(0),
      style_cache_misses_(0),
      style_cache_evicts_(0),
      has_vsync_(false),
      fade_surface_(nullptr) {}

Renderer::~Renderer() {
    clear_texture_caches();
    if (fade_surface_) SDL_FreeSurface(fade_surface_);

    if (font_small_ && font_small_->face) FT_Done_Face(font_small_->face);
    if (font_large_ && font_large_->face) FT_Done_Face(font_large_->face);
    delete font_small_;
    delete font_large_;

    if (font_lib_) FT_Done_FreeType(as_ftlib(font_lib_));

    SDL_Quit();
}

void Renderer::clear_texture_caches() {
    for (std::map<std::string, IconTexture>::iterator it = icon_cache_.begin(); it != icon_cache_.end(); ++it) {
        if (it->second.surface) SDL_FreeSurface(it->second.surface);
    }
    icon_cache_.clear();

    for (std::map<std::string, IconTexture>::iterator it = icon_scaled_cache_.begin(); it != icon_scaled_cache_.end(); ++it) {
        if (it->second.surface) SDL_FreeSurface(it->second.surface);
    }
    icon_scaled_cache_.clear();

    for (std::map<std::string, TextTexture>::iterator it = text_cache_.begin(); it != text_cache_.end(); ++it) {
        if (it->second.surface) SDL_FreeSurface(it->second.surface);
    }
    text_cache_.clear();
    text_width_cache_.clear();

    for (std::map<std::string, StyleTexture>::iterator it = style_cache_.begin(); it != style_cache_.end(); ++it) {
        if (it->second.surface) SDL_FreeSurface(it->second.surface);
    }
    style_cache_.clear();

    text_cache_bytes_ = 0;
    icon_cache_bytes_ = 0;
    icon_scaled_cache_bytes_ = 0;
    style_cache_bytes_ = 0;
}

bool Renderer::create_screen() {
    const int logical_w = 480;
    const int logical_h = 272;

    uint32_t flags = SDL_SWSURFACE | SDL_DOUBLEBUF;
    if (env_truthy("INCONSOLE_FULLSCREEN")) flags |= SDL_FULLSCREEN;

    screen_ = SDL_SetVideoMode(logical_w, logical_h, 32, flags);
    if (!screen_) {
        screen_ = SDL_SetVideoMode(logical_w, logical_h, 16, flags);
    }

    if (!screen_) return false;

    SDL_WM_SetCaption("InConsole Runtime", nullptr);
    SDL_ShowCursor(SDL_DISABLE);
    has_vsync_ = false;
    return true;
}

bool Renderer::init(Logger *logger) {
    logger_ = logger;
    const char *video = getenv("SDL_VIDEODRIVER");
    if (!video || video[0] == '\0') {
        setenv("SDL_VIDEODRIVER", "fbcon", 0);
    }

    if (logger) logger->info("Renderer: SDL_Init begin");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_JOYSTICK) != 0) {
        if (logger) logger->error(std::string("SDL_Init failed: ") + SDL_GetError());
        return false;
    }

    if (!create_screen()) {
        if (logger) logger->error(std::string("SDL_SetVideoMode failed: ") + SDL_GetError());
        return false;
    }

    FT_Library ft_lib = nullptr;
    if (FT_Init_FreeType(&ft_lib) != 0) {
        if (logger) logger->error("Renderer: FT_Init_FreeType failed");
        return false;
    }

    font_lib_ = ft_lib;
    const Typography &type = default_typography();
    font_small_ = open_font_face(ft_lib, type.small_px, logger);
    font_large_ = open_font_face(ft_lib, type.large_px, logger);

    if (!font_small_ || !font_large_) {
        if (logger) logger->error("Renderer: font loading failed");
        return false;
    }

    char driver[64];
    memset(driver, 0, sizeof(driver));
    const char *driver_name = SDL_VideoDriverName(driver, sizeof(driver)) ? driver : "unknown";

    if (logger) {
        logger->info("Renderer initialized: logical 480x272");
        logger->info(std::string("Renderer backend: sdl1/") + driver_name);
        logger->info("Renderer format: bpp=" + std::to_string(screen_->format ? screen_->format->BitsPerPixel : 0) +
                     " Rmask=" + std::to_string(screen_->format ? screen_->format->Rmask : 0) +
                     " Gmask=" + std::to_string(screen_->format ? screen_->format->Gmask : 0) +
                     " Bmask=" + std::to_string(screen_->format ? screen_->format->Bmask : 0));
        logger->info("Renderer VSync=OFF");
    }

    const bool low_mem_mode = env_truthy("INCONSOLE_FORCE_SOFTWARE_RENDERER") || env_truthy("INCONSOLE_LOW_MEM");
    text_cache_budget_bytes_ = env_megabytes_or_default("INCONSOLE_CACHE_TEXT_MB", low_mem_mode ? 2 : 6);
    icon_cache_budget_bytes_ = env_megabytes_or_default("INCONSOLE_CACHE_ICON_MB", low_mem_mode ? 2 : 6);
    icon_scaled_cache_budget_bytes_ = env_megabytes_or_default("INCONSOLE_CACHE_ICON_SCALED_MB", low_mem_mode ? 1 : 4);
    style_cache_budget_bytes_ = env_megabytes_or_default("INCONSOLE_CACHE_STYLE_MB", low_mem_mode ? 1 : 4);
    const size_t min_style_cache = 3U * 1024U * 1024U;
    if (style_cache_budget_bytes_ < min_style_cache) {
        style_cache_budget_bytes_ = min_style_cache;
        if (logger_) logger_->warn("Renderer: increased style cache to minimum 3MB to avoid redraw thrashing");
    }
    text_width_cache_limit_ = static_cast<size_t>(std::max(128, env_int_or_default("INCONSOLE_CACHE_WIDTH_ITEMS", low_mem_mode ? 768 : 2048)));
    cache_debug_ = env_int_or_default("INCONSOLE_CACHE_DEBUG", 0) > 0;
    cache_next_log_ms_ = monotonic_ms() + 5000;
    return true;
}

void Renderer::begin_frame(const Color &bg) {
    if (!screen_) return;

    Color top = color_mix(bg, Color{0, 0, 0}, 1, 3);
    Color bottom = color_mix(bg, Color{255, 255, 255}, 1, 8);

    for (int y = 0; y < 272; ++y) {
        Color line = color_mix(top, bottom, y, 271);
        SDL_Rect row;
        row.x = 0;
        row.y = y;
        row.w = 480;
        row.h = 1;
        SDL_FillRect(screen_, &row, SDL_MapRGB(screen_->format, line.r, line.g, line.b));
    }
}

void Renderer::present() {
    if (!screen_) return;
    SDL_Flip(screen_);
}

bool Renderer::disable_vsync() {
    has_vsync_ = false;
    return true;
}

int Renderer::width() const { return 480; }
int Renderer::height() const { return 272; }

void Renderer::fill_rect(int x, int y, int w, int h, const Color &color) {
    if (!screen_ || w <= 0 || h <= 0) return;
    SDL_Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    SDL_FillRect(screen_, &r, SDL_MapRGB(screen_->format, color.r, color.g, color.b));
}

void Renderer::draw_rect_outline(int x, int y, int w, int h, const Color &color) {
    if (!screen_ || w <= 0 || h <= 0) return;

    const Uint32 px = SDL_MapRGB(screen_->format, color.r, color.g, color.b);
    SDL_Rect r;

    r = SDL_Rect{static_cast<Sint16>(x), static_cast<Sint16>(y), static_cast<Uint16>(w), 1};
    SDL_FillRect(screen_, &r, px);
    r = SDL_Rect{static_cast<Sint16>(x), static_cast<Sint16>(y + h - 1), static_cast<Uint16>(w), 1};
    SDL_FillRect(screen_, &r, px);
    r = SDL_Rect{static_cast<Sint16>(x), static_cast<Sint16>(y), 1, static_cast<Uint16>(h)};
    SDL_FillRect(screen_, &r, px);
    r = SDL_Rect{static_cast<Sint16>(x + w - 1), static_cast<Sint16>(y), 1, static_cast<Uint16>(h)};
    SDL_FillRect(screen_, &r, px);
}

void Renderer::trim_text_cache() {
    while (text_cache_bytes_ > text_cache_budget_bytes_ && !text_cache_.empty()) {
        std::map<std::string, TextTexture>::iterator victim = text_cache_.begin();
        for (std::map<std::string, TextTexture>::iterator it = text_cache_.begin(); it != text_cache_.end(); ++it) {
            if (it->second.last_used < victim->second.last_used) victim = it;
        }
        if (victim->second.surface) SDL_FreeSurface(victim->second.surface);
        if (victim->second.bytes <= text_cache_bytes_) text_cache_bytes_ -= victim->second.bytes;
        ++text_cache_evicts_;
        text_cache_.erase(victim);
    }
}

void Renderer::trim_text_width_cache() {
    while (text_width_cache_.size() > text_width_cache_limit_ && !text_width_cache_.empty()) {
        std::map<std::string, TextWidthEntry>::iterator victim = text_width_cache_.begin();
        for (std::map<std::string, TextWidthEntry>::iterator it = text_width_cache_.begin(); it != text_width_cache_.end(); ++it) {
            if (it->second.last_used < victim->second.last_used) victim = it;
        }
        ++text_width_cache_evicts_;
        text_width_cache_.erase(victim);
    }
}

void Renderer::trim_icon_cache() {
    while (icon_cache_bytes_ > icon_cache_budget_bytes_ && !icon_cache_.empty()) {
        std::map<std::string, IconTexture>::iterator victim = icon_cache_.begin();
        for (std::map<std::string, IconTexture>::iterator it = icon_cache_.begin(); it != icon_cache_.end(); ++it) {
            if (it->second.last_used < victim->second.last_used) victim = it;
        }
        if (victim->second.surface) SDL_FreeSurface(victim->second.surface);
        if (victim->second.bytes <= icon_cache_bytes_) icon_cache_bytes_ -= victim->second.bytes;
        ++icon_cache_evicts_;
        icon_cache_.erase(victim);
    }
}

void Renderer::trim_scaled_icon_cache() {
    while (icon_scaled_cache_bytes_ > icon_scaled_cache_budget_bytes_ && !icon_scaled_cache_.empty()) {
        std::map<std::string, IconTexture>::iterator victim = icon_scaled_cache_.begin();
        for (std::map<std::string, IconTexture>::iterator it = icon_scaled_cache_.begin(); it != icon_scaled_cache_.end(); ++it) {
            if (it->second.last_used < victim->second.last_used) victim = it;
        }
        if (victim->second.surface) SDL_FreeSurface(victim->second.surface);
        if (victim->second.bytes <= icon_scaled_cache_bytes_) icon_scaled_cache_bytes_ -= victim->second.bytes;
        ++icon_scaled_cache_evicts_;
        icon_scaled_cache_.erase(victim);
    }
}

void Renderer::trim_style_cache() {
    while (style_cache_bytes_ > style_cache_budget_bytes_ && !style_cache_.empty()) {
        std::map<std::string, StyleTexture>::iterator victim = style_cache_.begin();
        for (std::map<std::string, StyleTexture>::iterator it = style_cache_.begin(); it != style_cache_.end(); ++it) {
            if (it->second.last_used < victim->second.last_used) victim = it;
        }
        if (victim->second.surface) SDL_FreeSurface(victim->second.surface);
        if (victim->second.bytes <= style_cache_bytes_) style_cache_bytes_ -= victim->second.bytes;
        ++style_cache_evicts_;
        style_cache_.erase(victim);
    }
}

void Renderer::maybe_log_cache_stats() {
    if (!cache_debug_ || !logger_) return;
    const uint64_t now = monotonic_ms();
    if (now < cache_next_log_ms_) return;
    cache_next_log_ms_ = now + 5000;

    logger_->info("Cache text: bytes=" + std::to_string(text_cache_bytes_) + " items=" + std::to_string(text_cache_.size()) +
                  " hit=" + std::to_string(text_cache_hits_) + " miss=" + std::to_string(text_cache_misses_) +
                  " evict=" + std::to_string(text_cache_evicts_));
    logger_->info("Cache icon: bytes=" + std::to_string(icon_cache_bytes_) + " items=" + std::to_string(icon_cache_.size()) +
                  " hit=" + std::to_string(icon_cache_hits_) + " miss=" + std::to_string(icon_cache_misses_) +
                  " evict=" + std::to_string(icon_cache_evicts_));
    logger_->info("Cache icon-scaled: bytes=" + std::to_string(icon_scaled_cache_bytes_) +
                  " items=" + std::to_string(icon_scaled_cache_.size()) + " hit=" + std::to_string(icon_scaled_cache_hits_) +
                  " miss=" + std::to_string(icon_scaled_cache_misses_) + " evict=" + std::to_string(icon_scaled_cache_evicts_));
    logger_->info("Cache text-width: items=" + std::to_string(text_width_cache_.size()) + "/" + std::to_string(text_width_cache_limit_) +
                  " hit=" + std::to_string(text_width_cache_hits_) + " miss=" + std::to_string(text_width_cache_misses_) +
                  " evict=" + std::to_string(text_width_cache_evicts_));
    logger_->info("Cache style: bytes=" + std::to_string(style_cache_bytes_) + " items=" + std::to_string(style_cache_.size()) +
                  " hit=" + std::to_string(style_cache_hits_) + " miss=" + std::to_string(style_cache_misses_) +
                  " evict=" + std::to_string(style_cache_evicts_));
}

Renderer::StyleTexture *Renderer::get_style_texture(const std::string &key) {
    std::map<std::string, StyleTexture>::iterator it = style_cache_.find(key);
    if (it == style_cache_.end()) return nullptr;
    ++style_cache_hits_;
    it->second.last_used = ++cache_tick_;
    return &it->second;
}

Renderer::StyleTexture *Renderer::cache_style_surface(const std::string &key, SDL_Surface *surface) {
    if (!surface) return nullptr;

    SDL_Surface *optimized = optimize_alpha_surface(surface);
    if (!optimized) return nullptr;
    SDL_SetAlpha(optimized, SDL_SRCALPHA, 255);

    StyleTexture style;
    style.surface = optimized;
    style.w = optimized->w;
    style.h = optimized->h;
    style.bytes = static_cast<size_t>(std::max(0, style.w)) * static_cast<size_t>(std::max(0, style.h)) * 4U;
    style.last_used = ++cache_tick_;
    style_cache_bytes_ += style.bytes;
    style_cache_[key] = style;
    trim_style_cache();
    return &style_cache_[key];
}

Renderer::TextTexture *Renderer::get_text_texture(const std::string &text, const Color &color, bool large) {
    if (!screen_ || text.empty()) return nullptr;
    maybe_log_cache_stats();

    const std::string key = text_cache_key(text, color, large);
    std::map<std::string, TextTexture>::iterator it = text_cache_.find(key);
    if (it != text_cache_.end()) {
        ++text_cache_hits_;
        it->second.last_used = ++cache_tick_;
        return &it->second;
    }
    ++text_cache_misses_;

    FontFace *font = large ? font_large_ : font_small_;
    SDL_Surface *surface = build_text_surface(font, text, color);
    if (!surface) return nullptr;

    SDL_Surface *optimized = optimize_alpha_surface(surface);
    SDL_FreeSurface(surface);
    if (!optimized) return nullptr;
    SDL_SetAlpha(optimized, SDL_SRCALPHA, 255);

    TextTexture entry;
    entry.surface = optimized;
    entry.w = optimized->w;
    entry.h = optimized->h;
    entry.bytes = static_cast<size_t>(std::max(0, entry.w)) * static_cast<size_t>(std::max(0, entry.h)) * 4U;
    entry.last_used = ++cache_tick_;
    text_cache_bytes_ += entry.bytes;
    text_cache_[key] = entry;
    trim_text_cache();
    return &text_cache_[key];
}

void Renderer::draw_text(const std::string &text, int x, int y, const Color &color, bool large) {
    TextTexture *tex = get_text_texture(text, color, large);
    if (!tex || !tex->surface || !screen_) return;

    SDL_Rect dst;
    dst.x = x;
    dst.y = y;
    dst.w = tex->w;
    dst.h = tex->h;
    SDL_BlitSurface(tex->surface, nullptr, screen_, &dst);
}

void Renderer::draw_text_centered(const std::string &text, int center_x, int y, const Color &color, bool large) {
    const int w = text_width(text, large);
    draw_text(text, center_x - w / 2, y, color, large);
}

int Renderer::text_width(const std::string &text, bool large) {
    if (text.empty()) return 0;
    const std::string key = text_width_cache_key(text, large);
    std::map<std::string, TextWidthEntry>::iterator it = text_width_cache_.find(key);
    if (it != text_width_cache_.end()) {
        ++text_width_cache_hits_;
        it->second.last_used = ++cache_tick_;
        return it->second.width;
    }
    ++text_width_cache_misses_;

    FontFace *font = large ? font_large_ : font_small_;
    const int width = measure_text_width(font, text);

    TextWidthEntry entry;
    entry.width = width;
    entry.last_used = ++cache_tick_;
    text_width_cache_[key] = entry;
    trim_text_width_cache();
    return width;
}

int Renderer::line_height(bool large) {
    FontFace *font = large ? font_large_ : font_small_;
    return ::inconsole::line_height(font);
}

std::string Renderer::ellipsize_to_width(const std::string &text, int max_width, bool large) {
    if (max_width <= 0) return std::string();
    if (text_width(text, large) <= max_width) return text;

    const std::string dots = "...";
    const int dots_w = text_width(dots, large);
    if (dots_w > max_width) return std::string();

    std::string out = text;
    auto pop_last_utf8 = [](std::string *s) {
        if (!s || s->empty()) return;
        size_t i = s->size() - 1;
        while (i > 0 && ((*s)[i] & static_cast<char>(0xC0)) == static_cast<char>(0x80)) --i;
        s->erase(i);
    };
    while (!out.empty() && text_width(out + dots, large) > max_width) {
        pop_last_utf8(&out);
    }
    return out + dots;
}

std::vector<std::string> Renderer::wrap_text_to_width(const std::string &text, int max_width, size_t max_lines, bool large) {
    std::vector<std::string> out;
    if (text.empty() || max_width <= 0 || max_lines == 0) return out;

    std::istringstream iss(text);
    std::string word;
    std::string current;

    while (iss >> word) {
        const std::string candidate = current.empty() ? word : current + " " + word;
        if (text_width(candidate, large) <= max_width) {
            current = candidate;
            continue;
        }

        if (!current.empty()) {
            out.push_back(current);
            current.clear();
            if (out.size() >= max_lines) break;
        }

        if (text_width(word, large) <= max_width) {
            current = word;
        } else {
            out.push_back(ellipsize_to_width(word, max_width, large));
            if (out.size() >= max_lines) break;
        }
    }

    if (out.size() < max_lines && !current.empty()) out.push_back(current);

    if (out.size() > max_lines) out.resize(max_lines);
    if (out.size() == max_lines) {
        out.back() = ellipsize_to_width(out.back(), max_width, large);
    }
    return out;
}

Renderer::IconTexture *Renderer::get_icon64(const std::string &path) {
    maybe_log_cache_stats();
    std::map<std::string, IconTexture>::iterator it = icon_cache_.find(path);
    if (it != icon_cache_.end()) {
        ++icon_cache_hits_;
        it->second.last_used = ++cache_tick_;
        return &it->second;
    }
    ++icon_cache_misses_;

    SDL_Surface *dst = create_rgba_surface(64, 64);
    if (!dst) return nullptr;
    SDL_FillRect(dst, nullptr, SDL_MapRGBA(dst->format, 0, 0, 0, 0));

    std::vector<uint8_t> rgba;
    int src_w = 0;
    int src_h = 0;

    if (load_png_rgba(path, &rgba, &src_w, &src_h) && src_w > 0 && src_h > 0) {
        Uint32 rmask = 0;
        Uint32 gmask = 0;
        Uint32 bmask = 0;
        Uint32 amask = 0;
        rgba_masks(&rmask, &gmask, &bmask, &amask);
        SDL_Surface *src = SDL_CreateRGBSurfaceFrom(rgba.data(), src_w, src_h, 32, src_w * 4, rmask, gmask, bmask, amask);
        if (src) {
            SDL_Rect full;
            full.x = 0;
            full.y = 0;
            full.w = 64;
            full.h = 64;
            SDL_SoftStretch(src, nullptr, dst, &full);
            SDL_FreeSurface(src);
        }
    } else {
        SDL_FillRect(dst, nullptr, SDL_MapRGB(dst->format, 40, 40, 40));
        SDL_Rect c;
        c.x = 8;
        c.y = 8;
        c.w = 48;
        c.h = 48;
        SDL_FillRect(dst, &c, SDL_MapRGB(dst->format, 70, 70, 70));
    }

    IconTexture icon;
    SDL_SetAlpha(dst, SDL_SRCALPHA, 255);
    icon.surface = dst;
    icon.w = dst->w;
    icon.h = dst->h;
    icon.bytes = static_cast<size_t>(icon.w * icon.h * 4);
    icon.last_used = ++cache_tick_;
    icon_cache_bytes_ += icon.bytes;
    icon_cache_[path] = icon;
    trim_icon_cache();
    return &icon_cache_[path];
}

Renderer::IconTexture *Renderer::get_scaled_icon(const std::string &path, int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    if (w == 64 && h == 64) return get_icon64(path);
    maybe_log_cache_stats();

    const std::string key = scaled_icon_cache_key(path, w, h);
    std::map<std::string, IconTexture>::iterator it = icon_scaled_cache_.find(key);
    if (it != icon_scaled_cache_.end()) {
        ++icon_scaled_cache_hits_;
        it->second.last_used = ++cache_tick_;
        return &it->second;
    }
    ++icon_scaled_cache_misses_;

    IconTexture *src = get_icon64(path);
    if (!src || !src->surface) return nullptr;

    SDL_Surface *dst = create_rgba_surface(w, h);
    if (!dst) return nullptr;
    SDL_FillRect(dst, nullptr, SDL_MapRGBA(dst->format, 0, 0, 0, 0));
    SDL_Rect full;
    full.x = 0;
    full.y = 0;
    full.w = w;
    full.h = h;
    SDL_SoftStretch(src->surface, nullptr, dst, &full);

    IconTexture icon;
    SDL_SetAlpha(dst, SDL_SRCALPHA, 255);
    icon.surface = dst;
    icon.w = dst->w;
    icon.h = dst->h;
    icon.bytes = static_cast<size_t>(icon.w * icon.h * 4);
    icon.last_used = ++cache_tick_;
    icon_scaled_cache_bytes_ += icon.bytes;
    icon_scaled_cache_[key] = icon;
    trim_scaled_icon_cache();
    return &icon_scaled_cache_[key];
}

void Renderer::draw_background_cached(const std::string &theme_key, const Color &top, const Color &bottom, const Color &scanline) {
    if (!screen_) return;
    const int w = width();
    const int h = height();

    std::ostringstream key_builder;
    key_builder << "bg:" << theme_key << ":" << w << "x" << h << ":" << static_cast<int>(top.r) << "," << static_cast<int>(top.g) << ","
                << static_cast<int>(top.b) << ":" << static_cast<int>(bottom.r) << "," << static_cast<int>(bottom.g) << ","
                << static_cast<int>(bottom.b) << ":" << static_cast<int>(scanline.r) << "," << static_cast<int>(scanline.g) << ","
                << static_cast<int>(scanline.b);
    const std::string bg_key = key_builder.str();

    StyleTexture *background = get_style_texture(bg_key);
    if (!background) {
        ++style_cache_misses_;
        SDL_Surface *surface = create_rgba_surface(w, h);
        if (!surface) return;
        for (int y = 0; y < h; ++y) {
            const float t = h > 1 ? static_cast<float>(y) / static_cast<float>(h - 1) : 0.0f;
            Color line = color_mixf(top, bottom, t);
            if ((y % 3) == 0) line = scanline;
            SDL_Rect row;
            row.x = 0;
            row.y = y;
            row.w = w;
            row.h = 1;
            SDL_FillRect(surface, &row, SDL_MapRGBA(surface->format, line.r, line.g, line.b, 255));
        }
        background = cache_style_surface(bg_key, surface);
        SDL_FreeSurface(surface);
    }

    if (background && background->surface) {
        SDL_Rect dst;
        dst.x = 0;
        dst.y = 0;
        dst.w = w;
        dst.h = h;
        SDL_BlitSurface(background->surface, nullptr, screen_, &dst);
    }
}

void Renderer::draw_panel_cached(const std::string &style_key,
                                 int x,
                                 int y,
                                 int w,
                                 int h,
                                 const Color &top,
                                 const Color &bottom,
                                 const Color &border) {
    if (!screen_ || w <= 0 || h <= 0) return;

    std::ostringstream key_builder;
    key_builder << "panel:" << style_key << ":" << w << "x" << h << ":" << static_cast<int>(top.r) << "," << static_cast<int>(top.g) << ","
                << static_cast<int>(top.b) << ":" << static_cast<int>(bottom.r) << "," << static_cast<int>(bottom.g) << ","
                << static_cast<int>(bottom.b) << ":" << static_cast<int>(border.r) << "," << static_cast<int>(border.g) << ","
                << static_cast<int>(border.b);
    const std::string key = key_builder.str();

    StyleTexture *panel = get_style_texture(key);
    if (!panel) {
        ++style_cache_misses_;
        SDL_Surface *surface = create_rgba_surface(w, h);
        if (!surface) return;

        for (int py = 0; py < h; ++py) {
            const float t = h > 1 ? static_cast<float>(py) / static_cast<float>(h - 1) : 0.0f;
            Color line = color_mixf(top, bottom, t);
            SDL_Rect row;
            row.x = 0;
            row.y = py;
            row.w = w;
            row.h = 1;
            SDL_FillRect(surface, &row, SDL_MapRGBA(surface->format, line.r, line.g, line.b, 255));
        }

        SDL_Rect border_outer = {0, 0, static_cast<Uint16>(w), 1};
        SDL_FillRect(surface, &border_outer, SDL_MapRGBA(surface->format, border.r, border.g, border.b, 255));
        border_outer = SDL_Rect{0, static_cast<Sint16>(h - 1), static_cast<Uint16>(w), 1};
        SDL_FillRect(surface, &border_outer, SDL_MapRGBA(surface->format, border.r, border.g, border.b, 255));
        border_outer = SDL_Rect{0, 0, 1, static_cast<Uint16>(h)};
        SDL_FillRect(surface, &border_outer, SDL_MapRGBA(surface->format, border.r, border.g, border.b, 255));
        border_outer = SDL_Rect{static_cast<Sint16>(w - 1), 0, 1, static_cast<Uint16>(h)};
        SDL_FillRect(surface, &border_outer, SDL_MapRGBA(surface->format, border.r, border.g, border.b, 255));

        if (w > 4 && h > 4) {
            const Color inner = color_mixf(border, Color{255, 255, 255}, 0.15f);
            SDL_Rect r1 = {1, 1, static_cast<Uint16>(w - 2), 1};
            SDL_Rect r2 = {1, static_cast<Sint16>(h - 2), static_cast<Uint16>(w - 2), 1};
            SDL_Rect r3 = {1, 1, 1, static_cast<Uint16>(h - 2)};
            SDL_Rect r4 = {static_cast<Sint16>(w - 2), 1, 1, static_cast<Uint16>(h - 2)};
            const uint32_t inner_rgba = SDL_MapRGBA(surface->format, inner.r, inner.g, inner.b, 255);
            SDL_FillRect(surface, &r1, inner_rgba);
            SDL_FillRect(surface, &r2, inner_rgba);
            SDL_FillRect(surface, &r3, inner_rgba);
            SDL_FillRect(surface, &r4, inner_rgba);
        }

        panel = cache_style_surface(key, surface);
        SDL_FreeSurface(surface);
    }

    if (!panel || !panel->surface) return;
    SDL_Rect dst;
    dst.x = x;
    dst.y = y;
    dst.w = w;
    dst.h = h;
    SDL_BlitSurface(panel->surface, nullptr, screen_, &dst);
}

void Renderer::draw_icon64(const std::string &path, int x, int y) {
    IconTexture *icon = get_icon64(path);
    if (!icon || !icon->surface || !screen_) return;

    SDL_Rect dst;
    dst.x = x;
    dst.y = y;
    dst.w = icon->w;
    dst.h = icon->h;
    SDL_BlitSurface(icon->surface, nullptr, screen_, &dst);
}

void Renderer::draw_icon(const std::string &path, int x, int y, int w, int h) {
    if (!screen_ || w <= 0 || h <= 0) return;
    IconTexture *icon = get_scaled_icon(path, w, h);
    if (!icon || !icon->surface) return;

    SDL_Rect dst;
    dst.x = x;
    dst.y = y;
    dst.w = w;
    dst.h = h;
    SDL_BlitSurface(icon->surface, nullptr, screen_, &dst);
}

void Renderer::draw_fade(uint8_t alpha) {
    if (!screen_ || alpha == 0) return;

    if (!fade_surface_ || fade_surface_->w != width() || fade_surface_->h != height()) {
        if (fade_surface_) SDL_FreeSurface(fade_surface_);
        fade_surface_ = create_rgba_surface(width(), height());
        if (!fade_surface_) return;
        SDL_FillRect(fade_surface_, nullptr, SDL_MapRGBA(fade_surface_->format, 0, 0, 0, 255));
    }
    SDL_SetAlpha(fade_surface_, SDL_SRCALPHA, alpha);

    SDL_Rect dst;
    dst.x = 0;
    dst.y = 0;
    dst.w = width();
    dst.h = height();
    SDL_BlitSurface(fade_surface_, nullptr, screen_, &dst);
}

bool Renderer::has_vsync() const { return has_vsync_; }

}  // namespace inconsole
