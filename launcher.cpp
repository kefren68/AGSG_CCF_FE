#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#ifndef CROSS_PLATFORM
#include <dlfcn.h>
#endif
#include <vector>
#include <string>
#include <dirent.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <map>
#include <cctype>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef CROSS_PLATFORM
#include <linux/input.h>
#endif
#include <iostream>
#include <set>

#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

// --- STRUCTURES ---
struct MenuItem { std::string name; bool is_dir; };
struct SoundSample { Uint8* buffer = NULL; Uint32 length = 0; };

// --- GLOBAL CONFIG ---
std::map<std::string, std::vector<std::string>> system_configs;
std::set<std::string> hidden_dirs; // Cartelle da nascondere (lowercase)
#ifdef NATIVE_BASE_PATH
// Runtime: use directory of the executable
static std::string get_native_base_path(const char* argv0) {
    std::string p = argv0;
#ifdef CROSS_PLATFORM
    // Use SDL_GetBasePath() for cross-platform support
    char* sdl_base = SDL_GetBasePath();
    if (sdl_base) { p = sdl_base; SDL_free(sdl_base); return p; }
#else
    // Try /proc/self/exe first (Linux)
    char buf[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) p = std::string(buf, len);
#endif
    size_t sl = p.rfind('/');
#ifdef _WIN32
    size_t sl2 = p.rfind('\\'); if (sl2 != std::string::npos && sl2 > sl) sl = sl2;
#endif
    return (sl != std::string::npos ? p.substr(0, sl + 1) : "./");
}
std::string base_p = "./"; // will be set in main()
#else
std::string base_p = "/sdcard/bin/AGSG_CCF_FE/";
#endif
std::string img_p = base_p + "images/";
std::string current_theme = "default"; // tema attivo

// Impostazioni display (visibilità elementi HUD)
bool show_wifi        = true;
bool show_battery     = true;
bool show_system_name = true;
bool show_help_bar    = true;
bool show_system_logo = true;
bool music_enabled    = true;

// HDMI detection
bool hdmi_connected = false;

bool detect_hdmi() {
    std::ifstream f("/sys/class/drm/card0-HDMI-A-1/status");
    if (!f) return false;
    std::string s; std::getline(f, s);
    // Trim whitespace/CR
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s == "connected";
}

// Systems supported in HDMI mode (folder names, lowercase)
const std::set<std::string> hdmi_supported_systems = {
    "atari2600", "atari2600paddle", "atari5200", "atari7800", "atarilynx",
    "doom", "gameandwatch", "gameboy", "gameboyadvance", "gameboycolor",
    "intellivision", "mame", "neogeo", "neogeopocketcolor", "nes",
    "odyssey2", "pcengine", "pcenginecd", "playstation", "scummvm",
    "sega32x", "segacd", "segagamegear", "segagenesis", "segamastersystem",
    "snes", "vectrex", "wonderswancolor", "pico8"
};

// Ritorna il percorso base del tema attivo: base_p + "themes/<current_theme>/"
std::string theme_p() { return base_p + "themes/" + current_theme + "/"; }

void load_active_theme() {
    std::ifstream f(base_p + "theme_active.txt");
    if (f) {
        std::string line;
        if (std::getline(f, line) && !line.empty()) current_theme = line;
    }
}

void save_active_theme() {
    std::ofstream f(base_p + "theme_active.txt");
    if (f) f << current_theme;
}

// ---- THEME CONFIG ----
struct ThemeConfig {
    // [carousel]
    int   carousel_device_base_w  = 320;
    int   carousel_device_base_h  = 0;   // 0 = no height limit
    int   carousel_y_center       = 300;
    int   carousel_prev_x         = 145;
    int   carousel_next_x         = 885;
    int   carousel_cur_x          = 515;
    float carousel_side_scale     = 0.6f;
    float carousel_side_alpha     = 0.4f;
    float carousel_slide_speed    = 3600.0f;
    bool  carousel_vertical       = false;  // vertical scroll mode (up/down instead of left/right)
    int   carousel_prev_y         = 150;    // y center for previous item in vertical mode
    int   carousel_next_y         = 450;    // y center for next item in vertical mode
    int   carousel_device_shadow_alpha = 0; // shadow under device images (0 = off)
    int   carousel_sys_name_x     = 10;
    int   carousel_sys_name_y     = 10;
    int   carousel_games_count_y  = 400;
    int   carousel_games_count_x  = -1;  // -1 = auto-center
    // controller image
    bool  carousel_show_controller   = true;
    int   carousel_ctrl_x            = 10;   // -1 = auto-center
    int   carousel_ctrl_y            = 460;
    int   carousel_ctrl_max_w        = 160;
    int   carousel_ctrl_max_h        = 80;
    float carousel_ctrl_alpha        = 0.85f;
    int   carousel_ctrl_shadow_alpha  = 150;
    // system description
    bool  carousel_show_desc         = true;
    int   carousel_desc_x            = 10;
    int   carousel_desc_y            = 480;
    int   carousel_desc_max_w        = 500;
    int   carousel_desc_max_h        = 80;
    int   carousel_desc_line_h       = 18;
    SDL_Color carousel_desc_color    = {200, 200, 200, 255};
    // [list]
    int   list_rows               = 10;
    int   list_row_height         = 48;
    int   list_y_start            = 60;
    int   list_text_x             = 75;
    int   list_text_max_w         = 450;
    int   list_highlight_alpha    = 120;
    int   list_highlight_w        = 455;
    int   list_highlight_h        = 38;
    // [scrollbar]
    int   scrollbar_x             = 10;
    int   scrollbar_w             = 8;
    // [boxart]
    int   boxart_area_x           = 640;
    int   boxart_area_y           = 65;
    int   boxart_max_w            = 320;
    int   boxart_max_h            = 240;
    int   boxart_border_padding   = 5;
    // [game_info]
    int   game_name_x             = 645;
    int   game_name_y             = 315;
    int   desc_x                  = 645;
    int   desc_y                  = 340;
    int   desc_max_w              = 310;
    int   desc_area_h             = 180;
    int   desc_line_h             = 18;
    float desc_scroll_speed       = 0.8f;
    // [logo]
    int   logo_x                  = 20;
    int   logo_y                  = 5;
    int   logo_max_w              = 300;
    int   logo_max_h              = 40;
    int   logo_shadow_alpha       = 150;
    int   counter_x            = -1;  // -1 = auto-center horizontally
    int   counter_y               = 15;
    // [wifi_icon]
    int   wifi_x                  = 910;
    int   wifi_y                  = 10;
    float wifi_scale              = 0.6f;
    int   wifi_src_w              = 68;
    int   wifi_src_h              = 50;
    // [battery_icon]
    int   battery_x               = 960;
    int   battery_y               = 13;
    float battery_scale           = 0.6f;
    int   battery_src_w           = 83;
    int   battery_src_h           = 45;
    // [helpbar] shared defaults, overridden by [helpbar_game] / [helpbar_menu]
    float helpbar_scale           = 0.8f;
    int   helpbar_bottom_margin   = 5;
    int   helpbar_x               = -1;  // -1 = centered
    // [helpbar_game]
    float helpbar_game_scale      = -1.0f; // -1 = use helpbar_scale
    int   helpbar_game_bottom_margin = -1; // -1 = use helpbar_bottom_margin
    int   helpbar_game_x          = -2;  // -2 = use helpbar_x
    // [helpbar_menu]
    float helpbar_menu_scale      = -1.0f;
    int   helpbar_menu_bottom_margin = -1;
    int   helpbar_menu_x          = -2;
    // [fonts]
    int   font_small              = 16;
    int   font_medium             = 20;
    int   font_large              = 24;
    // [colors] R,G,B,A
    SDL_Color color_text          = {255, 255, 255, 255};
    SDL_Color color_favorite      = {255, 140,   0, 255};
    SDL_Color color_desc          = {200, 200, 200, 255};
    SDL_Color color_highlight     = {255, 255, 255, 120};
    SDL_Color color_scrollbar_bg  = { 40,  40,  40, 255};
    SDL_Color color_scrollbar_thumb = {255, 255, 255, 255};
    SDL_Color color_boxart_border = {255, 255, 255, 255};
    // [misc]
    bool  shadows = true;
    int   shadow_alpha = 150;
    int   shadow_offset_x = 2;
    int   shadow_offset_y = 2;
    SDL_Color shadow_color = {0, 0, 0, 150};
    int   fast_scroll_interval = 80; // ms between steps when L1/R1 held
    // [video]
    int   video_delay_ms = 2000;     // ms to show boxart before starting video
    int   video_fade_ms  = 500;      // ms of crossfade from boxart to video
    // [menu] - theme selector popup
    SDL_Color menu_overlay          = { 10,   8,  35, 190};
    int       menu_box_w            = 500;
    int       menu_box_h            = 380;
    SDL_Color menu_box_bg           = { 30,  30,  50, 230};
    SDL_Color menu_box_border       = {100, 120, 200, 255};
    int       menu_tab_h            = 36;
    SDL_Color menu_tab_active_bg    = { 80, 100, 180, 255};
    SDL_Color menu_tab_inactive_bg  = { 40,  40,  70, 255};
    SDL_Color menu_tab_border       = {100, 120, 200, 200};
    SDL_Color menu_tab_label_active = {255, 220,  80, 255};
    SDL_Color menu_tab_label_normal = {160, 160, 180, 255};
    SDL_Color menu_preview_border   = {100, 120, 200, 180};
    SDL_Color menu_preview_bg       = { 50,  50,  70, 255};
    int       menu_preview_h        = 158;
    int       menu_list_row_h       = 34;
    SDL_Color menu_highlight        = { 80, 100, 180, 200};
    SDL_Color menu_item_selected    = {255, 220,  80, 255};
    SDL_Color menu_item_normal      = {200, 200, 220, 255};
    int       menu_disp_row_h       = 56;
    SDL_Color menu_badge_on         = { 80, 220,  80, 255};
    SDL_Color menu_badge_off        = {220,  80,  80, 255};
    SDL_Color menu_hint             = {180, 180, 180, 255};
};

ThemeConfig theme_cfg; // global theme config, reloaded on theme change

static SDL_Color parse_color(const std::string& s) {
    SDL_Color c = {255, 255, 255, 255};
    int r = 255, g = 255, b = 255, a = 255;
    if (sscanf(s.c_str(), "%d,%d,%d,%d", &r, &g, &b, &a) >= 3) {
        c.r = (Uint8)r; c.g = (Uint8)g; c.b = (Uint8)b; c.a = (Uint8)a;
    }
    return c;
}

static std::string trim_ini(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

ThemeConfig load_theme_config(const std::string& path) {
    ThemeConfig cfg;
    std::ifstream f(path);
    if (!f) return cfg;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim_ini(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos) section = trim_ini(line.substr(1, end - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_ini(line.substr(0, eq));
        std::string val = trim_ini(line.substr(eq + 1));
        // Strip inline comments (; ...)
        { size_t sc = val.find(';'); if (sc != std::string::npos) val = trim_ini(val.substr(0, sc)); }
        if (val.empty()) continue;
        try {
            if (section == "carousel") {
                if      (key == "device_base_w")   cfg.carousel_device_base_w = std::stoi(val);
                else if (key == "device_base_h")   cfg.carousel_device_base_h = std::stoi(val);
                else if (key == "y_center")        cfg.carousel_y_center = std::stoi(val);
                else if (key == "prev_x")          cfg.carousel_prev_x = std::stoi(val);
                else if (key == "next_x")          cfg.carousel_next_x = std::stoi(val);
                else if (key == "cur_x")           cfg.carousel_cur_x = std::stoi(val);
                else if (key == "side_scale")      cfg.carousel_side_scale = std::stof(val);
                else if (key == "side_alpha")      cfg.carousel_side_alpha = std::stof(val);
                else if (key == "slide_speed")     cfg.carousel_slide_speed = std::stof(val);
                else if (key == "vertical")        cfg.carousel_vertical = (val != "0" && val != "false");
                else if (key == "prev_y")          cfg.carousel_prev_y = std::stoi(val);
                else if (key == "next_y")          cfg.carousel_next_y = std::stoi(val);
                else if (key == "sys_name_x")      cfg.carousel_sys_name_x = std::stoi(val);
                else if (key == "sys_name_y")      cfg.carousel_sys_name_y = std::stoi(val);
                else if (key == "games_count_y")   cfg.carousel_games_count_y = std::stoi(val);
                else if (key == "games_count_x")   cfg.carousel_games_count_x = std::stoi(val);
                else if (key == "show_controller") cfg.carousel_show_controller = (val != "0" && val != "false");
                else if (key == "ctrl_x")          cfg.carousel_ctrl_x = std::stoi(val);
                else if (key == "ctrl_y")          cfg.carousel_ctrl_y = std::stoi(val);
                else if (key == "ctrl_max_w")      cfg.carousel_ctrl_max_w = std::stoi(val);
                else if (key == "ctrl_max_h")      cfg.carousel_ctrl_max_h = std::stoi(val);
                else if (key == "ctrl_alpha")      cfg.carousel_ctrl_alpha = std::stof(val);
                else if (key == "ctrl_shadow_alpha") cfg.carousel_ctrl_shadow_alpha = std::stoi(val);
                else if (key == "device_shadow_alpha") cfg.carousel_device_shadow_alpha = std::stoi(val);
                else if (key == "show_desc")       cfg.carousel_show_desc = (val != "0" && val != "false");
                else if (key == "desc_x")          cfg.carousel_desc_x = std::stoi(val);
                else if (key == "desc_y")          cfg.carousel_desc_y = std::stoi(val);
                else if (key == "desc_max_w")      cfg.carousel_desc_max_w = std::stoi(val);
                else if (key == "desc_max_h")      cfg.carousel_desc_max_h = std::stoi(val);
                else if (key == "desc_line_h")     cfg.carousel_desc_line_h = std::stoi(val);
                else if (key == "desc_color")      cfg.carousel_desc_color = parse_color(val);
            } else if (section == "list") {
                if      (key == "rows")            cfg.list_rows = std::stoi(val);
                else if (key == "row_height")      cfg.list_row_height = std::stoi(val);
                else if (key == "y_start")         cfg.list_y_start = std::stoi(val);
                else if (key == "text_x")          cfg.list_text_x = std::stoi(val);
                else if (key == "text_max_w")      cfg.list_text_max_w = std::stoi(val);
                else if (key == "highlight_alpha") cfg.list_highlight_alpha = std::stoi(val);
                else if (key == "highlight_w")     cfg.list_highlight_w = std::stoi(val);
                else if (key == "highlight_h")     cfg.list_highlight_h = std::stoi(val);
            } else if (section == "scrollbar") {
                if      (key == "x") cfg.scrollbar_x = std::stoi(val);
                else if (key == "w") cfg.scrollbar_w = std::stoi(val);
            } else if (section == "boxart") {
                if      (key == "area_x")          cfg.boxart_area_x = std::stoi(val);
                else if (key == "area_y")          cfg.boxart_area_y = std::stoi(val);
                else if (key == "max_w")           cfg.boxart_max_w = std::stoi(val);
                else if (key == "max_h")           cfg.boxart_max_h = std::stoi(val);
                else if (key == "border_padding")  cfg.boxart_border_padding = std::stoi(val);
            } else if (section == "game_info") {
                if      (key == "name_x")          cfg.game_name_x = std::stoi(val);
                else if (key == "name_y")          cfg.game_name_y = std::stoi(val);
                else if (key == "desc_x")          cfg.desc_x = std::stoi(val);
                else if (key == "desc_y")          cfg.desc_y = std::stoi(val);
                else if (key == "desc_max_w")      cfg.desc_max_w = std::stoi(val);
                else if (key == "desc_area_h")     cfg.desc_area_h = std::stoi(val);
                else if (key == "desc_line_h")     cfg.desc_line_h = std::stoi(val);
                else if (key == "desc_scroll_speed") cfg.desc_scroll_speed = std::stof(val);
            } else if (section == "logo") {
                if      (key == "x")               cfg.logo_x = std::stoi(val);
                else if (key == "y")               cfg.logo_y = std::stoi(val);
                else if (key == "max_w")           cfg.logo_max_w = std::stoi(val);
                else if (key == "max_h")           cfg.logo_max_h = std::stoi(val);
                else if (key == "shadow_alpha")    cfg.logo_shadow_alpha = std::stoi(val);
                else if (key == "counter_x")        cfg.counter_x = std::stoi(val);
                else if (key == "counter_y")       cfg.counter_y = std::stoi(val);
            } else if (section == "wifi_icon") {
                if      (key == "x")     cfg.wifi_x = std::stoi(val);
                else if (key == "y")     cfg.wifi_y = std::stoi(val);
                else if (key == "scale") cfg.wifi_scale = std::stof(val);
                else if (key == "src_w") cfg.wifi_src_w = std::stoi(val);
                else if (key == "src_h") cfg.wifi_src_h = std::stoi(val);
            } else if (section == "battery_icon") {
                if      (key == "x")     cfg.battery_x = std::stoi(val);
                else if (key == "y")     cfg.battery_y = std::stoi(val);
                else if (key == "scale") cfg.battery_scale = std::stof(val);
                else if (key == "src_w") cfg.battery_src_w = std::stoi(val);
                else if (key == "src_h") cfg.battery_src_h = std::stoi(val);
            } else if (section == "helpbar") {
                if      (key == "scale")          cfg.helpbar_scale = std::stof(val);
                else if (key == "bottom_margin")  cfg.helpbar_bottom_margin = std::stoi(val);
                else if (key == "x")              cfg.helpbar_x = std::stoi(val);
            } else if (section == "helpbar_game") {
                if      (key == "scale")          cfg.helpbar_game_scale = std::stof(val);
                else if (key == "bottom_margin")  cfg.helpbar_game_bottom_margin = std::stoi(val);
                else if (key == "x")              cfg.helpbar_game_x = std::stoi(val);
            } else if (section == "helpbar_menu") {
                if      (key == "scale")          cfg.helpbar_menu_scale = std::stof(val);
                else if (key == "bottom_margin")  cfg.helpbar_menu_bottom_margin = std::stoi(val);
                else if (key == "x")              cfg.helpbar_menu_x = std::stoi(val);
            } else if (section == "fonts") {
                if      (key == "small")  cfg.font_small = std::stoi(val);
                else if (key == "medium") cfg.font_medium = std::stoi(val);
                else if (key == "large")  cfg.font_large = std::stoi(val);
            } else if (section == "colors") {
                if      (key == "text")             cfg.color_text = parse_color(val);
                else if (key == "favorite")         cfg.color_favorite = parse_color(val);
                else if (key == "desc")             cfg.color_desc = parse_color(val);
                else if (key == "highlight")        cfg.color_highlight = parse_color(val);
                else if (key == "scrollbar_bg")     cfg.color_scrollbar_bg = parse_color(val);
                else if (key == "scrollbar_thumb")  cfg.color_scrollbar_thumb = parse_color(val);
                else if (key == "boxart_border")    cfg.color_boxart_border = parse_color(val);
            } else if (section == "misc") {
                if      (key == "shadows")             cfg.shadows = (val != "0" && val != "false");
                else if (key == "shadow_alpha")        cfg.shadow_alpha = std::stoi(val);
                else if (key == "shadow_offset_x")     cfg.shadow_offset_x = std::stoi(val);
                else if (key == "shadow_offset_y")     cfg.shadow_offset_y = std::stoi(val);
                else if (key == "shadow_color")        cfg.shadow_color = parse_color(val);
                else if (key == "fast_scroll_interval") cfg.fast_scroll_interval = std::max(20, std::stoi(val));
            } else if (section == "video") {
                if      (key == "delay_ms")  cfg.video_delay_ms = std::stoi(val);
                else if (key == "fade_ms")   cfg.video_fade_ms  = std::stoi(val);
            } else if (section == "menu") {
                if      (key == "overlay")           cfg.menu_overlay = parse_color(val);
                else if (key == "box_w")             cfg.menu_box_w = std::stoi(val);
                else if (key == "box_h")             cfg.menu_box_h = std::stoi(val);
                else if (key == "box_bg")            cfg.menu_box_bg = parse_color(val);
                else if (key == "box_border")        cfg.menu_box_border = parse_color(val);
                else if (key == "tab_h")             cfg.menu_tab_h = std::stoi(val);
                else if (key == "tab_active_bg")     cfg.menu_tab_active_bg = parse_color(val);
                else if (key == "tab_inactive_bg")   cfg.menu_tab_inactive_bg = parse_color(val);
                else if (key == "tab_border")        cfg.menu_tab_border = parse_color(val);
                else if (key == "tab_label_active")  cfg.menu_tab_label_active = parse_color(val);
                else if (key == "tab_label_normal")  cfg.menu_tab_label_normal = parse_color(val);
                else if (key == "preview_border")    cfg.menu_preview_border = parse_color(val);
                else if (key == "preview_bg")        cfg.menu_preview_bg = parse_color(val);
                else if (key == "preview_h")         cfg.menu_preview_h = std::stoi(val);
                else if (key == "list_row_h")        cfg.menu_list_row_h = std::stoi(val);
                else if (key == "highlight")         cfg.menu_highlight = parse_color(val);
                else if (key == "item_selected")     cfg.menu_item_selected = parse_color(val);
                else if (key == "item_normal")       cfg.menu_item_normal = parse_color(val);
                else if (key == "disp_row_h")        cfg.menu_disp_row_h = std::stoi(val);
                else if (key == "badge_on")          cfg.menu_badge_on = parse_color(val);
                else if (key == "badge_off")         cfg.menu_badge_off = parse_color(val);
                else if (key == "hint")              cfg.menu_hint = parse_color(val);
            }
        } catch (...) {} // Ignore malformed values
    }
    return cfg;
}

void load_display_settings() {
    std::ifstream f(base_p + "display_settings.txt");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("show_wifi=", 0) == 0)             show_wifi        = line.substr(10) != "0";
        else if (line.rfind("show_battery=", 0) == 0)      show_battery     = line.substr(13) != "0";
        else if (line.rfind("show_system_name=", 0) == 0)  show_system_name = line.substr(17) != "0";
        else if (line.rfind("show_help_bar=", 0) == 0)     show_help_bar    = line.substr(14) != "0";
        else if (line.rfind("show_system_logo=", 0) == 0)  show_system_logo = line.substr(16) != "0";
        else if (line.rfind("music_enabled=", 0) == 0)     music_enabled    = line.substr(14) != "0";
    }
}

void save_display_settings() {
    std::ofstream f(base_p + "display_settings.txt");
    if (!f) return;
    f << "show_wifi="        << (show_wifi        ? 1 : 0) << "\n";
    f << "show_battery="     << (show_battery     ? 1 : 0) << "\n";
    f << "show_system_name=" << (show_system_name ? 1 : 0) << "\n";
    f << "show_help_bar="    << (show_help_bar    ? 1 : 0) << "\n";
    f << "show_system_logo=" << (show_system_logo ? 1 : 0) << "\n";
    f << "music_enabled="    << (music_enabled    ? 1 : 0) << "\n";
}

// --- HELPERS ---

// Cerca una sottocartella per nome (case-insensitive). Ritorna il path reale o "" se non trovata.
std::string find_subdir_ignore_case(const std::string& parent, const std::string& name) {
    DIR* d = opendir(parent.c_str());
    if (!d) return "";
    std::string low_name = name;
    std::transform(low_name.begin(), low_name.end(), low_name.begin(), ::tolower);
    struct dirent* en;
    while ((en = readdir(d)) != NULL) {
        if (en->d_name[0] == '.' && (en->d_name[1] == '\0' || (en->d_name[1] == '.' && en->d_name[2] == '\0'))) continue;
        std::string low_en = en->d_name;
        std::transform(low_en.begin(), low_en.end(), low_en.begin(), ::tolower);
        if (low_en == low_name) {
            std::string result = parent + "/" + en->d_name;
            closedir(d);
            return result;
        }
    }
    closedir(d);
    return "";
}

SDL_Texture* load_texture(SDL_Renderer* renderer, const std::string& path) {
    if (path.empty() || access(path.c_str(), F_OK) == -1) return NULL;
    SDL_Surface* surface = IMG_Load(path.c_str());
    if (!surface) return NULL;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}

// Try loading a texture with .png extension first, then .jpg fallback
SDL_Texture* load_texture_png_jpg(SDL_Renderer* renderer, const std::string& path_no_ext) {
    SDL_Texture* t = load_texture(renderer, path_no_ext + ".png");
    if (!t) t = load_texture(renderer, path_no_ext + ".jpg");
    return t;
}

// Load an image from the active theme's images/ folder, fallback to default theme
SDL_Texture* load_theme_image(SDL_Renderer* renderer, const std::string& filename) {
    SDL_Texture* t = load_texture(renderer, img_p + filename);
    if (!t && current_theme != "default")
        t = load_texture(renderer, base_p + "themes/default/images/" + filename);
    return t;
}

std::string find_file_ignore_case(const std::string& folder, const std::string& filename) {
    DIR* dir = opendir(folder.c_str());
    if (!dir) return "";
    std::string target = filename;
    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string current = entry->d_name;
        if (current == "." || current == "..") continue;
        std::string lower_current = current;
        std::transform(lower_current.begin(), lower_current.end(), lower_current.begin(), ::tolower);
        size_t last_dot = lower_current.find_last_of('.');
        std::string name_no_ext = (last_dot == std::string::npos) ? lower_current : lower_current.substr(0, last_dot);
        if (lower_current == target || name_no_ext == target) {
            std::string res = folder + "/" + current;
            closedir(dir);
            return res;
        }
    }
    closedir(dir);
    return "";
}

std::string find_font_path() {
    const std::string font_folder = theme_p() + "fonts";
    std::vector<std::string> candidates = {"font.ttf", "font.otf"};
    for (const auto& candidate : candidates) {
        std::string path = find_file_ignore_case(font_folder, candidate);
        if (!path.empty()) return path;
    }
    DIR* dir = opendir(font_folder.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            std::string current = entry->d_name;
            if (current == "." || current == "..") continue;
            std::string lower_current = current;
            std::transform(lower_current.begin(), lower_current.end(), lower_current.begin(), ::tolower);
            if (lower_current.size() > 4 && (lower_current.substr(lower_current.size() - 4) == ".ttf" || lower_current.substr(lower_current.size() - 4) == ".otf")) {
                std::string res = font_folder + "/" + current;
                closedir(dir);
                return res;
            }
        }
        closedir(dir);
    }
    return "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
}

// --- TEXT RENDERING (TTF + bitmap fallback) ---
std::map<int, TTF_Font*> font_cache;
SDL_Texture* font_tex = nullptr;
bool ttf_available = false;

#ifdef CROSS_PLATFORM
// On non-Linux: link SDL2_ttf directly, no dlopen needed
bool ttf_init()  { return TTF_Init() == 0; }
void ttf_quit()  { TTF_Quit(); }
TTF_Font* ttf_open_font(const std::string& path, int size) { return TTF_OpenFont(path.c_str(), size); }
void ttf_close_font(TTF_Font* font) { if (font) TTF_CloseFont(font); }
SDL_Surface* ttf_render_text_blended(TTF_Font* font, const std::string& text, SDL_Color color) {
    if (!font || text.empty()) return nullptr;
    return TTF_RenderUTF8_Blended(font, text.c_str(), color);
}
#else
using TTF_Init_fn = int (*)();
using TTF_Quit_fn = void (*)();
using TTF_OpenFont_fn = TTF_Font* (*)(const char*, int);
using TTF_CloseFont_fn = void (*)(TTF_Font*);
using TTF_RenderText_Blended_fn = SDL_Surface* (*)(TTF_Font*, const char*, SDL_Color);
using TTF_SizeUTF8_fn = int (*)(TTF_Font*, const char*, int*, int*);

static void* ttf_lib = nullptr;
static TTF_Init_fn pTTF_Init = nullptr;
static TTF_Quit_fn pTTF_Quit = nullptr;
static TTF_OpenFont_fn pTTF_OpenFont = nullptr;
static TTF_CloseFont_fn pTTF_CloseFont = nullptr;
static TTF_RenderText_Blended_fn pTTF_RenderText_Blended = nullptr;
static TTF_SizeUTF8_fn pTTF_SizeUTF8 = nullptr;

bool load_ttf_library() {
    const char* libs[] = {"libSDL2_ttf-2.0.so.0", "libSDL2_ttf.so", "SDL2_ttf"};
    const std::string local_lib_folder = base_p + "libs/";
    for (const char* lib : libs) {
        std::string local_path = local_lib_folder + lib;
        ttf_lib = dlopen(local_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (ttf_lib) {
            std::cerr << "SDL2_ttf loaded from local libs: " << local_path << "\n";
            break;
        }
    }
    if (!ttf_lib) {
        for (const char* lib : libs) {
            ttf_lib = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
            if (ttf_lib) {
                std::cerr << "SDL2_ttf loaded from system library: " << lib << "\n";
                break;
            }
        }
    }
    if (!ttf_lib) return false;
    pTTF_Init = (TTF_Init_fn)dlsym(ttf_lib, "TTF_Init");
    pTTF_Quit = (TTF_Quit_fn)dlsym(ttf_lib, "TTF_Quit");
    pTTF_OpenFont = (TTF_OpenFont_fn)dlsym(ttf_lib, "TTF_OpenFont");
    pTTF_CloseFont = (TTF_CloseFont_fn)dlsym(ttf_lib, "TTF_CloseFont");
    pTTF_RenderText_Blended = (TTF_RenderText_Blended_fn)dlsym(ttf_lib, "TTF_RenderUTF8_Blended");
    pTTF_SizeUTF8 = (TTF_SizeUTF8_fn)dlsym(ttf_lib, "TTF_SizeUTF8");
    if (!pTTF_Init || !pTTF_Quit || !pTTF_OpenFont || !pTTF_CloseFont || !pTTF_RenderText_Blended) {
        dlclose(ttf_lib);
        ttf_lib = nullptr;
        return false;
    }
    return true;
}

bool ttf_init() {
    if (!load_ttf_library()) return false;
    return pTTF_Init() == 0;
}

void ttf_quit() {
    if (!ttf_available || !pTTF_Quit) return;
    pTTF_Quit();
    if (ttf_lib) {
        dlclose(ttf_lib);
        ttf_lib = nullptr;
    }
}

TTF_Font* ttf_open_font(const std::string& path, int size) {
    if (!ttf_available || !pTTF_OpenFont) return nullptr;
    return pTTF_OpenFont(path.c_str(), size);
}

void ttf_close_font(TTF_Font* font) {
    if (!ttf_available || !pTTF_CloseFont || !font) return;
    pTTF_CloseFont(font);
}

SDL_Surface* ttf_render_text_blended(TTF_Font* font, const std::string& text, SDL_Color color) {
    if (!ttf_available || !pTTF_RenderText_Blended || !font || text.empty()) return nullptr;
    return pTTF_RenderText_Blended(font, text.c_str(), color);
}
#endif // CROSS_PLATFORM

void draw_text_bitmap(SDL_Renderer* renderer, SDL_Texture* font_tex, const std::string& text, int x, int y, int size, SDL_Color color, bool shadow = true) {
    if (!font_tex || text.empty()) return;
    SDL_SetTextureBlendMode(font_tex, SDL_BLENDMODE_BLEND);
    int char_step = size - (size / 8);
    auto draw_pass = [&](int ox, int oy, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
        SDL_SetTextureColorMod(font_tex, r, g, b);
        SDL_SetTextureAlphaMod(font_tex, a);
        for (size_t i = 0; i < text.length(); i++) {
            unsigned char ch = (unsigned char)text[i];
            SDL_Rect src = { (ch % 16) * 16, (ch / 16) * 16, 16, 16 };
            SDL_Rect dst = { x + ox + ((int)i * char_step), y + oy, size, size };
            SDL_RenderCopy(renderer, font_tex, &src, &dst);
        }
    };
    if (shadow && theme_cfg.shadows) draw_pass(theme_cfg.shadow_offset_x, theme_cfg.shadow_offset_y, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b, theme_cfg.shadow_color.a);
    draw_pass(0, 0, color.r, color.g, color.b, color.a);
}

void draw_text(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, int size, SDL_Color color, bool shadow = true) {
    if (ttf_available && font) {
        SDL_Surface* surface = ttf_render_text_blended(font, text, color);
        if (surface) {
            SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_FreeSurface(surface);
            if (texture) {
                int text_w, text_h;
                SDL_QueryTexture(texture, NULL, NULL, &text_w, &text_h);
                if (shadow && theme_cfg.shadows) {
                    SDL_SetTextureColorMod(texture, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                    SDL_SetTextureAlphaMod(texture, theme_cfg.shadow_color.a);
                    SDL_Rect shadow_rect = { x + theme_cfg.shadow_offset_x, y + theme_cfg.shadow_offset_y, text_w, text_h };
                    SDL_RenderCopy(renderer, texture, NULL, &shadow_rect);
                    SDL_SetTextureColorMod(texture, 255, 255, 255);
                    SDL_SetTextureAlphaMod(texture, 255);
                }
                SDL_Rect text_rect = { x, y, text_w, text_h };
                SDL_RenderCopy(renderer, texture, NULL, &text_rect);
                SDL_DestroyTexture(texture);
                return;
            }
        }
    }
    draw_text_bitmap(renderer, font_tex, text, x, y, size, color, shadow);
}

// Cerchio pieno (midpoint)
void draw_filled_circle(SDL_Renderer* renderer, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt((double)(r * r - dy * dy));
        SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// Disegna tasto simulato: cerchio bianco con lettera/icona nera centrata
// Ritorna la larghezza occupata (diametro + spacing)
int draw_button(SDL_Renderer* renderer, TTF_Font* font, const std::string& label, int x, int cy, int radius, SDL_Texture* icon = nullptr) {
    // Button shadow circle
    if (theme_cfg.shadows) {
        SDL_SetRenderDrawColor(renderer, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b, theme_cfg.shadow_color.a);
        draw_filled_circle(renderer, x + radius + theme_cfg.shadow_offset_x, cy + theme_cfg.shadow_offset_y, radius);
    }
    // Cerchio bianco
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_filled_circle(renderer, x + radius, cy, radius);
    // Icona o lettera nera centrata (dimensione ridotta)
    if (icon) {
        int iw = radius * 2 - 8, ih = iw;
        SDL_Rect ir = {x + radius - iw/2, cy - ih/2, iw, ih};
        SDL_RenderCopy(renderer, icon, NULL, &ir);
    } else if (ttf_available) {
        // Usa font più piccolo per le lettere nei cerchi
        TTF_Font* btn_font = nullptr;
        for (auto& fc : font_cache) { if (fc.first <= 16 && fc.second) { btn_font = fc.second; break; } }
        if (!btn_font) btn_font = font;
        SDL_Color black = {0, 0, 0, 255};
        SDL_Surface* surf = ttf_render_text_blended(btn_font, label, black);
        if (surf) {
            SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
            if (tex) {
                int tw = surf->w, th = surf->h;
                int max_w = radius * 2 - 4;
                if (tw > max_w) {
                    float s = (float)max_w / tw;
                    tw = max_w; th = (int)(th * s);
                }
                SDL_Rect r = {x + radius - tw/2, cy - th/2, tw, th};
                SDL_RenderCopy(renderer, tex, NULL, &r);
                SDL_DestroyTexture(tex);
            }
            SDL_FreeSurface(surf);
        }
    }
    return radius * 2 + 6;
}

// Disegna tasto rettangolare (es. HOME) con ombra, ritorna larghezza occupata
int draw_button_rect(SDL_Renderer* renderer, TTF_Font* font, const std::string& label, int x, int cy, int height) {
    int pad = 6;
    int tw = 0, th = 0;
    SDL_Surface* surf = nullptr;
    if (ttf_available && font) {
        SDL_Color black = {0, 0, 0, 255};
        surf = ttf_render_text_blended(font, label, black);
        if (surf) { tw = surf->w; th = surf->h; }
    }
    int rw = tw + pad * 2, rh = height;
    int ry = cy - rh / 2;
    // Button shadow rectangle
    if (theme_cfg.shadows) {
        SDL_SetRenderDrawColor(renderer, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b, theme_cfg.shadow_color.a);
        SDL_Rect sh = {x + theme_cfg.shadow_offset_x, ry + theme_cfg.shadow_offset_y, rw, rh};
        SDL_RenderFillRect(renderer, &sh);
    }
    // Rettangolo bianco
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_Rect rc = {x, ry, rw, rh};
    SDL_RenderFillRect(renderer, &rc);
    // Testo nero centrato
    if (surf) {
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
        if (tex) {
            SDL_Rect tr = {x + pad, cy - th / 2, tw, th};
            SDL_RenderCopy(renderer, tex, NULL, &tr);
            SDL_DestroyTexture(tex);
        }
        SDL_FreeSurface(surf);
    }
    return rw + 6;
}

// Disegna testo help con ombra, ritorna larghezza
int draw_help_label(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int cy) {
    if (!ttf_available || !font) return 0;
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface* surf = ttf_render_text_blended(font, text, white);
    if (!surf) return 0;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    int tw = surf->w, th = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return 0;
    int ty = cy - th / 2;
    // Shadow
    if (theme_cfg.shadows) {
        SDL_SetTextureColorMod(tex, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b); SDL_SetTextureAlphaMod(tex, theme_cfg.shadow_color.a);
        SDL_Rect sr = {x + theme_cfg.shadow_offset_x, ty + theme_cfg.shadow_offset_y, tw, th}; SDL_RenderCopy(renderer, tex, NULL, &sr);
    }
    // Text
    SDL_SetTextureColorMod(tex, 255, 255, 255); SDL_SetTextureAlphaMod(tex, 255);
    SDL_Rect tr = {x, ty, tw, th}; SDL_RenderCopy(renderer, tex, NULL, &tr);
    SDL_DestroyTexture(tex);
    return tw + 4;
}

void draw_text_scissored(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, int size, int max_w, int offset, SDL_Color color) {
    SDL_Rect clip = { x, y - 5, max_w, size + 15 };
    SDL_RenderSetClipRect(renderer, &clip);
    draw_text(renderer, font, text, x - offset, y, size, color, true);
    SDL_RenderSetClipRect(renderer, NULL);
}

// --- THEME SELECTOR ---
// Forward declaration
void update_carousel_textures(SDL_Renderer* renderer, const std::vector<MenuItem>& items, int selected,
                              SDL_Texture** bg_cur, SDL_Texture** dev_cur, SDL_Texture** dev_prev, SDL_Texture** dev_next,
                              SDL_Texture** ctrl_cur = nullptr);
// Scansiona base_p/themes/ e ritorna la lista di cartelle tema (esclude file)
std::vector<std::string> scan_themes() {
    std::vector<std::string> themes;
    std::string theme_dir = base_p + "themes";
    DIR* d = opendir(theme_dir.c_str());
    if (!d) return themes;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (e->d_type == DT_DIR) themes.push_back(name);
    }
    closedir(d);
    std::sort(themes.begin(), themes.end());
    return themes;
}

// Forward declaration (defined after show_theme_selector)
void load_systems_desc();

// Forward declarations for background music functions (defined later)
extern std::vector<std::string> music_tracks;
extern int music_track_index;
void stop_music();
void start_music(const std::string& path);

// Apre la finestra di selezione tema.
// Tasti: su/giù scorrono, A applica, B/SELECT annulla.
// Ritorna true se il tema è stato cambiato.
bool show_theme_selector(SDL_Renderer* renderer, const std::string& font_path,
                         SDL_Texture** bg_cur, SDL_Texture** dev_cur,
                         SDL_Texture** dev_prev, SDL_Texture** dev_next,
                         SDL_Texture** list_bg_tex,
                         SDL_Texture** ctrl_cur,
                         const std::vector<MenuItem>& carousel_items, int carousel_sel) {
    std::vector<std::string> themes = scan_themes();
    if (themes.empty()) return false;

    // Ottieni/crea i font necessari (usa font_cache globale)
    if (font_cache.find(18) == font_cache.end()) font_cache[18] = ttf_open_font(font_path, 18);
    if (font_cache.find(22) == font_cache.end()) font_cache[22] = ttf_open_font(font_path, 22);
    if (font_cache.find(24) == font_cache.end()) font_cache[24] = ttf_open_font(font_path, 24);
    TTF_Font* f18 = font_cache.count(18) ? font_cache[18] : nullptr;
    TTF_Font* f22 = font_cache.count(22) ? font_cache[22] : nullptr;

    int tab = 0; // 0 = TEMA, 1 = DISPLAY

    // Posizione del tema corrente nella lista
    int sel = 0;
    for (int i = 0; i < (int)themes.size(); i++) {
        if (themes[i] == current_theme) { sel = i; break; }
    }

    // Voci tab DISPLAY
    struct DispItem { const char* label; bool* flag; };
    DispItem disp_items[] = {
        {"Show WiFi icon",              &show_wifi},
        {"Show battery icon",           &show_battery},
        {"Show system name (carousel)", &show_system_name},
        {"Show help bar",               &show_help_bar},
        {"Show system logo (list)",     &show_system_logo},
        {"Music in carousel",           &music_enabled},
    };
    const int disp_count = 6;
    int disp_sel = 0;

    SDL_Texture* preview_tex = nullptr;
    std::string preview_theme = "";
    auto load_preview = [&](const std::string& t) {
        if (t == preview_theme) return;
        preview_theme = t;
        if (preview_tex) { SDL_DestroyTexture(preview_tex); preview_tex = nullptr; }
        preview_tex = load_texture_png_jpg(renderer, base_p + "themes/" + t + "/bg/default");
    };
    load_preview(themes[sel]);

    bool changed = false;
    bool open = true;
    SDL_Event ev;
    Uint32 next_input = SDL_GetTicks() + 200;

    while (open) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_JOYDEVICEADDED) {
                SDL_JoystickOpen(ev.jdevice.which);
            }
            if (ev.type == SDL_JOYBUTTONDOWN) {
                if (SDL_GetTicks() < next_input) continue;
                int btn = ev.jbutton.button;
                // L1 (btn 2) → tab TEMA,  R1 (btn 5) → tab DISPLAY
                if (btn == 2) { tab = 0; next_input = SDL_GetTicks() + 200; }
                else if (btn == 5) { tab = 1; next_input = SDL_GetTicks() + 200; }
                else if (tab == 0) {
                    if (btn == 29 || btn == 30) {
                        sel = (sel - 1 + (int)themes.size()) % (int)themes.size();
                        load_preview(themes[sel]);
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 32 || btn == 31) {
                        sel = (sel + 1) % (int)themes.size();
                        load_preview(themes[sel]);
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 1) { // A: applica tema
                        current_theme = themes[sel];
                        save_active_theme();
                        img_p = theme_p() + "images/"; // Update image path to new theme
                        theme_cfg = load_theme_config(theme_p() + "theme.cfg"); // Reload layout config for new theme
                        load_systems_desc(); // Reload system descriptions for new theme
                        if (*list_bg_tex) { SDL_DestroyTexture(*list_bg_tex); *list_bg_tex = nullptr; }
                        *list_bg_tex = load_texture_png_jpg(renderer, theme_p() + "bg/list_bg");
                        update_carousel_textures(renderer, carousel_items, carousel_sel,
                                                 bg_cur, dev_cur, dev_prev, dev_next, ctrl_cur);
                        changed = true;
                        open = false;
                    } else if (btn == 3 || btn == 6) { open = false; }
                } else { // tab == 1: DISPLAY
                    if (btn == 29 || btn == 30) {
                        disp_sel = (disp_sel - 1 + disp_count) % disp_count;
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 32 || btn == 31) {
                        disp_sel = (disp_sel + 1) % disp_count;
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 1) { // A: toggle
                        *disp_items[disp_sel].flag = !*disp_items[disp_sel].flag;
                        save_display_settings();
                        // If music toggle changed, start or stop accordingly
                        if (disp_items[disp_sel].flag == &music_enabled) {
                            if (music_enabled && !music_tracks.empty())
                                start_music(music_tracks[music_track_index]);
                            else
                                stop_music();
                        }
                        next_input = SDL_GetTicks() + 200;
                    } else if (btn == 3 || btn == 6) { open = false; }
                }
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) open = false;
#ifdef NATIVE_BASE_PATH
            if (ev.type == SDL_KEYDOWN) {
                int vbtn = -1;
                switch (ev.key.keysym.sym) {
                    case SDLK_UP:     vbtn = 29; break;
                    case SDLK_DOWN:   vbtn = 32; break;
                    case SDLK_LEFT:   vbtn = 30; break;
                    case SDLK_RIGHT:  vbtn = 31; break;
                    case SDLK_a:
                    case SDLK_RETURN: vbtn = 1;  break;
                    case SDLK_b:      vbtn = 3;  break;
                    case SDLK_l:      vbtn = 2;  break;
                    case SDLK_r:      vbtn = 5;  break;
                    default: break;
                }
                if (vbtn >= 0 && SDL_GetTicks() >= next_input) {
                    SDL_Event fake;
                    SDL_memset(&fake, 0, sizeof(fake));
                    fake.type = SDL_JOYBUTTONDOWN;
                    fake.jbutton.button = (Uint8)vbtn;
                    SDL_PushEvent(&fake);
                }
            }
#endif
        }

        // Central box
        int bw = theme_cfg.menu_box_w, bh = theme_cfg.menu_box_h;
        int win_w = 0, win_h = 0;
        SDL_RenderGetLogicalSize(renderer, &win_w, &win_h);
        if (win_w <= 0 || win_h <= 0) SDL_GetRendererOutputSize(renderer, &win_w, &win_h);

        // Redraw carousel background before overlay (so transparent overlay works correctly)
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (*bg_cur) { SDL_Rect bgr = {0, 0, win_w, win_h}; SDL_RenderCopy(renderer, *bg_cur, NULL, &bgr); }

        // Dark overlay
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, theme_cfg.menu_overlay.r, theme_cfg.menu_overlay.g, theme_cfg.menu_overlay.b, theme_cfg.menu_overlay.a);
        SDL_RenderFillRect(renderer, NULL);

        int bx = (win_w - bw) / 2, by = (win_h - bh) / 2;
        SDL_Rect box = {bx, by, bw, bh};
        SDL_SetRenderDrawColor(renderer, theme_cfg.menu_box_bg.r, theme_cfg.menu_box_bg.g, theme_cfg.menu_box_bg.b, theme_cfg.menu_box_bg.a);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, theme_cfg.menu_box_border.r, theme_cfg.menu_box_border.g, theme_cfg.menu_box_border.b, theme_cfg.menu_box_border.a);
        SDL_RenderDrawRect(renderer, &box);

        // --- TAB BAR ---
        int tab_h = theme_cfg.menu_tab_h;
        int tab_w = bw / 2;
        const char* tab_labels[] = {"THEME", "DISPLAY"};
        for (int ti = 0; ti < 2; ti++) {
            SDL_Rect tr = {bx + ti * tab_w, by, tab_w, tab_h};
            if (ti == tab) SDL_SetRenderDrawColor(renderer, theme_cfg.menu_tab_active_bg.r,   theme_cfg.menu_tab_active_bg.g,   theme_cfg.menu_tab_active_bg.b,   theme_cfg.menu_tab_active_bg.a);
            else           SDL_SetRenderDrawColor(renderer, theme_cfg.menu_tab_inactive_bg.r, theme_cfg.menu_tab_inactive_bg.g, theme_cfg.menu_tab_inactive_bg.b, theme_cfg.menu_tab_inactive_bg.a);
            SDL_RenderFillRect(renderer, &tr);
            SDL_SetRenderDrawColor(renderer, theme_cfg.menu_tab_border.r, theme_cfg.menu_tab_border.g, theme_cfg.menu_tab_border.b, theme_cfg.menu_tab_border.a);
            SDL_RenderDrawRect(renderer, &tr);
            if (f22) {
                SDL_Color tc = (ti == tab) ? theme_cfg.menu_tab_label_active : theme_cfg.menu_tab_label_normal;
                SDL_Surface* s = ttf_render_text_blended(f22, tab_labels[ti], tc);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) {
                        SDL_Rect tlr = {bx + ti*tab_w + tab_w/2 - s->w/2, by + tab_h/2 - s->h/2, s->w, s->h};
                        SDL_RenderCopy(renderer, t, NULL, &tlr);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
            }
        }

        int cont_y = by + tab_h; // area contenuto sotto il tab bar

        if (tab == 0) {
            // --- TAB TEMA ---
            SDL_Rect pr = {bx + 10, cont_y + 8, bw - 20, theme_cfg.menu_preview_h};
            if (preview_tex) {
                SDL_RenderCopy(renderer, preview_tex, NULL, &pr);
            } else {
                SDL_SetRenderDrawColor(renderer, theme_cfg.menu_preview_bg.r, theme_cfg.menu_preview_bg.g, theme_cfg.menu_preview_bg.b, theme_cfg.menu_preview_bg.a);
                SDL_RenderFillRect(renderer, &pr);
            }
            SDL_SetRenderDrawColor(renderer, theme_cfg.menu_preview_border.r, theme_cfg.menu_preview_border.g, theme_cfg.menu_preview_border.b, theme_cfg.menu_preview_border.a);
            SDL_RenderDrawRect(renderer, &pr);

            int list_y = cont_y + theme_cfg.menu_preview_h + 9;
            int row_h = theme_cfg.menu_list_row_h;
            int visible = (bh - tab_h - theme_cfg.menu_preview_h - 24) / row_h;
            int scroll_t = std::max(0, sel - visible / 2);
            scroll_t = std::min(scroll_t, std::max(0, (int)themes.size() - visible));

            for (int i = scroll_t; i < std::min(scroll_t + visible, (int)themes.size()); i++) {
                int ry = list_y + (i - scroll_t) * row_h;
                if (i == sel) {
                    SDL_Rect hl = {bx + 8, ry - 2, bw - 16, row_h};
                    SDL_SetRenderDrawColor(renderer, theme_cfg.menu_highlight.r, theme_cfg.menu_highlight.g, theme_cfg.menu_highlight.b, theme_cfg.menu_highlight.a);
                    SDL_RenderFillRect(renderer, &hl);
                }
                std::string label = (themes[i] == current_theme) ? ("* " + themes[i]) : themes[i];
                SDL_Color col = (i == sel) ? theme_cfg.menu_item_selected : theme_cfg.menu_item_normal;
                if (f22) {
                    SDL_Surface* s = ttf_render_text_blended(f22, label, col);
                    if (s) {
                        SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                        if (t) {
                            SDL_Rect tlr = {bx + 20, ry + 4, s->w, s->h};
                            SDL_RenderCopy(renderer, t, NULL, &tlr);
                            SDL_DestroyTexture(t);
                        }
                        SDL_FreeSurface(s);
                    }
                }
            }

            if (f18) {
                SDL_Surface* s = ttf_render_text_blended(f18, "[A] Apply  [B] Close  [R1] Display", theme_cfg.menu_hint);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) {
                        SDL_Rect tlr = {bx + bw/2 - s->w/2, by + bh + 6, s->w, s->h};
                        SDL_RenderCopy(renderer, t, NULL, &tlr);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
            }
        } else {
            // --- TAB DISPLAY ---
            int row_h = theme_cfg.menu_disp_row_h;
            int start_y = cont_y + 20;
            for (int i = 0; i < disp_count; i++) {
                int ry = start_y + i * row_h;
                if (i == disp_sel) {
                    SDL_Rect hl = {bx + 8, ry - 4, bw - 16, row_h - 4};
                    SDL_SetRenderDrawColor(renderer, theme_cfg.menu_highlight.r, theme_cfg.menu_highlight.g, theme_cfg.menu_highlight.b, theme_cfg.menu_highlight.a);
                    SDL_RenderFillRect(renderer, &hl);
                }
                SDL_Color lc = (i == disp_sel) ? theme_cfg.menu_item_selected : theme_cfg.menu_item_normal;
                if (f22) {
                    SDL_Surface* s = ttf_render_text_blended(f22, disp_items[i].label, lc);
                    if (s) {
                        SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                        if (t) {
                            SDL_Rect tlr = {bx + 20, ry + row_h/2 - s->h/2, s->w, s->h};
                            SDL_RenderCopy(renderer, t, NULL, &tlr);
                            SDL_DestroyTexture(t);
                        }
                        SDL_FreeSurface(s);
                    }
                }
                bool val = *disp_items[i].flag;
                SDL_Color badge_col = val ? theme_cfg.menu_badge_on : theme_cfg.menu_badge_off;
                const char* badge_str = val ? "ON" : "OFF";
                if (f22) {
                    SDL_Surface* s = ttf_render_text_blended(f22, badge_str, badge_col);
                    if (s) {
                        SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                        if (t) {
                            SDL_Rect tlr = {bx + bw - s->w - 20, ry + row_h/2 - s->h/2, s->w, s->h};
                            SDL_RenderCopy(renderer, t, NULL, &tlr);
                            SDL_DestroyTexture(t);
                        }
                        SDL_FreeSurface(s);
                    }
                }
            }

            if (f18) {
                SDL_Surface* s = ttf_render_text_blended(f18, "[A] ON/OFF  [B] Close  [L1] Theme", theme_cfg.menu_hint);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) {
                        SDL_Rect tlr = {bx + bw/2 - s->w/2, by + bh + 6, s->w, s->h};
                        SDL_RenderCopy(renderer, t, NULL, &tlr);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (preview_tex) SDL_DestroyTexture(preview_tex);
    return changed;
}

// --- CAROUSEL UPDATE ---
void update_carousel_textures(SDL_Renderer* renderer, const std::vector<MenuItem>& items, int selected, 
                             SDL_Texture** bg_cur, SDL_Texture** dev_cur, SDL_Texture** dev_prev, SDL_Texture** dev_next,
                             SDL_Texture** ctrl_cur) {
    if (items.empty()) return;
    auto safe_release = [](SDL_Texture** t) { if (*t) { SDL_DestroyTexture(*t); *t = nullptr; } };
    safe_release(bg_cur); safe_release(dev_cur); safe_release(dev_prev); safe_release(dev_next);
    if (ctrl_cur) safe_release(ctrl_cur);

    int prev_idx = (selected - 1 + (int)items.size()) % (int)items.size();
    int next_idx = (selected + 1) % (int)items.size();

    *bg_cur = load_texture_png_jpg(renderer, theme_p() + "bg/" + items[selected].name);
    if (!*bg_cur) { std::string p = find_file_ignore_case(theme_p() + "bg", items[selected].name + ".png"); if (!p.empty()) *bg_cur = load_texture(renderer, p); }
    if (!*bg_cur) { std::string p = find_file_ignore_case(theme_p() + "bg", items[selected].name + ".jpg"); if (!p.empty()) *bg_cur = load_texture(renderer, p); }
    if (!*bg_cur) *bg_cur = load_texture_png_jpg(renderer, theme_p() + "bg/default");

    *dev_cur = load_texture(renderer, theme_p() + "systems/" + items[selected].name + ".png");
    if (!*dev_cur) { std::string p = find_file_ignore_case(theme_p() + "systems", items[selected].name + ".png"); if (!p.empty()) *dev_cur = load_texture(renderer, p); }
    if (!*dev_cur) *dev_cur = load_texture(renderer, theme_p() + "systems/default.png");

    *dev_prev = load_texture(renderer, theme_p() + "systems/" + items[prev_idx].name + ".png");
    if (!*dev_prev) { std::string p = find_file_ignore_case(theme_p() + "systems", items[prev_idx].name + ".png"); if (!p.empty()) *dev_prev = load_texture(renderer, p); }
    if (!*dev_prev) *dev_prev = load_texture(renderer, theme_p() + "systems/default.png");

    *dev_next = load_texture(renderer, theme_p() + "systems/" + items[next_idx].name + ".png");
    if (!*dev_next) { std::string p = find_file_ignore_case(theme_p() + "systems", items[next_idx].name + ".png"); if (!p.empty()) *dev_next = load_texture(renderer, p); }
    if (!*dev_next) *dev_next = load_texture(renderer, theme_p() + "systems/default.png");

    if (ctrl_cur) {
        std::string p = find_file_ignore_case(theme_p() + "controllers", items[selected].name + ".png");
        *ctrl_cur = !p.empty() ? load_texture(renderer, p) : nullptr;
        // no default fallback for controller: simply leave null if not found
    }
}

// --- FILE SYSTEM ---
std::map<std::string, int> game_count_cache;

int count_games_recursive_impl(const std::string& path, const std::string& sys_name, const std::vector<std::string>& exts) {
    int count = 0; DIR *dir = opendir(path.c_str()); struct dirent *ev;
    if (!dir) return 0;
    while ((ev = readdir(dir)) != NULL) {
        std::string name = ev->d_name; if (name == "." || name == "..") continue;
        if (ev->d_type == DT_DIR) count += count_games_recursive_impl(path + "/" + name, sys_name, exts);
        else {
            std::string ln = name; std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
            for(const auto& ext : exts) {
                if(ln.size() > ext.size() && ln.compare(ln.size()-ext.size(), ext.size(), ext) == 0) { count++; break; }
            }
        }
    }
    closedir(dir); return count;
}

int count_games_recursive(const std::string& path, const std::string& sys_name) {
    auto cache_it = game_count_cache.find(sys_name);
    if (cache_it != game_count_cache.end()) return cache_it->second;
    auto cfg_it = system_configs.find(sys_name);
    if (cfg_it == system_configs.end()) return 0;
    const auto& exts = cfg_it->second;
    int count = count_games_recursive_impl(path, sys_name, exts);
    game_count_cache[sys_name] = count;
    return count;
}

std::vector<MenuItem> scan_directory(const std::string& path, bool only_dirs, const std::string& sys_context = "") {
    std::vector<MenuItem> items; items.reserve(256);
    DIR *dir = opendir(path.c_str()); struct dirent *ev;
    if (!dir) return items;
    const auto* exts_ptr = sys_context.empty() ? nullptr : (system_configs.find(sys_context) != system_configs.end() ? &system_configs.at(sys_context) : nullptr);
    while ((ev = readdir(dir)) != NULL) {
        std::string name = ev->d_name; if (name == "." || name == "..") continue;
        bool is_dir = (ev->d_type == DT_DIR);
        // Filtra cartelle/file nascosti
        if (!hidden_dirs.empty()) {
            std::string low_name = name;
            std::transform(low_name.begin(), low_name.end(), low_name.begin(), ::tolower);
            if (hidden_dirs.count(low_name)) continue;
        }
        if (only_dirs) { if (is_dir) items.push_back({name, true}); }
        else {
            if (is_dir) items.push_back({name, true});
            else if (exts_ptr) {
                std::string ln = name; std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
                for(const auto& ext : *exts_ptr) {
                    if(ln.size() > ext.size() && ln.compare(ln.size()-ext.size(), ext.size(), ext) == 0) { items.push_back({name, false}); break; }
                }
            }
        }
    }
    closedir(dir);
    std::sort(items.begin(), items.end(), [](const MenuItem& a, const MenuItem& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        std::string s1 = a.name, s2 = b.name;
        std::transform(s1.begin(), s1.end(), s1.begin(), ::tolower); std::transform(s2.begin(), s2.end(), s2.begin(), ::tolower);
        return s1 < s2;
    });
    return items;
}
// fine prima parte



// --- AUDIO HANDLING ---
SoundSample s_click, s_enter, s_back, s_fav;
SDL_AudioDeviceID audio_device = 0;
SDL_AudioSpec target_spec;
void play_sound(SoundSample& s) { if (audio_device && s.buffer) { SDL_ClearQueuedAudio(audio_device); SDL_QueueAudio(audio_device, s.buffer, s.length); SDL_PauseAudioDevice(audio_device, 0); } }
void load_and_convert_sound(const char* path, SoundSample& s) {
    SDL_AudioSpec ws; Uint8* wb; Uint32 wl; if (SDL_LoadWAV(path, &ws, &wb, &wl) == NULL) return;
    SDL_AudioCVT cvt; if (SDL_BuildAudioCVT(&cvt, ws.format, ws.channels, ws.freq, target_spec.format, target_spec.channels, target_spec.freq) > 0) {
        cvt.len = wl; cvt.buf = (Uint8*)SDL_malloc(cvt.len * cvt.len_mult); SDL_memcpy(cvt.buf, wb, wl); SDL_ConvertAudio(&cvt);
        s.buffer = cvt.buf; s.length = cvt.len_cvt; SDL_FreeWAV(wb);
    } else { s.buffer = wb; s.length = wl; }
}

void load_extensions_cfg(const std::string& path) {
    std::ifstream file(path); std::string line; line.reserve(256);
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t sep = line.find(':');
        if (sep != std::string::npos) {
            std::string sys = line.substr(0, sep);
            if (sys == "_hide_dirs") {
                // Cartelle da nascondere: _hide_dirs:boxart,videos,gamelist.xml
                std::stringstream ss(line.substr(sep + 1)); std::string name; name.reserve(32);
                while (std::getline(ss, name, ',')) {
                    // Trim leading/trailing whitespace only, preserve internal spaces
                    size_t s = name.find_first_not_of(" \t\r\n");
                    size_t e = name.find_last_not_of(" \t\r\n");
                    name = (s == std::string::npos) ? "" : name.substr(s, e - s + 1);
                    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                    if (!name.empty()) hidden_dirs.insert(name);
                }
            } else {
                auto& exts = system_configs[sys];
                exts.reserve(32);
                std::stringstream ss(line.substr(sep + 1)); std::string ext; ext.reserve(16);
                while (std::getline(ss, ext, ',')) {
                    ext.erase(std::remove_if(ext.begin(), ext.end(), ::isspace), ext.end());
                    exts.push_back("." + ext);
                }
            }
        }
    }
}

// --- VIDEO PREVIEW ---
std::thread video_thread;
std::atomic<bool> video_running(false);
std::mutex video_mutex;
std::vector<uint8_t> video_frame_buffer;
std::atomic<bool> video_frame_ready(false);
int video_frame_w = 0, video_frame_h = 0;
SDL_Texture* video_texture = nullptr;

// Audio del video (SDL_QueueAudio - nessun callback, nessun mutex audio)
SDL_AudioDeviceID video_audio_device = 0;

// --- BACKGROUND MUSIC ---
std::atomic<bool> music_stop_flag{false};
std::atomic<bool> music_running_flag{false};
std::thread music_thread;
SDL_AudioDeviceID music_audio_device = 0;
std::vector<std::string> music_tracks;
int music_track_index = 0;
std::string music_track_name_overlay;   // displayed as overlay when track changes
Uint32 music_track_name_until = 0;      // SDL_GetTicks() deadline for overlay

std::vector<std::string> scan_music_tracks(const std::string& theme_name) {
    auto scan_dir = [](const std::string& music_dir) {
        std::vector<std::string> tracks;
        DIR* d = opendir(music_dir.c_str());
        if (!d) return tracks;
        const std::vector<std::string> exts = {".mp3", ".ogg", ".wav", ".flac", ".m4a"};
        struct dirent* en;
        while ((en = readdir(d)) != nullptr) {
            std::string name = en->d_name;
            if (name[0] == '.') continue;
            std::string low = name;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            for (auto& ext : exts) {
                if (low.size() > ext.size() && low.compare(low.size()-ext.size(), ext.size(), ext) == 0) {
                    tracks.push_back(music_dir + "/" + name);
                    break;
                }
            }
        }
        closedir(d);
        std::sort(tracks.begin(), tracks.end());
        return tracks;
    };

    std::vector<std::string> tracks = scan_dir(base_p + "themes/" + theme_name + "/music");
    // Fallback to default theme music if current theme has none
    if (tracks.empty() && theme_name != "default")
        tracks = scan_dir(base_p + "themes/default/music");
    return tracks;
}

void stop_music() {
    music_stop_flag = true;
    if (music_thread.joinable()) music_thread.join();
    if (music_audio_device) {
        SDL_PauseAudioDevice(music_audio_device, 1);
        SDL_ClearQueuedAudio(music_audio_device);
        SDL_CloseAudioDevice(music_audio_device);
        music_audio_device = 0;
    }
    music_running_flag = false;
}

void start_music(const std::string& path) {
    stop_music();
    if (path.empty()) return;
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = 44100; spec.format = AUDIO_S16SYS; spec.channels = 2; spec.samples = 4096; spec.callback = nullptr;
    music_audio_device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (!music_audio_device) return;
    SDL_PauseAudioDevice(music_audio_device, 0);
    music_stop_flag = false;
    music_running_flag = true;
    music_thread = std::thread([path]() {
        try {
            while (!music_stop_flag) {
                AVFormatContext* fmt = nullptr;
                if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) break;
                if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); break; }
                int audio_stream = -1;
                for (int i = 0; i < (int)fmt->nb_streams; i++) {
                    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                        !(fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                        audio_stream = i; break;
                    }
                }
                if (audio_stream < 0) { avformat_close_input(&fmt); break; }
                AVCodecContext* codec_ctx = avcodec_alloc_context3(nullptr);
                if (!codec_ctx) { avformat_close_input(&fmt); break; }
                avcodec_parameters_to_context(codec_ctx, fmt->streams[audio_stream]->codecpar);
                const AVCodec* codec = avcodec_find_decoder(codec_ctx->codec_id);
                if (!codec || avcodec_open2(codec_ctx, codec, nullptr) < 0) {
                    avcodec_free_context(&codec_ctx); avformat_close_input(&fmt); break;
                }
                int64_t in_layout = codec_ctx->channel_layout ? codec_ctx->channel_layout : AV_CH_LAYOUT_STEREO;
                SwrContext* swr = swr_alloc_set_opts(nullptr,
                    AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 44100,
                    in_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate, 0, nullptr);
                if (!swr || swr_init(swr) < 0) {
                    if (swr) swr_free(&swr);
                    avcodec_free_context(&codec_ctx); avformat_close_input(&fmt); break;
                }
                AVPacket* pkt = av_packet_alloc();
                AVFrame* frame = av_frame_alloc();
                if (!pkt || !frame) {
                    if (pkt) av_packet_free(&pkt);
                    if (frame) av_frame_free(&frame);
                    swr_free(&swr); avcodec_free_context(&codec_ctx); avformat_close_input(&fmt); break;
                }
                while (!music_stop_flag) {
                    int ret = av_read_frame(fmt, pkt);
                    if (ret < 0) break; // EOF → outer loop will restart (loop track)
                    if (pkt->stream_index != audio_stream) { av_packet_unref(pkt); continue; }
                    if (avcodec_send_packet(codec_ctx, pkt) < 0) { av_packet_unref(pkt); continue; }
                    av_packet_unref(pkt);
                    while (!music_stop_flag && avcodec_receive_frame(codec_ctx, frame) == 0) {
                        int out_samples = (int)av_rescale_rnd(
                            swr_get_delay(swr, codec_ctx->sample_rate) + frame->nb_samples,
                            44100, codec_ctx->sample_rate, AV_ROUND_UP);
                        if (out_samples <= 0 || out_samples > 192000) { av_frame_unref(frame); continue; }
                        std::vector<uint8_t> buf((size_t)(out_samples * 2 * 2));
                        uint8_t* out_ptr = buf.data();
                        int got = swr_convert(swr, &out_ptr, out_samples,
                            (const uint8_t**)frame->data, frame->nb_samples);
                        av_frame_unref(frame);
                        if (got > 0) {
                            while (!music_stop_flag && SDL_GetQueuedAudioSize(music_audio_device) > 44100u * 2u * 2u * 2u)
                                SDL_Delay(10);
                            if (!music_stop_flag)
                                SDL_QueueAudio(music_audio_device, buf.data(), (Uint32)(got * 2 * 2));
                        }
                    }
                }
                av_packet_free(&pkt);
                av_frame_free(&frame);
                swr_free(&swr);
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                // EOF reached: loop the track
            }
        } catch (...) {}
        music_running_flag = false;
    });
}

struct AudioPktQueue {
    std::queue<AVPacket*> pkts;
    std::mutex mtx;
    std::condition_variable cv;
    bool flush_requested = false;
    bool flush_done = false;
    bool done = false;
};

void stop_video_preview() {
    video_running = false;
    if (video_thread.joinable()) video_thread.join();
    if (video_audio_device) {
        SDL_PauseAudioDevice(video_audio_device, 1);
        SDL_ClearQueuedAudio(video_audio_device);
        SDL_CloseAudioDevice(video_audio_device);
        video_audio_device = 0;
    }
    std::lock_guard<std::mutex> lock(video_mutex);
    video_frame_buffer.clear();
    video_frame_ready = false;
    video_frame_w = video_frame_h = 0;
    // Destroy the texture so it is recreated with correct dimensions on next video
    if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
}

std::string find_video_file(const std::string& roms_base, const std::string& sys_name, const std::string& game_name) {
    // Cerca nella cartella videos dentro il sistema: Games/<sistema>/videos/
    std::string sys_folder = find_subdir_ignore_case(roms_base + "/" + sys_name, "videos");
    if (sys_folder.empty()) return "";
    // Cerca il file video (case-insensitive, estensioni mp4/avi/webm)
    const std::vector<std::string> video_exts = {".mp4", ".avi", ".webm", ".mkv"};
    DIR* vd = opendir(sys_folder.c_str());
    if (!vd) return "";
    std::string low_game = game_name;
    std::transform(low_game.begin(), low_game.end(), low_game.begin(), ::tolower);
    struct dirent* ve;
    while ((ve = readdir(vd)) != NULL) {
        std::string fname = ve->d_name;
        std::string low_fname = fname;
        std::transform(low_fname.begin(), low_fname.end(), low_fname.begin(), ::tolower);
        for (const auto& ext : video_exts) {
            if (low_fname.size() > ext.size() && low_fname.substr(low_fname.size() - ext.size()) == ext) {
                std::string name_no_ext = low_fname.substr(0, low_fname.size() - ext.size());
                if (name_no_ext == low_game) {
                    closedir(vd);
                    return sys_folder + "/" + fname;
                }
            }
        }
    }
    closedir(vd);
    return "";
}

void start_video_preview(const std::string& video_path) {
    stop_video_preview();
    video_running = true;

    // Apri device audio per il video in modalità queue (nessun callback)
    SDL_AudioSpec wanted_spec;
    SDL_zero(wanted_spec);
    wanted_spec.freq = 44100;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = 2;
    wanted_spec.samples = 4096;
    wanted_spec.callback = nullptr;  // SDL_QueueAudio mode
    wanted_spec.userdata = nullptr;
    video_audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, NULL, 0);
    if (video_audio_device) SDL_PauseAudioDevice(video_audio_device, 0);

    video_thread = std::thread([video_path]() {
      // Declare thread management objects outside try-catch so they are accessible
      // in the catch block and their destructors run only after catch completes
      std::shared_ptr<AudioPktQueue> apq;
      std::shared_ptr<AudioPktQueue> vpq;
      std::thread audio_decode_thread;
      std::thread video_decode_thread;
      try {
        AVFormatContext* fmt_ctx = nullptr;
        if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) != 0) return;
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) { avformat_close_input(&fmt_ctx); return; }

        // Trova stream video e audio (skip attached pictures / cover art)
        int video_stream = -1, audio_stream = -1;
        for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream == -1)
                video_stream = i;
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream == -1)
                audio_stream = i;
        }
        if (video_stream == -1) { avformat_close_input(&fmt_ctx); return; }

        // Setup video decoder
        AVCodecParameters* v_codecpar = fmt_ctx->streams[video_stream]->codecpar;
        const AVCodec* v_codec = avcodec_find_decoder(v_codecpar->codec_id);
        if (!v_codec) { avformat_close_input(&fmt_ctx); return; }
        AVCodecContext* v_codec_ctx = avcodec_alloc_context3(v_codec);
        if (!v_codec_ctx) { avformat_close_input(&fmt_ctx); return; }
        avcodec_parameters_to_context(v_codec_ctx, v_codecpar);
        if (avcodec_open2(v_codec_ctx, v_codec, nullptr) < 0) {
            avcodec_free_context(&v_codec_ctx); avformat_close_input(&fmt_ctx); return;
        }

        // Setup audio decoder (opzionale)
        AVCodecContext* a_codec_ctx = nullptr;
        SwrContext* swr_ctx = nullptr;
        if (audio_stream != -1) {
            AVCodecParameters* a_codecpar = fmt_ctx->streams[audio_stream]->codecpar;
            const AVCodec* a_codec = avcodec_find_decoder(a_codecpar->codec_id);
            if (a_codec) {
                a_codec_ctx = avcodec_alloc_context3(a_codec);
                if (!a_codec_ctx) { /* skip audio, not fatal */ }
                else { avcodec_parameters_to_context(a_codec_ctx, a_codecpar); }
                if (a_codec_ctx && avcodec_open2(a_codec_ctx, a_codec, nullptr) == 0) {
                    // Resample verso S16 stereo 44100Hz
                    swr_ctx = swr_alloc();
                    if (!swr_ctx) {
                        avcodec_free_context(&a_codec_ctx); a_codec_ctx = nullptr;
                    } else {
                        av_opt_set_int(swr_ctx, "in_channel_layout",
                            a_codec_ctx->channel_layout ? a_codec_ctx->channel_layout : av_get_default_channel_layout(a_codec_ctx->channels), 0);
                        av_opt_set_int(swr_ctx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
                        av_opt_set_int(swr_ctx, "in_sample_rate", a_codec_ctx->sample_rate, 0);
                        av_opt_set_int(swr_ctx, "out_sample_rate", 44100, 0);
                        av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", a_codec_ctx->sample_fmt, 0);
                        av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
                        if (swr_init(swr_ctx) < 0) {
                            swr_free(&swr_ctx); swr_ctx = nullptr;
                        }
                    }
                } else if (a_codec_ctx) {
                    avcodec_free_context(&a_codec_ctx); a_codec_ctx = nullptr;
                }
            }
        }

        // Verifica dimensioni valide dal decoder
        if (v_codec_ctx->width <= 0 || v_codec_ctx->height <= 0) {
            avcodec_free_context(&v_codec_ctx);
            if (a_codec_ctx) avcodec_free_context(&a_codec_ctx);
            if (swr_ctx) swr_free(&swr_ctx);
            avformat_close_input(&fmt_ctx);
            return;
        }

        // Scala il video se troppo grande (max 480px larghezza)
        int out_w = v_codec_ctx->width;
        int out_h = v_codec_ctx->height;
        if (out_w > 480) {
            out_h = (int)((float)out_h * 480.0f / out_w);
            out_w = 480;
        }
        if (out_h > 360) {
            out_w = (int)((float)out_w * 360.0f / out_h);
            out_h = 360;
        }
        if (out_w < 2) out_w = 2;
        if (out_h < 2) out_h = 2;

        SwsContext* sws_ctx = sws_getContext(
            v_codec_ctx->width, v_codec_ctx->height, v_codec_ctx->pix_fmt,
            out_w, out_h, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!sws_ctx) {
            avcodec_free_context(&v_codec_ctx);
            if (a_codec_ctx) avcodec_free_context(&a_codec_ctx);
            if (swr_ctx) swr_free(&swr_ctx);
            avformat_close_input(&fmt_ctx);
            return;
        }

        int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, out_w, out_h, 1);
        if (num_bytes <= 0 || num_bytes > 4*1024*1024) {
            sws_freeContext(sws_ctx);
            avcodec_free_context(&v_codec_ctx);
            if (a_codec_ctx) avcodec_free_context(&a_codec_ctx);
            if (swr_ctx) swr_free(&swr_ctx);
            avformat_close_input(&fmt_ctx);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(video_mutex);
            video_frame_buffer.resize(num_bytes);
            video_frame_w = out_w;
            video_frame_h = out_h;
        }

        double fps = 30.0;
        if (fmt_ctx->streams[video_stream]->avg_frame_rate.den > 0) {
            fps = av_q2d(fmt_ctx->streams[video_stream]->avg_frame_rate);
            if (fps <= 0 || fps > 120) fps = 30.0;
        }
        int frame_delay_ms = (int)(1000.0 / fps);

        // --- Architettura a 3 thread: demuxer / video decode / audio decode ---
        //
        // Il demuxer legge pacchetti il più veloce possibile e li smista nelle
        // code rispettive. Audio e video sono completamente disaccoppiati:
        // il sleep del timing video NON blocca mai la lettura dei pacchetti audio.
        //
        //  video_thread (questo) = demuxer
        //  video_decode_thread   = decodifica + scaling + timing presentazione
        //  audio_decode_thread   = decodifica + resample + SDL_QueueAudio

        apq = std::make_shared<AudioPktQueue>(); // coda pacchetti audio
        vpq = std::make_shared<AudioPktQueue>(); // coda pacchetti video

        // --- Thread decodifica audio ---
        if (a_codec_ctx && swr_ctx) {
            audio_decode_thread = std::thread([apq, a_codec_ctx, swr_ctx]() {
              try {
                AVFrame* af = av_frame_alloc();
                if (!af) { apq->done = true; apq->cv.notify_all(); return; }
                while (true) {
                    AVPacket* apkt = nullptr;
                    {
                        std::unique_lock<std::mutex> lk(apq->mtx);
                        apq->cv.wait(lk, [&]{
                            return !apq->pkts.empty() || apq->flush_requested || apq->done;
                        });
                        if (apq->flush_requested) {
                            while (!apq->pkts.empty()) {
                                av_packet_free(&apq->pkts.front());
                                apq->pkts.pop();
                            }
                            apq->flush_requested = false;
                            apq->flush_done = true;
                            apq->cv.notify_all();
                            continue;
                        }
                        if (apq->pkts.empty() && apq->done) break;
                        if (apq->pkts.empty()) continue;
                        apkt = apq->pkts.front();
                        apq->pkts.pop();
                    }
                    if (avcodec_send_packet(a_codec_ctx, apkt) == 0) {
                        while (avcodec_receive_frame(a_codec_ctx, af) == 0) {
                            if (a_codec_ctx->sample_rate <= 0) continue; // Guard against division by zero
                            int out_samples = (int)av_rescale_rnd(
                                swr_get_delay(swr_ctx, a_codec_ctx->sample_rate) + af->nb_samples,
                                44100, a_codec_ctx->sample_rate, AV_ROUND_UP);
                            // Guard against corrupt packet yielding invalid out_samples
                            if (out_samples <= 0 || out_samples > 192000) continue;
                            std::vector<uint8_t> out_buf(out_samples * 2 * 2);
                            uint8_t* out_ptr = out_buf.data();
                            int converted = swr_convert(swr_ctx, &out_ptr, out_samples,
                                (const uint8_t**)af->data, af->nb_samples);
                            if (converted > 0 && video_audio_device) {
                                // Throttle: max 2s di audio pre-bufferato in SDL
                                while (SDL_GetQueuedAudioSize(video_audio_device) > 44100u * 2 * 2 * 2) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                }
                                SDL_QueueAudio(video_audio_device, out_buf.data(), (Uint32)(converted * 2 * 2));
                            }
                        }
                    }
                    av_packet_free(&apkt);
                }
                av_frame_free(&af);
              } catch (...) {
                  // Prevent std::terminate from killing the process on any exception
                  apq->done = true;
                  apq->cv.notify_all();
              }
            });
        }

        // --- Thread decodifica video + timing presentazione ---
        video_decode_thread = std::thread([vpq, v_codec_ctx, sws_ctx, out_w, out_h,
                                         frame_delay_ms, num_bytes]() {
          try {
            AVFrame* vf  = av_frame_alloc();
            AVFrame* rvf = av_frame_alloc();
            if (!vf || !rvf) {
                av_frame_free(&vf); av_frame_free(&rvf);
                vpq->done = true; vpq->cv.notify_all(); return;
            }
            // Use av_malloc for 32-byte aligned buffer required by NEON sws_scale on ARM
            // (std::vector is not guaranteed to be SIMD-aligned, causing crashes on identity-size conversions)
            uint8_t* lbuf = (uint8_t*)av_malloc(num_bytes);
            if (!lbuf) {
                av_frame_free(&vf); av_frame_free(&rvf);
                vpq->done = true; vpq->cv.notify_all(); return;
            }
            av_image_fill_arrays(rvf->data, rvf->linesize, lbuf,
                AV_PIX_FMT_RGBA, out_w, out_h, 1);

            auto next_frame_time = std::chrono::steady_clock::now();

            while (true) {
                AVPacket* vpkt = nullptr;
                {
                    std::unique_lock<std::mutex> lk(vpq->mtx);
                    vpq->cv.wait(lk, [&]{
                        return !vpq->pkts.empty() || vpq->flush_requested || vpq->done;
                    });
                    if (vpq->flush_requested) {
                        while (!vpq->pkts.empty()) {
                            av_packet_free(&vpq->pkts.front());
                            vpq->pkts.pop();
                        }
                        vpq->flush_requested = false;
                        vpq->flush_done = true;
                        vpq->cv.notify_all();
                        next_frame_time = std::chrono::steady_clock::now();
                        continue;
                    }
                    if (vpq->pkts.empty() && vpq->done) break;
                    if (vpq->pkts.empty()) continue;
                    vpkt = vpq->pkts.front();
                    vpq->pkts.pop();
                    vpq->cv.notify_all(); // sveglia il demuxer se stava aspettando spazio
                }
                if (avcodec_send_packet(v_codec_ctx, vpkt) == 0) {
                    while (avcodec_receive_frame(v_codec_ctx, vf) == 0) {
                        if (!video_running) break;
                        sws_scale(sws_ctx, vf->data, vf->linesize, 0,
                                  v_codec_ctx->height, rvf->data, rvf->linesize);
                        {
                            std::lock_guard<std::mutex> lock(video_mutex);
                            memcpy(video_frame_buffer.data(), lbuf, num_bytes);
                            video_frame_ready = true;
                        }
                        // Timing presentazione: questo thread può dormire liberamente
                        // senza impatto sull'audio (demuxer e audio thread sono indipendenti)
                        next_frame_time += std::chrono::milliseconds(frame_delay_ms);
                        auto now = std::chrono::steady_clock::now();
                        if (next_frame_time > now) {
                            std::this_thread::sleep_for(next_frame_time - now);
                        } else {
                            next_frame_time = now;
                        }
                    }
                }
                av_packet_free(&vpkt);
            }
            av_free(lbuf);
            av_frame_free(&vf);
            av_frame_free(&rvf);
          } catch (...) {
              // Prevent std::terminate from killing the process on any exception
              vpq->done = true;
              vpq->cv.notify_all();
          }
        });

        // --- Demuxer: legge pacchetti e smista a vpq / apq ---
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) throw std::runtime_error("av_packet_alloc failed");

        while (video_running) {
            av_seek_frame(fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);

            // Flush coda video (attendi conferma dal video decode thread)
            {
                std::unique_lock<std::mutex> lk(vpq->mtx);
                vpq->flush_done = false;
                vpq->flush_requested = true;
                vpq->cv.notify_all();
                vpq->cv.wait(lk, [&]{ return vpq->flush_done || !video_running; });
            }
            avcodec_flush_buffers(v_codec_ctx);

            // Flush coda audio (attendi conferma dall'audio decode thread)
            if (a_codec_ctx) {
                {
                    std::unique_lock<std::mutex> lk(apq->mtx);
                    apq->flush_done = false;
                    apq->flush_requested = true;
                    apq->cv.notify_all();
                    apq->cv.wait(lk, [&]{ return apq->flush_done || !video_running; });
                }
                avcodec_flush_buffers(a_codec_ctx);
            }
            if (video_audio_device) SDL_ClearQueuedAudio(video_audio_device);

            while (video_running) {
                int ret = av_read_frame(fmt_ctx, pkt);
                if (ret < 0) break; // fine file → seek

                if (pkt->stream_index == video_stream) {
                    AVPacket* vpkt = av_packet_alloc();
                    if (vpkt) {
                        av_packet_ref(vpkt, pkt);
                        {
                            // Aspetta spazio nella coda con cv (non polling): reagisce
                            // in <1ms quando il decode thread consuma un pacchetto
                            std::unique_lock<std::mutex> lk(vpq->mtx);
                            vpq->cv.wait(lk, [&]{
                                return vpq->pkts.size() < 256 || !video_running || vpq->flush_requested;
                            });
                            if (video_running && !vpq->flush_requested) {
                                vpq->pkts.push(vpkt);
                                vpq->cv.notify_all();
                            } else {
                                av_packet_free(&vpkt);
                            }
                        }
                    }

                } else if (pkt->stream_index == audio_stream && a_codec_ctx && swr_ctx) {
                    AVPacket* apkt = av_packet_alloc();
                    if (apkt) {
                        av_packet_ref(apkt, pkt);
                        std::unique_lock<std::mutex> lk(apq->mtx);
                        if (apq->pkts.size() < 512) {
                            apq->pkts.push(apkt);
                            apq->cv.notify_one();
                        } else {
                            av_packet_free(&apkt);
                        }
                    }
                }
                av_packet_unref(pkt);
            }
        }

        // Termina il thread video decode
        {
            std::unique_lock<std::mutex> lk(vpq->mtx);
            while (!vpq->pkts.empty()) {
                av_packet_free(&vpq->pkts.front());
                vpq->pkts.pop();
            }
            vpq->done = true;
            vpq->cv.notify_all();
        }
        if (video_decode_thread.joinable()) video_decode_thread.join();

        // Termina il thread audio decode
        {
            std::unique_lock<std::mutex> lk(apq->mtx);
            while (!apq->pkts.empty()) {
                av_packet_free(&apq->pkts.front());
                apq->pkts.pop();
            }
            apq->done = true;
            apq->cv.notify_all();
        }
        if (audio_decode_thread.joinable()) audio_decode_thread.join();

        av_packet_free(&pkt);
        sws_freeContext(sws_ctx);
        if (swr_ctx) swr_free(&swr_ctx);
        avcodec_free_context(&v_codec_ctx);
        if (a_codec_ctx) avcodec_free_context(&a_codec_ctx);
        avformat_close_input(&fmt_ctx);
      } catch (...) {
          // Catch any unhandled exception: signal threads to exit and join them
          // before they go out of scope (avoids std::terminate on joinable std::thread destructor)
          video_running = false;
          if (vpq) { std::lock_guard<std::mutex> lk(vpq->mtx); vpq->done = true; vpq->cv.notify_all(); }
          if (apq) { std::lock_guard<std::mutex> lk(apq->mtx); apq->done = true; apq->cv.notify_all(); }
          if (video_decode_thread.joinable()) video_decode_thread.join();
          if (audio_decode_thread.joinable()) audio_decode_thread.join();
      }
    });
}

// --- GESTIONE PREFERITI CON PERCORSO COMPLETO ---
struct FavoriteGame {
    std::string game_id;        // "sistema/percorso/gioco.ext"
    std::string system;         // Sistema di origine (per boxart)
    std::string display_name;   // Nome gioco senza estensione
};

std::vector<FavoriteGame> favorites_list;

void load_favs() {
    favorites_list.clear();
    favorites_list.reserve(256);
    std::ifstream f(base_p + "favorites.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) {
            // Normalizza il game_id se il sistema è duplicato (retrocompatibilità)
            size_t first_slash = line.find('/');
            if (first_slash != std::string::npos) {
                std::string sys = line.substr(0, first_slash);
                std::string rest = line.substr(first_slash + 1);
                
                // Controlla se il resto inizia con lo stesso sistema (duplicazione)
                std::string sys_with_slash = sys + "/";
                if (rest.find(sys_with_slash) == 0) {
                    // C'è duplicazione! Rimuovila
                    rest = rest.substr(sys_with_slash.length());
                    line = sys + "/" + rest;
                }
                
                // Ora procedi normalmente con il game_id normalizzato
                std::string game_name = line.substr(line.find_last_of('/') + 1);
                std::string display = game_name;
                size_t dot = display.find_last_of(".");
                if (dot != std::string::npos) display = display.substr(0, dot);
                favorites_list.push_back({line, sys, display});
            }
        }
    }
}

void save_favs() {
    std::ofstream f(base_p + "favorites.txt");
    for (const auto& fav : favorites_list) {
        f << fav.game_id << "\n";
    }
}

std::vector<MenuItem> load_favorites_as_items() {
    std::vector<MenuItem> fav_items;
    fav_items.reserve(favorites_list.size());
    for (const auto& fav : favorites_list) {
        fav_items.push_back({fav.display_name, false});
    }
    return fav_items;
}

// --- GAMELIST.XML PARSER ---
struct GameInfo {
    std::string path;        // "./Castlevania.lha"
    std::string name;
    std::string desc;
    std::string image;
    std::string video;
    std::string genre;
    std::string developer;
    std::string publisher;
    std::string rating;
    std::string releasedate;
};

// Mappa: filename (lowercase, senza estensione) -> GameInfo
std::map<std::string, GameInfo> current_gamelist;
// Cache gamelists per sistema
std::map<std::string, std::map<std::string, GameInfo>> gamelist_cache;
std::string gamelist_roms_base; // salvato per uso in find_game_info_for_system

static std::string xml_unescape(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&' && i + 1 < s.size()) {
            if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; }
            else if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; }
            else if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 2; }
            else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; }
            else if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; }
            else out += s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string extract_tag(const std::string& xml, const std::string& tag) {
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    size_t start = xml.find(open);
    if (start == std::string::npos) return "";
    start += open.size();
    size_t end = xml.find(close, start);
    if (end == std::string::npos) return "";
    return xml_unescape(xml.substr(start, end - start));
}

void load_gamelist(const std::string& system_path) {
    current_gamelist.clear();
    std::string gl_path = system_path + "/gamelist.xml";
    std::ifstream f(gl_path);
    if (!f.is_open()) return;

    // Leggi tutto il file
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Cerca ogni blocco <game>...</game>
    size_t pos = 0;
    while (pos < content.size()) {
        size_t game_start = content.find("<game>", pos);
        if (game_start == std::string::npos) break;
        size_t game_end = content.find("</game>", game_start);
        if (game_end == std::string::npos) break;
        game_end += 7; // lunghezza "</game>"

        std::string block = content.substr(game_start, game_end - game_start);

        GameInfo gi;
        gi.path = extract_tag(block, "path");
        gi.name = extract_tag(block, "name");
        gi.desc = extract_tag(block, "desc");
        gi.image = extract_tag(block, "image");
        gi.video = extract_tag(block, "video");
        gi.genre = extract_tag(block, "genre");
        gi.developer = extract_tag(block, "developer");
        gi.publisher = extract_tag(block, "publisher");
        gi.rating = extract_tag(block, "rating");
        gi.releasedate = extract_tag(block, "releasedate");

        // Chiave: filename senza path e senza estensione, lowercase
        std::string key = gi.path;
        // Rimuovi "./" iniziale
        if (key.size() > 2 && key[0] == '.' && key[1] == '/') key = key.substr(2);
        // Prendi solo il filename (ultimo componente del path)
        size_t last_slash = key.find_last_of('/');
        if (last_slash != std::string::npos) key = key.substr(last_slash + 1);
        // Rimuovi estensione
        size_t dot = key.find_last_of('.');
        if (dot != std::string::npos) key = key.substr(0, dot);
        // Lowercase
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (!key.empty()) {
            current_gamelist[key] = gi;
        }

        pos = game_end;
    }
}

// Carica e mette in cache il gamelist di un sistema specifico
const std::map<std::string, GameInfo>& get_system_gamelist(const std::string& system) {
    auto it = gamelist_cache.find(system);
    if (it != gamelist_cache.end()) return it->second;
    // Carica e metti in cache
    std::map<std::string, GameInfo> old = current_gamelist;
    load_gamelist(gamelist_roms_base + "/" + system);
    gamelist_cache[system] = current_gamelist;
    current_gamelist = old;
    return gamelist_cache[system];
}

// --- SYSTEMS_DESC.XML PARSER ---
// Mappa: system name (lowercase) -> description
std::map<std::string, std::string> systems_desc;

void load_systems_desc() {
    systems_desc.clear();
    std::string path = theme_p() + "systems_desc.xml";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    size_t pos = 0;
    while (pos < content.size()) {
        size_t gs = content.find("<game>", pos);
        if (gs == std::string::npos) break;
        size_t ge = content.find("</game>", gs);
        if (ge == std::string::npos) break;
        ge += 7;
        std::string block = content.substr(gs, ge - gs);
        std::string name = extract_tag(block, "name");
        std::string desc = extract_tag(block, "desc");
        if (!name.empty()) {
            std::string key = name;
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            systems_desc[key] = desc;
        }
        pos = ge;
    }
}

std::string get_system_desc(const std::string& sys_name) {
    std::string key = sys_name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = systems_desc.find(key);
    return (it != systems_desc.end()) ? it->second : "";
}

const GameInfo* find_game_info(const std::string& game_name) {
    std::string key = game_name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = current_gamelist.find(key);
    if (it != current_gamelist.end()) return &it->second;
    return nullptr;
}

// Cerca info gioco nel gamelist di un sistema specifico (per favoriti)
const GameInfo* find_game_info_for_system(const std::string& system, const std::string& game_name) {
    const auto& gl = get_system_gamelist(system);
    std::string key = game_name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = gl.find(key);
    if (it != gl.end()) return &it->second;
    return nullptr;
}

// Funzione helper per word-wrap di una descrizione
std::vector<std::string> wrap_text(TTF_Font* font, const std::string& text, int max_w) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;

    // Sostituisci newline con spazi per wrapping uniforme
    std::string clean = text;
    for (char& c : clean) { if (c == '\n' || c == '\r') c = ' '; }
    // Rimuovi spazi multipli
    std::string collapsed;
    bool last_space = false;
    for (char c : clean) {
        if (c == ' ') { if (!last_space) { collapsed += c; last_space = true; } }
        else { collapsed += c; last_space = false; }
    }

    std::istringstream iss(collapsed);
    std::string word;
    std::string current_line;
    while (iss >> word) {
        std::string test = current_line.empty() ? word : current_line + " " + word;
        int tw = 0;
        if (font) {
            SDL_Surface* s = ttf_render_text_blended(font, test, {255,255,255,255});
            if (s) { tw = s->w; SDL_FreeSurface(s); }
        } else {
            tw = (int)test.size() * 10;
        }
        if (tw > max_w && !current_line.empty()) {
            lines.push_back(current_line);
            current_line = word;
        } else {
            current_line = test;
        }
    }
    if (!current_line.empty()) lines.push_back(current_line);
    return lines;
}

int main(int, char* argv[]) {
#ifdef NATIVE_BASE_PATH
    base_p = get_native_base_path(argv[0]);
    img_p  = base_p + "images/";
#else
    (void)argv;
#endif
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO);
#ifndef CROSS_PLATFORM
    signal(SIGTERM, SIG_IGN); 
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN); 
#endif
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    load_extensions_cfg(base_p + "extensions_cfg.txt");
    load_favs();
    target_spec.freq = 44100; target_spec.format = AUDIO_S16SYS; target_spec.channels = 2; target_spec.samples = 2048; target_spec.callback = NULL;
    audio_device = SDL_OpenAudioDevice(NULL, 0, &target_spec, NULL, 0);
    
    // Open all currently connected joysticks (built-in + any wireless already paired)
    SDL_JoystickEventState(SDL_ENABLE);
    for (int ji = 0; ji < SDL_NumJoysticks(); ji++) SDL_JoystickOpen(ji);
#ifdef NATIVE_BASE_PATH
    SDL_Window* window = SDL_CreateWindow("Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 600, SDL_WINDOW_RESIZABLE);
#else
    SDL_Window* window = SDL_CreateWindow("Launcher", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
#endif
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "Error: SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderSetLogicalSize(renderer, 1024, 600); // Scale to any output resolution (HDMI)
    hdmi_connected = detect_hdmi();

    load_active_theme();          // Load saved theme (or use "default")
    img_p = theme_p() + "images/"; // Update image path to active theme
    load_display_settings();      // Load HUD visibility settings
    theme_cfg = load_theme_config(theme_p() + "theme.cfg"); // Load theme layout/color config
    load_systems_desc();          // Load system descriptions from systems_desc.xml
    load_and_convert_sound((theme_p() + "sounds/click.wav").c_str(), s_click);
    load_and_convert_sound((theme_p() + "sounds/enter.wav").c_str(), s_enter);
    load_and_convert_sound((theme_p() + "sounds/back.wav").c_str(), s_back);
    load_and_convert_sound((theme_p() + "sounds/fav.wav").c_str(), s_fav);

    int screen_w, screen_h; SDL_GetRendererOutputSize(renderer, &screen_w, &screen_h);

    font_tex = load_theme_image(renderer, "font_map.png");
    std::string font_path = find_font_path();
    TTF_Font* font_16 = nullptr;
    TTF_Font* font_20 = nullptr;
    TTF_Font* font_24 = nullptr;
    
    if (ttf_init()) {
        ttf_available = true;
        font_16 = ttf_open_font(font_path, theme_cfg.font_small);
        font_20 = ttf_open_font(font_path, theme_cfg.font_medium);
        font_24 = ttf_open_font(font_path, theme_cfg.font_large);
        if (!font_16 || !font_20 || !font_24) {
            std::cerr << "Warning: TTF font load failed from " << font_path << ". Falling back to bitmap font.\n";
            ttf_quit();
            ttf_available = false;
            font_16 = font_20 = font_24 = nullptr;
        } else {
            font_cache[theme_cfg.font_small]  = font_16;
            font_cache[theme_cfg.font_medium] = font_20;
            font_cache[theme_cfg.font_large]  = font_24;
        }
    } else {
        std::cerr << "Warning: SDL2_ttf not available. Using bitmap font fallback.\n";
    }

    if (!ttf_available && !font_tex) {
        std::cerr << "Error: No usable font available (bitmap and TTF missing).\n";
        return 1;
    }
    
    SDL_Texture *icons_tex = load_theme_image(renderer, "icons.png");
    SDL_Texture *favorite_tex = load_theme_image(renderer, "Favorite.png");
    SDL_Texture *no_art_tex = load_theme_image(renderer, "no_art.png");
    SDL_Texture *list_bg_tex = load_texture_png_jpg(renderer, theme_p() + "bg/list_bg");
    SDL_Texture *help_game_tex = load_theme_image(renderer, "help.png");
    SDL_Texture *help_menu_tex = load_theme_image(renderer, "help_menu.png");
    SDL_Texture *arrow_up_tex = load_theme_image(renderer, "up.png");
    SDL_Texture *arrow_down_tex = load_theme_image(renderer, "down.png");
    SDL_Texture *arrow_left_tex = load_theme_image(renderer, "left.png");
    SDL_Texture *arrow_right_tex = load_theme_image(renderer, "right.png");

            // --- CARICAMENTO ICONE BATTERIA (CORRETTO) ---
    std::vector<SDL_Texture*> batt_icons;
    batt_icons.push_back(load_theme_image(renderer, "Battery 01.png"));
    batt_icons.push_back(load_theme_image(renderer, "Battery 02.png"));
    batt_icons.push_back(load_theme_image(renderer, "Battery 03.png"));
    batt_icons.push_back(load_theme_image(renderer, "Battery 04-.png"));
    batt_icons.push_back(load_theme_image(renderer, "Battery 04.png"));
    SDL_Texture* batt_ch_tex = load_theme_image(renderer, "Battery 05.png");

         // --- CARICAMENTO ICONE WIFI (CORRETTO) ---
    std::vector<SDL_Texture*> wifi_icons;
    wifi_icons.push_back(load_theme_image(renderer, "Wifi_logo0.png"));
    wifi_icons.push_back(load_theme_image(renderer, "Wifi_logo1.png"));
    wifi_icons.push_back(load_theme_image(renderer, "Wifi_logo2.png"));
    wifi_icons.push_back(load_theme_image(renderer, "Wifi_logo3.png"));

    // --- Reload all theme images after a theme change ---
    auto reload_theme_images = [&]() {
        auto rel = [](SDL_Texture*& t) { if (t) { SDL_DestroyTexture(t); t = nullptr; } };
        rel(icons_tex);      icons_tex      = load_theme_image(renderer, "icons.png");
        rel(favorite_tex);   favorite_tex   = load_theme_image(renderer, "Favorite.png");
        rel(no_art_tex);     no_art_tex     = load_theme_image(renderer, "no_art.png");
        rel(help_game_tex);  help_game_tex  = load_theme_image(renderer, "help.png");
        rel(help_menu_tex);  help_menu_tex  = load_theme_image(renderer, "help_menu.png");
        rel(arrow_up_tex);   arrow_up_tex   = load_theme_image(renderer, "up.png");
        rel(arrow_down_tex); arrow_down_tex = load_theme_image(renderer, "down.png");
        rel(arrow_left_tex); arrow_left_tex = load_theme_image(renderer, "left.png");
        rel(arrow_right_tex);arrow_right_tex= load_theme_image(renderer, "right.png");
        rel(batt_ch_tex);    batt_ch_tex    = load_theme_image(renderer, "Battery 05.png");
        for (int i = 0; i < (int)batt_icons.size(); i++) rel(batt_icons[i]);
        batt_icons[0] = load_theme_image(renderer, "Battery 01.png");
        batt_icons[1] = load_theme_image(renderer, "Battery 02.png");
        batt_icons[2] = load_theme_image(renderer, "Battery 03.png");
        batt_icons[3] = load_theme_image(renderer, "Battery 04-.png");
        batt_icons[4] = load_theme_image(renderer, "Battery 04.png");
        for (int i = 0; i < (int)wifi_icons.size(); i++) rel(wifi_icons[i]);
        wifi_icons[0] = load_theme_image(renderer, "Wifi_logo0.png");
        wifi_icons[1] = load_theme_image(renderer, "Wifi_logo1.png");
        wifi_icons[2] = load_theme_image(renderer, "Wifi_logo2.png");
        wifi_icons[3] = load_theme_image(renderer, "Wifi_logo3.png");
        // Rescan music for new theme and restart if enabled
        music_tracks = scan_music_tracks(current_theme);
        music_track_index = 0;
        if (music_enabled && !music_tracks.empty())
            start_music(music_tracks[music_track_index]);
        else
            stop_music();
    };

    int w_idx = 0; // Indice per l'icona attuale
    Uint32 last_w_check = 0;


    int b_idx = 0; 
    bool is_ch = false;
    Uint32 last_b_check = 0;

    // --- CARICAMENTO ICONE VOLUME ---
    SDL_Texture* vol_icons[11] = {};
    for (int i = 0; i <= 10; i++) {
        vol_icons[i] = load_theme_image(renderer, "vol_" + std::to_string(i) + ".png");
    }
    int vol_level = -1;       // Livello volume corrente (0-10)
    Uint32 vol_show_until = 0; // Tick fino a cui mostrare overlay

#ifndef CROSS_PLATFORM
    // Apri /dev/input/eventN in non-blocking per leggere tasti volume
    int vol_fds[8]; int vol_fd_count = 0;
    for (int i = 0; i < 8 && vol_fd_count < 8; i++) {
        char devpath[64];
        snprintf(devpath, sizeof(devpath), "/dev/input/event%d", i);
        int fd = open(devpath, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) { vol_fds[vol_fd_count++] = fd; }
    }

    // Leggi volume iniziale da amixer Master
    {
        FILE* fp = popen("amixer sget Master 2>/dev/null", "r");
        if (fp) {
            char buf[256]; int pct = -1;
            while (fgets(buf, sizeof(buf), fp)) {
                char* p = strstr(buf, "[");
                while (p) {
                    int v = 0;
                    if (sscanf(p, "[%d%%]", &v) == 1) { pct = v; break; }
                    p = strstr(p + 1, "[");
                }
                if (pct >= 0) break;
            }
            pclose(fp);
            if (pct >= 0) { vol_level = (pct + 5) / 10; if (vol_level > 10) vol_level = 10; }
        }
        if (vol_level < 0) vol_level = 7;
    }
#else
    vol_level = 7; // Default on non-Linux
#endif

#ifdef NATIVE_BASE_PATH
    // base_p is .../bin/AGSG_CCF_FE/ — go up 2 levels to reach SD root
    auto go_up = [](const std::string& p, int n) {
        std::string r = p;
        if (!r.empty() && r.back() == '/') r.pop_back();
        for (int i = 0; i < n; i++) {
            size_t sl = r.rfind('/');
            if (sl == std::string::npos) return std::string("./");
            r = r.substr(0, sl);
        }
        return r + "/";
    };
    std::string sd_root = go_up(base_p, 2);
    std::string roms_base = find_subdir_ignore_case(sd_root, "Games");
    if (roms_base.empty()) roms_base = sd_root + "Games";
#else
    std::string roms_base = find_subdir_ignore_case("/sdcard", "Games");
    if (roms_base.empty()) roms_base = "/sdcard/Games"; // fallback
#endif
    gamelist_roms_base = roms_base;
    std::string current_rel = "", current_sys = ""; 
    std::vector<MenuItem> items_all = scan_directory(roms_base, true);
    // Filter systems for HDMI mode
    std::vector<MenuItem> items;
    for (const auto& it : items_all) {
        if (hdmi_connected) {
            // Normalize same way as start_local_sd_HDMI.sh: lowercase + remove spaces
            std::string low = it.name;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            low.erase(std::remove(low.begin(), low.end(), ' '), low.end());
            if (hdmi_supported_systems.count(low) == 0) continue;
        }
        items.push_back(it);
    }
    // Aggiungi FAVORITES all'inizio della lista sistemi
    if (!items.empty()) {
        MenuItem fav_item = {"FAVORITES", true};
        items.insert(items.begin(), fav_item);
    }
    int selected = 0, scroll = 0, last_main_sel = 0, g_count = 0; 
    bool in_games = false, in_favorites = false, running = true;
#ifdef NATIVE_BASE_PATH
    bool show_kb_help = false;
#endif
    SDL_Event event;
    // Lista sistemi per navigazione sinistra/destra nella lista giochi
    std::vector<std::string> system_list;
    for (const auto& it : items) {
        if (it.name != "FAVORITES") system_list.push_back(it.name);
    }
    
    SDL_Texture *bg_cur = nullptr, *dev_cur = nullptr, *dev_prev = nullptr, *dev_next = nullptr, *box_art = nullptr, *ctrl_cur = nullptr;
    bool last_carousel_vertical = theme_cfg.carousel_vertical;
    float slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f; int slide_dir = 1; std::string last_bg = "";
    SDL_Color white = theme_cfg.color_text;
    Uint32 selection_timer = 0, input_delay = 0; 
    bool needs_art_update = false;
    std::string pending_video_path = ""; // video waiting for delay before start
    Uint32 video_start_timer = 0;        // when pending video was queued
    float video_fade_alpha = 1.0f;       // 1.0 = full boxart, 0.0 = full video
    bool video_fading = false;           // fade in progress
    float marquee_pos = -40.0f; float desc_marquee_pos = -2.0f; Uint32 last_tick = SDL_GetTicks();
    // Hold tracking for fast scroll (left/right in carousel, up/down in game list)
    bool left_held = false, right_held = false;
    bool up_held = false, down_held = false;
    Uint32 left_hold_start = 0, right_hold_start = 0;
    Uint32 up_hold_start = 0, down_hold_start = 0;
    Uint32 fast_scroll_next = UINT32_MAX; // Never fires until a directional key is first pressed

    if (!items.empty()) {
        update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
        last_bg = items[selected].name;
        if (items[selected].name == "FAVORITES") {
            g_count = (int)favorites_list.size();
        } else {
            g_count = count_games_recursive(roms_base + "/" + items[selected].name, items[selected].name);
        }
    }

    // Start background music (carousel)
    music_tracks = scan_music_tracks(current_theme);
    music_track_index = 0;
    if (music_enabled && !music_tracks.empty())
        start_music(music_tracks[music_track_index]);

    while (running) {
        Uint32 tick = SDL_GetTicks();
        float dt = (tick - last_tick) / 1000.0f;
        last_tick = tick;

        // --- LOGICA MARQUEE ---
        static int last_selected = -1;
        static std::string last_dn = "";
        int lim_w = theme_cfg.list_text_max_w;
        if (in_games && !items.empty() && selected < (int)items.size()) {
            std::string dn = items[selected].name;
            if (!items[selected].is_dir) {
                size_t dot = dn.find_last_of(".");
                if (dot != std::string::npos) dn = dn.substr(0, dot);
            }
            if (in_favorites && selected < (int)favorites_list.size()) {
                std::string sys = favorites_list[selected].system;
                for (auto& c : sys) c = toupper(c);
                dn = "[" + sys + "] " + dn;
            }
            // Se cambia selezione o nome, resetta marquee
            if (selected != last_selected || dn != last_dn) {
                marquee_pos = -40.0f;
                desc_marquee_pos = -2.0f;
                last_selected = selected;
                last_dn = dn;
            }
            // Calcola larghezza reale con font_24 per coerenza con il rendering
            int tw = 0;
            if (ttf_available && font_24) {
                SDL_Surface* surf = ttf_render_text_blended(font_24, dn.c_str(), {255,255,255,255});
                if (surf) { tw = surf->w; SDL_FreeSurface(surf); }
            }
            if (tw > lim_w) {
                marquee_pos += 60.0f * dt; // Velocità scorrimento
                if (marquee_pos > (tw - lim_w + 40)) {
                    marquee_pos = -40.0f; // Loop
                }
            } else {
                marquee_pos = -40.0f;
            }
        } else {
            marquee_pos = -40.0f;
        }

#ifndef CROSS_PLATFORM
        // --- WIFI CHECK (ogni 5 secondi) ---
        if (tick - last_w_check > 5000 || last_w_check == 0) { 
            std::ifstream f_wifi("/proc/net/wireless");
            std::string line;
            int level = -999;
            if (f_wifi.is_open()) {
                while (std::getline(f_wifi, line)) {
                    if (line.find(":") != std::string::npos) {
                        std::stringstream ss(line);
                        std::string iface, status, link, level_str;
                        ss >> iface >> status >> link >> level_str;
                        try {
                            level_str.erase(std::remove(level_str.begin(), level_str.end(), '.'), level_str.end());
                            if (!level_str.empty()) level = std::stoi(level_str);
                        } catch(...) {}
                    }
                }
                f_wifi.close();
            }
            if (level == -999 || level == 0) {
                w_idx = 0;
            } else if (level < 0) {
                if (level > -55) w_idx = 3;
                else if (level > -75) w_idx = 2;
                else if (level > -90) w_idx = 1;
                else w_idx = 0;
            } else {
                if (level > 60) w_idx = 3;
                else if (level > 35) w_idx = 2;
                else if (level > 10) w_idx = 1;
                else w_idx = 0;
            }
            last_w_check = tick;
        }

        // --- BATTERIA: stato carica (ogni ciclo) ---
        {
            const char* s_paths[] = { "/sys/class/power_supply/battery/status", "/sys/class/power_supply/axp20x-battery/status", "/sys/class/power_supply/BAT0/status" };
            for (const char* p : s_paths) {
                std::ifstream f_stat(p);
                if (f_stat.is_open()) {
                    std::string status; f_stat >> status; f_stat.close();
                    is_ch = (status == "Charging" || status == "Full");
                    break;
                }
            }
        }

        // --- BATTERIA: percentuale (ogni 30 secondi) ---
        if (tick - last_b_check > 30000 || last_b_check == 0) {
            int pct = -1;
            const char* c_paths[] = { "/sys/class/power_supply/battery/capacity", "/sys/class/power_supply/axp20x-battery/capacity", "/sys/class/power_supply/BAT0/capacity" };
            for (const char* p : c_paths) {
                std::ifstream f_cap(p);
                if (f_cap.is_open()) {
                    f_cap >> pct; f_cap.close();
                    break;
                }
            }
            if (pct != -1) {
                if (pct > 80) b_idx = 0;
                else if (pct > 55) b_idx = 1;
                else if (pct > 30) b_idx = 2;
                else if (pct > 10) b_idx = 3;
                else b_idx = 4;
            }
            last_b_check = tick;
        }
#endif // CROSS_PLATFORM

        if (!in_games && !items.empty() && items[selected].name != last_bg) {
            update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
            last_bg = items[selected].name; 
            slide_pos = 0.0f;
            
            // Se è FAVORITES, mostra il numero di favoriti; altrimenti conta i giochi nel sistema
            if (items[selected].name == "FAVORITES") {
                g_count = (int)favorites_list.size();
            } else {
                g_count = count_games_recursive(roms_base + "/" + items[selected].name, items[selected].name);
            }
        }

        if (in_games && needs_art_update && (tick - selection_timer > 200)) {
            if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
            stop_video_preview();
            if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
            pending_video_path = "";
            video_fading = false;
            video_fade_alpha = 1.0f;

            if (!items.empty() && !items[selected].is_dir) {
                std::string g_name = items[selected].name;
                size_t dot = g_name.find_last_of("."); if (dot != std::string::npos) g_name = g_name.substr(0, dot);

                std::string sys_to_search = current_sys;
                if (in_favorites && selected < (int)favorites_list.size()) {
                    sys_to_search = favorites_list[selected].system;
                }

                // 1. Cerca boxart
                std::string boxart_folder = find_subdir_ignore_case(roms_base + "/" + sys_to_search, "boxart");
                if (!boxart_folder.empty()) {
                    std::string found_p = find_file_ignore_case(boxart_folder, g_name);
                    box_art = load_texture(renderer, found_p);
                }

                // 2. Cerca video: se trovato, pianifica avvio dopo delay
                std::string video_path = find_video_file(roms_base, sys_to_search, g_name);
                if (!video_path.empty()) {
                    pending_video_path = video_path;
                    video_start_timer = tick;
                } else if (!box_art) {
                    // No video AND no boxart: try no_art.mp4 from theme images folder, fallback to default theme
                    std::string no_art_vid = img_p + "no_art.mp4";
                    if (access(no_art_vid.c_str(), F_OK) != 0 && current_theme != "default")
                        no_art_vid = base_p + "themes/default/images/no_art.mp4";
                    if (access(no_art_vid.c_str(), F_OK) == 0) {
                        pending_video_path = no_art_vid;
                        video_start_timer = tick;
                    }
                }
            }
            needs_art_update = false;
        }

        // --- AVVIO VIDEO DOPO DELAY ---
        if (!pending_video_path.empty() && (tick - video_start_timer >= (Uint32)theme_cfg.video_delay_ms)) {
            start_video_preview(pending_video_path);
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
            input_delay = tick + 300;
            pending_video_path = "";
            video_fading = true;
            video_fade_alpha = 1.0f;
        }

        // --- AGGIORNAMENTO FADE BOXART→VIDEO ---
        if (video_fading && video_running && video_texture) {
            float fade_step = dt / (theme_cfg.video_fade_ms > 0 ? (float)theme_cfg.video_fade_ms : 500.0f);
            video_fade_alpha -= fade_step;
            if (video_fade_alpha <= 0.0f) {
                video_fade_alpha = 0.0f;
                video_fading = false;
            }
        }

        // Detect vertical/horizontal mode change and reset slide_pos
        if (theme_cfg.carousel_vertical != last_carousel_vertical) {
            last_carousel_vertical = theme_cfg.carousel_vertical;
            slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
        }
        { float slide_max = theme_cfg.carousel_vertical ? 600.0f : 1024.0f; if (slide_pos < slide_max) { slide_pos += theme_cfg.carousel_slide_speed * dt; if (slide_pos > slide_max) slide_pos = slide_max; } }

#ifndef CROSS_PLATFORM
        // --- LEGGI TASTI VOLUME da /dev/input/eventN (FUORI dal loop SDL) ---
        for (int fi = 0; fi < vol_fd_count; fi++) {
            struct input_event ie;
            while (read(vol_fds[fi], &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
                if (ie.type == EV_KEY && ie.value == 1) {
                    if (ie.code == KEY_VOLUMEUP) {
                        if (vol_level < 10) vol_level++;
                        vol_show_until = tick + 2000;
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "amixer sset Master %d%% 2>/dev/null &", vol_level * 10);
                        system(cmd);
                    } else if (ie.code == KEY_VOLUMEDOWN) {
                        if (vol_level > 0) vol_level--;
                        vol_show_until = tick + 2000;
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "amixer sset Master %d%% 2>/dev/null &", vol_level * 10);
                        system(cmd);
                    }
                }
            }
        }
#endif

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { running = false; }
#ifdef NATIVE_BASE_PATH
            if (event.type == SDL_KEYDOWN) {
                // F11: toggle fullscreen
                if (event.key.keysym.sym == SDLK_F11) {
                    static bool is_fullscreen = false;
                    is_fullscreen = !is_fullscreen;
                    SDL_SetWindowFullscreen(window, is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
                // F1: toggle keyboard help overlay
                if (event.key.keysym.sym == SDLK_F1) { show_kb_help = !show_kb_help; }
                // F5: reload theme.cfg without restarting
                if (event.key.keysym.sym == SDLK_F5) {
                    theme_cfg = load_theme_config(theme_p() + "theme.cfg");
                    load_systems_desc();
                    img_p = theme_p() + "images/";
                    if (list_bg_tex) { SDL_DestroyTexture(list_bg_tex); list_bg_tex = nullptr; }
                    list_bg_tex = load_texture_png_jpg(renderer, theme_p() + "bg/list_bg");
                    update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                    slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
                }
                // Key "1": prev music track / Key "3": next music track (carousel only)
                if (!in_games && !music_tracks.empty() && music_enabled && !event.key.repeat) {
                    auto set_track_overlay = [&](int idx) {
                        std::string p = music_tracks[idx];
                        size_t sl = p.rfind('/'); std::string n = (sl != std::string::npos) ? p.substr(sl+1) : p;
                        size_t dot = n.rfind('.'); if (dot != std::string::npos) n = n.substr(0, dot);
                        music_track_name_overlay = n;
                        music_track_name_until = tick + 3000;
                    };
                    if (event.key.keysym.sym == SDLK_1) {
                        music_track_index = (music_track_index - 1 + (int)music_tracks.size()) % (int)music_tracks.size();
                        start_music(music_tracks[music_track_index]);
                        set_track_overlay(music_track_index);
                    } else if (event.key.keysym.sym == SDLK_3) {
                        music_track_index = (music_track_index + 1) % (int)music_tracks.size();
                        start_music(music_tracks[music_track_index]);
                        set_track_overlay(music_track_index);
                    }
                }
                int vbtn = -1;
                switch (event.key.keysym.sym) {
                    case SDLK_UP:     vbtn = 29; break; // D-PAD up
                    case SDLK_DOWN:   vbtn = 32; break; // D-PAD down
                    case SDLK_LEFT:   vbtn = 30; break; // D-PAD left
                    case SDLK_RIGHT:  vbtn = 31; break; // D-PAD right
                    case SDLK_a:      vbtn = 1;  break; // A
                    case SDLK_b:      vbtn = 3;  break; // B
                    case SDLK_y:      vbtn = 4;  break; // Y
                    case SDLK_l:      vbtn = 2;  break; // L1
                    case SDLK_r:      vbtn = 5;  break; // R1
                    case SDLK_RETURN: vbtn = 1;  break; // Enter = A
                    case SDLK_ESCAPE:
                        if (show_kb_help) { show_kb_help = false; }
                        else { vbtn = 3; } // Esc = B (only if overlay not open)
                        break;
                    case SDLK_q:      vbtn = 28; break; // Q = HOME/exit
                    case SDLK_LSHIFT:
                    case SDLK_RSHIFT: vbtn = 6;  break; // Shift = SELECT (theme selector)
                    default: break;
                }
                // Set held flags directly on first press (not via fake event, to avoid race with KEYUP)
                if (!event.key.repeat) {
                    if (event.key.keysym.sym == SDLK_LEFT)  { left_held  = true; left_hold_start  = tick; fast_scroll_next = tick + 2000; }
                    if (event.key.keysym.sym == SDLK_RIGHT) { right_held = true; right_hold_start = tick; fast_scroll_next = tick + 2000; }
                    if (event.key.keysym.sym == SDLK_UP)    { up_held    = true; up_hold_start    = tick; fast_scroll_next = tick + 2000; }
                    if (event.key.keysym.sym == SDLK_DOWN)  { down_held  = true; down_hold_start  = tick; fast_scroll_next = tick + 2000; }
                }
                // Don't push fake event for repeated arrow keys (held flags already set)
                bool is_directional = (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_DOWN ||
                                       event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT);
                if (vbtn >= 0 && !(event.key.repeat && is_directional)) {
                    SDL_Event fake;
                    SDL_memset(&fake, 0, sizeof(fake));
                    fake.type = SDL_JOYBUTTONDOWN;
                    fake.jbutton.button = (Uint8)vbtn;
                    SDL_PushEvent(&fake);
                }
            }
            if (event.type == SDL_KEYUP) {
                // Release directional holds on key release
                if (event.key.keysym.sym == SDLK_LEFT)  { left_held  = false; }
                if (event.key.keysym.sym == SDLK_RIGHT) { right_held = false; }
                if (event.key.keysym.sym == SDLK_UP)    { up_held    = false; }
                if (event.key.keysym.sym == SDLK_DOWN)  { down_held  = false; }
            }
#endif
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                // Release all held flags when window loses focus to avoid stuck auto-scroll
                left_held = right_held = up_held = down_held = false;
                fast_scroll_next = UINT32_MAX;
            }
            if (event.type == SDL_JOYDEVICEADDED) {
                SDL_JoystickOpen(event.jdevice.which);
            }
            if (event.type == SDL_JOYBUTTONUP) {
                if (event.jbutton.button == 30) left_held  = false;
                if (event.jbutton.button == 31) right_held = false;
                if (event.jbutton.button == 29) up_held    = false;
                if (event.jbutton.button == 32) down_held  = false;
            }
            if (event.type == SDL_JOYBUTTONDOWN) {
                if (tick < input_delay) continue;
                int btn = event.jbutton.button; marquee_pos = -40.0f;
                // Track directional holds for fast scroll (ARM only; native uses KEYDOWN directly)
                if (btn == 30) { left_held  = true; left_hold_start  = tick; fast_scroll_next = tick + 2000; }
                if (btn == 31) { right_held = true; right_hold_start = tick; fast_scroll_next = tick + 2000; }
                if (btn == 29) { up_held    = true; up_hold_start    = tick; fast_scroll_next = tick + 2000; }
                if (btn == 32) { down_held  = true; down_hold_start  = tick; fast_scroll_next = tick + 2000; }
                if (btn == 28) running = false;
                if (btn == 6 && !in_games) { // SELECT: apri selettore tema
                    stop_video_preview();
                    bool theme_changed = show_theme_selector(renderer, font_path,
                                        &bg_cur, &dev_cur, &dev_prev, &dev_next,
                                        &list_bg_tex, &ctrl_cur, items, selected);
                    if (theme_changed) {
                        reload_theme_images();
                        slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
                    }
                    input_delay = tick + 300;
                    continue;
                }
                if (!in_games) {
                    // Music track prev/next (keypad 1 = btn 20, keypad 3 = btn 22)
                    if (!music_tracks.empty() && music_enabled) {
                        if (btn == 20) { // prev track
                            music_track_index = (music_track_index - 1 + (int)music_tracks.size()) % (int)music_tracks.size();
                            start_music(music_tracks[music_track_index]);
                            // Extract filename without path and extension for overlay
                            std::string p = music_tracks[music_track_index];
                            size_t sl = p.rfind('/'); std::string n = (sl != std::string::npos) ? p.substr(sl+1) : p;
                            size_t dot = n.rfind('.'); if (dot != std::string::npos) n = n.substr(0, dot);
                            music_track_name_overlay = n;
                            music_track_name_until = tick + 3000;
                        } else if (btn == 22) { // next track
                            music_track_index = (music_track_index + 1) % (int)music_tracks.size();
                            start_music(music_tracks[music_track_index]);
                            std::string p = music_tracks[music_track_index];
                            size_t sl = p.rfind('/'); std::string n = (sl != std::string::npos) ? p.substr(sl+1) : p;
                            size_t dot = n.rfind('.'); if (dot != std::string::npos) n = n.substr(0, dot);
                            music_track_name_overlay = n;
                            music_track_name_until = tick + 3000;
                        }
                    }
                    // --- REVERSED SLIDE DIRECTIONS HERE ---
                    if (theme_cfg.carousel_vertical) {
                        if (btn == 32) { selected++; slide_dir = 1; play_sound(s_click); }        // D-PAD down
                        else if (btn == 29) { selected--; slide_dir = -1; play_sound(s_click); }  // D-PAD up
                    } else {
                        if (btn == 31) { selected++; slide_dir = 1; play_sound(s_click); }        // Right btn
                        else if (btn == 30) { selected--; slide_dir = -1; play_sound(s_click); }  // Left btn
                    }
                    if (btn == 5) { selected += 5; slide_dir = 1; play_sound(s_click); input_delay = tick + 150; } 
                    else if (btn == 2) { selected -= 5; slide_dir = -1; play_sound(s_click); input_delay = tick + 150; }
                    if (btn == 1 && !items.empty()) { 
                        play_sound(s_enter); last_main_sel = selected; current_sys = items[selected].name;
                        
                        // GESTIONE SPECIALE PER FAVORITES
                        if (items[selected].name == "FAVORITES") {
                            in_favorites = true;
                            items = load_favorites_as_items();
                            current_rel = "";
                            current_sys = "";
                            current_gamelist.clear();
                        } else {
                            in_favorites = false;
                            current_rel = "/" + current_sys;
                            items = scan_directory(roms_base + current_rel, false, current_sys);
                            load_gamelist(roms_base + "/" + current_sys);
                        }
                        
                        selected = 0; scroll = 0; in_games = true; needs_art_update = true; selection_timer = tick;
                        stop_music(); // Stop music when entering game list
                    }
                } else {
                    if (btn == 4) { // Tasto Y: Aggiungi/Rimuovi Preferito
                        if (!items.empty() && !items[selected].is_dir) {
                            std::string game_id;
                            
                            if (in_favorites) {
                                // Siamo nella lista preferiti: rimuovi il favorito
                                if (selected < (int)favorites_list.size()) {
                                    game_id = favorites_list[selected].game_id;
                                    // Rimuovi dalla lista
                                    favorites_list.erase(favorites_list.begin() + selected);
                                    selected = std::max(0, selected - 1);
                                    // Ricreaload items
                                    items = load_favorites_as_items();
                                }
                            } else {
                                // Siamo in un sistema normale: aggiungi/rimuovi favorito
                                // Costruisci il game_id senza duplicare il sistema
                                std::string rel_path = current_rel.substr(("/" + current_sys).length());
                                game_id = current_sys + rel_path + "/" + items[selected].name;
                                
                                // Controlla se è già nei favoriti
                                bool already_fav = false;
                                for (int i = 0; i < (int)favorites_list.size(); i++) {
                                    if (favorites_list[i].game_id == game_id) {
                                        favorites_list.erase(favorites_list.begin() + i);
                                        already_fav = true;
                                        break;
                                    }
                                }
                                
                                // Se non era nei favoriti, aggiungilo
                                if (!already_fav) {
                                    std::string game_name = items[selected].name;
                                    std::string display = game_name;
                                    size_t dot = display.find_last_of(".");
                                    if (dot != std::string::npos) display = display.substr(0, dot);
                                    favorites_list.push_back({game_id, current_sys, display});
                                }
                            }
                            
                            save_favs();
                            play_sound(s_fav);
                        }
                    }

                    if (btn == 32) { selected++; play_sound(s_click); needs_art_update = true; selection_timer = tick; } 
                    else if (btn == 29) { selected--; play_sound(s_click); needs_art_update = true; selection_timer = tick; }
                    else if (btn == 5) { selected += 10; play_sound(s_click); needs_art_update = true; selection_timer = tick; input_delay = tick + 150; }
                    else if (btn == 2) { selected -= 10; play_sound(s_click); needs_art_update = true; selection_timer = tick; input_delay = tick + 150; }

                    // Cambio sistema con destra/sinistra (include FAVORITES)
                    if ((btn == 31 || btn == 30) && !system_list.empty()) {
                        // Indice corrente nella lista completa (FAVORITES=0, poi sistemi 1..N)
                        int cur_idx = 0; // default FAVORITES
                        if (!in_favorites) {
                            for (int si = 0; si < (int)system_list.size(); si++) {
                                if (system_list[si] == current_sys) { cur_idx = si + 1; break; }
                            }
                        }
                        int total = (int)system_list.size() + 1; // +1 per FAVORITES
                        if (btn == 31) cur_idx = (cur_idx + 1) % total;
                        else cur_idx = (cur_idx - 1 + total) % total;

                        stop_video_preview();
                        if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                        if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }

                        if (cur_idx == 0) {
                            // Vai a FAVORITES
                            in_favorites = true;
                            items = load_favorites_as_items();
                            current_rel = "";
                            current_sys = "";
                            current_gamelist.clear();
                            last_main_sel = 0;
                        } else {
                            // Vai a sistema normale
                            in_favorites = false;
                            current_sys = system_list[cur_idx - 1];
                            current_rel = "/" + current_sys;
                            items = scan_directory(roms_base + current_rel, false, current_sys);
                            load_gamelist(roms_base + "/" + current_sys);
                            last_main_sel = cur_idx;
                        }
                        selected = 0; scroll = 0;
                        needs_art_update = true; selection_timer = tick;
                        play_sound(s_click);
                    }
                    if (btn == 1 && !items.empty()) {
                        if (items[selected].is_dir) {
                            play_sound(s_enter); current_rel += "/" + items[selected].name;
                            items = scan_directory(roms_base + current_rel, false, current_sys); selected = 0; scroll = 0;
                            needs_art_update = true; selection_timer = tick;
                        } else {
                            stop_video_preview();
                            if (in_favorites && selected < (int)favorites_list.size()) {
                                // Lancio dal sistema FAVORITES
                                const FavoriteGame& fav = favorites_list[selected];
                                std::string full_path = roms_base + "/" + fav.game_id;
                                std::cerr << "[DEBUG] Lancio FAVORITES: " << full_path << std::endl;
                                system(("sh /sdcard/start_local_sd.sh \"\" \"\" \"\" \"" + full_path + "\"").c_str());
                            } else {
                                // Lancio dal sistema normale
                                std::string full_path = roms_base + current_rel + "/" + items[selected].name;
                                std::cerr << "[DEBUG] Lancio SISTEMA: " << full_path << std::endl;
                                system(("sh /sdcard/start_local_sd.sh \"\" \"\" \"\" \"" + full_path + "\"").c_str());
                            }

                            // --- RIPRISTINO DISPLAY POST-GIOCO ---
                            // Retroarch releases HDMI on exit; force SDL to re-acquire it
                            SDL_Delay(800);
                            SDL_SetWindowFullscreen(window, 0);
                            SDL_Delay(300);
                            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                            SDL_RenderSetLogicalSize(renderer, 1024, 600);
                            SDL_RaiseWindow(window);
                            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                            SDL_RenderClear(renderer);
                            SDL_RenderPresent(renderer);
                            SDL_PumpEvents();
                            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
                            last_tick = SDL_GetTicks();
                            input_delay = last_tick + 300;
                            needs_art_update = true;
                            // ----------------------------------------

                            // --- RIPRISTINO LED LAUNCHER ---
                            system("echo 0 > /sys/class/leds/key_led1/brightness");
                            system("echo 0 > /sys/class/leds/key_led3/brightness");
                            system("echo 0 > /sys/class/leds/key_led4/brightness");
                            system("echo 0 > /sys/class/leds/key_led5/brightness");
                            system("echo 255 > /sys/class/leds/key_led15/brightness");
                            system("echo 255 > /sys/class/leds/key_led16/brightness");
                            system("echo 255 > /sys/class/leds/key_led17/brightness");
                            system("echo 255 > /sys/class/leds/key_led18/brightness");
                            system("echo 255 > /sys/class/leds/key_led19/brightness");
                            system("echo 255 > /sys/class/leds/key_led20/brightness");
                            system("echo 255 > /sys/class/leds/key_led22/brightness");
                            // --------------------------------
                        }
                    }

                    if (btn == 3) { 
                        play_sound(s_back); 
                        
                        if (in_favorites) {
                            // Tornare dai favoriti al menu principale
                            in_games = false;
                            in_favorites = false;
                            current_rel = "";
                            current_sys = "";
                            items = scan_directory(roms_base, true);
                            // Re-inserisci FAVORITES all'inizio
                            if (!items.empty()) {
                                MenuItem fav_item = {"FAVORITES", true};
                                items.insert(items.begin(), fav_item);
                            }
                            selected = last_main_sel;
                            last_bg = "";
                            stop_video_preview();
                            if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                            if (box_art) SDL_DestroyTexture(box_art);
                            box_art = nullptr;
                            update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                            if (music_enabled && !music_tracks.empty()) start_music(music_tracks[music_track_index]);
                        } else {
                            // Tornare normale (dalle cartelle o dalla lista giochi)
                            size_t ls = current_rel.find_last_of("/");
                            std::string parent = (ls == 0 || ls == std::string::npos) ? "" : current_rel.substr(0, ls);
                            if (parent.empty()) { 
                                in_games = false; 
                                current_rel = ""; 
                                items = scan_directory(roms_base, true);
                                // Re-inserisci FAVORITES all'inizio
                                if (!items.empty()) {
                                    MenuItem fav_item = {"FAVORITES", true};
                                    items.insert(items.begin(), fav_item);
                                }
                                selected = last_main_sel; 
                                last_bg = ""; 
                                stop_video_preview();
                                if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                                if (box_art) SDL_DestroyTexture(box_art); 
                                box_art = nullptr; 
                                update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                                if (music_enabled && !music_tracks.empty()) start_music(music_tracks[music_track_index]);
                            } else { 
                                current_rel = parent; 
                                items = scan_directory(roms_base + current_rel, false, current_sys);
                                stop_video_preview();
                                if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                                if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                                selected = 0; 
                            }
                        }
                        
                        scroll = 0; 
                        needs_art_update = true;
                    }
                }
            }
        }

        // Fast scroll: directional button held for more than 2 seconds
        if (!items.empty() && tick >= fast_scroll_next) {
            if (!in_games) {
                // Carousel fast scroll (direction depends on vertical mode)
                bool prev_held  = theme_cfg.carousel_vertical ? up_held        : left_held;
                bool next_held  = theme_cfg.carousel_vertical ? down_held      : right_held;
                Uint32 prev_start = theme_cfg.carousel_vertical ? up_hold_start : left_hold_start;
                Uint32 next_start = theme_cfg.carousel_vertical ? down_hold_start : right_hold_start;
                if (prev_held && (tick - prev_start > 2000)) {
                    selected--; slide_dir = -1;
                    play_sound(s_click);
                    fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
                } else if (next_held && (tick - next_start > 2000)) {
                    selected++; slide_dir = 1;
                    play_sound(s_click);
                    fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
                }
            } else {
                // Game list: up/down fast scroll
                if (up_held && (tick - up_hold_start > 2000)) {
                    selected--;
                    needs_art_update = true; selection_timer = tick;
                    play_sound(s_click);
                    fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
                } else if (down_held && (tick - down_hold_start > 2000)) {
                    selected++;
                    needs_art_update = true; selection_timer = tick;
                    play_sound(s_click);
                    fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
                }
            }
        }

        if (!items.empty()) {
            if (selected < 0) {
                selected = (int)items.size() - 1;
            }
            if (selected >= (int)items.size()) {
                selected = 0;
            }
            if (selected < scroll) {
                scroll = selected;
            }
            if (selected >= scroll + theme_cfg.list_rows) {
                scroll = selected - (theme_cfg.list_rows - 1);
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
     // fine 3° parte   
        if (!in_games) {
            if (bg_cur) { SDL_Rect r = { 0, 0, 1024, 600 }; SDL_RenderCopy(renderer, bg_cur, NULL, &r); }
            auto draw_device = [&](SDL_Texture* d, int x_center, int y_center, float scale, float alpha) {
                if (!d) return;
                int dw, dh; SDL_QueryTexture(d, NULL, NULL, &dw, &dh);
                float scale_w = (float)theme_cfg.carousel_device_base_w / dw;
                float scale_h = (theme_cfg.carousel_device_base_h > 0) ? ((float)theme_cfg.carousel_device_base_h / dh) : scale_w;
                float final_scale = std::min(scale_w, scale_h) * scale;
                int tw = (int)(dw * final_scale); int th = (int)(dh * final_scale);
                int dx = x_center - (tw / 2), dy = y_center - (th / 2);
                // Device shadow
                if (theme_cfg.carousel_device_shadow_alpha > 0) {
                    SDL_SetTextureColorMod(d, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                    SDL_SetTextureAlphaMod(d, (Uint8)(255 * alpha * theme_cfg.carousel_device_shadow_alpha / 255.0f));
                    SDL_Rect sr = { dx + theme_cfg.shadow_offset_x, dy + theme_cfg.shadow_offset_y, tw, th };
                    SDL_RenderCopy(renderer, d, NULL, &sr);
                    SDL_SetTextureColorMod(d, 255, 255, 255);
                }
                SDL_SetTextureAlphaMod(d, (Uint8)(255 * alpha));
                SDL_Rect dr = { dx, dy, tw, th };
                SDL_RenderCopy(renderer, d, NULL, &dr);
            };
            if (theme_cfg.carousel_vertical) {
                float slide_max = 600.0f;
                float anim_off = (slide_max - slide_pos) * slide_dir;
                draw_device(dev_prev, theme_cfg.carousel_cur_x, theme_cfg.carousel_prev_y + (int)anim_off, theme_cfg.carousel_side_scale, theme_cfg.carousel_side_alpha);
                draw_device(dev_next, theme_cfg.carousel_cur_x, theme_cfg.carousel_next_y + (int)anim_off, theme_cfg.carousel_side_scale, theme_cfg.carousel_side_alpha);
                draw_device(dev_cur,  theme_cfg.carousel_cur_x, theme_cfg.carousel_y_center + (int)anim_off, 1.0f, 1.0f);
            } else {
                float anim_off = (1024.0f - slide_pos) * slide_dir;
                draw_device(dev_prev, theme_cfg.carousel_prev_x + (int)anim_off, theme_cfg.carousel_y_center, theme_cfg.carousel_side_scale, theme_cfg.carousel_side_alpha);
                draw_device(dev_next, theme_cfg.carousel_next_x + (int)anim_off, theme_cfg.carousel_y_center, theme_cfg.carousel_side_scale, theme_cfg.carousel_side_alpha);
                draw_device(dev_cur,  theme_cfg.carousel_cur_x  + (int)anim_off, theme_cfg.carousel_y_center, 1.0f, 1.0f);
            }
            // Mostra solo il nome del sistema in alto a sinistra
            if (show_system_name && !items.empty()) draw_text(renderer, font_24, items[selected].name, theme_cfg.carousel_sys_name_x, theme_cfg.carousel_sys_name_y, theme_cfg.font_large, white);
            // Mostra il numero di giochi centrato a y=400
            if (!items.empty()) {
                std::string games_str = std::to_string(g_count) + " Games";
                int text_w = 0;
                if (font_24 && ttf_available) {
                    SDL_Surface* surf = ttf_render_text_blended(font_24, games_str, white);
                    if (surf) {
                        text_w = surf->w;
                        SDL_FreeSurface(surf);
                    }
                } else {
                    // fallback: stima larghezza carattere * num caratteri
                    text_w = (int)games_str.size() * 14;
                }
                int center_x = (theme_cfg.carousel_games_count_x < 0) ? (512 - text_w / 2) : theme_cfg.carousel_games_count_x;
                draw_text(renderer, font_24, games_str, center_x, theme_cfg.carousel_games_count_y, theme_cfg.font_large, white);
            }
            // Controller image
            if (theme_cfg.carousel_show_controller && ctrl_cur) {
                int cw, ch; SDL_QueryTexture(ctrl_cur, NULL, NULL, &cw, &ch);
                float scale = std::min((float)theme_cfg.carousel_ctrl_max_w / cw, (float)theme_cfg.carousel_ctrl_max_h / ch);
                int dw = (int)(cw * scale), dh = (int)(ch * scale);
                int cx = (theme_cfg.carousel_ctrl_x < 0) ? (512 - dw / 2) : theme_cfg.carousel_ctrl_x;
                // Controller shadow
                if (theme_cfg.carousel_ctrl_shadow_alpha > 0) {
                    SDL_SetTextureColorMod(ctrl_cur, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                    SDL_SetTextureAlphaMod(ctrl_cur, (Uint8)(255 * theme_cfg.carousel_ctrl_alpha * theme_cfg.carousel_ctrl_shadow_alpha / 255.0f));
                    SDL_Rect sr = { cx + theme_cfg.shadow_offset_x, theme_cfg.carousel_ctrl_y + theme_cfg.shadow_offset_y, dw, dh };
                    SDL_RenderCopy(renderer, ctrl_cur, NULL, &sr);
                    SDL_SetTextureColorMod(ctrl_cur, 255, 255, 255);
                }
                SDL_SetTextureAlphaMod(ctrl_cur, (Uint8)(255 * theme_cfg.carousel_ctrl_alpha));
                SDL_Rect cr = { cx, theme_cfg.carousel_ctrl_y, dw, dh };
                SDL_RenderCopy(renderer, ctrl_cur, NULL, &cr);
                SDL_SetTextureAlphaMod(ctrl_cur, 255);
            }
            // System description
            if (theme_cfg.carousel_show_desc && font_20 && !items.empty()) {
                std::string desc = get_system_desc(items[selected].name);
                if (!desc.empty()) {
                    SDL_Color dc = theme_cfg.carousel_desc_color;
                    // Word-wrap within carousel_desc_max_w / carousel_desc_max_h
                    int max_w = theme_cfg.carousel_desc_max_w;
                    int max_h = theme_cfg.carousel_desc_max_h;
                    int lh    = theme_cfg.carousel_desc_line_h;
                    int dy    = theme_cfg.carousel_desc_y;
                    // Simple word-wrap
                    std::vector<std::string> lines;
                    std::istringstream stream(desc);
                    std::string word, line_buf;
                    while (stream >> word) {
                        std::string test = line_buf.empty() ? word : line_buf + " " + word;
                        int tw = 0;
                        if (font_20 && ttf_available) {
                            SDL_Surface* ts = ttf_render_text_blended(font_20, test, dc);
                            if (ts) { tw = ts->w; SDL_FreeSurface(ts); }
                        } else { tw = (int)test.size() * 10; }
                        if (tw > max_w && !line_buf.empty()) {
                            lines.push_back(line_buf);
                            line_buf = word;
                        } else { line_buf = test; }
                    }
                    if (!line_buf.empty()) lines.push_back(line_buf);
                    for (int li = 0; li < (int)lines.size() && (li * lh) < max_h; li++) {
                        draw_text(renderer, font_20, lines[li], theme_cfg.carousel_desc_x, dy + li * lh, theme_cfg.font_medium, dc);
                    }
                }
            }
        } else {
            if (list_bg_tex) SDL_RenderCopy(renderer, list_bg_tex, NULL, NULL);

            // Mostra logo del sistema o fallback testo
            std::string system_label = in_favorites ? "FAVORITES" : current_sys;
            std::string logo_path = show_system_logo ? find_file_ignore_case(theme_p() + "logos", system_label) : "";
            if (!logo_path.empty()) {
                SDL_Texture* logo_tex = load_texture(renderer, logo_path);
                if (logo_tex) {
                    int lw, lh; SDL_QueryTexture(logo_tex, NULL, NULL, &lw, &lh);
                    float scale = std::min((float)theme_cfg.logo_max_h / lh, (float)theme_cfg.logo_max_w / lw);
                    int dw = (int)(lw * scale), dh = (int)(lh * scale);
                    // Shadow
                    if (theme_cfg.shadows) {
                        SDL_SetTextureColorMod(logo_tex, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                        SDL_SetTextureAlphaMod(logo_tex, (Uint8)theme_cfg.logo_shadow_alpha);
                        SDL_Rect lr_sh = { theme_cfg.logo_x + theme_cfg.shadow_offset_x, theme_cfg.logo_y + theme_cfg.shadow_offset_y, dw, dh };
                        SDL_RenderCopy(renderer, logo_tex, NULL, &lr_sh);
                    }
                    // Logo
                    SDL_SetTextureColorMod(logo_tex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(logo_tex, 255);
                    SDL_Rect lr = { theme_cfg.logo_x, theme_cfg.logo_y, dw, dh };
                    SDL_RenderCopy(renderer, logo_tex, NULL, &lr);
                    SDL_DestroyTexture(logo_tex);
                } else {
                    draw_text(renderer, font_20, "System: " + system_label, theme_cfg.logo_x, theme_cfg.counter_y, theme_cfg.font_medium, white);
                }
            } else {
                draw_text(renderer, font_20, "System: " + system_label, theme_cfg.logo_x, theme_cfg.counter_y, theme_cfg.font_medium, white);
            }

            if (!items.empty()) {
                bool sel_is_dir = items[selected].is_dir;
                std::string counter_str = std::to_string(selected+1) + " of " + std::to_string(items.size()) + (sel_is_dir ? " Directories" : " Games");
                int cx = theme_cfg.counter_x;
                if (cx < 0) {
                    int tw = 0;
                    if (font_20 && ttf_available) {
                        SDL_Surface* ts = ttf_render_text_blended(font_20, counter_str, white);
                        if (ts) { tw = ts->w; SDL_FreeSurface(ts); }
                    } else {
                        tw = (int)counter_str.size() * 12;
                    }
                    cx = 512 - tw / 2;
                }
                draw_text(renderer, font_20, counter_str, cx, theme_cfg.counter_y, theme_cfg.font_medium, white);
            }

            // --- AGGIORNAMENTO FRAME VIDEO ---
            if (video_running && video_frame_ready) {
                std::lock_guard<std::mutex> lock(video_mutex);
                if (!video_frame_buffer.empty() && video_frame_w > 0 && video_frame_h > 0) {
                    // Recreate texture if dimensions changed (different video)
                    if (video_texture) {
                        int tw, th; SDL_QueryTexture(video_texture, NULL, NULL, &tw, &th);
                        if (tw != video_frame_w || th != video_frame_h) {
                            SDL_DestroyTexture(video_texture); video_texture = nullptr;
                        }
                    }
                    if (!video_texture) {
                        video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                            SDL_TEXTUREACCESS_STREAMING, video_frame_w, video_frame_h);
                    }
                    if (video_texture) {
                        SDL_UpdateTexture(video_texture, NULL, video_frame_buffer.data(), video_frame_w * 4);
                    }
                    video_frame_ready = false;
                }
            }

            // --- RENDERING BOXART / VIDEO / NO_ART ---
            {
                // Calculate destination rect based on texture aspect ratio
                auto calc_art_rect = [&](SDL_Texture* tex, int& dx, int& dy, int& tw, int& th) {
                    int aw, ah; SDL_QueryTexture(tex, NULL, NULL, &aw, &ah);
                    float asp = (float)aw/ah;
                    if (asp > 1.33f) { tw = theme_cfg.boxart_max_w; th = (int)(theme_cfg.boxart_max_w/asp); }
                    else             { th = theme_cfg.boxart_max_h; tw = (int)(theme_cfg.boxart_max_h*asp); }
                    dx = theme_cfg.boxart_area_x + (theme_cfg.boxart_max_w - tw) / 2;
                    dy = theme_cfg.boxart_area_y + (theme_cfg.boxart_max_h - th) / 2;
                };

                int bp = theme_cfg.boxart_border_padding;
                auto draw_border = [&](int dx, int dy, int tw, int th) {
                    SDL_SetRenderDrawColor(renderer, theme_cfg.color_boxart_border.r, theme_cfg.color_boxart_border.g, theme_cfg.color_boxart_border.b, theme_cfg.color_boxart_border.a);
                    SDL_Rect br = { dx - bp, dy - bp, tw + bp*2, th + bp*2 };
                    SDL_RenderFillRect(renderer, &br);
                };

                if (video_texture && video_running) {
                    // Video is ready: border always follows VIDEO dimensions
                    int vdx, vdy, vtw, vth;
                    calc_art_rect(video_texture, vdx, vdy, vtw, vth);
                    draw_border(vdx, vdy, vtw, vth);

                    // If fading: boxart fades out (at its own rect, behind the video)
                    if (video_fading && box_art) {
                        int bdx, bdy, btw, bth;
                        calc_art_rect(box_art, bdx, bdy, btw, bth);
                        SDL_SetTextureAlphaMod(box_art, (Uint8)(video_fade_alpha * 255));
                        SDL_Rect rba = { bdx, bdy, btw, bth };
                        SDL_RenderCopy(renderer, box_art, NULL, &rba);
                        SDL_SetTextureAlphaMod(box_art, 255);
                    }

                    // Video fades in or is fully shown
                    Uint8 va = video_fading ? (Uint8)((1.0f - video_fade_alpha) * 255) : 255;
                    SDL_SetTextureAlphaMod(video_texture, va);
                    SDL_Rect rvid = { vdx, vdy, vtw, vth };
                    SDL_RenderCopy(renderer, video_texture, NULL, &rvid);
                    SDL_SetTextureAlphaMod(video_texture, 255);

                } else if (!video_running && !video_fading) {
                    // No video at all: show boxart or no_art with matching border
                    SDL_Texture* art = box_art ? box_art : no_art_tex;
                    if (art) {
                        int dx, dy, tw, th;
                        calc_art_rect(art, dx, dy, tw, th);
                        draw_border(dx, dy, tw, th);
                        SDL_Rect rb = { dx, dy, tw, th };
                        SDL_RenderCopy(renderer, art, NULL, &rb);
                    }
                } else if (box_art) {
                    // Video starting but first frame not yet decoded: show boxart
                    int dx, dy, tw, th;
                    calc_art_rect(box_art, dx, dy, tw, th);
                    draw_border(dx, dy, tw, th);
                    SDL_Rect rb = { dx, dy, tw, th };
                    SDL_RenderCopy(renderer, box_art, NULL, &rb);
                }
            }
            // --- RENDERING GAME NAME + DESCRIPTION ---
            if (!items.empty() && !items[selected].is_dir) {
                std::string gname = items[selected].name;
                size_t dot = gname.find_last_of('.');
                if (dot != std::string::npos) gname = gname.substr(0, dot);
                const GameInfo* gi = nullptr;
                if (in_favorites && selected < (int)favorites_list.size()) {
                    gi = find_game_info_for_system(favorites_list[selected].system, gname);
                } else {
                    gi = find_game_info(gname);
                }
                if (gi) {
                // Game name in configured favorite/accent color
                std::string display_name = !gi->name.empty() ? gi->name : gname;
                draw_text(renderer, font_20, display_name, theme_cfg.game_name_x, theme_cfg.game_name_y, theme_cfg.font_medium, theme_cfg.color_favorite);
                // Description with vertical scroll
                if (!gi->desc.empty()) {
                    int desc_x = theme_cfg.desc_x, desc_y = theme_cfg.desc_y, desc_max_w = theme_cfg.desc_max_w;
                    int desc_area_h = theme_cfg.desc_area_h, line_h = theme_cfg.desc_line_h;
                    int max_visible = desc_area_h / line_h;
                    std::vector<std::string> lines = wrap_text(font_16, gi->desc, desc_max_w);
                    int total_lines = (int)lines.size();
                    // Scroll verticale automatico se troppe righe
                    int scroll_offset = 0;
                    if (total_lines > max_visible) {
                        int extra = total_lines - max_visible;
                        desc_marquee_pos += theme_cfg.desc_scroll_speed * dt;
                        float cycle = (float)extra + 3.0f;
                        if (desc_marquee_pos > cycle) desc_marquee_pos = -2.0f;
                        float eff = desc_marquee_pos < 0 ? 0 : desc_marquee_pos;
                        if (eff > (float)extra) eff = (float)extra;
                        scroll_offset = (int)(eff * line_h);
                    }
                    SDL_Rect clip = { desc_x, desc_y, desc_max_w, desc_area_h };
                    SDL_RenderSetClipRect(renderer, &clip);
                    for (int li = 0; li < total_lines; li++) {
                        int ly_d = desc_y + li * line_h - scroll_offset;
                        if (ly_d + line_h < desc_y || ly_d > desc_y + desc_area_h) continue;
                        draw_text(renderer, font_16, lines[li], desc_x, ly_d, theme_cfg.font_small, theme_cfg.color_desc);
                    }
                    SDL_RenderSetClipRect(renderer, NULL);
                }
                } // end if (gi)
            }
            // --- LIST SETTINGS FROM THEME CONFIG ---
            int num_rows = theme_cfg.list_rows;
            int row_height = theme_cfg.list_row_height;
            int ly = theme_cfg.list_y_start, sb_h = num_rows * row_height;
            SDL_SetRenderDrawColor(renderer, theme_cfg.color_scrollbar_bg.r, theme_cfg.color_scrollbar_bg.g, theme_cfg.color_scrollbar_bg.b, theme_cfg.color_scrollbar_bg.a);
            SDL_Rect sb_rect = { theme_cfg.scrollbar_x, ly, theme_cfg.scrollbar_w, sb_h }; SDL_RenderFillRect(renderer, &sb_rect);
            if (!items.empty()) {
                float vr = (float)num_rows / items.size(); int th = (int)(sb_h * (vr > 1.0f ? 1.0f : vr)); if (th < 20) th = 20;
                int ty = ly + (int)((sb_h - th) * ((float)scroll / (items.size() > num_rows ? (items.size() - num_rows) : 1)));
                SDL_SetRenderDrawColor(renderer, theme_cfg.color_scrollbar_thumb.r, theme_cfg.color_scrollbar_thumb.g, theme_cfg.color_scrollbar_thumb.b, theme_cfg.color_scrollbar_thumb.a);
                SDL_Rect thm = { theme_cfg.scrollbar_x, ty, theme_cfg.scrollbar_w, th }; SDL_RenderFillRect(renderer, &thm);
            }
            for (int i = 0; i < num_rows && (scroll+i) < (int)items.size(); i++) {
                int idx = scroll+i, yp = ly + (i * row_height);
                std::string dn = items[idx].name;
                if (!items[idx].is_dir) { size_t dot = dn.find_last_of("."); if (dot != std::string::npos) dn = dn.substr(0, dot); }
                if (in_favorites && idx < (int)favorites_list.size()) {
                    std::string sys = favorites_list[idx].system;
                    for (auto& c : sys) c = toupper(c);
                    dn = "[" + sys + "] " + dn;
                }

                // --- CONTROLLO PREFERITO E COLORE ---
                bool is_fav = false;
                if (!in_favorites) {
                    std::string rel_path = current_rel.substr(("/" + current_sys).length());
                    std::string full_id = current_sys + rel_path + "/" + items[idx].name;
                    for (const auto& fav : favorites_list) {
                        if (fav.game_id == full_id) {
                            is_fav = true;
                            break;
                        }
                    }
                }
                SDL_Color textColor = is_fav ? theme_cfg.color_favorite : theme_cfg.color_text;

                // Highlight
                int text_x = theme_cfg.list_text_x;
                if (idx == selected) {
                    SDL_SetRenderDrawColor(renderer, theme_cfg.color_highlight.r, theme_cfg.color_highlight.g, theme_cfg.color_highlight.b, theme_cfg.color_highlight.a);
                    SDL_Rect rs = { text_x, yp+10, theme_cfg.list_highlight_w, theme_cfg.list_highlight_h };
                    SDL_RenderFillRect(renderer, &rs);
                }

                // Icona (Cartella o Controller)
                if (is_fav && favorite_tex && !items[idx].is_dir) {
                    int fav_w = 36, fav_h = 32;
                    float scale = 0.6f; // 60%
                    int draw_w = (int)(fav_w * scale), draw_h = (int)(fav_h * scale);
                    int icon_x = 32;
                    // Allinea centro icona con centro testo (testo 24px, icona draw_h)
                    int text_size = 24;
                    int text_y = yp + (row_height - text_size) / 2 + 2;
                    int icon_y = text_y + (text_size - draw_h) / 2;
                    if (theme_cfg.shadows) {
                        SDL_SetTextureColorMod(favorite_tex, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                        SDL_SetTextureAlphaMod(favorite_tex, (Uint8)theme_cfg.shadow_alpha);
                        SDL_Rect fav_shadow = { icon_x + theme_cfg.shadow_offset_x, icon_y + theme_cfg.shadow_offset_y, draw_w, draw_h };
                        SDL_RenderCopy(renderer, favorite_tex, NULL, &fav_shadow);
                    }
                    SDL_SetTextureColorMod(favorite_tex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(favorite_tex, 255);
                    SDL_Rect fav_rect = { icon_x, icon_y, draw_w, draw_h };
                    SDL_RenderCopy(renderer, favorite_tex, NULL, &fav_rect);
                } else if (icons_tex) {
                    SDL_Rect si = { items[idx].is_dir ? 0 : 56, 0, 56, 56 };
                    int icon_size = 40;
                    int text_size = 24;
                    int text_y = yp + (row_height - text_size) / 2 + 2;
                    int icon_y = text_y + (text_size - icon_size) / 2;
                    SDL_Rect di = { 24, icon_y, icon_size, icon_size };
                    if (theme_cfg.shadows) {
                        SDL_SetTextureColorMod(icons_tex, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                        SDL_SetTextureAlphaMod(icons_tex, (Uint8)theme_cfg.shadow_alpha);
                        SDL_Rect di_shadow = { di.x + theme_cfg.shadow_offset_x, di.y + theme_cfg.shadow_offset_y, di.w, di.h };
                        SDL_RenderCopy(renderer, icons_tex, &si, &di_shadow);
                    }
                    SDL_SetTextureColorMod(icons_tex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(icons_tex, 255);
                    SDL_RenderCopy(renderer, icons_tex, &si, &di);
                }

                // Text
                int lim_w = theme_cfg.list_text_max_w;
                int text_size = theme_cfg.font_large;
                int text_y = yp + (row_height - text_size) / 2 + 2;
                std::string f_dn = dn;
                if (idx != selected) {
                    int tw = 0;
                    SDL_Surface* surf = ttf_render_text_blended(font_24, f_dn.c_str(), textColor);
                    if (surf) { tw = surf->w; SDL_FreeSurface(surf); }
                    if (tw > lim_w) {
                        // Ricerca binaria per trovare quanti caratteri entrano
                        int lo = 0, hi = (int)f_dn.length();
                        while (lo < hi) {
                            int mid = (lo + hi + 1) / 2;
                            std::string test = f_dn.substr(0, mid);
                            SDL_Surface* s2 = ttf_render_text_blended(font_24, test.c_str(), textColor);
                            int t2w = 0;
                            if (s2) { t2w = s2->w; SDL_FreeSurface(s2); }
                            if (t2w <= lim_w) lo = mid;
                            else hi = mid - 1;
                        }
                        if (lo > 3) f_dn = f_dn.substr(0, lo - 3) + "...";
                        else f_dn = "...";
                    }
                }
                if (idx == selected) {
                    int tw = 0;
                    SDL_Surface* surf = ttf_render_text_blended(font_24, dn.c_str(), textColor);
                    if (surf) { tw = surf->w; SDL_FreeSurface(surf); }
                    if (tw > lim_w) {
                        draw_text_scissored(renderer, font_24, dn, text_x, text_y, text_size, lim_w, (marquee_pos < 0 ? 0 : (int)marquee_pos), textColor);
                    } else {
                        draw_text(renderer, font_24, f_dn, text_x, text_y, text_size, textColor);
                    }
                } else {
                    draw_text(renderer, font_24, f_dn, text_x, text_y, text_size, textColor);
                }


            }
        }

        if (show_help_bar) {
        SDL_Texture* active_help = in_games ? help_game_tex : help_menu_tex;
        if (active_help) {
            int wo, ho; SDL_QueryTexture(active_help, NULL, NULL, &wo, &ho);
            float hb_scale  = in_games ? (theme_cfg.helpbar_game_scale  >= 0 ? theme_cfg.helpbar_game_scale  : theme_cfg.helpbar_scale)
                                       : (theme_cfg.helpbar_menu_scale  >= 0 ? theme_cfg.helpbar_menu_scale  : theme_cfg.helpbar_scale);
            int   hb_margin = in_games ? (theme_cfg.helpbar_game_bottom_margin >= 0 ? theme_cfg.helpbar_game_bottom_margin : theme_cfg.helpbar_bottom_margin)
                                       : (theme_cfg.helpbar_menu_bottom_margin >= 0 ? theme_cfg.helpbar_menu_bottom_margin : theme_cfg.helpbar_bottom_margin);
            int   hb_x_cfg  = in_games ? (theme_cfg.helpbar_game_x >= -1 ? theme_cfg.helpbar_game_x : theme_cfg.helpbar_x)
                                       : (theme_cfg.helpbar_menu_x >= -1 ? theme_cfg.helpbar_menu_x : theme_cfg.helpbar_x);
            int hw = (int)(wo * hb_scale), hh = (int)(ho * hb_scale);
            if (hw > 1000) { float cap = 1000.0f / hw; hw = 1000; hh = (int)(hh * cap); }
            int hx = (hb_x_cfg >= 0 ? hb_x_cfg : (screen_w - hw) / 2);
            int hy = screen_h - hh - hb_margin;
            if (theme_cfg.shadows) {
                SDL_SetTextureColorMod(active_help, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b); SDL_SetTextureAlphaMod(active_help, (Uint8)theme_cfg.shadow_alpha);
                SDL_Rect shr = { hx + theme_cfg.shadow_offset_x, hy + theme_cfg.shadow_offset_y, hw, hh }; SDL_RenderCopy(renderer, active_help, NULL, &shr);
            }
            SDL_SetTextureColorMod(active_help, 255, 255, 255); SDL_SetTextureAlphaMod(active_help, 255);
            SDL_Rect mnr = { hx, hy, hw, hh }; SDL_RenderCopy(renderer, active_help, NULL, &mnr);
        } else if (font_20) {
            // Help testuale fallback con tasti simulati
            int btn_r = 11; // raggio cerchio tasto
            int cy = screen_h - 25; // centro verticale
            int px = 0; // cursor x (calcoliamo prima la larghezza totale per centrare)
            TTF_Font* help_font = font_20;

            struct HelpEntry { std::string btn; std::string label; bool is_rect; };
            std::vector<HelpEntry> entries;
            if (in_games) {
                entries = {{"U", " ", false}, {"D", "Navigate  ", false},
                           {"L", " ", false}, {"R", "Change System  ", false},
                           {"A", "Select  ", false}, {"B", "Back  ", false}, {"Y", "Favorite  ", false},
                           {"L1", " ", false}, {"R1", "\xc2\xb1" "10  ", false},
                           {"SELECT", "Options  ", true}, {"HOME", "Exit", true}};
            } else {
                entries = {{"L", " ", false}, {"R", "Change System  ", false},
                           {"L1", " ", false}, {"R1", "\xc2\xb1" "5  ", false},
                           {"A", "Enter  ", false}, {"SELECT", "Options  ", true}, {"HOME", "Exit", true}};
            }

            // Calcola larghezza totale per centrare
            int total_w = 0;
            for (const auto& e : entries) {
                if (!e.btn.empty()) {
                    if (e.is_rect) {
                        SDL_Surface* bs = ttf_render_text_blended(help_font, e.btn, {0,0,0,255});
                        if (bs) { total_w += bs->w + 12 + 6; SDL_FreeSurface(bs); }
                    } else {
                        total_w += btn_r * 2 + 6;
                    }
                }
                SDL_Surface* s = ttf_render_text_blended(help_font, e.label, {255,255,255,255});
                if (s) { total_w += s->w + 4; SDL_FreeSurface(s); }
            }
            px = (screen_w - total_w) / 2;

            for (const auto& e : entries) {
                if (!e.btn.empty()) {
                    if (e.is_rect)
                        px += draw_button_rect(renderer, help_font, e.btn, px, cy, btn_r * 2);
                    else {
                        SDL_Texture* arrow = nullptr;
                        if (e.btn == "U") arrow = arrow_up_tex;
                        else if (e.btn == "D") arrow = arrow_down_tex;
                        else if (e.btn == "L") arrow = arrow_left_tex;
                        else if (e.btn == "R") arrow = arrow_right_tex;
                        px += draw_button(renderer, help_font, e.btn, px, cy, btn_r, arrow);
                    }
                }
                px += draw_help_label(renderer, help_font, e.label, px, cy);
            }
        }
        } // end show_help_bar

              // --- DISEGNO ICONA WIFI SCALATA CON OMBRA ---

        if (show_wifi && w_idx < (int)wifi_icons.size() && wifi_icons[w_idx]) {
           int ww = (int)(theme_cfg.wifi_src_w * theme_cfg.wifi_scale);
           int wh = (int)(theme_cfg.wifi_src_h * theme_cfg.wifi_scale);
           int wx = theme_cfg.wifi_x;
           int wy = theme_cfg.wifi_y;

         // Shadow
         if (theme_cfg.shadows) {
             SDL_SetTextureColorMod(wifi_icons[w_idx], theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
             SDL_SetTextureAlphaMod(wifi_icons[w_idx], (Uint8)theme_cfg.shadow_alpha);
             SDL_Rect w_sh = { wx + theme_cfg.shadow_offset_x, wy + theme_cfg.shadow_offset_y, ww, wh };
             SDL_RenderCopy(renderer, wifi_icons[w_idx], NULL, &w_sh);
         }

        // Icon
        SDL_SetTextureColorMod(wifi_icons[w_idx], 255, 255, 255);
        SDL_SetTextureAlphaMod(wifi_icons[w_idx], 255);
        SDL_Rect w_rc = { wx, wy, ww, wh };
        SDL_RenderCopy(renderer, wifi_icons[w_idx], NULL, &w_rc);
    }


                       // --- DISEGNO BATTERIA SCALATA CON OMBRA ---
        if (show_battery) {
        SDL_Texture* cur_b_tex = is_ch ? batt_ch_tex : (b_idx < (int)batt_icons.size() ? batt_icons[b_idx] : NULL);
        
        if (cur_b_tex) {
            float batt_scale = theme_cfg.battery_scale;
            
            int bw = (int)(theme_cfg.battery_src_w * batt_scale);
            int bh = (int)(theme_cfg.battery_src_h * batt_scale);
            
            int bx = theme_cfg.battery_x; 
            int by = theme_cfg.battery_y;

            // 1. Shadow
            if (theme_cfg.shadows) {
                SDL_SetTextureColorMod(cur_b_tex, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b); 
                SDL_SetTextureAlphaMod(cur_b_tex, (Uint8)theme_cfg.shadow_alpha);
                SDL_Rect b_sh = { bx + theme_cfg.shadow_offset_x, by + theme_cfg.shadow_offset_y, bw, bh }; 
                SDL_RenderCopy(renderer, cur_b_tex, NULL, &b_sh);
            }

            // 2. Icon
            SDL_SetTextureColorMod(cur_b_tex, 255, 255, 255); 
            SDL_SetTextureAlphaMod(cur_b_tex, 255);
            SDL_Rect b_rc = { bx, by, bw, bh }; 
            SDL_RenderCopy(renderer, cur_b_tex, NULL, &b_rc);
        }
        } // end show_battery

        // --- VOLUME OVERLAY ---
        if (tick < vol_show_until && vol_level >= 0 && vol_level <= 10 && vol_icons[vol_level]) {
            SDL_Texture* vtex = vol_icons[vol_level];
            int vw, vh; SDL_QueryTexture(vtex, NULL, NULL, &vw, &vh);
            int vx = (screen_w - vw) / 2;
            int vy = 10;
            SDL_Rect vr = { vx, vy, vw, vh };
            SDL_RenderCopy(renderer, vtex, NULL, &vr);
        }

        // --- MUSIC TRACK NAME OVERLAY ---
        if (tick < music_track_name_until && !music_track_name_overlay.empty() && ttf_available && font_20) {
            int pad = 18, box_h = 48;
            SDL_Color tc = {255, 255, 255, 255};
            int tw = 0, th = 0;
            if (pTTF_SizeUTF8) pTTF_SizeUTF8(font_20, music_track_name_overlay.c_str(), &tw, &th);
            int box_w = tw + pad * 2 + 28; // 28 for music note icon space
            int bx = (1024 - box_w) / 2, by = 600 - box_h - 14;
            // Compute fade-out alpha in last 500ms
            Uint32 remain = music_track_name_until - tick;
            Uint8 alpha = (remain < 500) ? (Uint8)(remain * 255 / 500) : 220;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 10, 10, 30, alpha);
            SDL_Rect bg_r = { bx, by, box_w, box_h };
            SDL_RenderFillRect(renderer, &bg_r);
            SDL_SetRenderDrawColor(renderer, 80, 100, 200, alpha);
            SDL_RenderDrawRect(renderer, &bg_r);
            SDL_Color col = {255, 220, 80, alpha};
            draw_text(renderer, font_20, ("\xe2\x99\xab " + music_track_name_overlay).c_str(), bx + pad, by + (box_h - th) / 2, 20, col);
        }

#ifdef NATIVE_BASE_PATH
        if (show_kb_help && font_20) {
            struct KbEntry { const char* key; const char* action; };
            static const KbEntry entries[] = {
                { "Arrow Keys",   "Navigate (D-PAD)" },
                { "A",            "Select / Enter" },
                { "B / Esc",      "Back" },
                { "Y",            "Toggle Favorite" },
                { "Enter",        "Select / Enter" },
                { "L",            "L1 (-10 games)" },
                { "R",            "R1 (+10 games)" },
                { "Shift",        "SELECT (Theme Selector)" },
                { "Q",            "Quit" },
                { "F1",           "Toggle this help" },
                { "F5",           "Reload theme.cfg" },
                { "F11",          "Toggle Fullscreen" },
            };
            int n = (int)(sizeof(entries) / sizeof(entries[0]));
            int pad = 24, row_h = 26, title_h = 36;
            int box_w = 440, box_h = pad * 2 + title_h + n * row_h;
            int bx = (1024 - box_w) / 2, by = (600 - box_h) / 2;
            // Overlay background
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 10, 10, 30, 210);
            SDL_Rect bg_r = { 0, 0, 1024, 600 }; SDL_RenderFillRect(renderer, &bg_r);
            // Box
            SDL_SetRenderDrawColor(renderer, 30, 30, 60, 240);
            SDL_Rect box_r = { bx, by, box_w, box_h }; SDL_RenderFillRect(renderer, &box_r);
            SDL_SetRenderDrawColor(renderer, 100, 120, 220, 255);
            SDL_RenderDrawRect(renderer, &box_r);
            // Title
            SDL_Color white = { 255, 255, 255, 255 };
            SDL_Color yellow = { 255, 220, 80, 255 };
            SDL_Color gray = { 180, 180, 200, 255 };
            draw_text(renderer, font_20, "Keyboard Shortcuts", bx + pad, by + (title_h - 20) / 2, 20, yellow);
            // Entries
            for (int i = 0; i < n; i++) {
                int ry = by + title_h + pad / 2 + i * row_h;
                draw_text(renderer, font_20, entries[i].key,    bx + pad,       ry, 18, white);
                draw_text(renderer, font_20, entries[i].action, bx + pad + 180, ry, 18, gray);
            }
            // Hint
            draw_text(renderer, font_20, "Press F1 or Esc to close", bx + pad, by + box_h - pad, 16, gray);
        }
#endif
        SDL_RenderPresent(renderer); SDL_Delay(16);
    }
    // Cleanup risorse
#ifndef CROSS_PLATFORM
    for (int fi = 0; fi < vol_fd_count; fi++) close(vol_fds[fi]);
#endif
    stop_video_preview();
    if (video_texture) SDL_DestroyTexture(video_texture);
    if (box_art) SDL_DestroyTexture(box_art);
    if (ctrl_cur) SDL_DestroyTexture(ctrl_cur);
    if (help_game_tex) SDL_DestroyTexture(help_game_tex);
    if (help_menu_tex) SDL_DestroyTexture(help_menu_tex);
    if (arrow_up_tex) SDL_DestroyTexture(arrow_up_tex);
    if (arrow_down_tex) SDL_DestroyTexture(arrow_down_tex);
    if (arrow_left_tex) SDL_DestroyTexture(arrow_left_tex);
    if (arrow_right_tex) SDL_DestroyTexture(arrow_right_tex);
    stop_music();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    if (ttf_available) {
        ttf_close_font(font_16);
        ttf_close_font(font_20);
        ttf_close_font(font_24);
        ttf_quit();
    }
    SDL_Quit();
    return 0;
}