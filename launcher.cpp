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
#include <cstdio>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#define LAUNCHER_VERSION "1.4.6 Stable"

// --- STRUCTURES ---
struct MenuItem {
    std::string name;
    bool is_dir;
    bool is_superfolder;
    bool is_collection;
    MenuItem() : is_dir(false), is_superfolder(false), is_collection(false) {}
    MenuItem(const std::string& n, bool d, bool sf = false, bool col = false) : name(n), is_dir(d), is_superfolder(sf), is_collection(col) {}
};
struct SoundSample { Uint8* buffer = NULL; Uint32 length = 0; };

// --- GLOBAL CONFIG ---
std::map<std::string, std::vector<std::string>> system_configs;
std::set<std::string> hidden_dirs; // Cartelle da nascondere (lowercase) - globale
std::map<std::string, std::set<std::string>> system_hidden_dirs; // Cartelle da nascondere per sistema specifico

#ifndef CROSS_PLATFORM
// --- LED BLINK (batteria scarica) ---
std::atomic<bool> led_blink_active{false};
#endif

// --- SUPERFOLDER SUPPORT ---
struct SuperFolderNode {
    std::string name;
    std::vector<SuperFolderNode> sub_folders;
    std::vector<std::string> systems; // leaf: real system folder names
};
std::vector<SuperFolderNode> superfolder_roots;
std::set<std::string>        systems_in_superfolders;
std::set<std::string>        collections_in_superfolders;

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
bool show_battery_percent = false;
bool show_clock       = false;
int  clock_format     = 0;   // 0=24H, 1=12H AM/PM
int  clock_tz_index   = 0;   // indice in clock_tz_list (0=System)
static const int clock_tz_count = 34;
bool show_date        = false;
bool show_system_name = true;
bool show_help_bar    = true;
bool show_system_logo = true;
bool music_enabled    = true;
bool show_gamelist_names = false;    // true = mostra nome da gamelist.xml invece del nome file ROM
bool show_gamesearch_names = true;   // true = mostra nome gamelist nella ricerca; false = mostra nome ROM
int  home_mode        = 1;  // 0=disabilitato, 1=esci, 2=torna carousel, 3=torna carousel+Favorites
int  startup_volume     = -1; // -1=disabilitato, 0-100=percentuale volume da impostare all'avvio
int  startup_brightness = -1; // -1=disabilitato, 10-100=percentuale luminosità da impostare all'avvio
int  system_sort_order  =  0; // 0=nome sistema, 1=nome cartella, 2=custom (systems_order.xml)
bool music_autoadvance = false; // true = alla fine di una traccia passa alla successiva
bool show_empty_systems = true; // false = nascondi sistemi senza ROM
bool video_preview_enabled = true; // false = non mostrare video preview nella gamelist

// --- CAROUSEL PRELOADER ---
// g_count_mutex: protects game_count_cache (preload thread + main thread)
// g_surf_mutex:  protects g_surf_cache    (IMG_Load is thread-safe; SDL_CreateTexture is not)
static std::mutex                          g_count_mutex;
static std::mutex                          g_surf_mutex;
static std::map<std::string, SDL_Surface*> g_surf_cache; // path → surface (nullptr = not found)
static std::atomic<bool>                   g_preload_stop{false};
static std::thread                         g_preload_th;

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

// Systems supported in HDMI mode — loaded at runtime from /sdcard/start_local_sd_HDMI
// (one folder name per line, case-insensitive; empty file = no systems shown)
std::set<std::string> hdmi_supported_systems;

void load_hdmi_systems() {
    hdmi_supported_systems.clear();
    std::ifstream f("/sdcard/start_local_sd_HDMI.sh");
    if (!f) return;
    // Parse case patterns like:  /sdcard/games/<name>/*) or /sdcard/games/<name>/<sub>/*)
    // Extract the first path component after /sdcard/games/ as the system folder name.
    const std::string marker = "/sdcard/games/";
    std::string line;
    while (std::getline(f, line)) {
        std::string low = line;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        size_t pos = low.find(marker);
        if (pos == std::string::npos) continue;
        pos += marker.size();
        size_t end = low.find('/', pos);
        if (end == std::string::npos) continue;
        std::string sysname = low.substr(pos, end - pos);
        if (!sysname.empty())
            hdmi_supported_systems.insert(sysname);
    }
}

// Ritorna il percorso base del tema attivo: base_p + "themes/<current_theme>/"
std::string theme_p() { return base_p + "themes/" + current_theme + "/"; }

std::string get_system_fullname(const std::string& sys_name);

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
    // effetti visivi carousel
    bool  carousel_ease_out        = true;   // ease-out sulla transizione slide
    float carousel_ease_out_factor = 10.0f;  // rigidità ease-out (più alto = più veloce)
    bool  carousel_idle_float      = true;   // galleggiamento del device centrale a riposo
    float carousel_idle_float_amp  = 6.0f;   // ampiezza galleggiamento in pixel
    float carousel_idle_float_speed = 0.5f;  // velocità galleggiamento in Hz
    bool  carousel_idle_zoom       = true;   // zoom pulse del device centrale a riposo
    float carousel_idle_zoom_amp   = 0.04f;  // ampiezza zoom (es. 0.04 = ±4% della dimensione)
    float carousel_idle_zoom_speed = 0.35f;  // velocità zoom in Hz
    bool  carousel_vignette        = true;   // vignette scura ai bordi dello schermo
    int   carousel_vignette_alpha  = 130;    // opacità vignette (0-255)
    int   carousel_vignette_w      = 220;    // larghezza vignette in pixel
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
    // [boxart]  — main media area (screenshot/video/no_art)
    int   boxart_area_x           = 640;
    int   boxart_area_y           = 65;
    int   boxart_max_w            = 320;
    int   boxart_max_h            = 240;
    int   boxart_border_padding   = 5;
    bool  boxart_border_enabled   = true;
    // [boxart_overlay]  — boxart thumbnail shown as overlay
    bool  boxart_overlay_enabled      = false;
    int   boxart_overlay_x            = 645;
    int   boxart_overlay_y            = 310;
    int   boxart_overlay_max_w        = 120;
    int   boxart_overlay_max_h        = 90;
    int   boxart_overlay_alpha        = 220;
    bool  boxart_overlay_shadow       = true;
    int   boxart_overlay_shadow_alpha = 120;
    // [crt_overlay]  -1 = segue automaticamente il rect del media (video/boxart)
    int   crt_overlay_x           = -1;
    int   crt_overlay_y           = -1;
    int   crt_overlay_w           = -1;
    int   crt_overlay_h           = -1;
    // [marquee]
    bool  marquee_enabled         = true;
    int   marquee_x               = 645;
    int   marquee_y               = 10;
    float marquee_scale           = 0.5f;
    int   marquee_max_w           = 0;   // 0 = no limit
    int   marquee_max_h           = 0;   // 0 = no limit
    int   marquee_alpha           = 255;
    bool  marquee_shadow          = true;
    int   marquee_shadow_alpha    = 150;
    // [game_info]
    int   game_name_x             = 645;
    int   game_name_y             = 315;
    int   desc_x                  = 645;
    int   desc_y                  = 340;
    int   desc_max_w              = 310;
    int   desc_area_h             = 180;
    int   desc_line_h             = 18;
    float desc_scroll_speed       = 0.8f;
    // [game_meta]
    bool      game_meta_enabled   = true;
    int       game_meta_x         = -1;   // -1 = auto (desc_x)
    int       game_meta_y         = -1;   // -1 = auto (desc_y + desc_area_h + 6)
    int       game_meta_line_h    = 18;
    int       game_meta_font_size = 16;
    SDL_Color game_meta_color     = {200, 200, 200, 255};
    // [personal_rating]
    bool      personal_rating_enabled   = true;
    int       personal_rating_x         = 645;
    int       personal_rating_y         = 535;
    int       personal_rating_font_size = 18;
    int       personal_rating_star_size = 15;
    SDL_Color personal_rating_color     = {255, 220, 80, 255};
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
    int   video_delay_ms = 2000;     // ms to show screenshot before starting video (0 if no screenshot)
    int   art_delay_ms   = -1;       // ms dopo cui appaiono marquee/boxart-overlay (-1 = stesso di video_delay_ms)
    int   video_fade_ms  = 500;      // ms of crossfade from screenshot to video
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
    // [search] - search popup
    int       search_box_w          = 560;
    int       search_box_h          = 360;
    int       search_letter_bar_h   = 48;
    // [gamelist_3dbox]
    bool      gamelist_3dbox_enabled       = false;
    int       gamelist_3dbox_center_x      = 512;
    int       gamelist_3dbox_center_y      = 280;
    int       gamelist_3dbox_w             = 200;
    int       gamelist_3dbox_h             = 280;
    float     gamelist_3dbox_side_scale    = 0.65f;
    float     gamelist_3dbox_side_alpha    = 0.55f;
    int       gamelist_3dbox_visible_sides = 2;
    int       gamelist_3dbox_spacing       = 20;
    float     gamelist_3dbox_slide_speed   = 3600.0f;
    int       gamelist_3dbox_highlight_alpha = 80;
    bool      gamelist_3dbox_show_name     = true;
    int       gamelist_3dbox_name_y        = -1;  // -1 = auto (center_y + box_h/2 + 15)
    SDL_Color gamelist_3dbox_name_color    = {255, 255, 255, 255};
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
                else if (key == "ease_out")          cfg.carousel_ease_out = (val != "0" && val != "false");
                else if (key == "ease_out_factor")   cfg.carousel_ease_out_factor = std::stof(val);
                else if (key == "idle_float")        cfg.carousel_idle_float = (val != "0" && val != "false");
                else if (key == "idle_float_amp")    cfg.carousel_idle_float_amp = std::stof(val);
                else if (key == "idle_float_speed")  cfg.carousel_idle_float_speed = std::stof(val);
                else if (key == "idle_zoom")         cfg.carousel_idle_zoom = (val != "0" && val != "false");
                else if (key == "idle_zoom_amp")     cfg.carousel_idle_zoom_amp = std::stof(val);
                else if (key == "idle_zoom_speed")   cfg.carousel_idle_zoom_speed = std::stof(val);
                else if (key == "vignette")          cfg.carousel_vignette = (val != "0" && val != "false");
                else if (key == "vignette_alpha")    cfg.carousel_vignette_alpha = std::stoi(val);
                else if (key == "vignette_w")        cfg.carousel_vignette_w = std::stoi(val);
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
                else if (key == "border_enabled")  cfg.boxart_border_enabled = (val != "0" && val != "false");
            } else if (section == "boxart_overlay") {
                if      (key == "enabled")       cfg.boxart_overlay_enabled = (val != "0" && val != "false");
                else if (key == "x")             cfg.boxart_overlay_x = std::stoi(val);
                else if (key == "y")             cfg.boxart_overlay_y = std::stoi(val);
                else if (key == "max_w")         cfg.boxart_overlay_max_w = std::stoi(val);
                else if (key == "max_h")         cfg.boxart_overlay_max_h = std::stoi(val);
                else if (key == "alpha")         cfg.boxart_overlay_alpha = std::stoi(val);
                else if (key == "shadow")        cfg.boxart_overlay_shadow = (val != "0" && val != "false");
                else if (key == "shadow_alpha")  cfg.boxart_overlay_shadow_alpha = std::stoi(val);
            } else if (section == "crt_overlay") {
                if      (key == "x") cfg.crt_overlay_x = std::stoi(val);
                else if (key == "y") cfg.crt_overlay_y = std::stoi(val);
                else if (key == "w") cfg.crt_overlay_w = std::stoi(val);
                else if (key == "h") cfg.crt_overlay_h = std::stoi(val);
            } else if (section == "marquee") {
                if      (key == "enabled")       cfg.marquee_enabled = (val != "0" && val != "false");
                else if (key == "x")             cfg.marquee_x = std::stoi(val);
                else if (key == "y")             cfg.marquee_y = std::stoi(val);
                else if (key == "scale")         cfg.marquee_scale = std::stof(val);
                else if (key == "max_w")         cfg.marquee_max_w = std::stoi(val);
                else if (key == "max_h")         cfg.marquee_max_h = std::stoi(val);
                else if (key == "alpha")         cfg.marquee_alpha = std::stoi(val);
                else if (key == "shadow")        cfg.marquee_shadow = (val != "0" && val != "false");
                else if (key == "shadow_alpha")  cfg.marquee_shadow_alpha = std::stoi(val);
            } else if (section == "game_info") {
                if      (key == "name_x")          cfg.game_name_x = std::stoi(val);
                else if (key == "name_y")          cfg.game_name_y = std::stoi(val);
                else if (key == "desc_x")          cfg.desc_x = std::stoi(val);
                else if (key == "desc_y")          cfg.desc_y = std::stoi(val);
                else if (key == "desc_max_w")      cfg.desc_max_w = std::stoi(val);
                else if (key == "desc_area_h")     cfg.desc_area_h = std::stoi(val);
                else if (key == "desc_line_h")     cfg.desc_line_h = std::stoi(val);
                else if (key == "desc_scroll_speed") cfg.desc_scroll_speed = std::stof(val);
            } else if (section == "game_meta") {
                if      (key == "enabled")   cfg.game_meta_enabled = (val != "0" && val != "false");
                else if (key == "x")         cfg.game_meta_x = std::stoi(val);
                else if (key == "y")         cfg.game_meta_y = std::stoi(val);
                else if (key == "line_h")    cfg.game_meta_line_h = std::max(10, std::stoi(val));
                else if (key == "font_size") cfg.game_meta_font_size = std::max(10, std::stoi(val));
                else if (key == "color")     cfg.game_meta_color = parse_color(val);
            } else if (section == "personal_rating") {
                if      (key == "enabled")   cfg.personal_rating_enabled = (val != "0" && val != "false");
                else if (key == "x")         cfg.personal_rating_x = std::stoi(val);
                else if (key == "y")         cfg.personal_rating_y = std::stoi(val);
                else if (key == "font_size") cfg.personal_rating_font_size = std::max(10, std::stoi(val));
                else if (key == "star_size") cfg.personal_rating_star_size = std::max(8, std::stoi(val));
                else if (key == "color")     cfg.personal_rating_color = parse_color(val);
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
                if      (key == "delay_ms")     cfg.video_delay_ms = std::stoi(val);
                else if (key == "art_delay_ms") cfg.art_delay_ms   = std::stoi(val);
                else if (key == "fade_ms")      cfg.video_fade_ms  = std::stoi(val);
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
            } else if (section == "search") {
                if      (key == "box_w")        cfg.search_box_w = std::stoi(val);
                else if (key == "box_h")        cfg.search_box_h = std::stoi(val);
                else if (key == "letter_bar_h") cfg.search_letter_bar_h = std::stoi(val);
            } else if (section == "gamelist_3dbox") {
                if      (key == "enabled")           cfg.gamelist_3dbox_enabled = (val != "0" && val != "false");
                else if (key == "center_x")          cfg.gamelist_3dbox_center_x = std::stoi(val);
                else if (key == "center_y")          cfg.gamelist_3dbox_center_y = std::stoi(val);
                else if (key == "box_w")             cfg.gamelist_3dbox_w = std::stoi(val);
                else if (key == "box_h")             cfg.gamelist_3dbox_h = std::stoi(val);
                else if (key == "side_scale")        cfg.gamelist_3dbox_side_scale = std::stof(val);
                else if (key == "side_alpha")        cfg.gamelist_3dbox_side_alpha = std::stof(val);
                else if (key == "visible_sides")     cfg.gamelist_3dbox_visible_sides = std::max(1, std::stoi(val));
                else if (key == "spacing")           cfg.gamelist_3dbox_spacing = std::stoi(val);
                else if (key == "slide_speed")       cfg.gamelist_3dbox_slide_speed = std::stof(val);
                else if (key == "highlight_alpha")   cfg.gamelist_3dbox_highlight_alpha = std::stoi(val);
                else if (key == "show_name")         cfg.gamelist_3dbox_show_name = (val != "0" && val != "false");
                else if (key == "name_y")            cfg.gamelist_3dbox_name_y = std::stoi(val);
                else if (key == "name_color")        cfg.gamelist_3dbox_name_color = parse_color(val);
            }
        } catch (...) {} // Ignore malformed values
    }
    return cfg;
}

// --- Per-theme music track persistence ---
static void save_music_track_for_theme(const std::string& theme, const std::string& filename) {
    std::ofstream f(base_p + "themes/" + theme + "/music_last.txt");
    if (f) f << filename;
}
static std::string load_music_track_for_theme(const std::string& theme) {
    std::ifstream f(base_p + "themes/" + theme + "/music_last.txt");
    std::string s;
    if (f) std::getline(f, s);
    return s;
}
static int resolve_music_track_index(const std::vector<std::string>& tracks, const std::string& filename) {
    if (filename.empty()) return 0;
    for (int i = 0; i < (int)tracks.size(); i++) {
        size_t sl = tracks[i].rfind('/');
        std::string fn = (sl != std::string::npos) ? tracks[i].substr(sl+1) : tracks[i];
        if (fn == filename) return i;
    }
    return 0;
}

static void apply_screen_brightness(int percent) {
    const char* bl_path = "/sys/class/backlight/backlight";
    char max_path[256], br_path[256];
    snprintf(max_path, sizeof(max_path), "%s/max_brightness", bl_path);
    snprintf(br_path,  sizeof(br_path),  "%s/brightness",     bl_path);
    int max_val = 255;
    { std::ifstream mf(max_path); if (mf) mf >> max_val; }
    if (max_val <= 0) max_val = 255;
    int val = percent * max_val / 100;
    if (val < 1) val = 1;
    std::ofstream bf(br_path);
    if (bf) bf << val << "\n";
}

void save_options_settings() {
    std::ofstream f(base_p + "launcher_options.txt");
    if (!f) return;
    f << "theme="               <<  current_theme           << "\n";
    f << "show_wifi="             << (show_wifi             ? 1 : 0) << "\n";
    f << "show_battery="          << (show_battery          ? 1 : 0) << "\n";
    f << "show_battery_percent="  << (show_battery_percent  ? 1 : 0) << "\n";
    f << "show_clock="            << (show_clock            ? 1 : 0) << "\n";
    f << "show_date="             << (show_date             ? 1 : 0) << "\n";
    f << "show_system_name="      << (show_system_name      ? 1 : 0) << "\n";
    f << "show_help_bar="         << (show_help_bar         ? 1 : 0) << "\n";
    f << "show_system_logo="      << (show_system_logo      ? 1 : 0) << "\n";
    f << "music_enabled="         << (music_enabled         ? 1 : 0) << "\n";
    f << "show_gamelist_names="   << (show_gamelist_names   ? 1 : 0) << "\n";
    f << "show_gamesearch_names=" << (show_gamesearch_names ? 1 : 0) << "\n";
    f << "music_autoadvance="     << (music_autoadvance     ? 1 : 0) << "\n";
    f << "startup_volume="        << startup_volume        << "\n";
    f << "startup_brightness="    << startup_brightness    << "\n";
    f << "system_sort_order="     << system_sort_order     << "\n";
    f << "show_empty_systems="    << (show_empty_systems   ? 1 : 0) << "\n";
    f << "video_preview_enabled=" << (video_preview_enabled ? 1 : 0) << "\n";
    f << "clock_format="          << clock_format          << "\n";
    f << "clock_tz_index="        << clock_tz_index        << "\n";
}
void load_options_settings() {
    std::ifstream f(base_p + "launcher_options.txt");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if      (line.rfind("theme=",                 0) == 0) { std::string v = line.substr(6); if (!v.empty()) current_theme = v; }
        else if (line.rfind("show_wifi=",             0) == 0) show_wifi             = line.substr(10) != "0";
        else if (line.rfind("show_battery_percent=",  0) == 0) show_battery_percent  = line.substr(20) != "0";
        else if (line.rfind("show_battery=",          0) == 0) show_battery          = line.substr(13) != "0";
        else if (line.rfind("show_clock=",            0) == 0) show_clock            = line.substr(11) != "0";
        else if (line.rfind("show_date=",             0) == 0) show_date             = line.substr(10) != "0";
        else if (line.rfind("show_system_name=",      0) == 0) show_system_name      = line.substr(17) != "0";
        else if (line.rfind("show_help_bar=",         0) == 0) show_help_bar         = line.substr(14) != "0";
        else if (line.rfind("show_system_logo=",      0) == 0) show_system_logo      = line.substr(16) != "0";
        else if (line.rfind("music_enabled=",         0) == 0) music_enabled         = line.substr(14) != "0";
        else if (line.rfind("show_gamelist_names=",   0) == 0) show_gamelist_names   = line.substr(20) != "0";
        else if (line.rfind("show_gamesearch_names=", 0) == 0) show_gamesearch_names = line.substr(22) != "0";
        else if (line.rfind("music_autoadvance=",     0) == 0) music_autoadvance     = line.substr(18) != "0";
        else if (line.rfind("startup_volume=",        0) == 0) {
            try { int v = std::stoi(line.substr(15)); startup_volume = (v >= 0 && v <= 100) ? v : -1; } catch (...) {}
        }
        else if (line.rfind("startup_brightness=",    0) == 0) {
            try { int v = std::stoi(line.substr(19)); startup_brightness = (v >= 10 && v <= 100) ? v : -1; } catch (...) {}
        }
        else if (line.rfind("system_sort_order=",     0) == 0) {
            try { int v = std::stoi(line.substr(18)); system_sort_order = (v >= 0 && v <= 2) ? v : 0; } catch (...) {}
        }
        else if (line.rfind("show_empty_systems=",    0) == 0) show_empty_systems    = line.substr(19) != "0";
        else if (line.rfind("video_preview_enabled=", 0) == 0) video_preview_enabled = line.substr(22) != "0";
        else if (line.rfind("clock_format=",          0) == 0) {
            try { int v = std::stoi(line.substr(13)); clock_format   = (v == 0 || v == 1) ? v : 0; } catch (...) {}
        }
        else if (line.rfind("clock_tz_index=",        0) == 0) {
            try { int v = std::stoi(line.substr(15)); clock_tz_index = (v >= 0 && v < clock_tz_count) ? v : 0; } catch (...) {}
        }
    }
}

static const char* clock_tz_name(int idx) {
    static const char* const tz[34] = {
        "System",                          // usa il fuso del dispositivo
        "UTC",                             // UTC+0 (niente DST)
        "Pacific/Honolulu",                // UTC-10 (niente DST)
        "America/Anchorage",               // UTC-9/-8
        "America/Los_Angeles",             // UTC-8/-7
        "America/Denver",                  // UTC-7/-6
        "America/Chicago",                 // UTC-6/-5
        "America/New_York",                // UTC-5/-4
        "America/Caracas",                 // UTC-4 (niente DST)
        "America/Sao_Paulo",               // UTC-3/-2
        "Atlantic/Azores",                 // UTC-1/0
        "Europe/London",                   // UTC+0/+1
        "Europe/Rome",                     // UTC+1/+2
        "Europe/Paris",                    // UTC+1/+2
        "Europe/Berlin",                   // UTC+1/+2
        "Europe/Helsinki",                 // UTC+2/+3
        "Africa/Cairo",                    // UTC+2 (niente DST)
        "Europe/Moscow",                   // UTC+3 (niente DST)
        "Asia/Riyadh",                     // UTC+3 (niente DST)
        "Asia/Tehran",                     // UTC+3:30/+4:30
        "Asia/Dubai",                      // UTC+4 (niente DST)
        "Asia/Karachi",                    // UTC+5 (niente DST)
        "Asia/Kolkata",                    // UTC+5:30 (niente DST)
        "Asia/Kathmandu",                  // UTC+5:45 (niente DST)
        "Asia/Dhaka",                      // UTC+6
        "Asia/Yangon",                     // UTC+6:30 (niente DST)
        "Asia/Bangkok",                    // UTC+7 (niente DST)
        "Asia/Shanghai",                   // UTC+8 (niente DST)
        "Asia/Singapore",                  // UTC+8 (niente DST)
        "Asia/Tokyo",                      // UTC+9 (niente DST)
        "Asia/Seoul",                      // UTC+9 (niente DST)
        "Australia/Adelaide",              // UTC+9:30/+10:30
        "Australia/Sydney",                // UTC+10/+11
        "Pacific/Auckland"                 // UTC+12/+13
    };
    if (idx < 0 || idx >= clock_tz_count) return tz[0];
    return tz[idx];
}

// --- SUPERFOLDER HELPERS ---
static void sf_add_to_node(SuperFolderNode& node, const std::vector<std::string>& parts, int depth) {
    if (depth == (int)parts.size() - 1) { node.systems.push_back(parts[depth]); return; }
    for (auto& sf : node.sub_folders) {
        if (sf.name == parts[depth]) { sf_add_to_node(sf, parts, depth + 1); return; }
    }
    node.sub_folders.push_back({parts[depth], {}, {}});
    sf_add_to_node(node.sub_folders.back(), parts, depth + 1);
}

std::string find_file_ignore_case(const std::string& folder, const std::string& filename); // forward declaration

void load_superfolders() {
    superfolder_roots.clear();
    systems_in_superfolders.clear();
    std::string sf_path = find_file_ignore_case(base_p, "SuperFolders.txt");
    if (sf_path.empty()) return;
    std::ifstream f(sf_path);
    if (!f) return;
    std::string line;
    int cur = -1;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        while (!line.empty() && line.front() == ' ') line = line.substr(1);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            superfolder_roots.push_back({line.substr(1, line.size() - 2), {}, {}});
            cur = (int)superfolder_roots.size() - 1;
        } else if (cur >= 0) {
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string seg;
            while (std::getline(ss, seg, '/')) {
                while (!seg.empty() && seg.back()  == ' ') seg.pop_back();
                while (!seg.empty() && seg.front() == ' ') seg = seg.substr(1);
                if (!seg.empty()) parts.push_back(seg);
            }
            if (parts.empty()) continue;
            sf_add_to_node(superfolder_roots[cur], parts, 0);
            systems_in_superfolders.insert(parts.back());
        }
    }
}

// Forward decl — implementazione dopo load_systems_desc
std::vector<std::string> systems_custom_order;
static bool compare_systems(const MenuItem& a, const MenuItem& b);

std::vector<MenuItem> items_from_sfnode(const SuperFolderNode& node) {
    std::vector<MenuItem> result;
    for (const auto& sf : node.sub_folders) result.push_back({sf.name, true, true});
    std::vector<std::string> systems = node.systems;
    if (system_sort_order == 2 && !systems_custom_order.empty()) {
        // Custom: ordina secondo systems_custom_order, i non trovati vanno in fondo
        std::vector<std::string> ordered, rest;
        for (const auto& s : systems_custom_order)
            for (const auto& sys : systems)
                if (sys == s) { ordered.push_back(sys); break; }
        for (const auto& sys : systems) {
            bool found = std::find(ordered.begin(), ordered.end(), sys) != ordered.end();
            if (!found) rest.push_back(sys);
        }
        systems = ordered;
        systems.insert(systems.end(), rest.begin(), rest.end());
    } else {
        std::sort(systems.begin(), systems.end(), [](const std::string& a, const std::string& b) {
            MenuItem ma{a,true,false}, mb{b,true,false};
            return compare_systems(ma, mb);
        });
    }
    for (const auto& sys : systems) {
        bool is_col = collections_in_superfolders.count(sys) > 0;
        result.push_back({sys, true, false, is_col});
    }
    return result;
}

const SuperFolderNode* find_sfroot(const std::string& name) {
    for (const auto& sf : superfolder_roots) if (sf.name == name) return &sf;
    return nullptr;
}

const SuperFolderNode* find_sfchild(const SuperFolderNode& node, const std::string& name) {
    for (const auto& sf : node.sub_folders) if (sf.name == name) return &sf;
    return nullptr;
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

// --- Surface cache helpers (for carousel preloader) ---
// Thread-safe: load and cache a surface. IMG_Load is thread-safe.
// Returns the cached SDL_Surface* (don't free it — owned by the cache).
static SDL_Surface* preload_get_surface(const std::string& path) {
    if (path.empty()) return nullptr;
    {
        std::lock_guard<std::mutex> lk(g_surf_mutex);
        auto it = g_surf_cache.find(path);
        if (it != g_surf_cache.end()) return it->second; // cached (may be nullptr = not found)
    }
    // Load outside lock so we don't block other threads during slow I/O
    SDL_Surface* s = (access(path.c_str(), F_OK) == 0) ? IMG_Load(path.c_str()) : nullptr;
    {
        std::lock_guard<std::mutex> lk(g_surf_mutex);
        auto res = g_surf_cache.emplace(path, s);
        if (!res.second && s) { SDL_FreeSurface(s); s = res.first->second; } // lost race: free duplicate
    }
    return s;
}

// Main-thread only: create texture from cached surface (fast), else fall back to load_texture.
static SDL_Texture* load_texture_cached(SDL_Renderer* renderer, const std::string& path) {
    if (path.empty()) return nullptr;
    SDL_Surface* s = preload_get_surface(path);
    if (s) return SDL_CreateTextureFromSurface(renderer, s);
    return nullptr;
}

static SDL_Texture* load_texture_cached_png_jpg(SDL_Renderer* renderer, const std::string& path_no_ext) {
    SDL_Texture* t = load_texture_cached(renderer, path_no_ext + ".png");
    if (!t) t = load_texture_cached(renderer, path_no_ext + ".jpg");
    return t;
}

// Free all preloaded surfaces. Must be called from main thread after stopping the preload thread.
static void clear_surface_cache() {
    std::lock_guard<std::mutex> lk(g_surf_mutex);
    for (auto& kv : g_surf_cache) SDL_FreeSurface(kv.second);
    g_surf_cache.clear();
}

// Stop and join the preload thread (idempotent).
static void preload_stop_join() {
    g_preload_stop.store(true);
    if (g_preload_th.joinable()) g_preload_th.join();
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
SDL_Texture* tex_star_filled = nullptr;
SDL_Texture* tex_star_empty  = nullptr;
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
int ttf_size_utf8(TTF_Font* font, const char* text, int* w, int* h) {
    if (!font || !text) return -1;
    return TTF_SizeUTF8(font, text, w, h);
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
int ttf_size_utf8(TTF_Font* font, const char* text, int* w, int* h) {
    if (!ttf_available || !pTTF_SizeUTF8 || !font || !text) return -1;
    return pTTF_SizeUTF8(font, text, w, h);
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
std::string get_system_fullname(const std::string& sys_name);

// Forward declarations for audio (defined later in the file)
extern SoundSample s_click, s_enter, s_back, s_fav;
void play_sound(SoundSample& s);

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

    int tab = 0; // 0 = TEMA, 1 = DISPLAY, 2 = OPTIONS

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
        {"Show battery %",              &show_battery_percent},
        {"Show time",                   &show_clock},
        {"Show date (MM/DD/YYYY)",      &show_date},
        {"Show system name (carousel)", &show_system_name},
        {"Show help bar",               &show_help_bar},
        {"Show system logo (list)",     &show_system_logo},
    };
    const int disp_count = 10; // 8 bool + 2 cycle (clock format, timezone)
    int disp_sel = 0;

    // Voci tab OPTIONS
    // Item 0-2: bool toggle; item 3: startup volume (int, -1=OFF)
    struct OptItem { const char* label; bool* flag; };
    OptItem opt_items[] = {
        {"Show game names (list)",   &show_gamelist_names},
        {"Show game names (search)", &show_gamesearch_names},
        {"Music in carousel",        &music_enabled},
        {"Music auto-advance",       &music_autoadvance},
        {"Video preview",            &video_preview_enabled},
    };
    const int opt_bool_count = 5;
    const int opt_count = 9; // 5 bool + 1 int (volume) + 1 int (brightness) + 1 int (sort order) + 1 bool (empty systems)
    int opt_sel = 0;

    SDL_Texture* preview_tex = nullptr;
    std::string preview_theme = "";
    auto load_preview = [&](const std::string& t) {
        if (t == preview_theme) return;
        preview_theme = t;
        if (preview_tex) { SDL_DestroyTexture(preview_tex); preview_tex = nullptr; }
        preview_tex = load_texture_png_jpg(renderer, base_p + "themes/" + t + "/bg/default");
    };
    load_preview(themes[sel]);

    // --- INFO TAB: system information ---
    struct SysInfo {
        std::string ip, ssid, sd_free, sd_total,
                    battery_level, battery_status, ram_free, uptime;
    };
    SysInfo sysinfo;
    Uint32 sysinfo_next_refresh = 0;

    auto refresh_sysinfo = [&]() {
        sysinfo_next_refresh = SDL_GetTicks() + 5000;
        auto trim = [](std::string s) -> std::string {
            while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
            return s;
        };
        auto rdfile = [&](const char* p) -> std::string {
            FILE* f = fopen(p, "r"); if (!f) return "";
            char buf[256] = {}; fgets(buf, sizeof(buf), f); fclose(f);
            return trim(std::string(buf));
        };
        auto rdcmd = [&](const char* cmd) -> std::string {
            FILE* f = popen(cmd, "r"); if (!f) return "";
            char buf[256] = {}; fgets(buf, sizeof(buf), f); pclose(f);
            return trim(std::string(buf));
        };
#ifndef CROSS_PLATFORM
        sysinfo.ip   = rdcmd("ip route get 1 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i==\"src\") print $(i+1)}' | head -1");
        sysinfo.ssid = rdcmd("iwgetid wlan0 -r 2>/dev/null");
        struct statvfs sv;
        if (statvfs("/sdcard", &sv) == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f GB", (double)sv.f_blocks * sv.f_frsize / 1e9);
            sysinfo.sd_total = buf;
            snprintf(buf, sizeof(buf), "%.1f GB", (double)sv.f_bavail * sv.f_frsize / 1e9);
            sysinfo.sd_free = buf;
        } else { sysinfo.sd_total = "N/A"; sysinfo.sd_free = "N/A"; }
        sysinfo.battery_level  = rdfile("/sys/class/power_supply/battery/capacity");
        if (!sysinfo.battery_level.empty()) sysinfo.battery_level += "%"; else sysinfo.battery_level = "N/A";
        sysinfo.battery_status = rdfile("/sys/class/power_supply/battery/status");
        if (sysinfo.battery_status.empty()) sysinfo.battery_status = "N/A";
        FILE* mf = fopen("/proc/meminfo", "r");
        if (mf) {
            long long tot = 0, avail = 0; char line[128];
            while (fgets(line, sizeof(line), mf)) {
                long long v = 0;
                if (sscanf(line, "MemTotal: %lld kB", &v)==1) tot=v;
                else if (sscanf(line, "MemAvailable: %lld kB", &v)==1) avail=v;
            }
            fclose(mf);
            char buf[64]; snprintf(buf, sizeof(buf), "%lld MB / %lld MB", avail/1024, tot/1024);
            sysinfo.ram_free = buf;
        } else sysinfo.ram_free = "N/A";
        FILE* uf = fopen("/proc/uptime", "r");
        if (uf) {
            double up = 0; fscanf(uf, "%lf", &up); fclose(uf);
            int h=(int)(up/3600), m=(int)((up-h*3600)/60);
            char buf[32]; snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
            sysinfo.uptime = buf;
        } else sysinfo.uptime = "N/A";
#else
        sysinfo.ip             = "192.168.1.100";
        sysinfo.ssid           = "MyWiFi";
        sysinfo.sd_total       = "32.0 GB";
        sysinfo.sd_free        = "18.5 GB";
        sysinfo.battery_level  = "75%";
        sysinfo.battery_status = "Discharging";
        sysinfo.ram_free       = "256 MB / 512 MB";
        sysinfo.uptime         = "1h 23m";
#endif
    };
    refresh_sysinfo();

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
                // L1 (btn 2) → prev tab,  R1 (btn 5) → next tab
                if (btn == 2) { tab = (tab + 3) % 4; play_sound(s_click); next_input = SDL_GetTicks() + 200; }
                else if (btn == 5) { tab = (tab + 1) % 4; play_sound(s_click); next_input = SDL_GetTicks() + 200; }
                else if (tab == 0) {
                    if (btn == 29 || btn == 30) {
                        sel = (sel - 1 + (int)themes.size()) % (int)themes.size();
                        load_preview(themes[sel]);
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 32 || btn == 31) {
                        sel = (sel + 1) % (int)themes.size();
                        load_preview(themes[sel]);
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 1) { // A: applica tema
                        play_sound(s_enter);
                        current_theme = themes[sel];
                        save_options_settings();
                        img_p = theme_p() + "images/"; // Update image path to new theme
                        theme_cfg = load_theme_config(theme_p() + "theme.cfg"); // Reload layout config for new theme
                        load_systems_desc(); // Reload system descriptions for new theme
                        if (*list_bg_tex) { SDL_DestroyTexture(*list_bg_tex); *list_bg_tex = nullptr; }
                        *list_bg_tex = load_texture_png_jpg(renderer, theme_p() + "bg/list_bg");
                        if (tex_star_filled) { SDL_DestroyTexture(tex_star_filled); tex_star_filled = nullptr; }
                        if (tex_star_empty)  { SDL_DestroyTexture(tex_star_empty);  tex_star_empty  = nullptr; }
                        tex_star_filled = load_theme_image(renderer, "star_filled.png");
                        tex_star_empty  = load_theme_image(renderer, "star_empty.png");
                        update_carousel_textures(renderer, carousel_items, carousel_sel,
                                                 bg_cur, dev_cur, dev_prev, dev_next, ctrl_cur);
                        changed = true;
                        open = false;
                    } else if (btn == 3 || btn == 6) { play_sound(s_back); open = false; }
                } else if (tab == 1) { // tab == 1: DISPLAY
                    if (btn == 29 || btn == 30) {
                        disp_sel = (disp_sel - 1 + disp_count) % disp_count;
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 32 || btn == 31) {
                        disp_sel = (disp_sel + 1) % disp_count;
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 1 || ((disp_sel == 4 || disp_sel == 5) && btn == 31)) { // A o RIGHT sui cycle items
                        if (disp_sel == 4) {
                            clock_format = (clock_format + 1) % 2;
                        } else if (disp_sel == 5) {
                            clock_tz_index = (clock_tz_index + 1) % clock_tz_count;
                        } else {
                            bool* flag = (disp_sel < 4) ? disp_items[disp_sel].flag : disp_items[disp_sel - 2].flag;
                            *flag = !*flag;
                        }
                        play_sound(s_click);
                        save_options_settings();
                        next_input = SDL_GetTicks() + 200;
                    } else if ((disp_sel == 4 || disp_sel == 5) && btn == 28) { // LEFT sui cycle items: cicla indietro
                        if (disp_sel == 4) {
                            clock_format = (clock_format + 1) % 2;
                        } else if (disp_sel == 5) {
                            clock_tz_index = (clock_tz_index - 1 + clock_tz_count) % clock_tz_count;
                        }
                        play_sound(s_click);
                        save_options_settings();
                        next_input = SDL_GetTicks() + 200;
                    } else if (btn == 3 || btn == 6) { play_sound(s_back); open = false; }
                } else if (tab == 2) { // OPTIONS
                    if (btn == 29 || btn == 30) {
                        opt_sel = (opt_sel - 1 + opt_count) % opt_count;
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 32 || btn == 31) {
                        opt_sel = (opt_sel + 1) % opt_count;
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 150;
                    } else if (btn == 1) { // A: toggle/cycle
                        if (opt_sel < opt_bool_count) {
                            *opt_items[opt_sel].flag = !*opt_items[opt_sel].flag;
                            if (opt_items[opt_sel].flag == &music_enabled) {
                                if (music_enabled && !music_tracks.empty())
                                    start_music(music_tracks[music_track_index]);
                                else
                                    stop_music();
                            }
                        } else if (opt_sel == opt_bool_count) { // startup_volume: cycle OFF→5→10→...→100→OFF
                            if (startup_volume < 0) startup_volume = 5;
                            else if (startup_volume >= 100) startup_volume = -1;
                            else startup_volume += 5;
                        } else if (opt_sel == opt_bool_count + 1) { // startup_brightness: cycle OFF→10→20→...→100→OFF
                            if (startup_brightness < 0) startup_brightness = 10;
                            else if (startup_brightness >= 100) startup_brightness = -1;
                            else startup_brightness += 10;
                            if (startup_brightness > 0) apply_screen_brightness(startup_brightness);
                        } else if (opt_sel == opt_bool_count + 2) { // system_sort_order: cycle 0→1→2→0
                            system_sort_order = (system_sort_order + 1) % 3;
                            changed = true;
                            open = false;
                        } else { // display empty systems: toggle + rebuild carousel
                            show_empty_systems = !show_empty_systems;
                            changed = true;
                            open = false;
                        }
                        play_sound(s_click);
                        save_options_settings();
                        next_input = SDL_GetTicks() + 200;
                    } else if (btn == 3 || btn == 6) { play_sound(s_back); open = false; }
                } else { // tab == 3: INFO (read-only)
                    if (btn == 3 || btn == 6) { play_sound(s_back); open = false; }
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
        int tab_w = bw / 4;
        const char* tab_labels[] = {"THEME", "DISPLAY", "OPTIONS", "INFO"};
        for (int ti = 0; ti < 4; ti++) {
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
        } else if (tab == 1) {
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
                // Ordine: 0-3 bool, 4=Time format, 5=Timezone, 6-9 bool (disp_items[i-2])
                const char* row_label = (i < 4)  ? disp_items[i].label
                                      : (i == 4) ? "Time format"
                                      : (i == 5) ? "Timezone"
                                      :             disp_items[i - 2].label;
                if (f22) {
                    SDL_Surface* s = ttf_render_text_blended(f22, row_label, lc);
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
                // Badge: ON/OFF per i bool, valore testuale per i cycle
                std::string badge_str_s;
                SDL_Color badge_col;
                if (i == 4) {
                    badge_col = theme_cfg.menu_badge_on;
                    badge_str_s = (clock_format == 0) ? "24H" : "12H AM/PM";
                } else if (i == 5) {
                    badge_col = theme_cfg.menu_badge_on;
                    const char* tz = clock_tz_name(clock_tz_index);
                    const char* slash = strrchr(tz, '/');
                    badge_str_s = slash ? (slash + 1) : tz;
                } else {
                    bool* flag = (i < 4) ? disp_items[i].flag : disp_items[i - 2].flag;
                    bool val = *flag;
                    badge_col = val ? theme_cfg.menu_badge_on : theme_cfg.menu_badge_off;
                    badge_str_s = val ? "ON" : "OFF";
                }
                if (f22) {
                    SDL_Surface* s = ttf_render_text_blended(f22, badge_str_s.c_str(), badge_col);
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
                SDL_Surface* s = ttf_render_text_blended(f18, "[A/►] Toggle/Cycle  [◄] Prev  [B] Close  [L1/R1] Tab", theme_cfg.menu_hint);
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
        } else if (tab == 2) {
            // --- TAB OPTIONS ---
            int row_h = theme_cfg.menu_disp_row_h;
            int start_y = cont_y + 20;
            for (int i = 0; i < opt_count; i++) {
                int ry = start_y + i * row_h;
                if (i == opt_sel) {
                    SDL_Rect hl = {bx + 8, ry - 4, bw - 16, row_h - 4};
                    SDL_SetRenderDrawColor(renderer, theme_cfg.menu_highlight.r, theme_cfg.menu_highlight.g, theme_cfg.menu_highlight.b, theme_cfg.menu_highlight.a);
                    SDL_RenderFillRect(renderer, &hl);
                }
                const char* item_label = (i < opt_bool_count) ? opt_items[i].label
                                        : (i == opt_bool_count)     ? "Startup volume"
                                        : (i == opt_bool_count + 1) ? "Screen brightness"
                                        : (i == opt_bool_count + 2) ? "System sort order"
                                        : "Display empty systems";
                SDL_Color lc = (i == opt_sel) ? theme_cfg.menu_item_selected : theme_cfg.menu_item_normal;
                if (f22) {
                    SDL_Surface* s = ttf_render_text_blended(f22, item_label, lc);
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
                // Badge: ON/OFF for bool, "OFF"/"XX%" for volume/brightness
                std::string badge_str;
                SDL_Color badge_col;
                if (i < opt_bool_count) {
                    bool val = *opt_items[i].flag;
                    badge_str = val ? "ON" : "OFF";
                    badge_col = val ? theme_cfg.menu_badge_on : theme_cfg.menu_badge_off;
                } else if (i == opt_bool_count) {
                    if (startup_volume < 0) { badge_str = "OFF"; badge_col = theme_cfg.menu_badge_off; }
                    else { badge_str = std::to_string(startup_volume) + "%"; badge_col = theme_cfg.menu_badge_on; }
                } else if (i == opt_bool_count + 1) {
                    if (startup_brightness < 0) { badge_str = "OFF"; badge_col = theme_cfg.menu_badge_off; }
                    else { badge_str = std::to_string(startup_brightness) + "%"; badge_col = theme_cfg.menu_badge_on; }
                } else if (i == opt_bool_count + 2) {
                    const char* sort_labels[] = { "By name", "By folder", "Custom" };
                    badge_str = sort_labels[system_sort_order];
                    badge_col = theme_cfg.menu_badge_on;
                } else {
                    badge_str = show_empty_systems ? "ON" : "OFF";
                    badge_col = show_empty_systems ? theme_cfg.menu_badge_on : theme_cfg.menu_badge_off;
                }
                if (f22) {
                    SDL_Surface* s = ttf_render_text_blended(f22, badge_str.c_str(), badge_col);
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
                SDL_Surface* s = ttf_render_text_blended(f18, "[A] Toggle/Cycle  [B] Close  [L1/R1] Tab", theme_cfg.menu_hint);
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
        } else { // tab == 3: INFO
            // Refresh every 5 seconds
            if (SDL_GetTicks() >= sysinfo_next_refresh) refresh_sysinfo();

            int row_h = theme_cfg.menu_disp_row_h;
            int ry = cont_y + 14;
            std::pair<const char*, std::string> info_rows[] = {
                {"Launcher",    LAUNCHER_VERSION},
                {"IP Address",  sysinfo.ip.empty()   ? "N/A" : sysinfo.ip},
                {"WiFi SSID",   sysinfo.ssid.empty() ? "N/A" : sysinfo.ssid},
                {"SD Total",    sysinfo.sd_total},
                {"SD Free",     sysinfo.sd_free},
                {"Battery",     sysinfo.battery_level + " (" + sysinfo.battery_status + ")"},
                {"RAM",         sysinfo.ram_free},
                {"Uptime",      sysinfo.uptime},
            };
            for (auto& row : info_rows) {
                if (f22) {
                    SDL_Color lc = theme_cfg.menu_item_normal;
                    SDL_Surface* s = ttf_render_text_blended(f22, row.first, lc);
                    if (s) {
                        SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                        if (t) { SDL_Rect r = {bx+20, ry+row_h/2-s->h/2, s->w, s->h}; SDL_RenderCopy(renderer, t, NULL, &r); SDL_DestroyTexture(t); }
                        SDL_FreeSurface(s);
                    }
                    SDL_Color vc = theme_cfg.menu_badge_on;
                    s = ttf_render_text_blended(f22, row.second.c_str(), vc);
                    if (s) {
                        SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                        if (t) { SDL_Rect r = {bx+bw-s->w-20, ry+row_h/2-s->h/2, s->w, s->h}; SDL_RenderCopy(renderer, t, NULL, &r); SDL_DestroyTexture(t); }
                        SDL_FreeSurface(s);
                    }
                }
                ry += row_h;
            }
            if (f18) {
                SDL_Surface* s = ttf_render_text_blended(f18, "[B] Close  [L1/R1] Tab", theme_cfg.menu_hint);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) { SDL_Rect r = {bx+bw/2-s->w/2, by+bh+6, s->w, s->h}; SDL_RenderCopy(renderer, t, NULL, &r); SDL_DestroyTexture(t); }
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

// GameInfo (forward-declared here for show_search_menu)
struct GameInfo {
    std::string path;
    std::string name;
    std::string desc;
    std::string image;
    std::string video;
    std::string genre;
    std::string developer;
    std::string publisher;
    std::string players;
    std::string rating;
    std::string releasedate;
    bool cheevos = false;
};

// --- SEARCH MENU ---
// Mostra un pannello di ricerca alfabetica.
// Restituisce l'indice in `items` da selezionare, oppure -1 se annullato.
static int show_search_menu(SDL_Renderer* renderer, const std::string& font_path,
                            SDL_Texture* bg_tex,
                            const std::vector<MenuItem>& items,
                            const std::map<std::string, GameInfo>* gamelist = nullptr,
                            const std::string& label = "SEARCH",
                            bool allow_delete = false,
                            int* out_delete_item = nullptr) {
    if (items.empty()) return -1;

    // LED key_led11: accendilo quando la ricerca è in modalità giochi con delete abilitato (tasto 0)
#ifndef CROSS_PLATFORM
    if (allow_delete)
        system("echo 255 > /sys/class/leds/key_led11/brightness 2>/dev/null");
#endif

    if (font_cache.find(18) == font_cache.end()) font_cache[18] = ttf_open_font(font_path, 18);
    if (font_cache.find(22) == font_cache.end()) font_cache[22] = ttf_open_font(font_path, 22);
    TTF_Font* f18 = font_cache.count(18) ? font_cache[18] : nullptr;
    TTF_Font* f22 = font_cache.count(22) ? font_cache[22] : nullptr;

    const std::string LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
    const int N_LETTERS = (int)LETTERS.size(); // 27

    auto get_first_char = [](const std::string& name) -> char {
        if (name.empty()) return '#';
        char c = (char)toupper((unsigned char)name[0]);
        if (c >= 'A' && c <= 'Z') return c;
        return '#';
    };

    // Restituisce il nome visualizzato per l'item i (nome gamelist se disponibile, altrimenti nome ROM senza estensione)
    auto get_display_name = [&](int i) -> std::string {
        std::string disp = items[i].name;
        if (!items[i].is_dir) {
            size_t dot = disp.rfind('.');
            std::string rom_name = (dot != std::string::npos) ? disp.substr(0, dot) : disp;
            if (show_gamesearch_names && gamelist) {
                std::string key = rom_name;
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                auto git = gamelist->find(key);
                if (git != gamelist->end() && !git->second.name.empty())
                    return git->second.name;
            }
            return rom_name;
        }
        // In ricerca sistemi (gamelist == nullptr), usa il fullname definito in systems_desc.xml.
        if (!gamelist && !items[i].is_superfolder)
            return get_system_fullname(disp);
        return disp;
    };

    std::set<char> has_items_set;
    for (int i = 0; i < (int)items.size(); i++)
        has_items_set.insert(get_first_char(get_display_name(i)));

    // First letter with matches
    int letter_sel = 0;
    for (int i = 0; i < N_LETTERS; i++) {
        if (has_items_set.count(LETTERS[i])) { letter_sel = i; break; }
    }

    auto build_filtered = [&](int li) -> std::vector<int> {
        std::vector<int> res;
        char ch = LETTERS[li];
        for (int i = 0; i < (int)items.size(); i++)
            if (get_first_char(get_display_name(i)) == ch) res.push_back(i);
        std::sort(res.begin(), res.end(), [&](int a, int b) {
            std::string na = get_display_name(a);
            std::string nb = get_display_name(b);
            std::transform(na.begin(), na.end(), na.begin(), [](unsigned char c){ return std::tolower(c); });
            std::transform(nb.begin(), nb.end(), nb.begin(), [](unsigned char c){ return std::tolower(c); });
            return na < nb;
        });
        return res;
    };

    std::vector<int> filtered = build_filtered(letter_sel);
    int list_sel = 0, list_scroll = 0;
    int page_size = 8; // updated each frame in draw section
    bool confirm_open = false; // finestra conferma eliminazione

    bool open = true;
    int result = -1;
    SDL_Event ev;
    Uint32 next_input = SDL_GetTicks() + 200;
    bool up_held = false, down_held = false;
    Uint32 up_hold_start = 0, down_hold_start = 0;
    Uint32 fast_scroll_next = UINT32_MAX;

    int win_w = 0, win_h = 0;
    SDL_RenderGetLogicalSize(renderer, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) SDL_GetRendererOutputSize(renderer, &win_w, &win_h);

    while (open) {
        Uint32 tick = SDL_GetTicks();
        if (!filtered.empty() && tick >= fast_scroll_next) {
            if (up_held && (tick - up_hold_start > 2000)) {
                list_sel = (list_sel - 1 + (int)filtered.size()) % (int)filtered.size();
                play_sound(s_click);
                fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
            } else if (down_held && (tick - down_hold_start > 2000)) {
                list_sel = (list_sel + 1) % (int)filtered.size();
                play_sound(s_click);
                fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
            }
        }
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_JOYDEVICEADDED) SDL_JoystickOpen(ev.jdevice.which);
            if (ev.type == SDL_JOYBUTTONUP) {
                if (ev.jbutton.button == 29) up_held = false;
                if (ev.jbutton.button == 32) down_held = false;
            }
            if (ev.type == SDL_JOYBUTTONDOWN) {
                if (SDL_GetTicks() < next_input) continue;
                int btn = ev.jbutton.button;
                if (confirm_open) {
                    if (btn == 1) { // A: conferma eliminazione
                        if (out_delete_item && !filtered.empty())
                            *out_delete_item = filtered[list_sel];
                        open = false;
                    } else if (btn == 3 || btn == 0) { // B o X: annulla
                        confirm_open = false;
                        next_input = SDL_GetTicks() + 200;
                    }
                    continue;
                }
                if (btn == 30 || btn == 31) { // Left/Right: change letter
                    int dir = (btn == 31) ? 1 : -1;
                    int prev = letter_sel;
                    do {
                        letter_sel = (letter_sel + dir + N_LETTERS) % N_LETTERS;
                    } while (!has_items_set.count(LETTERS[letter_sel]) && letter_sel != prev);
                    filtered = build_filtered(letter_sel);
                    list_sel = 0; list_scroll = 0;
                    play_sound(s_click);
                    next_input = SDL_GetTicks() + 150;
                } else if (btn == 29) { // Up: scroll list
                    if (!up_held) { up_held = true; up_hold_start = SDL_GetTicks(); fast_scroll_next = up_hold_start + 2000; }
                    down_held = false;
                    if (!filtered.empty()) {
                        list_sel = (list_sel - 1 + (int)filtered.size()) % (int)filtered.size();
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 150;
                    }
                } else if (btn == 32) { // Down: scroll list
                    if (!down_held) { down_held = true; down_hold_start = SDL_GetTicks(); fast_scroll_next = down_hold_start + 2000; }
                    up_held = false;
                    if (!filtered.empty()) {
                        list_sel = (list_sel + 1) % (int)filtered.size();
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 150;
                    }
                } else if (btn == 5) { // R1: page down
                    if (!filtered.empty()) {
                        list_sel = std::min(list_sel + page_size, (int)filtered.size() - 1);
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 200;
                    }
                } else if (btn == 2) { // L1: page up
                    if (!filtered.empty()) {
                        list_sel = std::max(list_sel - page_size, 0);
                        play_sound(s_click);
                        next_input = SDL_GetTicks() + 200;
                    }
                } else if (btn == 1) { // A: confirm
                    if (!filtered.empty()) { play_sound(s_enter); result = filtered[list_sel]; open = false; }
                } else if (btn == 3 || btn == 0) { // B or X: cancel
                    play_sound(s_back); open = false;
                } else if (btn == 23) { // 0: elimina gioco
                    if (allow_delete && !filtered.empty() && !items[filtered[list_sel]].is_dir) {
                        confirm_open = true;
                        next_input = SDL_GetTicks() + 300;
                    }
                }
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                if (confirm_open) confirm_open = false;
                else open = false;
            }
#ifdef NATIVE_BASE_PATH
            if (ev.type == SDL_KEYDOWN) {
                int vbtn = -1;
                switch (ev.key.keysym.sym) {
                    case SDLK_UP:       vbtn = 29; break;
                    case SDLK_DOWN:     vbtn = 32; break;
                    case SDLK_LEFT:     vbtn = 30; break;
                    case SDLK_RIGHT:    vbtn = 31; break;
                    case SDLK_PAGEDOWN:
                    case SDLK_r:        vbtn = 5;  break; // R1: page down
                    case SDLK_PAGEUP:
                    case SDLK_l:        vbtn = 2;  break; // L1: page up
                    case SDLK_a:
                    case SDLK_RETURN:   vbtn = 1;  break;
                    case SDLK_b:
                    case SDLK_x:        vbtn = 3;  break;
                    case SDLK_0:        vbtn = 23; break; // 0: elimina gioco
                    default: break;
                }
                if (vbtn >= 0 && SDL_GetTicks() >= next_input) {
                    SDL_Event fake; SDL_memset(&fake, 0, sizeof(fake));
                    fake.type = SDL_JOYBUTTONDOWN;
                    fake.jbutton.button = (Uint8)vbtn;
                    SDL_PushEvent(&fake);
                }
            }
            if (ev.type == SDL_KEYUP) {
                if (ev.key.keysym.sym == SDLK_UP)   up_held = false;
                if (ev.key.keysym.sym == SDLK_DOWN) down_held = false;
            }
#endif
        }

        // --- DRAW ---
        const int bw    = theme_cfg.search_box_w;
        const int bh    = theme_cfg.search_box_h;
        const int bar_h = theme_cfg.search_letter_bar_h;
        const int bx    = (win_w - bw) / 2;
        const int by    = (win_h - bh) / 2;
        const int title_h = 36;

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (bg_tex) { SDL_Rect bgr = {0, 0, win_w, win_h}; SDL_RenderCopy(renderer, bg_tex, NULL, &bgr); }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, theme_cfg.menu_overlay.r, theme_cfg.menu_overlay.g,
                               theme_cfg.menu_overlay.b, theme_cfg.menu_overlay.a);
        SDL_RenderFillRect(renderer, NULL);

        // Box
        SDL_Rect box = {bx, by, bw, bh};
        SDL_SetRenderDrawColor(renderer, theme_cfg.menu_box_bg.r, theme_cfg.menu_box_bg.g,
                               theme_cfg.menu_box_bg.b, theme_cfg.menu_box_bg.a);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, theme_cfg.menu_box_border.r, theme_cfg.menu_box_border.g,
                               theme_cfg.menu_box_border.b, theme_cfg.menu_box_border.a);
        SDL_RenderDrawRect(renderer, &box);

        // Title
        if (f22) {
            SDL_Surface* s = ttf_render_text_blended(f22, label.c_str(), theme_cfg.menu_tab_label_active);
            if (s) {
                SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                if (t) { SDL_Rect tr = {bx + bw/2 - s->w/2, by + 8, s->w, s->h}; SDL_RenderCopy(renderer, t, NULL, &tr); SDL_DestroyTexture(t); }
                SDL_FreeSurface(s);
            }
        }

        // Letter bar background
        const int letter_bar_y = by + title_h;
        SDL_Rect lbr = {bx, letter_bar_y, bw, bar_h};
        SDL_SetRenderDrawColor(renderer, theme_cfg.menu_tab_inactive_bg.r, theme_cfg.menu_tab_inactive_bg.g,
                               theme_cfg.menu_tab_inactive_bg.b, theme_cfg.menu_tab_inactive_bg.a);
        SDL_RenderFillRect(renderer, &lbr);

        // Letters
        const int cell_w = bw / N_LETTERS;
        for (int i = 0; i < N_LETTERS; i++) {
            int cx = bx + i * cell_w;
            bool active = (i == letter_sel);
            bool avail  = has_items_set.count(LETTERS[i]) > 0;
            if (active) {
                SDL_Rect hl = {cx, letter_bar_y, cell_w, bar_h};
                SDL_SetRenderDrawColor(renderer, theme_cfg.menu_tab_active_bg.r, theme_cfg.menu_tab_active_bg.g,
                                       theme_cfg.menu_tab_active_bg.b, theme_cfg.menu_tab_active_bg.a);
                SDL_RenderFillRect(renderer, &hl);
            }
            if (f18) {
                char ch_str[2] = {LETTERS[i], 0};
                SDL_Color lc = active ? theme_cfg.menu_tab_label_active
                             : avail  ? theme_cfg.menu_item_normal
                                      : SDL_Color{80, 80, 100, 160};
                SDL_Surface* s = ttf_render_text_blended(f18, ch_str, lc);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) {
                        SDL_Rect tr = {cx + cell_w/2 - s->w/2, letter_bar_y + bar_h/2 - s->h/2, s->w, s->h};
                        SDL_RenderCopy(renderer, t, NULL, &tr);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
            }
        }

        // Separator below letter bar
        SDL_SetRenderDrawColor(renderer, theme_cfg.menu_box_border.r, theme_cfg.menu_box_border.g,
                               theme_cfg.menu_box_border.b, theme_cfg.menu_box_border.a);
        SDL_RenderDrawLine(renderer, bx, letter_bar_y + bar_h, bx + bw, letter_bar_y + bar_h);

        // List area
        const int list_area_y = letter_bar_y + bar_h + 4;
        const int hint_h      = 28;
        const int row_h       = theme_cfg.menu_list_row_h;
        const int list_area_h = bh - title_h - bar_h - hint_h - 8;
        const int visible     = list_area_h / row_h;
        page_size = std::max(visible - 1, 1);

        if (list_sel < list_scroll) list_scroll = list_sel;
        if (list_sel >= list_scroll + visible) list_scroll = list_sel - visible + 1;

        if (filtered.empty()) {
            if (f22) {
                SDL_Color nc = {140, 140, 160, 200};
                SDL_Surface* s = ttf_render_text_blended(f22, "No results", nc);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) {
                        SDL_Rect tr = {bx + bw/2 - s->w/2, list_area_y + list_area_h/2 - s->h/2, s->w, s->h};
                        SDL_RenderCopy(renderer, t, NULL, &tr);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
            }
        } else {
            for (int i = list_scroll; i < std::min(list_scroll + visible, (int)filtered.size()); i++) {
                int ry = list_area_y + (i - list_scroll) * row_h;
                bool sel = (i == list_sel);
                if (sel) {
                    SDL_Rect hl = {bx + 8, ry - 2, bw - 16, row_h};
                    SDL_SetRenderDrawColor(renderer, theme_cfg.menu_highlight.r, theme_cfg.menu_highlight.g,
                                           theme_cfg.menu_highlight.b, theme_cfg.menu_highlight.a);
                    SDL_RenderFillRect(renderer, &hl);
                }
                if (f22) {
                    std::string disp = get_display_name(filtered[i]);
                    SDL_Color col = sel ? theme_cfg.menu_item_selected : theme_cfg.menu_item_normal;
                    SDL_Surface* s = ttf_render_text_blended(f22, disp.c_str(), col);
                    if (s) {
                        SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                        if (t) {
                            int tw = std::min(s->w, bw - 28);
                            SDL_Rect src = {0, 0, tw, s->h};
                            SDL_Rect dst = {bx + 20, ry + row_h/2 - s->h/2, tw, s->h};
                            SDL_RenderCopy(renderer, t, &src, &dst);
                            SDL_DestroyTexture(t);
                        }
                        SDL_FreeSurface(s);
                    }
                }
            }
        }

        // Hint
        if (f18) {
            const char* hint_str = allow_delete
                ? "[<>] Letter  [^v] Scroll  [A] Go  [B] Close  [0] Del"
                : "[<>] Letter  [^v] Scroll  [A] Go  [B] Close";
            SDL_Surface* s = ttf_render_text_blended(f18, hint_str, theme_cfg.menu_hint);
            if (s) {
                SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                if (t) {
                    SDL_Rect tr = {bx + bw/2 - s->w/2, by + bh + 6, s->w, s->h};
                    SDL_RenderCopy(renderer, t, NULL, &tr);
                    SDL_DestroyTexture(t);
                }
                SDL_FreeSurface(s);
            }
        }

        // Confirm delete overlay
        if (confirm_open && !filtered.empty() && list_sel < (int)filtered.size()) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
            SDL_RenderFillRect(renderer, &box);
            const int cbw = bw - 60, cbh = 100;
            const int cbx = bx + 30, cby = by + bh/2 - cbh/2;
            SDL_Rect cbox = {cbx, cby, cbw, cbh};
            SDL_SetRenderDrawColor(renderer, theme_cfg.menu_box_bg.r, theme_cfg.menu_box_bg.g,
                                   theme_cfg.menu_box_bg.b, 245);
            SDL_RenderFillRect(renderer, &cbox);
            SDL_SetRenderDrawColor(renderer, theme_cfg.menu_box_border.r, theme_cfg.menu_box_border.g,
                                   theme_cfg.menu_box_border.b, theme_cfg.menu_box_border.a);
            SDL_RenderDrawRect(renderer, &cbox);
            if (f22) {
                std::string gn = items[filtered[list_sel]].name;
                size_t dt = gn.rfind('.'); if (dt != std::string::npos) gn = gn.substr(0, dt);
                std::string msg = "Delete: " + gn + "?";
                SDL_Surface* s = ttf_render_text_blended(f22, msg.c_str(), theme_cfg.menu_item_selected);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) {
                        int tw = std::min(s->w, cbw - 20);
                        SDL_Rect src = {0, 0, tw, s->h};
                        SDL_Rect dst = {cbx + cbw/2 - tw/2, cby + 20, tw, s->h};
                        SDL_RenderCopy(renderer, t, &src, &dst);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
            }
            if (f18) {
                SDL_Surface* s = ttf_render_text_blended(f18, "[A] Confirm  [B] Cancel", theme_cfg.menu_hint);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) {
                        SDL_Rect tr = {cbx + cbw/2 - s->w/2, cby + cbh - 26, s->w, s->h};
                        SDL_RenderCopy(renderer, t, NULL, &tr);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

#ifndef CROSS_PLATFORM
    if (allow_delete)
        system("echo 0 > /sys/class/leds/key_led11/brightness 2>/dev/null");
#endif
    return result;
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

    *bg_cur = load_texture_cached_png_jpg(renderer, theme_p() + "bg/" + items[selected].name);
    if (!*bg_cur) { std::string p = find_file_ignore_case(theme_p() + "bg", items[selected].name + ".png"); if (!p.empty()) *bg_cur = load_texture_cached(renderer, p); }
    if (!*bg_cur) { std::string p = find_file_ignore_case(theme_p() + "bg", items[selected].name + ".jpg"); if (!p.empty()) *bg_cur = load_texture_cached(renderer, p); }
    if (!*bg_cur) *bg_cur = load_texture_cached_png_jpg(renderer, theme_p() + "bg/default");

    *dev_cur = load_texture_cached(renderer, theme_p() + "systems/" + items[selected].name + ".png");
    if (!*dev_cur) { std::string p = find_file_ignore_case(theme_p() + "systems", items[selected].name + ".png"); if (!p.empty()) *dev_cur = load_texture_cached(renderer, p); }
    if (!*dev_cur) *dev_cur = load_texture_cached(renderer, theme_p() + "systems/default.png");

    *dev_prev = load_texture_cached(renderer, theme_p() + "systems/" + items[prev_idx].name + ".png");
    if (!*dev_prev) { std::string p = find_file_ignore_case(theme_p() + "systems", items[prev_idx].name + ".png"); if (!p.empty()) *dev_prev = load_texture_cached(renderer, p); }
    if (!*dev_prev) *dev_prev = load_texture_cached(renderer, theme_p() + "systems/default.png");

    *dev_next = load_texture_cached(renderer, theme_p() + "systems/" + items[next_idx].name + ".png");
    if (!*dev_next) { std::string p = find_file_ignore_case(theme_p() + "systems", items[next_idx].name + ".png"); if (!p.empty()) *dev_next = load_texture_cached(renderer, p); }
    if (!*dev_next) *dev_next = load_texture_cached(renderer, theme_p() + "systems/default.png");

    if (ctrl_cur) {
        std::string p = find_file_ignore_case(theme_p() + "controllers", items[selected].name + ".png");
        *ctrl_cur = !p.empty() ? load_texture_cached(renderer, p) : nullptr;
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
        if (ev->d_type == DT_DIR) {
            std::string low_name = name;
            std::transform(low_name.begin(), low_name.end(), low_name.begin(), ::tolower);
            if (!hidden_dirs.empty() && hidden_dirs.count(low_name)) continue;
            {
                auto sit = system_hidden_dirs.find(sys_name);
                if (sit != system_hidden_dirs.end() && sit->second.count(low_name)) continue;
            }
            count += count_games_recursive_impl(path + "/" + name, sys_name, exts);
        } else {
            std::string ln = name; std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
            for(const auto& ext : exts) {
                if(ln.size() > ext.size() && ln.compare(ln.size()-ext.size(), ext.size(), ext) == 0) { count++; break; }
            }
        }
    }
    closedir(dir); return count;
}

int count_games_recursive(const std::string& path, const std::string& sys_name) {
    {
        std::lock_guard<std::mutex> lk(g_count_mutex);
        auto it = game_count_cache.find(sys_name);
        if (it != game_count_cache.end()) return it->second;
    }
    // Not cached yet — compute (can be slow on large collections, done outside lock)
    auto cfg_it = system_configs.find(sys_name);
    if (cfg_it == system_configs.end()) return 0;
    int count = count_games_recursive_impl(path, sys_name, cfg_it->second);
    {
        std::lock_guard<std::mutex> lk(g_count_mutex);
        game_count_cache[sys_name] = count; // harmless double-write on race
    }
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
        {
            std::string low_name = name;
            std::transform(low_name.begin(), low_name.end(), low_name.begin(), ::tolower);
            if (!hidden_dirs.empty() && hidden_dirs.count(low_name)) continue;
            if (!sys_context.empty()) {
                auto sit = system_hidden_dirs.find(sys_context);
                if (sit != system_hidden_dirs.end() && sit->second.count(low_name)) continue;
            }
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
            } else if (sys.rfind("_hide_dirs_", 0) == 0 && sys.size() > 11) {
                // Cartelle da nascondere per sistema specifico: _hide_dirs_snes:subdir1,subdir2
                std::string target_sys = sys.substr(11);
                auto& sset = system_hidden_dirs[target_sys];
                std::stringstream ss(line.substr(sep + 1)); std::string name; name.reserve(32);
                while (std::getline(ss, name, ',')) {
                    size_t s = name.find_first_not_of(" \t\r\n");
                    size_t e = name.find_last_not_of(" \t\r\n");
                    name = (s == std::string::npos) ? "" : name.substr(s, e - s + 1);
                    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                    if (!name.empty()) sset.insert(name);
                }
            } else if (sys == "_home_mode") {
                try { home_mode = std::stoi(line.substr(sep + 1)); } catch (...) {}
            } else if (sys == "_show_gamelist_names") {
                std::string val = line.substr(sep + 1);
                val.erase(std::remove_if(val.begin(), val.end(), ::isspace), val.end());
                show_gamelist_names = (val == "1" || val == "true");
            } else if (sys == "_show_gamesearch_names") {
                std::string val = line.substr(sep + 1);
                val.erase(std::remove_if(val.begin(), val.end(), ::isspace), val.end());
                show_gamesearch_names = (val == "1" || val == "true");
            } else if (sys == "_startup_volume") {
                try {
                    int v = std::stoi(line.substr(sep + 1));
                    if (v >= 0 && v <= 100) startup_volume = v;
                    else startup_volume = -1;
                } catch (...) {}
            } else if (sys == "_music_autoadvance") {
                std::string val = line.substr(sep + 1);
                val.erase(std::remove_if(val.begin(), val.end(), ::isspace), val.end());
                music_autoadvance = (val == "1" || val == "true");
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
std::atomic<uint64_t> video_audio_queued_bytes{0}; // totale byte accodati, per fade-in

// --- BACKGROUND MUSIC ---
std::atomic<bool> music_stop_flag{false};
std::atomic<bool> music_track_ended{false}; // segnala al main loop che la traccia è finita (autoadvance)
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
                // EOF reached naturally (not stopped by stop_music): advance or loop
                if (music_autoadvance && !music_stop_flag) { music_track_ended = true; break; }
                // else: loop the same track (or stop_flag set → outer while exits normally)
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

// Cerca un file video in una cartella (non ricorsivo).
// Restituisce il path completo o "" se non trovato.
static std::string find_video_in_folder(const std::string& folder,
                                         const std::string& game_name) {
    const std::vector<std::string> video_exts = {".mp4", ".avi", ".webm", ".mkv"};
    DIR* vd = opendir(folder.c_str());
    if (!vd) return "";
    std::string low_game = game_name;
    std::transform(low_game.begin(), low_game.end(), low_game.begin(), ::tolower);
    struct dirent* ve;
    while ((ve = readdir(vd)) != NULL) {
        std::string fname = ve->d_name;
        std::string low_fname = fname;
        std::transform(low_fname.begin(), low_fname.end(), low_fname.begin(), ::tolower);
        for (const auto& ext : video_exts) {
            if (low_fname.size() > ext.size() &&
                low_fname.substr(low_fname.size() - ext.size()) == ext) {
                if (low_fname.substr(0, low_fname.size() - ext.size()) == low_game) {
                    closedir(vd);
                    return folder + "/" + fname;
                }
            }
        }
    }
    closedir(vd);
    return "";
}

// Cerca il video di un gioco.
// Se cur_rel punta a una subfolder del sistema, prova prima lì poi ricade
// sulla cartella videos del sistema.
std::string find_video_file(const std::string& roms_base, const std::string& sys_name,
                             const std::string& game_name,
                             const std::string& cur_rel = "") {
    // 1. Prova nella subfolder corrente (se siamo dentro una)
    std::string sys_prefix = "/" + sys_name;
    if (cur_rel.size() > sys_prefix.size() &&
        cur_rel.compare(0, sys_prefix.size(), sys_prefix) == 0) {
        std::string sub_vf = find_subdir_ignore_case(roms_base + cur_rel, "videos");
        if (!sub_vf.empty()) {
            std::string found = find_video_in_folder(sub_vf, game_name);
            if (!found.empty()) return found;
        }
    }
    // 2. Ricade sulla cartella videos del sistema
    std::string sys_folder = find_subdir_ignore_case(roms_base + "/" + sys_name, "videos");
    if (sys_folder.empty()) return "";
    return find_video_in_folder(sys_folder, game_name);
}

// Cerca il file media (boxart/screenshots/marquees/3dboxes/ecc.) per un gioco.
// Se cur_rel punta a una subfolder, prova prima <subfolder>/<categoria>/
// poi ricade su <sistema>/<categoria>/.
// Per favorites/lastplayed cur_rel è "" quindi usa sempre solo la root.
static std::string find_game_media(const std::string& roms_base,
                                    const std::string& sys,
                                    const std::string& cur_rel,
                                    const std::string& category,
                                    const std::string& g_name) {
    std::string sys_prefix = "/" + sys;
    // 1. Prova nella subfolder corrente
    if (cur_rel.size() > sys_prefix.size() &&
        cur_rel.compare(0, sys_prefix.size(), sys_prefix) == 0) {
        std::string sub_media = find_subdir_ignore_case(roms_base + cur_rel, category);
        if (!sub_media.empty()) {
            std::string found = find_file_ignore_case(sub_media, g_name);
            if (!found.empty()) return found;
        }
    }
    // 2. Ricade sulla root del sistema
    std::string root_media = find_subdir_ignore_case(roms_base + "/" + sys, category);
    if (root_media.empty()) return "";
    return find_file_ignore_case(root_media, g_name);
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
    video_audio_queued_bytes = 0; // reset contatore per fade-in

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
                                // Fade-in audio sincronizzato al fade visivo (video_fade_ms da theme.cfg)
                                // 44100 Hz stereo S16 = 176400 byte/s = 176.4 byte/ms
                                int fade_ms = (theme_cfg.video_fade_ms > 0) ? theme_cfg.video_fade_ms : 500;
                                uint64_t total_q = video_audio_queued_bytes.load();
                                float fade_end_bytes = (float)fade_ms * 176.4f;
                                if ((float)total_q < fade_end_bytes) {
                                    int16_t* smp = (int16_t*)out_buf.data();
                                    for (int fi = 0; fi < converted; fi++) {
                                        float pos = (float)(total_q + (uint64_t)fi * 4);
                                        if (pos >= fade_end_bytes) break;
                                        float vol = pos / fade_end_bytes;
                                        smp[fi * 2]     = (int16_t)((int)(smp[fi * 2])     * vol);
                                        smp[fi * 2 + 1] = (int16_t)((int)(smp[fi * 2 + 1]) * vol);
                                    }
                                }
                                video_audio_queued_bytes.fetch_add((uint64_t)converted * 4);
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
                                         fps, num_bytes]() {
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

            // Audio-master clock: il video si sincronizza sull'audio reale.
            // Questo compensa automaticamente l'overhead del kernel ARM (HZ=100 → sleep arrotonda a 10ms)
            // senza dipendere da un timer fisso che accumula errore.
            int64_t frame_count = 0;

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
                        frame_count = 0; // reset contatore frame al nuovo loop
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

                        // Posizione video corrente (in secondi)
                        double video_pos_sec = (double)frame_count / fps;
                        frame_count++;

                        // Posizione audio corrente: byte_suonati / byte_per_secondo
                        // byte_suonati = totale_inviati_a_SDL - byte_ancora_in_coda_SDL
                        double audio_pos_sec = 0.0;
                        if (video_audio_device) {
                            uint64_t q   = video_audio_queued_bytes.load();
                            uint64_t sdl = (uint64_t)SDL_GetQueuedAudioSize(video_audio_device);
                            uint64_t played = (q > sdl) ? q - sdl : 0;
                            audio_pos_sec = (double)played / 176400.0; // 44100*2ch*2byte
                        }

                        double diff = video_pos_sec - audio_pos_sec; // >0 video avanti, <0 video indietro

                        if (diff < -0.100) {
                            // Video >100ms in ritardo: salta scala/display per recuperare
                            // (il decode è già fatto e mantiene lo stato del codec)
                            continue;
                        }

                        sws_scale(sws_ctx, vf->data, vf->linesize, 0,
                                  v_codec_ctx->height, rvf->data, rvf->linesize);
                        {
                            std::lock_guard<std::mutex> lock(video_mutex);
                            memcpy(video_frame_buffer.data(), lbuf, num_bytes);
                            video_frame_ready = true;
                        }

                        // Se video in anticipo sull'audio: dormi la differenza esatta.
                        // Questo compensa automaticamente l'overshoot del kernel ARM:
                        // se sleep(33ms)→40ms, il prossimo diff sarà 7ms in meno → sleep più corto.
                        if (diff > 0.005 && !vpq->flush_requested && video_running) {
                            int64_t sleep_us = (int64_t)(diff * 1.0e6);
                            if (sleep_us > 200000) sleep_us = 200000; // cap 200ms
                            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
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
            if (video_audio_device) {
                SDL_ClearQueuedAudio(video_audio_device);
                video_audio_queued_bytes = 0; // reset clock per il nuovo loop
            }

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

// --- COLLECTIONS ---
std::vector<std::string> collection_names;
std::map<std::string, std::vector<std::string>> collection_games;

void load_collections() {
    collection_names.clear();
    collection_games.clear();
    std::ifstream f(base_p + "collections.txt");
    if (!f) return;
    std::string line, current;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            current = line.substr(1, line.size() - 2);
            if (!current.empty() && collection_games.find(current) == collection_games.end()) {
                collection_names.push_back(current);
                collection_games[current] = {};
            }
        } else if (!current.empty()) {
            collection_games[current].push_back(line);
        }
    }
}

void save_collections() {
    std::ofstream f(base_p + "collections.txt");
    for (const auto& name : collection_names) {
        f << "[" << name << "]\n";
        for (const auto& gid : collection_games.at(name))
            f << gid << "\n";
    }
}

// Chiamato dopo load_superfolders() e load_collections():
// le collection elencate in SuperFolders.txt vengono nascoste dal carousel
// e rimosse da systems_in_superfolders (per non apparire come sistemi nei SF)
void compute_collections_in_superfolders() {
    collections_in_superfolders.clear();
    for (const auto& cname : collection_names) {
        if (systems_in_superfolders.count(cname)) {
            collections_in_superfolders.insert(cname);
            systems_in_superfolders.erase(cname);
        }
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

// --- LAST PLAYED ---
struct LastPlayedEntry {
    std::string game_id;
    std::string system;
    std::string display_name;
};

static const int LP_MAX_ENTRIES = 50;
std::vector<LastPlayedEntry> lastplayed_list;

void load_lastplayed() {
    lastplayed_list.clear();
    std::ifstream f("/sdcard/bin/AGSG_CCF_FE/lastplayed.txt");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::string gid  = line.substr(0, t1);
        std::string sys  = line.substr(t1 + 1, t2 - t1 - 1);
        std::string disp = line.substr(t2 + 1);
        if (!gid.empty() && !sys.empty())
            lastplayed_list.push_back({gid, sys, disp});
    }
}

void save_lastplayed() {
    std::ofstream f("/sdcard/bin/AGSG_CCF_FE/lastplayed.txt");
    for (const auto& lp : lastplayed_list)
        f << lp.game_id << '\t' << lp.system << '\t' << lp.display_name << '\n';
}

void add_to_lastplayed(const std::string& game_id, const std::string& sys, const std::string& display) {
    lastplayed_list.erase(
        std::remove_if(lastplayed_list.begin(), lastplayed_list.end(),
            [&](const LastPlayedEntry& e){ return e.game_id == game_id; }),
        lastplayed_list.end());
    lastplayed_list.insert(lastplayed_list.begin(), {game_id, sys, display});
    if ((int)lastplayed_list.size() > LP_MAX_ENTRIES)
        lastplayed_list.resize(LP_MAX_ENTRIES);
    save_lastplayed();
}

std::vector<MenuItem> load_lastplayed_as_items() {
    std::vector<MenuItem> lp_items;
    lp_items.reserve(lastplayed_list.size());
    for (const auto& lp : lastplayed_list)
        lp_items.push_back({lp.display_name, false});
    return lp_items;
}

// --- USER PERSONAL RATINGS (separate XML storage) ---
std::map<std::string, int> user_ratings; // game_id -> 1..5, 0 means unset

static std::string user_ratings_path() {
    return base_p + "user_ratings.xml";
}

static std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string extract_attr(const std::string& tag, const std::string& attr) {
    std::string needle = attr + "=\"";
    size_t p = tag.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    size_t e = tag.find('"', p);
    if (e == std::string::npos) return "";
    return tag.substr(p, e - p);
}

static std::string xml_unescape(const std::string& s);

void load_user_ratings() {
    user_ratings.clear();
    std::ifstream f(user_ratings_path());
    if (!f.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    size_t pos = 0;
    while (pos < content.size()) {
        size_t gs = content.find("<game", pos);
        if (gs == std::string::npos) break;
        size_t ge = content.find('>', gs);
        if (ge == std::string::npos) break;
        std::string tag = content.substr(gs, ge - gs + 1);
        std::string id = xml_unescape(extract_attr(tag, "id"));
        std::string val = extract_attr(tag, "value");
        if (!id.empty() && !val.empty()) {
            try {
                int v = std::stoi(val);
                if (v >= 1 && v <= 5) user_ratings[id] = v;
            } catch (...) {}
        }
        pos = ge + 1;
    }
}

void save_user_ratings() {
    const std::string path = user_ratings_path();
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp);
    if (!f.is_open()) return;
    f << "<ratings>\n";
    for (const auto& kv : user_ratings) {
        if (kv.second < 1 || kv.second > 5) continue;
        f << "  <game id=\"" << xml_escape(kv.first) << "\" value=\"" << kv.second << "\"/>\n";
    }
    f << "</ratings>\n";
    f.close();
    std::rename(tmp.c_str(), path.c_str());
}

static int get_user_rating(const std::string& game_id) {
    auto it = user_ratings.find(game_id);
    if (it != user_ratings.end()) return it->second;
    // Fallback: prova con solo sistema/nomefile (ignora sottocartelle intermedie)
    // utile quando il game_id in favorites include un subfolder (es. Amiga/0/file.lha)
    // ma il rating era stato impostato con la vista radice (Amiga/file.lha)
    size_t first_slash = game_id.find('/');
    size_t last_slash  = game_id.rfind('/');
    if (first_slash != std::string::npos && last_slash != first_slash) {
        std::string fallback = game_id.substr(0, first_slash + 1) + game_id.substr(last_slash + 1);
        auto it2 = user_ratings.find(fallback);
        if (it2 != user_ratings.end()) return it2->second;
    }
    return 0;
}

static void set_user_rating(const std::string& game_id, int rating) {
    if (game_id.empty()) return;
    if (rating < 0) rating = 0;
    if (rating > 5) rating = 5;
    if (rating == 0) user_ratings.erase(game_id);
    else user_ratings[game_id] = rating;
    save_user_ratings();
}

static std::string build_selected_game_id(const std::vector<MenuItem>& items, int selected,
                                          bool in_favorites, bool in_lastplayed,
                                          const std::string& current_sys, const std::string& current_rel,
                                          bool in_collection = false, const std::string& current_collection_name = "") {
    if (selected < 0 || selected >= (int)items.size()) return "";
    if (items[selected].is_dir) return "";

    if (in_favorites && selected < (int)favorites_list.size())
        return favorites_list[selected].game_id;
    if (in_lastplayed && selected < (int)lastplayed_list.size())
        return lastplayed_list[selected].game_id;
    if (in_collection && collection_games.count(current_collection_name)) {
        const auto& gids = collection_games.at(current_collection_name);
        if (selected < (int)gids.size()) return gids[selected];
        return "";
    }

    if (current_sys.empty()) return "";
    std::string rel_path;
    std::string prefix = "/" + current_sys;
    if (current_rel.rfind(prefix, 0) == 0)
        rel_path = current_rel.substr(prefix.length());
    return current_sys + rel_path + "/" + items[selected].name;
}

// Estrae sistema e nome file da un game_id per uso in collezioni/preferiti
static void split_game_id(const std::string& gid, std::string& out_sys, std::string& out_fname) {
    size_t sl = gid.find('/');
    out_sys = (sl != std::string::npos) ? gid.substr(0, sl) : "";
    size_t last_sl = gid.rfind('/');
    out_fname = (last_sl != std::string::npos) ? gid.substr(last_sl + 1) : gid;
}

// Ricava da un game_id il current_rel equivalente da passare a find_game_media.
// "Atari 2600/GSG Atari/game.rom" → "/Atari 2600/GSG Atari"
// "Atari 2600/game.rom"           → "/Atari 2600"  (nessuna subfolder, lookup root)
static std::string game_id_to_cur_rel(const std::string& gid) {
    size_t last_sl = gid.rfind('/');
    if (last_sl == std::string::npos) return "";
    return "/" + gid.substr(0, last_sl);   // tutto tranne il filename
}

static std::string format_release_date(const std::string& raw) {
    std::string d;
    for (char c : raw) if (isdigit((unsigned char)c)) d += c;
    if (d.size() >= 8) {
        return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
    }
    return raw;
}

static std::string format_external_rating(const std::string& raw) {
    if (raw.empty()) return "N/A";
    try {
        double r = std::stod(raw);
        if (r <= 1.0) r *= 5.0;
        else if (r > 5.0 && r <= 100.0) r /= 20.0;
        if (r < 0.0) r = 0.0;
        if (r > 5.0) r = 5.0;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f/5", r);
        return std::string(buf);
    } catch (...) {
        return raw;
    }
}

static void draw_star_rating(SDL_Renderer* renderer, int rating, int x, int y, int sz = 15) {
    for (int i = 0; i < 5; i++) {
        SDL_Texture* tex = (i < rating) ? tex_star_filled : tex_star_empty;
        SDL_Rect dst = { x + i * (sz + 2), y, sz, sz };
        if (tex) {
            SDL_RenderCopy(renderer, tex, NULL, &dst);
        } else {
            // fallback testo se le immagini non sono disponibili
            SDL_SetRenderDrawColor(renderer, 255, 220, 80, 255);
            SDL_RenderFillRect(renderer, &dst);
        }
    }
}

// --- GAMELIST.XML PARSER ---
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

// Parsa un gamelist.xml e inserisce le entry nella mappa dest.
// Le entry già presenti in dest non vengono sovrascritte (chiama con la mappa
// vuota per un caricamento da zero; chiamala di nuovo con la mappa già popolata
// per fare un merge dove le nuove entry vincono su quelle esistenti).
static void parse_gamelist_into(std::map<std::string, GameInfo>& dest,
                                 const std::string& gl_path) {
    std::ifstream f(gl_path);
    if (!f.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    size_t pos = 0;
    while (pos < content.size()) {
        size_t game_start = content.find("<game", pos);
        if (game_start == std::string::npos) break;
        char nc = (game_start + 5 < content.size()) ? content[game_start + 5] : 0;
        if (nc != '>' && nc != ' ' && nc != '\t' && nc != '\n' && nc != '\r')
            { pos = game_start + 5; continue; }
        size_t game_end = content.find("</game>", game_start);
        if (game_end == std::string::npos) break;
        game_end += 7;

        std::string block = content.substr(game_start, game_end - game_start);

        GameInfo gi;
        gi.path        = extract_tag(block, "path");
        gi.name        = extract_tag(block, "name");
        gi.desc        = extract_tag(block, "desc");
        gi.image       = extract_tag(block, "image");
        gi.video       = extract_tag(block, "video");
        gi.genre       = extract_tag(block, "genre");
        gi.developer   = extract_tag(block, "developer");
        gi.publisher   = extract_tag(block, "publisher");
        gi.players     = extract_tag(block, "players");
        gi.rating      = extract_tag(block, "rating");
        gi.releasedate = extract_tag(block, "releasedate");
        gi.cheevos     = (extract_tag(block, "cheevos") == "1");

        // Chiave: filename senza path e senza estensione, lowercase
        std::string key = gi.path;
        if (key.size() > 2 && key[0] == '.' && key[1] == '/') key = key.substr(2);
        size_t last_slash = key.find_last_of('/');
        if (last_slash != std::string::npos) key = key.substr(last_slash + 1);
        size_t dot = key.find_last_of('.');
        if (dot != std::string::npos) key = key.substr(0, dot);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (!key.empty())
            dest[key] = gi;   // le entry più recenti vincono (merge)

        pos = game_end;
    }
}

// Carica il gamelist del sistema e, se current_rel punta a una subfolder,
// fa merge del gamelist di quella subfolder sopra (le sue entry vincono).
// Uso: load_gamelist(roms_base + "/" + sys) oppure
//      load_gamelist(roms_base + "/" + sys, roms_base + current_rel)
void load_gamelist(const std::string& system_path,
                   const std::string& subfolder_path = "") {
    current_gamelist.clear();
    // 1. Carica sempre il gamelist root del sistema
    parse_gamelist_into(current_gamelist, system_path + "/gamelist.xml");
    // 2. Se siamo in una subfolder diversa dalla root, carica anche il suo
    //    gamelist e fa merge sopra (le entry della subfolder hanno priorità)
    if (!subfolder_path.empty() && subfolder_path != system_path) {
        parse_gamelist_into(current_gamelist, subfolder_path + "/gamelist.xml");
    }
}

// Rimuove la voce di un gioco da gamelist.xml (dato il path della cartella sistema e il filename ROM)
static void remove_from_gamelist_xml(const std::string& system_path, const std::string& rom_filename) {
    std::string gl_path = system_path + "/gamelist.xml";
    std::ifstream f(gl_path);
    if (!f.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Match by full filename WITH extension (lowercase) — prevents false matches on shared base names
    std::string key = rom_filename;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    std::string result;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t gs = content.find("<game", pos);
        if (gs == std::string::npos) { result += content.substr(pos); break; }
        char nc = (gs + 5 < content.size()) ? content[gs + 5] : 0;
        if (nc != '>' && nc != ' ' && nc != '\t' && nc != '\n' && nc != '\r')
            { result += content.substr(pos, gs - pos + 5); pos = gs + 5; continue; }
        result += content.substr(pos, gs - pos);
        size_t ge = content.find("</game>", gs);
        if (ge == std::string::npos) { result += content.substr(gs); break; }
        ge += 7;
        std::string block = content.substr(gs, ge - gs);
        std::string path_val = extract_tag(block, "path");
        // Extract just the filename from the path (strip any leading ./ or directory components)
        std::string fname = path_val;
        size_t sl = fname.rfind('/'); if (sl != std::string::npos) fname = fname.substr(sl + 1);
        if (fname.size() > 2 && fname[0] == '.' && fname[1] == '/') fname = fname.substr(2);
        std::transform(fname.begin(), fname.end(), fname.begin(), ::tolower);
        if (fname != key) result += block; // keep; else skip (delete exact match)
        pos = ge;
    }
    std::ofstream out(gl_path);
    if (out.is_open()) out << result;
}

// Carica e mette in cache il gamelist di un sistema specifico.
// Mergia anche i gamelist presenti nelle sottocartelle immediate, così
// find_game_info_for_system trova i giochi dei subfolder senza bisogno
// di conoscere la sottocartella specifica.
const std::map<std::string, GameInfo>& get_system_gamelist(const std::string& system) {
    auto it = gamelist_cache.find(system);
    if (it != gamelist_cache.end()) return it->second;
    // Salva current_gamelist e carica root gamelist
    std::map<std::string, GameInfo> old = current_gamelist;
    std::string sys_path = gamelist_roms_base + "/" + system;
    load_gamelist(sys_path);
    // Mergia anche i gamelist nelle sottocartelle immediate.
    // Usa d_type invece di stat() per evitare una syscall per ogni ROM:
    // su FAT32/ext4/exFAT (tipici della GSG) d_type è sempre valorizzato.
    // stat() viene usato solo come fallback quando d_type == DT_UNKNOWN.
    DIR* dir = opendir(sys_path.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            bool is_dir = false;
            if (ent->d_type == DT_DIR) {
                is_dir = true;
            } else if (ent->d_type == DT_UNKNOWN) {
                struct stat st;
                std::string sub_path = sys_path + "/" + ent->d_name;
                if (stat(sub_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                    is_dir = true;
            }
            if (is_dir)
                parse_gamelist_into(current_gamelist, sys_path + "/" + ent->d_name + "/gamelist.xml");
        }
        closedir(dir);
    }
    gamelist_cache[system] = current_gamelist;
    current_gamelist = old;
    return gamelist_cache[system];
}

// --- SYSTEMS_DESC.XML PARSER ---
// Mappa: system name (lowercase) -> description / fullname
std::map<std::string, std::string> systems_desc;
std::map<std::string, std::string> systems_fullname; // folder_name(lower) -> display name

// --- SYSTEMS_ORDER.XML ---

void load_systems_order() {
    systems_custom_order.clear();
    // Cerca nella stessa directory di systems_desc.xml
    std::string path = base_p + "systems_order.xml";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        // Rimuovi spazi/CR
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        while (!line.empty() && line.front() == ' ') line = line.substr(1);
        if (line.empty() || line[0] == '#') continue;
        // Accetta sia <system>nome</system> che righe plain
        if (line.rfind("<system>", 0) == 0) {
            size_t e = line.find("</system>");
            if (e != std::string::npos)
                line = line.substr(8, e - 8);
            else
                line = line.substr(8);
        }
        if (!line.empty()) systems_custom_order.push_back(line);
    }
}

// Comparatore per l'ordinamento dei sistemi secondo system_sort_order
static bool compare_systems(const MenuItem& a, const MenuItem& b) {
    if (system_sort_order == 1) {
        // Ordine per nome cartella (case-insensitive)
        std::string fa = a.name, fb = b.name;
        std::transform(fa.begin(), fa.end(), fa.begin(), ::tolower);
        std::transform(fb.begin(), fb.end(), fb.begin(), ::tolower);
        return fa < fb;
    }
    // Default (0 o 2 non gestito qui): ordine per nome sistema
    std::string da = get_system_fullname(a.name);
    std::string db = get_system_fullname(b.name);
    std::transform(da.begin(), da.end(), da.begin(), ::tolower);
    std::transform(db.begin(), db.end(), db.begin(), ::tolower);
    return da < db;
}

void load_systems_desc() {
    systems_desc.clear();
    systems_fullname.clear();
    std::string path = theme_p() + "systems_desc.xml";
    std::ifstream f(path);
    if (!f.is_open()) {
        path = base_p + "systems_desc.xml";
        f.open(path);
        if (!f.is_open()) return;
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    size_t pos = 0;
    while (pos < content.size()) {
        size_t gs = content.find("<game", pos);
        if (gs == std::string::npos) break;
        char nc2 = (gs + 5 < content.size()) ? content[gs + 5] : 0;
        if (nc2 != '>' && nc2 != ' ' && nc2 != '\t' && nc2 != '\n' && nc2 != '\r')
            { pos = gs + 5; continue; }
        size_t ge = content.find("</game>", gs);
        if (ge == std::string::npos) break;
        ge += 7;
        std::string block = content.substr(gs, ge - gs);
        std::string name = extract_tag(block, "name");
        std::string desc = extract_tag(block, "desc");
        std::string fullname = extract_tag(block, "fullname");
        if (!name.empty()) {
            std::string key = name;
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            systems_desc[key] = desc;
            if (!fullname.empty())
                systems_fullname[key] = fullname;
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

// Restituisce il nome visualizzato del sistema: <fullname> se definito, altrimenti il nome cartella
std::string get_system_fullname(const std::string& sys_name) {
    std::string key = sys_name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = systems_fullname.find(key);
    return (it != systems_fullname.end()) ? it->second : sys_name;
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

// Ri-ordina items alfabeticamente per nome visualizzato (gamelist o filename)
// Usata quando show_gamelist_names e' attivo, dopo aver caricato il gamelist
static void sort_items_by_display_name(std::vector<MenuItem>& items,
                                       const std::map<std::string, GameInfo>& gl) {
    std::sort(items.begin(), items.end(), [&gl](const MenuItem& a, const MenuItem& b) {
        if (a.is_dir != b.is_dir) return a.is_dir; // cartelle prima
        auto get_key = [&gl](const MenuItem& m) -> std::string {
            std::string k = m.name;
            if (!m.is_dir) {
                size_t dot = k.find_last_of('.');
                if (dot != std::string::npos) k = k.substr(0, dot);
                std::string low = k;
                std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                auto it = gl.find(low);
                if (it != gl.end() && !it->second.name.empty()) k = it->second.name;
            }
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            return k;
        };
        return get_key(a) < get_key(b);
    });
}

#ifndef CROSS_PLATFORM
// Thread che fa lampeggiare il LED di alimentazione (ON/OFF ogni 300ms)
static void* led_blink_thread(void*) {
    const char* led_paths[] = {
        "/sys/class/leds/led_pwr/brightness",
        "/sys/class/leds/key_led/brightness",
        nullptr
    };
    const char* led_path = nullptr;
    for (int i = 0; led_paths[i]; ++i) {
        std::ifstream test(led_paths[i]);
        if (test.good()) { led_path = led_paths[i]; break; }
    }
    if (!led_path) return nullptr;
    while (led_blink_active.load()) {
        { std::ofstream f(led_path); if (f) f << "1\n"; }
        for (int i = 0; i < 30 && led_blink_active.load(); ++i) usleep(10000); // 300ms ON
        { std::ofstream f(led_path); if (f) f << "0\n"; }
        for (int i = 0; i < 30 && led_blink_active.load(); ++i) usleep(10000); // 300ms OFF
    }
    // Ripristina LED acceso all'uscita
    { std::ofstream f(led_path); if (f) f << "1\n"; }
    return nullptr;
}
#endif

void show_collection_picker(SDL_Renderer* renderer, const std::string& font_path,
                            SDL_Texture* bg_tex,
                            const std::string& game_id, const std::string& game_name) {
    if (collection_names.empty()) return;

    if (font_cache.find(22) == font_cache.end()) font_cache[22] = ttf_open_font(font_path, 22);
    if (font_cache.find(18) == font_cache.end()) font_cache[18] = ttf_open_font(font_path, 18);
    TTF_Font* f22 = font_cache.count(22) ? font_cache[22] : nullptr;
    TTF_Font* f18 = font_cache.count(18) ? font_cache[18] : nullptr;

    int sel = 0;
    bool open = true;
    SDL_Event ev;
    Uint32 next_input = SDL_GetTicks() + 200;

    while (open) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_JOYDEVICEADDED) SDL_JoystickOpen(ev.jdevice.which);
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) open = false;
#ifdef NATIVE_BASE_PATH
            if (ev.type == SDL_KEYDOWN) {
                int vbtn = -1;
                switch (ev.key.keysym.sym) {
                    case SDLK_UP:     vbtn = 29; break;
                    case SDLK_DOWN:   vbtn = 32; break;
                    case SDLK_a:
                    case SDLK_RETURN: vbtn = 1;  break;
                    case SDLK_b:      vbtn = 3;  break;
                    case SDLK_s:      vbtn = 7;  break;
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
            if (ev.type == SDL_JOYBUTTONDOWN) {
                if (SDL_GetTicks() < next_input) continue;
                int btn = ev.jbutton.button;
                if (btn == 29 || btn == 30) {
                    sel = (sel - 1 + (int)collection_names.size()) % (int)collection_names.size();
                    play_sound(s_click); next_input = SDL_GetTicks() + 150;
                } else if (btn == 32 || btn == 31) {
                    sel = (sel + 1) % (int)collection_names.size();
                    play_sound(s_click); next_input = SDL_GetTicks() + 150;
                } else if (btn == 1) {
                    const std::string& cname = collection_names[sel];
                    auto& glist = collection_games[cname];
                    auto it = std::find(glist.begin(), glist.end(), game_id);
                    if (it != glist.end()) { glist.erase(it); play_sound(s_back); }
                    else { glist.push_back(game_id); play_sound(s_enter); }
                    save_collections();
                    next_input = SDL_GetTicks() + 200;
                } else if (btn == 3 || btn == 6 || btn == 7) {
                    play_sound(s_back); open = false;
                }
            }
        }

        if (bg_tex) {
            SDL_SetTextureAlphaMod(bg_tex, 140);
            SDL_RenderCopy(renderer, bg_tex, NULL, NULL);
            SDL_SetTextureAlphaMod(bg_tex, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
        }

        int sw = 0, sh = 0;
        SDL_GetRendererOutputSize(renderer, &sw, &sh);
        int row_h = theme_cfg.menu_disp_row_h;
        int max_visible = std::min((int)collection_names.size(), 8);
        int bw = sw * 2 / 3;
        int bh = 50 + max_visible * row_h;
        int bx = (sw - bw) / 2, by = (sh - bh) / 2;

        SDL_SetRenderDrawColor(renderer, 20, 20, 40, 230);
        SDL_Rect panel = {bx, by, bw, bh};
        SDL_RenderFillRect(renderer, &panel);
        SDL_SetRenderDrawColor(renderer, 80, 80, 180, 255);
        SDL_RenderDrawRect(renderer, &panel);

        if (f22) {
            std::string title = "Add to collection: " + game_name;
            SDL_Surface* s = ttf_render_text_blended(f22, title.c_str(), theme_cfg.menu_item_selected);
            if (s) {
                SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                if (t) { SDL_Rect r = {bx + bw/2 - s->w/2, by + 8, s->w, s->h}; SDL_RenderCopy(renderer, t, NULL, &r); SDL_DestroyTexture(t); }
                SDL_FreeSurface(s);
            }
        }

        int scroll_off = std::max(0, sel - max_visible + 1);
        for (int i = scroll_off; i < (int)collection_names.size() && (i - scroll_off) < max_visible; i++) {
            int ry = by + 40 + (i - scroll_off) * row_h;
            if (i == sel) {
                SDL_Rect hl = {bx + 4, ry, bw - 8, row_h};
                SDL_SetRenderDrawColor(renderer, theme_cfg.menu_highlight.r, theme_cfg.menu_highlight.g, theme_cfg.menu_highlight.b, theme_cfg.menu_highlight.a);
                SDL_RenderFillRect(renderer, &hl);
            }
            const std::string& cname = collection_names[i];
            const auto& glist = collection_games.at(cname);
            bool has = std::find(glist.begin(), glist.end(), game_id) != glist.end();
            SDL_Color lc = (i == sel) ? theme_cfg.menu_item_selected : theme_cfg.menu_item_normal;
            if (f22) {
                SDL_Surface* s = ttf_render_text_blended(f22, cname.c_str(), lc);
                if (s) {
                    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                    if (t) { SDL_Rect r = {bx + 20, ry + row_h/2 - s->h/2, s->w, s->h}; SDL_RenderCopy(renderer, t, NULL, &r); SDL_DestroyTexture(t); }
                    SDL_FreeSurface(s);
                }
                SDL_Color bc = has ? theme_cfg.menu_badge_on : theme_cfg.menu_badge_off;
                SDL_Surface* bs = ttf_render_text_blended(f22, has ? "YES" : "NO", bc);
                if (bs) {
                    SDL_Texture* bt = SDL_CreateTextureFromSurface(renderer, bs);
                    if (bt) { SDL_Rect r = {bx + bw - bs->w - 20, ry + row_h/2 - bs->h/2, bs->w, bs->h}; SDL_RenderCopy(renderer, bt, NULL, &r); SDL_DestroyTexture(bt); }
                    SDL_FreeSurface(bs);
                }
            }
        }

        if (f18) {
            SDL_Surface* s = ttf_render_text_blended(f18, "[A] Add/Remove  [B/START] Close", theme_cfg.menu_hint);
            if (s) {
                SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
                if (t) { SDL_Rect r = {bx + bw/2 - s->w/2, by + bh + 6, s->w, s->h}; SDL_RenderCopy(renderer, t, NULL, &r); SDL_DestroyTexture(t); }
                SDL_FreeSurface(s);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
}

// Background worker: pre-populates game_count_cache (phase 1) and
// g_surf_cache for carousel images (phase 2). Exits early if g_preload_stop is set.
// system_configs is read-only after startup → safe to access without mutex.
static void carousel_preload_worker(std::vector<std::string> sys_names,
                                    std::string roms_base_copy,
                                    std::string theme_path_copy) {
    // Phase 1: game counts (most impactful — recursive readdir can be slow for large collections)
    for (const auto& name : sys_names) {
        if (g_preload_stop.load(std::memory_order_relaxed)) return;
        {
            std::lock_guard<std::mutex> lk(g_count_mutex);
            if (game_count_cache.count(name)) continue; // already cached (user visited it first)
        }
        auto cfg_it = system_configs.find(name);
        if (cfg_it == system_configs.end()) continue;
        int count = count_games_recursive_impl(roms_base_copy + "/" + name, name, cfg_it->second);
        {
            std::lock_guard<std::mutex> lk(g_count_mutex);
            game_count_cache.emplace(name, count);
        }
    }
    // Phase 2: carousel images as SDL_Surface (IMG_Load is thread-safe).
    // SDL_CreateTextureFromSurface is called later on the main thread via load_texture_cached().
    for (const auto& name : sys_names) {
        if (g_preload_stop.load(std::memory_order_relaxed)) return;
        preload_get_surface(theme_path_copy + "bg/" + name + ".png");
        preload_get_surface(theme_path_copy + "bg/" + name + ".jpg");
        preload_get_surface(theme_path_copy + "systems/" + name + ".png");
    }
    preload_get_surface(theme_path_copy + "bg/default.png");
    preload_get_surface(theme_path_copy + "bg/default.jpg");
    preload_get_surface(theme_path_copy + "systems/default.png");
}

// Build the list of system names for the preloader (excludes virtual/special entries).
static std::vector<std::string> preload_sys_names(const std::vector<MenuItem>& items) {
    std::vector<std::string> v;
    v.reserve(items.size());
    for (const auto& it : items)
        if (!it.is_superfolder && !it.is_collection &&
            it.name != "FAVORITES" && it.name != "LAST PLAYED")
            v.push_back(it.name);
    return v;
}

// Start (or restart) the preload thread. Stops any running thread first.
static void preload_start(const std::vector<MenuItem>& items,
                          const std::string& roms_base, const std::string& theme_path) {
    preload_stop_join();
    g_preload_stop.store(false);
    g_preload_th = std::thread(carousel_preload_worker,
                               preload_sys_names(items), roms_base, theme_path);
}

int main(int, char* argv[]) {
#ifdef NATIVE_BASE_PATH
    base_p = get_native_base_path(argv[0]);
    img_p  = base_p + "images/";
#else
    (void)argv;
#endif
#ifdef BUILD_TIMESTAMP
    fprintf(stderr, "Launcher build: %s\n", BUILD_TIMESTAMP);
#else
    fprintf(stderr, "Launcher build: %s %s\n", __DATE__, __TIME__);
#endif
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO);
#ifndef CROSS_PLATFORM
    signal(SIGTERM, SIG_IGN); 
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN); 
#endif
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    load_extensions_cfg(base_p + "extensions_cfg.txt");
    if (startup_volume >= 0) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "amixer sset Master %d%% -q 2>/dev/null", startup_volume);
        system(cmd);
    }
    if (startup_brightness >= 10) {
        apply_screen_brightness(startup_brightness);
    }
    load_superfolders();
    load_favs();
    load_lastplayed();
    load_collections();
    compute_collections_in_superfolders();

    // Helper: costruisce gli item di una collection ordinati per nome visualizzato.
    // Modifica collection_games[name] in-place (solo ordine, non contenuto) e
    // pre-warma il gamelist cache di tutti i sistemi presenti.
    // Sicuro da chiamare più volte: la seconda chiamata trova la cache già calda.
    auto build_collection_items = [&](const std::string& name) -> std::vector<MenuItem> {
        auto& gids = collection_games.at(name);
        std::vector<MenuItem> col_items;
        std::set<std::string> seen_sys;
        for (const auto& gid : gids) {
            col_items.push_back({gid.substr(gid.rfind('/') + 1), false});
            size_t sl = gid.find('/');
            if (sl != std::string::npos) {
                std::string sys = gid.substr(0, sl);
                if (!sys.empty() && !seen_sys.count(sys))
                    { seen_sys.insert(sys); get_system_gamelist(sys); }
            }
        }
        // Ordina per nome visualizzato (gamelist name se disponibile, altrimenti stem)
        std::vector<std::pair<std::string, int>> ord;
        ord.reserve(col_items.size());
        for (int k = 0; k < (int)col_items.size(); k++) {
            std::string cs, cf; split_game_id(gids[k], cs, cf);
            size_t d = cf.rfind('.'); if (d != std::string::npos) cf = cf.substr(0, d);
            const GameInfo* gi = find_game_info_for_system(cs, cf);
            std::string key = (gi && !gi->name.empty()) ? gi->name : cf;
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            ord.push_back({key, k});
        }
        std::stable_sort(ord.begin(), ord.end(),
            [](const std::pair<std::string,int>& a, const std::pair<std::string,int>& b)
            { return a.first < b.first; });
        std::vector<MenuItem> si; std::vector<std::string> sg;
        si.reserve(col_items.size()); sg.reserve(gids.size());
        for (int r = 0; r < (int)ord.size(); r++) {
            si.push_back(col_items[ord[r].second]);
            sg.push_back(gids[ord[r].second]);
        }
        col_items = std::move(si);
        gids      = std::move(sg);
        return col_items;
    };
    load_user_ratings();
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
    load_hdmi_systems();

    load_options_settings();      // Load all settings: theme, display, options
    img_p = theme_p() + "images/"; // Update image path to active theme
    theme_cfg = load_theme_config(theme_p() + "theme.cfg"); // Load theme layout/color config
    load_systems_desc();          // Load system descriptions from systems_desc.xml
    load_systems_order();         // Load custom system sort order (systems_order.xml)
    load_and_convert_sound((theme_p() + "sounds/click.wav").c_str(), s_click);
    load_and_convert_sound((theme_p() + "sounds/enter.wav").c_str(), s_enter);
    load_and_convert_sound((theme_p() + "sounds/back.wav").c_str(), s_back);
    load_and_convert_sound((theme_p() + "sounds/fav.wav").c_str(), s_fav);

    int screen_w, screen_h; SDL_GetRendererOutputSize(renderer, &screen_w, &screen_h);

    font_tex        = load_theme_image(renderer, "font_map.png");
    tex_star_filled = load_theme_image(renderer, "star_filled.png");
    tex_star_empty  = load_theme_image(renderer, "star_empty.png");
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
    SDL_Texture *trophy_tex   = load_theme_image(renderer, "trophy.png");
    SDL_Texture *no_art_tex = load_theme_image(renderer, "no_art.png");
    // Default 3dbox placeholder: themes/<theme>/images/3dbox_default.{png,jpg,...}
    auto load_box3d_default = [&]() -> SDL_Texture* {
        std::string fp = find_file_ignore_case(img_p, "3dbox_default");
        if (fp.empty() && current_theme != "default")
            fp = find_file_ignore_case(base_p + "themes/default/images", "3dbox_default");
        return fp.empty() ? nullptr : load_texture(renderer, fp);
    };
    SDL_Texture *box3d_default_tex = load_box3d_default();
    // Folder 3dbox placeholder: themes/<theme>/images/3dbox_folder.{png,jpg,...}
    auto load_box3d_folder = [&]() -> SDL_Texture* {
        std::string fp = find_file_ignore_case(img_p, "3dbox_folder");
        if (fp.empty() && current_theme != "default")
            fp = find_file_ignore_case(base_p + "themes/default/images", "3dbox_folder");
        return fp.empty() ? nullptr : load_texture(renderer, fp);
    };
    SDL_Texture *box3d_folder_tex = load_box3d_folder();
    // CRT overlay: themes/<theme>/images/crt.png
    auto load_crt_tex = [&]() -> SDL_Texture* {
        std::string fp = find_file_ignore_case(img_p, "crt");
        if (fp.empty() && current_theme != "default")
            fp = find_file_ignore_case(base_p + "themes/default/images", "crt");
        return fp.empty() ? nullptr : load_texture(renderer, fp);
    };
    SDL_Texture *crt_tex = load_crt_tex();
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
        rel(icons_tex);       icons_tex       = load_theme_image(renderer, "icons.png");
        rel(favorite_tex);    favorite_tex    = load_theme_image(renderer, "Favorite.png");
        rel(trophy_tex);      trophy_tex      = load_theme_image(renderer, "trophy.png");
        rel(no_art_tex);      no_art_tex      = load_theme_image(renderer, "no_art.png");
        rel(help_game_tex);  help_game_tex  = load_theme_image(renderer, "help.png");
        rel(help_menu_tex);  help_menu_tex  = load_theme_image(renderer, "help_menu.png");
        rel(arrow_up_tex);   arrow_up_tex   = load_theme_image(renderer, "up.png");
        rel(arrow_down_tex); arrow_down_tex = load_theme_image(renderer, "down.png");
        rel(arrow_left_tex); arrow_left_tex = load_theme_image(renderer, "left.png");
        rel(arrow_right_tex);arrow_right_tex= load_theme_image(renderer, "right.png");
        rel(box3d_default_tex); box3d_default_tex = load_box3d_default();
        rel(box3d_folder_tex);  box3d_folder_tex  = load_box3d_folder();
        rel(crt_tex);           crt_tex           = load_crt_tex();
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
        music_track_index = resolve_music_track_index(music_tracks, load_music_track_for_theme(current_theme));
        if (music_enabled && !music_tracks.empty())
            start_music(music_tracks[music_track_index]);
        else
            stop_music();
    };

    int w_idx = 0; // Indice per l'icona attuale
    Uint32 last_w_check = 0;


    int b_idx = 0;
    int battery_pct = -1;
    bool is_ch = false;
#ifndef CROSS_PLATFORM
    bool prev_is_ch = true; // Inizializzato a true per forzare la scrittura LED al primo ciclo
#endif
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

    // --- SUPERFOLDER: carousel stack and current node ---
    struct CarouselState { std::vector<MenuItem> items; int selected; const SuperFolderNode* sf_node; };
    std::vector<CarouselState> carousel_stack;
    const SuperFolderNode* current_sf_node = nullptr; // nullptr = main carousel

    // Build main carousel: SuperFolders first, then unassigned systems, FAVORITES at front
    auto build_main_carousel = [&]() -> std::vector<MenuItem> {
        std::vector<MenuItem> result;
        for (const auto& sf : superfolder_roots)
            result.push_back({sf.name, true, true});
        std::vector<MenuItem> all_sys = scan_directory(roms_base, true);
        std::vector<MenuItem> unassigned;
        for (auto& it : all_sys) {
            if (hdmi_connected && !hdmi_supported_systems.empty()) {
                std::string low = it.name;
                std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                low.erase(std::remove(low.begin(), low.end(), ' '), low.end());
                if (hdmi_supported_systems.count(low) == 0) continue;
            }
            if (systems_in_superfolders.count(it.name) == 0)
                unassigned.push_back(it);
        }
        if (!show_empty_systems) {
            unassigned.erase(std::remove_if(unassigned.begin(), unassigned.end(), [&](const MenuItem& mit) {
                return count_games_recursive(roms_base + "/" + mit.name, mit.name) == 0;
            }), unassigned.end());
        }
        if (system_sort_order == 2 && !systems_custom_order.empty()) {
            // Custom: ordina secondo systems_custom_order, i non trovati vanno in fondo
            std::vector<MenuItem> ordered, rest;
            for (const auto& s : systems_custom_order)
                for (const auto& it : unassigned)
                    if (it.name == s) { ordered.push_back(it); break; }
            for (const auto& it : unassigned) {
                bool found = false;
                for (const auto& o : ordered) if (o.name == it.name) { found = true; break; }
                if (!found) rest.push_back(it);
            }
            unassigned = ordered;
            unassigned.insert(unassigned.end(), rest.begin(), rest.end());
        } else {
            std::sort(unassigned.begin(), unassigned.end(), compare_systems);
        }
        result.insert(result.end(), unassigned.begin(), unassigned.end());
        if (!result.empty())
            result.insert(result.begin(), MenuItem{"FAVORITES", true, false});
        result.push_back(MenuItem{"LAST PLAYED", true, false});
        for (const auto& cname : collection_names) {
            if (collections_in_superfolders.count(cname)) continue;
            if (!show_empty_systems && collection_games.at(cname).empty()) continue;
            result.push_back(MenuItem{cname, true, false, true});
        }
        return result;
    };

    std::vector<MenuItem> items = build_main_carousel();
    int selected = 0, scroll = 0, last_main_sel = 0, g_count = 0;
    bool in_games = false, in_favorites = false, in_lastplayed = false, in_collection = false, running = true;
    std::string current_collection_name;
    std::vector<int> subfolder_nav_stack; // memorizza gli indici dei folder aperti per ripristinarli al B
#ifdef NATIVE_BASE_PATH
    bool show_kb_help = false;
#endif
    SDL_Event event;
    // Lista sistemi per navigazione sinistra/destra nella lista giochi
    std::vector<std::string> system_list;
    auto rebuild_system_list = [&]() {
        system_list.clear();
        for (const auto& it : items)
            if (it.name != "FAVORITES" && it.name != "LAST PLAYED" && !it.is_superfolder && !it.is_collection) system_list.push_back(it.name);
    };
    rebuild_system_list();

    SDL_Texture *bg_cur = nullptr, *dev_cur = nullptr, *dev_prev = nullptr, *dev_next = nullptr, *box_art = nullptr, *screenshot_tex = nullptr, *marquee_tex = nullptr, *ctrl_cur = nullptr;
    bool last_carousel_vertical = theme_cfg.carousel_vertical;
    float slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f; int slide_dir = 1; std::string last_bg = "";
    float carousel_idle_time = 0.0f; // tempo a riposo per l'effetto floating
    SDL_Color white = theme_cfg.color_text;
    Uint32 selection_timer = 0, input_delay = 0;
    // One-shot joystick reinit 2s dopo il boot: al boot il device può essere aperto
    // prima che il kernel/udev finisca il suo init, causando button ID errati (es. vol 14/16).
    // Il reinit rilegge il device con il mapping definitivo, identico a quello post-gioco.
    Uint32 boot_joy_reinit = SDL_GetTicks() + 2000;
    bool needs_art_update = false;
    bool needs_boxart_immediate = false; // in 3dbox mode: carica boxart subito senza delay
    Uint32 boxart_loaded_at_timer = 0xFFFFFFFFu; // selection_timer al momento dell'ultimo caricamento immediato
    std::string pending_video_path = ""; // video waiting for delay before start
    Uint32 video_start_timer = 0;        // when pending video was queued
    Uint32 art_show_timer    = 0;        // quando l'art è stato caricato (per art_delay_ms)
    bool   art_visible       = false;    // true quando marquee/boxart-overlay devono essere mostrati
    float video_fade_alpha = 1.0f;       // 1.0 = full boxart, 0.0 = full video
    bool video_fading = false;           // fade in progress
    // Bounce on select
    float bounce_scale = 1.0f;          // scale multiplier for bounce animation
    float bounce_timer = -1.0f;         // -1 = inactive
    bool  bounce_enter_pending = false; // deferred system entry after bounce
    float marquee_pos = -40.0f; float desc_marquee_pos = -2.0f; float carousel_desc_marquee_pos = -2.0f; Uint32 last_tick = SDL_GetTicks();
    // Hold tracking for fast scroll (left/right in carousel, up/down in game list)
    bool left_held = false, right_held = false;
    bool up_held = false, down_held = false;
    Uint32 left_hold_start = 0, right_hold_start = 0;
    Uint32 up_hold_start = 0, down_hold_start = 0;
    Uint32 fast_scroll_next = UINT32_MAX; // Never fires until a directional key is first pressed
    // 3dbox carousel state
    std::map<int, SDL_Texture*> box3d_cache; // keyed by item index
    float box3d_slide_pos = 1.0f;           // animation: 0=start, slot_w=done
    int   box3d_slide_dir = 0;              // -1=left, +1=right, 0=none

    if (!items.empty()) {
        update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
        last_bg = items[selected].name;
        if (items[selected].name == "FAVORITES") {
            g_count = (int)favorites_list.size();
        } else if (items[selected].is_superfolder) {
            g_count = 0;
        } else {
            g_count = count_games_recursive(roms_base + "/" + items[selected].name, items[selected].name);
        }
    }

    // Start carousel preloader (game counts + carousel images)
    preload_start(items, roms_base, theme_p());

    // Start background music (carousel)
    music_tracks = scan_music_tracks(current_theme);
    music_track_index = resolve_music_track_index(music_tracks, load_music_track_for_theme(current_theme));
    if (music_enabled && !music_tracks.empty())
        start_music(music_tracks[music_track_index]);

    while (running) {
        Uint32 tick = SDL_GetTicks();
        float dt = (tick - last_tick) / 1000.0f;
        last_tick = tick;

        // Auto-advance: quando una traccia finisce, avvia la successiva
        if (music_autoadvance && music_track_ended.exchange(false) && music_enabled && !music_tracks.empty()) {
            music_track_index = (music_track_index + 1) % (int)music_tracks.size();
            const std::string& p = music_tracks[music_track_index];
            size_t sl = p.find_last_of('/');
            music_track_name_overlay = (sl != std::string::npos) ? p.substr(sl + 1) : p;
            size_t dot = music_track_name_overlay.find_last_of('.');
            if (dot != std::string::npos) music_track_name_overlay = music_track_name_overlay.substr(0, dot);
            music_track_name_until = tick + 3000;
            save_music_track_for_theme(current_theme, music_track_name_overlay);
            start_music(p);
        }

#ifndef CROSS_PLATFORM
        // One-shot: reinit joystick 2s dopo il boot per riprendere il mapping definitivo
        if (boot_joy_reinit && tick >= boot_joy_reinit) {
            boot_joy_reinit = 0;
            SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
            SDL_InitSubSystem(SDL_INIT_JOYSTICK);
            SDL_JoystickEventState(SDL_ENABLE);
            for (int ji = 0; ji < SDL_NumJoysticks(); ji++) SDL_JoystickOpen(ji);
        }
#endif

        // --- LOGICA MARQUEE ---
        static int last_selected = -1;
        static std::string last_dn = "";
        int lim_w = theme_cfg.list_text_max_w;
        if (in_games && !items.empty() && selected < (int)items.size()) {
            std::string dn = items[selected].name;
            if (!items[selected].is_dir) {
                // Per i favoriti usa il nome dal game_id (con estensione ROM), come nel blocco
                // di caricamento art. Evita troncamenti errati per nomi con punto (es. "Dr. Mario").
                if (in_favorites && selected < (int)favorites_list.size()) {
                    const std::string& gid = favorites_list[selected].game_id;
                    size_t sl = gid.find_last_of('/');
                    dn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                } else if (in_collection && collection_games.count(current_collection_name)) {
                    const auto& gids = collection_games.at(current_collection_name);
                    if (selected < (int)gids.size()) { size_t sl = gids[selected].rfind('/'); dn = (sl != std::string::npos) ? gids[selected].substr(sl + 1) : gids[selected]; }
                } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                    const std::string& gid = lastplayed_list[selected].game_id;
                    size_t sl = gid.find_last_of('/');
                    dn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                }
                size_t dot = dn.find_last_of(".");
                if (dot != std::string::npos) dn = dn.substr(0, dot);
                if (show_gamelist_names) {
                    const GameInfo* gi_m = nullptr;
                    if (in_favorites && selected < (int)favorites_list.size())
                        gi_m = find_game_info_for_system(favorites_list[selected].system, dn);
                    else if (in_lastplayed && selected < (int)lastplayed_list.size())
                        gi_m = find_game_info_for_system(lastplayed_list[selected].system, dn);
                    else if (in_collection && collection_games.count(current_collection_name)) {
                        const auto& gids = collection_games.at(current_collection_name);
                        if (selected < (int)gids.size()) { std::string col_sys, col_fn; split_game_id(gids[selected], col_sys, col_fn); size_t d = col_fn.rfind('.'); if (d != std::string::npos) col_fn = col_fn.substr(0, d); gi_m = find_game_info_for_system(col_sys, col_fn); }
                    } else
                        gi_m = find_game_info(dn);
                    if (gi_m && !gi_m->name.empty()) dn = gi_m->name;
                }
            }
            if (in_favorites && selected < (int)favorites_list.size()) {
                std::string sys = favorites_list[selected].system;
                for (auto& c : sys) c = toupper(c);
                dn = "[" + sys + "] " + dn;
            } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                std::string sys = lastplayed_list[selected].system;
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

        // --- POWER LED: acceso quando in carica, spento quando non in carica ---
        if (!led_blink_active.load() && is_ch != prev_is_ch) {
            const char* pwr_led_paths[] = {
                "/sys/class/leds/led_pwr/brightness",
                "/sys/class/leds/key_led/brightness",
                nullptr
            };
            for (int i = 0; pwr_led_paths[i]; ++i) {
                std::ofstream f(pwr_led_paths[i]);
                if (f) { f << (is_ch ? "1\n" : "0\n"); break; }
            }
            prev_is_ch = is_ch;
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
                battery_pct = pct;
                if (pct > 80) b_idx = 0;
                else if (pct > 55) b_idx = 1;
                else if (pct > 30) b_idx = 2;
                else if (pct > 10) b_idx = 3;
                else b_idx = 4;
            }
            // LED blink: attiva se batteria <= 10% e non in carica,
            // oppure se esiste il file test_led_blink.txt (per il test)
            bool test_blink = std::ifstream(base_p + "test_led_blink.txt").good();
            bool want_blink = test_blink || (b_idx == 4 && !is_ch);
            if (want_blink && !led_blink_active.load()) {
                led_blink_active = true;
                pthread_t led_tid;
                pthread_create(&led_tid, nullptr, led_blink_thread, nullptr);
                pthread_detach(led_tid);
            } else if (!want_blink && led_blink_active.load()) {
                led_blink_active = false;
            }
            last_b_check = tick;
        }
#endif // CROSS_PLATFORM

        if (!in_games && !items.empty() && items[selected].name != last_bg) {
            update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
            last_bg = items[selected].name;
            slide_pos = 0.0f;
            carousel_desc_marquee_pos = -2.0f;
            
            // Se è FAVORITES, mostra il numero di favoriti; altrimenti conta i giochi nel sistema
            if (items[selected].name == "FAVORITES") {
                g_count = (int)favorites_list.size();
            } else if (items[selected].name == "LAST PLAYED") {
                g_count = (int)lastplayed_list.size();
            } else if (items[selected].is_collection) {
                g_count = (int)collection_games.at(items[selected].name).size();
            } else if (items[selected].is_superfolder) {
                g_count = 0;
            } else {
                g_count = count_games_recursive(roms_base + "/" + items[selected].name, items[selected].name);
            }
        }

        // --- CARICAMENTO BOXART IMMEDIATO (solo modalità 3dbox) ---
        // Carica box_art e marquee subito al cambio selezione, senza aspettare il delay video.
        // Usa selection_timer come chiave: si attiva una sola volta per ogni nuova selezione.
        if (in_games && theme_cfg.gamelist_3dbox_enabled && needs_art_update
                && boxart_loaded_at_timer != selection_timer) {
            needs_boxart_immediate = true;
        }
        if (in_games && theme_cfg.gamelist_3dbox_enabled && needs_boxart_immediate) {
            needs_boxart_immediate = false;
            boxart_loaded_at_timer = selection_timer;
            if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
            if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
            if (!items.empty() && !items[selected].is_dir) {
                std::string g_name = items[selected].name;
                if (in_favorites && selected < (int)favorites_list.size()) {
                    const std::string& gid = favorites_list[selected].game_id;
                    size_t sl = gid.find_last_of('/');
                    g_name = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                    const std::string& gid = lastplayed_list[selected].game_id;
                    size_t sl = gid.find_last_of('/');
                    g_name = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                } else if (in_collection && collection_games.count(current_collection_name)) {
                    const auto& gids = collection_games.at(current_collection_name);
                    if (selected < (int)gids.size()) { size_t sl = gids[selected].rfind('/'); g_name = (sl != std::string::npos) ? gids[selected].substr(sl + 1) : gids[selected]; }
                }
                size_t dot = g_name.find_last_of("."); if (dot != std::string::npos) g_name = g_name.substr(0, dot);
                std::string sys_to_search = current_sys;
                std::string media_rel = current_rel;
                if (in_favorites && selected < (int)favorites_list.size()) {
                    sys_to_search = favorites_list[selected].system;
                    media_rel = game_id_to_cur_rel(favorites_list[selected].game_id);
                } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                    sys_to_search = lastplayed_list[selected].system;
                    media_rel = game_id_to_cur_rel(lastplayed_list[selected].game_id);
                } else if (in_collection && collection_games.count(current_collection_name)) {
                    const auto& gids = collection_games.at(current_collection_name);
                    if (selected < (int)gids.size()) {
                        std::string tmp; split_game_id(gids[selected], sys_to_search, tmp);
                        media_rel = game_id_to_cur_rel(gids[selected]);
                    }
                }
                {
                    std::string found_p = find_game_media(roms_base, sys_to_search, media_rel, "boxart", g_name);
                    if (!found_p.empty()) box_art = load_texture(renderer, found_p);
                }
                if (theme_cfg.marquee_enabled) {
                    std::string found_mq = find_game_media(roms_base, sys_to_search, media_rel, "marquees", g_name);
                    if (!found_mq.empty()) marquee_tex = load_texture(renderer, found_mq);
                }
            }
        }

        if (in_games && needs_art_update && (tick - selection_timer > 200)) {
            // In 3dbox mode box_art e marquee_tex sono già stati caricati dal blocco immediato:
            // non distruggerli per evitare il flash di sparizione.
            if (!theme_cfg.gamelist_3dbox_enabled) {
                if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
            }
            if (screenshot_tex) { SDL_DestroyTexture(screenshot_tex); screenshot_tex = nullptr; }
            stop_video_preview();
            if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
            pending_video_path = "";
            video_fading = false;
            video_fade_alpha = 1.0f;
            art_show_timer = 0;
            art_visible    = false;

            if (!items.empty() && !items[selected].is_dir) {
                std::string g_name = items[selected].name;
                // Per i favoriti usa il nome reale dal game_id, non il display_name
                if (in_favorites && selected < (int)favorites_list.size()) {
                    const std::string& gid = favorites_list[selected].game_id;
                    size_t sl = gid.find_last_of('/');
                    g_name = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                    const std::string& gid = lastplayed_list[selected].game_id;
                    size_t sl = gid.find_last_of('/');
                    g_name = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                } else if (in_collection && collection_games.count(current_collection_name)) {
                    const auto& gids = collection_games.at(current_collection_name);
                    if (selected < (int)gids.size()) { size_t sl = gids[selected].rfind('/'); g_name = (sl != std::string::npos) ? gids[selected].substr(sl + 1) : gids[selected]; }
                }
                size_t dot = g_name.find_last_of("."); if (dot != std::string::npos) g_name = g_name.substr(0, dot);

                std::string sys_to_search = current_sys;
                std::string media_rel = current_rel;
                if (in_favorites && selected < (int)favorites_list.size()) {
                    sys_to_search = favorites_list[selected].system;
                    media_rel = game_id_to_cur_rel(favorites_list[selected].game_id);
                } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                    sys_to_search = lastplayed_list[selected].system;
                    media_rel = game_id_to_cur_rel(lastplayed_list[selected].game_id);
                } else if (in_collection && collection_games.count(current_collection_name)) {
                    const auto& gids = collection_games.at(current_collection_name);
                    if (selected < (int)gids.size()) {
                        std::string tmp; split_game_id(gids[selected], sys_to_search, tmp);
                        media_rel = game_id_to_cur_rel(gids[selected]);
                    }
                }

                // 1. Cerca boxart (solo se non già caricata dal blocco immediato)
                if (!box_art) {
                    std::string found_p = find_game_media(roms_base, sys_to_search, media_rel, "boxart", g_name);
                    if (!found_p.empty()) box_art = load_texture(renderer, found_p);
                }

                // 2. Cerca screenshot
                {
                    std::string found_ss = find_game_media(roms_base, sys_to_search, media_rel, "screenshots", g_name);
                    if (!found_ss.empty()) screenshot_tex = load_texture(renderer, found_ss);
                }

                // 3. Cerca marquee (solo se non già caricato dal blocco immediato)
                if (theme_cfg.marquee_enabled && !marquee_tex) {
                    std::string found_mq = find_game_media(roms_base, sys_to_search, media_rel, "marquees", g_name);
                    if (!found_mq.empty()) marquee_tex = load_texture(renderer, found_mq);
                }

                // 4. Cerca video: se trovato, pianifica avvio
                //    - con screenshot: aspetta video_delay_ms (screenshot visibile prima)
                //    - senza screenshot: avvia subito (nessuna immagine statica da mostrare)
                {
                    std::string video_path = find_video_file(roms_base, sys_to_search, g_name, media_rel);
                    if (!video_path.empty()) {
                        pending_video_path = video_path;
                        // Se non c'è screenshot avvia il video senza delay
                        video_start_timer = screenshot_tex ? tick : tick - (Uint32)theme_cfg.video_delay_ms;
                    } else if (!screenshot_tex) {
                        // No screenshot AND no video: mostra no_art.mp4
                        std::string no_art_vid = img_p + "no_art.mp4";
                        if (access(no_art_vid.c_str(), F_OK) != 0 && current_theme != "default")
                            no_art_vid = base_p + "themes/default/images/no_art.mp4";
                        if (access(no_art_vid.c_str(), F_OK) == 0) {
                            pending_video_path = no_art_vid;
                            video_start_timer = tick - (Uint32)theme_cfg.video_delay_ms;
                        }
                    }
                }
            }
            // --- 3dbox cache update ---
            if (theme_cfg.gamelist_3dbox_enabled && !items.empty()) {
                int vis   = theme_cfg.gamelist_3dbox_visible_sides;
                int total = (int)items.size();
                // Evict textures outside the visible range (wrap-aware)
                for (auto it = box3d_cache.begin(); it != box3d_cache.end(); ) {
                    int off = it->first - selected;
                    if (total > 0) {
                        while (off >  total / 2) off -= total;
                        while (off < -total / 2) off += total;
                    }
                    if (off < -(vis + 1) || off > (vis + 1)) {
                        SDL_DestroyTexture(it->second);
                        it = box3d_cache.erase(it);
                    } else { ++it; }
                }
                // Load textures for items newly in range
                for (int o = -vis; o <= vis; ++o) {
                    int idx = ((selected + o) % total + total) % total;
                    if (box3d_cache.count(idx)) continue;
                    std::string gn = items[idx].name;
                    std::string search_sys = current_sys;
                    std::string item_rel   = current_rel;
                    // Per i favoriti usa il nome reale dal game_id, non il display_name
                    if (in_favorites && idx < (int)favorites_list.size()) {
                        const std::string& gid = favorites_list[idx].game_id;
                        size_t sl = gid.find_last_of('/');
                        gn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                        search_sys = favorites_list[idx].system;
                        item_rel   = game_id_to_cur_rel(gid);
                    } else if (in_lastplayed && idx < (int)lastplayed_list.size()) {
                        const std::string& gid = lastplayed_list[idx].game_id;
                        size_t sl = gid.find_last_of('/');
                        gn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                        search_sys = lastplayed_list[idx].system;
                        item_rel   = game_id_to_cur_rel(gid);
                    } else if (in_collection && collection_games.count(current_collection_name)) {
                        const auto& gids = collection_games.at(current_collection_name);
                        if (idx < (int)gids.size()) {
                            const std::string& gid = gids[idx];
                            size_t sl = gid.find_last_of('/');
                            gn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                            std::string tmp; split_game_id(gid, search_sys, tmp);
                            item_rel = game_id_to_cur_rel(gid);
                        }
                    }
                    // Per i file rimuovi l'estensione; le cartelle non ce l'hanno
                    if (!items[idx].is_dir) {
                        size_t d = gn.find_last_of('.'); if (d != std::string::npos) gn = gn.substr(0, d);
                    }
                    SDL_Texture* tex = nullptr;
                    // Try 3dboxes (subfolder-aware)
                    std::string fp3d = find_game_media(roms_base, search_sys, item_rel, "3dboxes", gn);
                    if (!fp3d.empty()) tex = load_texture(renderer, fp3d);
                    // For games (not dirs), fallback to boxart (subfolder-aware)
                    if (!tex && !items[idx].is_dir) {
                        std::string fpba = find_game_media(roms_base, search_sys, item_rel, "boxart", gn);
                        if (!fpba.empty()) tex = load_texture(renderer, fpba);
                    }
                    if (tex) box3d_cache[idx] = tex;
                }
            }
            art_show_timer = tick;
            needs_art_update = false;
        }

        // --- AVVIO VIDEO DOPO DELAY ---
        if (in_games && !pending_video_path.empty() && (tick - video_start_timer >= (Uint32)theme_cfg.video_delay_ms)) {
            if (!video_preview_enabled) {
                pending_video_path = ""; // scarta il video senza avviarlo
            } else {
                start_video_preview(pending_video_path);
                SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
                input_delay = tick + 300;
                pending_video_path = "";
                video_fading = true;
                video_fade_alpha = 1.0f;
            }
        }

        // --- ART VISIBILITY (marquee / boxart-overlay dopo art_delay_ms) ---
        {
            int eff = (theme_cfg.art_delay_ms >= 0) ? theme_cfg.art_delay_ms : theme_cfg.video_delay_ms;
            art_visible = video_running
                       || theme_cfg.gamelist_3dbox_enabled
                       || (art_show_timer > 0 && (int)(tick - art_show_timer) >= eff);
        }

        // --- AGGIORNAMENTO FADE BOXART→VIDEO ---
        if (video_fading && video_running && video_texture) {
            float fade_step = dt / (theme_cfg.video_fade_ms > 0 ? (float)theme_cfg.video_fade_ms * 0.001f : 0.5f);
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
        {
            float slide_max = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
            if (slide_pos < slide_max) {
                if (theme_cfg.carousel_ease_out) {
                    slide_pos += (slide_max - slide_pos) * theme_cfg.carousel_ease_out_factor * dt;
                    if (slide_max - slide_pos < 0.5f) slide_pos = slide_max;
                } else {
                    slide_pos += theme_cfg.carousel_slide_speed * dt;
                    if (slide_pos > slide_max) slide_pos = slide_max;
                }
                carousel_idle_time = 0.0f;
            } else {
                if (!in_games) carousel_idle_time += dt;
            }
        }

        // --- BOUNCE ON SELECT: aggiorna animazione e ingresso posticipato ---
        if (bounce_timer >= 0.0f) {
            bounce_timer += dt;
            const float BOUNCE_DUR = 0.28f;
            if (bounce_timer < BOUNCE_DUR) {
                float t = bounce_timer / BOUNCE_DUR;
                bounce_scale = 1.0f + sinf(t * 3.14159265f) * 0.13f;
            } else {
                bounce_scale = 1.0f;
                bounce_timer = -1.0f;
                if (bounce_enter_pending) {
                    bounce_enter_pending = false;
                    // Esegui l'ingresso nel sistema (posticipato al termine del bounce)
                    in_favorites = (items[selected].name == "FAVORITES");
                    in_lastplayed = (items[selected].name == "LAST PLAYED");
                    in_collection = items[selected].is_collection;
                    current_collection_name = in_collection ? items[selected].name : "";
                    current_sys = items[selected].name;
                    if (in_favorites) {
                        items = load_favorites_as_items();
                        current_rel = "";
                        current_sys = "";
                        current_gamelist.clear();
                        // Pre-warm gamelist cache per tutti i sistemi nei preferiti
                        { std::set<std::string> seen;
                          for (const auto& f : favorites_list)
                              if (!f.system.empty() && !seen.count(f.system))
                                  { seen.insert(f.system); get_system_gamelist(f.system); } }
                    } else if (in_lastplayed) {
                        items = load_lastplayed_as_items();
                        current_rel = "";
                        current_sys = "";
                        current_gamelist.clear();
                        // Pre-warm gamelist cache per tutti i sistemi nei lastplayed
                        { std::set<std::string> seen;
                          for (const auto& lp : lastplayed_list)
                              if (!lp.system.empty() && !seen.count(lp.system))
                                  { seen.insert(lp.system); get_system_gamelist(lp.system); } }
                    } else if (in_collection) {
                        items = build_collection_items(current_collection_name);
                        current_rel = "";
                        current_sys = "";
                        current_gamelist.clear();
                    } else {
                        current_rel = "/" + current_sys;
                        items = scan_directory(roms_base + current_rel, false, current_sys);
                        load_gamelist(roms_base + "/" + current_sys);
                        if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist);
                    }
                    selected = 0; scroll = 0; in_games = true; needs_art_update = true; selection_timer = tick;
                    subfolder_nav_stack.clear();
                    left_held = right_held = up_held = down_held = false;
                    fast_scroll_next = UINT32_MAX;
                    stop_music();
                }
            }
        }

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
                    if (tex_star_filled) { SDL_DestroyTexture(tex_star_filled); tex_star_filled = nullptr; }
                    if (tex_star_empty)  { SDL_DestroyTexture(tex_star_empty);  tex_star_empty  = nullptr; }
                    tex_star_filled = load_theme_image(renderer, "star_filled.png");
                    tex_star_empty  = load_theme_image(renderer, "star_empty.png");
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
                        { std::string p = music_tracks[music_track_index]; size_t sl = p.rfind('/'); save_music_track_for_theme(current_theme, (sl != std::string::npos) ? p.substr(sl+1) : p); }
                    } else if (event.key.keysym.sym == SDLK_3) {
                        music_track_index = (music_track_index + 1) % (int)music_tracks.size();
                        start_music(music_tracks[music_track_index]);
                        set_track_overlay(music_track_index);
                        { std::string p = music_tracks[music_track_index]; size_t sl = p.rfind('/'); save_music_track_for_theme(current_theme, (sl != std::string::npos) ? p.substr(sl+1) : p); }
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
                    case SDLK_x:      vbtn = 0;  break; // X = ricerca
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
                    case SDLK_s:      vbtn = 7;  break; // S = START (collections)
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
                // --- Volume via keypad 7 (btn 14) = down, keypad 9 (btn 16) = up ---
                // Handled before input_delay so volume responds immediately
                {
                    int vbtn = event.jbutton.button;
                    if (vbtn == 14 || vbtn == 16) {
                        if (vbtn == 16 && vol_level < 10) vol_level++;
                        if (vbtn == 14 && vol_level > 0)  vol_level--;
                        vol_show_until = tick + 2000;
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "amixer sset Master %d%% 2>/dev/null &", vol_level * 10);
                        system(cmd);
                    }
                }
                if (tick < input_delay) continue;
                int btn = event.jbutton.button; marquee_pos = -40.0f;
                // Track directional holds for fast scroll (ARM only; native uses KEYDOWN directly)
                if (btn == 30) { left_held  = true; left_hold_start  = tick; fast_scroll_next = tick + 2000; }
                if (btn == 31) { right_held = true; right_hold_start = tick; fast_scroll_next = tick + 2000; }
                if (btn == 29) { up_held    = true; up_hold_start    = tick; fast_scroll_next = tick + 2000; }
                if (btn == 32) { down_held  = true; down_hold_start  = tick; fast_scroll_next = tick + 2000; }
                if (btn == 28) {
                    if (home_mode == 1) {
                        running = false;
                    } else if (home_mode == 2 || home_mode == 3) {
                        if (in_games) {
                            // Torna alla carousel (come B al livello root)
                            play_sound(s_back);
                            stop_video_preview();
                            pending_video_path = "";
                            video_fading = false;
                            video_fade_alpha = 1.0f;
                            if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                            if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                            if (screenshot_tex) { SDL_DestroyTexture(screenshot_tex); screenshot_tex = nullptr; }
                            if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                            { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                            in_games = false; in_favorites = false; in_lastplayed = false; in_collection = false; current_collection_name = "";
                            current_rel = ""; current_sys = "";
                            carousel_stack.clear(); current_sf_node = nullptr;
                            items = build_main_carousel();
                            rebuild_system_list();
                            selected = last_main_sel; last_bg = "";
                            update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                            if (music_enabled && !music_tracks.empty()) start_music(music_tracks[music_track_index]);
                            input_delay = tick + 300;
                        } else if (home_mode == 3 && !items.empty() && items[0].name == "FAVORITES" && selected != 0) {
                            // Già in carousel: porta FAVORITES in primo piano
                            selected = 0; slide_dir = -1;
                            update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                            play_sound(s_click);
                            input_delay = tick + 150;
                        }
                        // home_mode == 2 && !in_games: non fa nulla
                    }
                    // home_mode == 0: non fa nulla
                }
                if (btn == 6 && !in_games) { // SELECT: apri selettore tema
                    stop_video_preview();
                    bool theme_changed = show_theme_selector(renderer, font_path,
                                        &bg_cur, &dev_cur, &dev_prev, &dev_next,
                                        &list_bg_tex, &ctrl_cur, items, selected);
                    if (theme_changed) {
                        reload_theme_images();
                        carousel_stack.clear(); current_sf_node = nullptr;
                        items = build_main_carousel(); selected = 0; last_bg = "";
                        rebuild_system_list();
                        update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                        slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
                        // Restart preloader: new theme → new image paths, clear surface cache
                        clear_surface_cache();
                        preload_start(items, roms_base, theme_p());
                    }
                    input_delay = tick + 300;
                    continue;
                }
                if (btn == 0) { // X: apri ricerca (sistemi in carousel, giochi in gamelist)
                    play_sound(s_click);
                    stop_video_preview();
                    SDL_Texture* search_bg = in_games ? list_bg_tex : bg_cur;
                    std::string search_label = in_games ? "SEARCH GAMES" : "SEARCH SYSTEMS";
                    // Per favorites/lastplayed/collection current_gamelist è vuota:
                    // costruiamo una proxy map nome→GameInfo dalla cache dei sistemi
                    std::map<std::string, GameInfo> search_proxy;
                    const std::map<std::string, GameInfo>* gl_ptr = nullptr;
                    if (in_games) {
                        if (in_favorites) {
                            for (const auto& fav : favorites_list) {
                                std::string key = fav.display_name;
                                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                                const GameInfo* gi = find_game_info_for_system(fav.system, fav.display_name);
                                if (gi && !gi->name.empty()) search_proxy[key] = *gi;
                            }
                            gl_ptr = &search_proxy;
                        } else if (in_lastplayed) {
                            for (const auto& lp : lastplayed_list) {
                                std::string key = lp.display_name;
                                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                                const GameInfo* gi = find_game_info_for_system(lp.system, lp.display_name);
                                if (gi && !gi->name.empty()) search_proxy[key] = *gi;
                            }
                            gl_ptr = &search_proxy;
                        } else if (in_collection && collection_games.count(current_collection_name)) {
                            for (const auto& gid : collection_games.at(current_collection_name)) {
                                std::string col_sys, col_fn; split_game_id(gid, col_sys, col_fn);
                                std::string stem = col_fn; { size_t d = stem.rfind('.'); if (d != std::string::npos) stem = stem.substr(0, d); }
                                std::string key = stem; std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                                const GameInfo* gi = find_game_info_for_system(col_sys, stem);
                                if (gi && !gi->name.empty()) search_proxy[key] = *gi;
                            }
                            gl_ptr = &search_proxy;
                        } else {
                            gl_ptr = &current_gamelist;
                        }
                    }
                    int del_idx = -1;
                    int res = show_search_menu(renderer, font_path, search_bg, items, gl_ptr, search_label,
                                              in_games && !in_favorites && !in_lastplayed && !in_collection, &del_idx);
                    if (del_idx >= 0 && del_idx < (int)items.size()) {
                        // Elimina il file ROM dal disco
                        std::string del_name = items[del_idx].name;
                        std::string full_path = roms_base + current_rel + "/" + del_name;
                        std::remove(full_path.c_str());
                        // Rimuovi da gamelist.xml
                        remove_from_gamelist_xml(roms_base + "/" + current_sys, del_name);
                        // Rimuovi dalla mappa gamelist in memoria
                        std::string gl_key = del_name;
                        size_t gl_dot = gl_key.rfind('.'); if (gl_dot != std::string::npos) gl_key = gl_key.substr(0, gl_dot);
                        std::transform(gl_key.begin(), gl_key.end(), gl_key.begin(), ::tolower);
                        current_gamelist.erase(gl_key);
                        if (gamelist_cache.count(current_sys)) gamelist_cache[current_sys].erase(gl_key);
                        // Rimuovi dai preferiti (game_id = current_sys + rel_path + "/" + del_name)
                        std::string rel_path = current_rel.substr(("/" + current_sys).length());
                        std::string del_game_id = current_sys + rel_path + "/" + del_name;
                        favorites_list.erase(std::remove_if(favorites_list.begin(), favorites_list.end(),
                            [&](const FavoriteGame& fg) { return fg.game_id == del_game_id; }),
                            favorites_list.end());
                        save_favs();
                        // Rimuovi dagli items e aggiusta selected
                        items.erase(items.begin() + del_idx);
                        if (selected > del_idx) selected--;  // fix off-by-one quando selected era dopo il gioco cancellato
                        if (selected >= (int)items.size()) selected = std::max(0, (int)items.size() - 1);
                        g_count = std::max(0, g_count - 1);
                        // Aggiorna la cache game_count_cache così il carousel mostra il conteggio corretto
                        { std::lock_guard<std::mutex> lk(g_count_mutex); game_count_cache[current_sys] = g_count; }
                        // Pulisci cache 3dbox e forza reload immediato (senza attesa 200ms)
                        { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                        if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                        if (screenshot_tex) { SDL_DestroyTexture(screenshot_tex); screenshot_tex = nullptr; }
                        if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                        needs_art_update = true;
                        selection_timer = (tick > 250u) ? tick - 250u : 0u;  // delayed block scatta al prossimo frame
                        boxart_loaded_at_timer = 0xFFFFFFFFu;               // immediate block scatta al prossimo frame
                        play_sound(s_back);
                    } else if (res >= 0) {
                        selected = res;
                        if (!in_games) {
                            update_carousel_textures(renderer, items, selected,
                                                     &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                            slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
                            last_bg = "";
                        } else {
                            needs_art_update = true;
                            selection_timer = tick;
                        }
                        play_sound(s_click);
                    }
                    input_delay = tick + 300;
                    continue;
                }
                if (!in_games) {
                    // --- B: torna al carousel genitore se siamo in un nested carousel ---
                    if (btn == 3 && !carousel_stack.empty()) {
                        play_sound(s_back);
                        CarouselState prev = carousel_stack.back();
                        carousel_stack.pop_back();
                        items = prev.items;
                        selected = prev.selected;
                        current_sf_node = prev.sf_node;
                        rebuild_system_list();
                        update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                        last_bg = "";
                        if (!items.empty()) {
                            if (items[selected].name == "FAVORITES") g_count = (int)favorites_list.size();
                            else if (items[selected].name == "LAST PLAYED") g_count = (int)lastplayed_list.size();
                            else if (items[selected].is_superfolder)  g_count = 0;
                            else if (items[selected].is_collection) g_count = (int)collection_games.at(items[selected].name).size();
                            else g_count = count_games_recursive(roms_base + "/" + items[selected].name, items[selected].name);
                        }
                        slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
                        slide_dir = -1;
                        left_held = right_held = up_held = down_held = false;
                        fast_scroll_next = UINT32_MAX;
                        input_delay = tick + 300;
                        continue;
                    }
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
                            save_music_track_for_theme(current_theme, (sl != std::string::npos) ? p.substr(sl+1) : p);
                        } else if (btn == 22) { // next track
                            music_track_index = (music_track_index + 1) % (int)music_tracks.size();
                            start_music(music_tracks[music_track_index]);
                            std::string p = music_tracks[music_track_index];
                            size_t sl = p.rfind('/'); std::string n = (sl != std::string::npos) ? p.substr(sl+1) : p;
                            size_t dot = n.rfind('.'); if (dot != std::string::npos) n = n.substr(0, dot);
                            music_track_name_overlay = n;
                            music_track_name_until = tick + 3000;
                            save_music_track_for_theme(current_theme, (sl != std::string::npos) ? p.substr(sl+1) : p);
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
                        if (items[selected].is_superfolder) {
                            // Entra nel nested carousel del SuperFolder
                            play_sound(s_enter);
                            carousel_stack.push_back({items, selected, current_sf_node});
                            const SuperFolderNode* child = (current_sf_node == nullptr)
                                ? find_sfroot(items[selected].name)
                                : find_sfchild(*current_sf_node, items[selected].name);
                            current_sf_node = child;
                            items = child ? items_from_sfnode(*child) : std::vector<MenuItem>{};
                            selected = 0; scroll = 0;
                            rebuild_system_list();
                            update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                            last_bg = ""; g_count = 0;
                            slide_pos = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
                            slide_dir = 1;
                            left_held = right_held = up_held = down_held = false;
                            fast_scroll_next = UINT32_MAX;
                            input_delay = tick + 300;
                        } else {
                            // Avvia bounce: l'ingresso reale è posticipato al termine dell'animazione
                            play_sound(s_enter);
                            last_main_sel = selected;
                            bounce_timer = 0.0f;
                            bounce_scale = 1.0f;
                            bounce_enter_pending = true;
                            input_delay = tick + 600; // blocca input durante il bounce
                        }
                    }
                } else {
                    if (btn == 6) { // SELECT: personal rating cycle (0->1->...->5->0)
                        if (!in_favorites && !in_lastplayed && !items.empty() && !items[selected].is_dir) {
                            std::string gid = build_selected_game_id(items, selected, in_favorites, in_lastplayed, current_sys, current_rel);
                            if (!gid.empty()) {
                                int cur = get_user_rating(gid);
                                int next = (cur >= 5) ? 0 : (cur + 1);
                                set_user_rating(gid, next);
                                play_sound(s_click);
                                input_delay = tick + 150;
                            }
                        }
                    }
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
                            } else if (in_lastplayed) {
                                // Siamo in LAST PLAYED: aggiungi/rimuovi dai preferiti (NON toccare lastplayed_list)
                                if (selected < (int)lastplayed_list.size()) {
                                    std::string lp_game_id   = lastplayed_list[selected].game_id;
                                    std::string lp_system    = lastplayed_list[selected].system;
                                    std::string lp_display   = lastplayed_list[selected].display_name;
                                    bool already_fav = false;
                                    for (int i = 0; i < (int)favorites_list.size(); i++) {
                                        if (favorites_list[i].game_id == lp_game_id) {
                                            favorites_list.erase(favorites_list.begin() + i);
                                            already_fav = true;
                                            break;
                                        }
                                    }
                                    if (!already_fav) {
                                        favorites_list.push_back({lp_game_id, lp_system, lp_display});
                                    }
                                }
                            } else if (in_collection && collection_games.count(current_collection_name)) {
                                // Siamo in una collection: rimuovi il gioco dalla collection
                                auto& glist = collection_games[current_collection_name];
                                if (selected < (int)glist.size()) {
                                    glist.erase(glist.begin() + selected);
                                    save_collections();
                                    items.erase(items.begin() + selected);
                                    if (selected >= (int)items.size()) selected = std::max(0, (int)items.size() - 1);
                                    needs_art_update = true; selection_timer = tick;
                                }
                                play_sound(s_back);
                                input_delay = tick + 300;
                                continue;
                            } else if (!current_sys.empty()) {
                                // Siamo in un sistema normale: aggiungi/rimuovi favorito
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
                            if (theme_cfg.gamelist_3dbox_enabled) {
                                { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                                needs_art_update = true; selection_timer = tick;
                            }
                        }
                    }

                    if (btn == 32) {
                        if (theme_cfg.gamelist_3dbox_enabled && !system_list.empty()) {
                            // Giù in 3dbox = sistema successivo
                            stop_video_preview();
                            if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                            if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                            if (screenshot_tex) { SDL_DestroyTexture(screenshot_tex); screenshot_tex = nullptr; }
                            if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                            { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                            if (current_sf_node != nullptr) {
                                // SF mode: 0-based, no FAVORITES slot
                                int sys_idx = 0;
                                for (int si = 0; si < (int)system_list.size(); si++)
                                    if (system_list[si] == current_sys) { sys_idx = si; break; }
                                sys_idx = (sys_idx + 1) % (int)system_list.size();
                                in_favorites = false;
                                current_sys = system_list[sys_idx];
                                current_rel = "/" + current_sys;
                                items = scan_directory(roms_base + current_rel, false, current_sys);
                                load_gamelist(roms_base + "/" + current_sys);
                                if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist);
                                last_main_sel = sys_idx;
                            } else {
                                int nsys3 = (int)system_list.size(), ncol3 = (int)collection_names.size();
                                int tot3 = nsys3 + 2 + ncol3;
                                int cur_idx = in_favorites ? 0 : in_lastplayed ? nsys3 + 1 : 0;
                                if (in_collection) { cur_idx = nsys3 + 2; for (int ci = 0; ci < ncol3; ci++) if (collection_names[ci] == current_collection_name) { cur_idx = nsys3 + 2 + ci; break; } }
                                else if (!in_favorites && !in_lastplayed) for (int si = 0; si < nsys3; si++) if (system_list[si] == current_sys) { cur_idx = si + 1; break; }
                                cur_idx = (cur_idx + 1) % tot3;
                                if (cur_idx == 0) { in_favorites = true; in_lastplayed = false; in_collection = false; current_collection_name = ""; items = load_favorites_as_items(); current_rel = ""; current_sys = ""; current_gamelist.clear(); last_main_sel = 0;
                                } else if (cur_idx == nsys3 + 1) { in_lastplayed = true; in_favorites = false; in_collection = false; current_collection_name = ""; items = load_lastplayed_as_items(); current_rel = ""; current_sys = ""; current_gamelist.clear(); last_main_sel = nsys3 + 1;
                                } else if (cur_idx > nsys3 + 1) { int ci = cur_idx - nsys3 - 2; in_collection = true; in_favorites = false; in_lastplayed = false; current_collection_name = collection_names[ci]; items = build_collection_items(current_collection_name); current_rel = ""; current_sys = ""; current_gamelist.clear(); last_main_sel = cur_idx;
                                } else { in_favorites = false; in_lastplayed = false; in_collection = false; current_collection_name = ""; current_sys = system_list[cur_idx - 1]; current_rel = "/" + current_sys; items = scan_directory(roms_base + current_rel, false, current_sys); load_gamelist(roms_base + "/" + current_sys); if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist); last_main_sel = cur_idx; }
                            }
                            selected = 0; scroll = 0; needs_art_update = true; selection_timer = tick;
                            play_sound(s_click);
                        } else {
                            selected++; play_sound(s_click); needs_art_update = true; selection_timer = tick;
                        }
                    } else if (btn == 29) {
                        if (theme_cfg.gamelist_3dbox_enabled && !system_list.empty()) {
                            // Su in 3dbox = sistema precedente
                            stop_video_preview();
                            if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                            if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                            if (screenshot_tex) { SDL_DestroyTexture(screenshot_tex); screenshot_tex = nullptr; }
                            if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                            { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                            if (current_sf_node != nullptr) {
                                // SF mode: 0-based, no FAVORITES slot
                                int sys_idx = 0;
                                for (int si = 0; si < (int)system_list.size(); si++)
                                    if (system_list[si] == current_sys) { sys_idx = si; break; }
                                sys_idx = (sys_idx - 1 + (int)system_list.size()) % (int)system_list.size();
                                in_favorites = false;
                                current_sys = system_list[sys_idx];
                                current_rel = "/" + current_sys;
                                items = scan_directory(roms_base + current_rel, false, current_sys);
                                load_gamelist(roms_base + "/" + current_sys);
                                if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist);
                                last_main_sel = sys_idx;
                            } else {
                                int nsys3 = (int)system_list.size(), ncol3 = (int)collection_names.size();
                                int tot3 = nsys3 + 2 + ncol3;
                                int cur_idx = in_favorites ? 0 : in_lastplayed ? nsys3 + 1 : 0;
                                if (in_collection) { cur_idx = nsys3 + 2; for (int ci = 0; ci < ncol3; ci++) if (collection_names[ci] == current_collection_name) { cur_idx = nsys3 + 2 + ci; break; } }
                                else if (!in_favorites && !in_lastplayed) for (int si = 0; si < nsys3; si++) if (system_list[si] == current_sys) { cur_idx = si + 1; break; }
                                cur_idx = (cur_idx - 1 + tot3) % tot3;
                                if (cur_idx == 0) { in_favorites = true; in_lastplayed = false; in_collection = false; current_collection_name = ""; items = load_favorites_as_items(); current_rel = ""; current_sys = ""; current_gamelist.clear(); last_main_sel = 0;
                                } else if (cur_idx == nsys3 + 1) { in_lastplayed = true; in_favorites = false; in_collection = false; current_collection_name = ""; items = load_lastplayed_as_items(); current_rel = ""; current_sys = ""; current_gamelist.clear(); last_main_sel = nsys3 + 1;
                                } else if (cur_idx > nsys3 + 1) { int ci = cur_idx - nsys3 - 2; in_collection = true; in_favorites = false; in_lastplayed = false; current_collection_name = collection_names[ci]; items = build_collection_items(current_collection_name); current_rel = ""; current_sys = ""; current_gamelist.clear(); last_main_sel = cur_idx;
                                } else { in_favorites = false; in_lastplayed = false; in_collection = false; current_collection_name = ""; current_sys = system_list[cur_idx - 1]; current_rel = "/" + current_sys; items = scan_directory(roms_base + current_rel, false, current_sys); load_gamelist(roms_base + "/" + current_sys); if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist); last_main_sel = cur_idx; }
                            }
                            selected = 0; scroll = 0; needs_art_update = true; selection_timer = tick;
                            play_sound(s_click);
                        } else {
                            selected--; play_sound(s_click); needs_art_update = true; selection_timer = tick;
                        }
                    }
                    else if (btn == 5) { selected += 10; play_sound(s_click); needs_art_update = true; selection_timer = tick; input_delay = tick + 150; }
                    else if (btn == 2) { selected -= 10; play_sound(s_click); needs_art_update = true; selection_timer = tick; input_delay = tick + 150; }

                    // Cambio sistema con destra/sinistra (include FAVORITES) o navigazione 3dbox
                    if ((btn == 31 || btn == 30) && (!system_list.empty() || in_collection)) {
                        if (theme_cfg.gamelist_3dbox_enabled) {
                            // In 3dbox mode: left/right navigate the game list horizontally
                            if (btn == 31) {
                                selected++; play_sound(s_click); needs_art_update = true; selection_timer = tick;
                                box3d_slide_dir = 1; box3d_slide_pos = 0.0f;
                            } else {
                                selected--; play_sound(s_click); needs_art_update = true; selection_timer = tick;
                                box3d_slide_dir = -1; box3d_slide_pos = 0.0f;
                            }
                        } else {
                        if (current_sf_node != nullptr) {
                            // Inside a SuperFolder: cycle only between SF systems, no FAVORITES
                            int sys_idx = 0;
                            for (int si = 0; si < (int)system_list.size(); si++)
                                if (system_list[si] == current_sys) { sys_idx = si; break; }
                            int total_sys = (int)system_list.size();
                            if (total_sys > 0) {
                                if (btn == 31) sys_idx = (sys_idx + 1) % total_sys;
                                else           sys_idx = (sys_idx - 1 + total_sys) % total_sys;
                                stop_video_preview();
                                if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                                if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                                if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                                in_favorites = false;
                                current_sys = system_list[sys_idx];
                                current_rel = "/" + current_sys;
                                items = scan_directory(roms_base + current_rel, false, current_sys);
                                load_gamelist(roms_base + "/" + current_sys);
                                if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist);
                                last_main_sel = sys_idx;
                                selected = 0; scroll = 0;
                                needs_art_update = true; selection_timer = tick;
                                play_sound(s_click);
                            }
                        } else {
                        // Indice corrente: FAVORITES=0, sistemi=1..N, LAST PLAYED=N+1, collezioni=N+2..
                        int nsys = (int)system_list.size();
                        int ncol = (int)collection_names.size();
                        int total = nsys + 2 + ncol;
                        int cur_idx = 0;
                        if (in_favorites)   cur_idx = 0;
                        else if (in_lastplayed) cur_idx = nsys + 1;
                        else if (in_collection) {
                            cur_idx = nsys + 2;
                            for (int ci = 0; ci < ncol; ci++)
                                if (collection_names[ci] == current_collection_name) { cur_idx = nsys + 2 + ci; break; }
                        } else {
                            for (int si = 0; si < nsys; si++)
                                if (system_list[si] == current_sys) { cur_idx = si + 1; break; }
                        }
                        if (btn == 31) cur_idx = (cur_idx + 1) % total;
                        else cur_idx = (cur_idx - 1 + total) % total;

                        stop_video_preview();
                        if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                        if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                        if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }

                        if (cur_idx == 0) {
                            in_favorites = true; in_lastplayed = false; in_collection = false; current_collection_name = "";
                            items = load_favorites_as_items();
                            current_rel = ""; current_sys = ""; current_gamelist.clear();
                            last_main_sel = 0;
                        } else if (cur_idx == nsys + 1) {
                            in_lastplayed = true; in_favorites = false; in_collection = false; current_collection_name = "";
                            items = load_lastplayed_as_items();
                            current_rel = ""; current_sys = ""; current_gamelist.clear();
                            last_main_sel = nsys + 1;
                        } else if (cur_idx > nsys + 1) {
                            // Vai a una collezione
                            int ci = cur_idx - nsys - 2;
                            in_collection = true; in_favorites = false; in_lastplayed = false;
                            current_collection_name = collection_names[ci];
                            items = build_collection_items(current_collection_name);
                            current_rel = ""; current_sys = ""; current_gamelist.clear();
                            last_main_sel = cur_idx;
                        } else {
                            // Vai a sistema normale
                            in_favorites = false; in_lastplayed = false; in_collection = false; current_collection_name = "";
                            current_sys = system_list[cur_idx - 1];
                            current_rel = "/" + current_sys;
                            items = scan_directory(roms_base + current_rel, false, current_sys);
                            load_gamelist(roms_base + "/" + current_sys);
                            if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist);
                            last_main_sel = cur_idx;
                        }
                        selected = 0; scroll = 0;
                        needs_art_update = true; selection_timer = tick;
                        play_sound(s_click);
                        } // end else (main carousel)
                        } // end else (!3dbox)
                    }
                    if (btn == 1 && !items.empty()) {
                        if (items[selected].is_dir) {
                            play_sound(s_enter);
                            stop_video_preview();
                            pending_video_path = "";
                            video_fading = false; video_fade_alpha = 1.0f;
                            if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                            if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                            if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                            { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                            subfolder_nav_stack.push_back(selected); // ricorda la posizione del folder appena aperto
                            current_rel += "/" + items[selected].name;
                            items = scan_directory(roms_base + current_rel, false, current_sys);
                            // Carica il gamelist della subfolder (merge sopra la root)
                            load_gamelist(roms_base + "/" + current_sys, roms_base + current_rel);
                            if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist);
                            selected = 0; scroll = 0;
                            needs_art_update = true; selection_timer = tick;
                        } else {
                            stop_video_preview();
                            stop_music();
                            // Release the ALSA device completely so the emulator can open it.
                            // SDL_CloseAudioDevice() alone is not enough — the SDL audio
                            // subsystem keeps /dev/snd/* open until SDL_QuitSubSystem().
                            SDL_QuitSubSystem(SDL_INIT_AUDIO);
                            // NON chiudiamo SDL_INIT_JOYSTICK prima del lancio:
                            // RetroArch legge /dev/input direttamente (non tramite SDL del launcher),
                            // e i remapping di start_local_sd.sh sono a livello kernel → arrivano
                            // automaticamente nel nostro event stream senza riaprire il device.
                            // SDL_QuitSubSystem(JOYSTICK) + SDL_InitSubSystem(JOYSTICK) al ritorno
                            // impiegava 8-10s su GSG (enumerazione di molti device virtuali agpio-keys)
                            // bloccando completamente il thread UI.
                            // LED management before launch: do NOT call gsgparm here.
                            // start_local_sd.sh sets game-specific LEDs for each system
                            // via direct sysfs writes — any prior gsgparm call with -0 all
                            // sets a persistent locked state that prevents those writes.
                            // The launcher LED layout is restored after the game exits.
#ifndef CROSS_PLATFORM
                            // Imposta stato LED prima del lancio del gioco
                            system("echo 0 > /sys/class/leds/key_led1/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led2/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led3/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led4/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led5/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led6/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led7/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led8/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led9/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led10/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led11/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led12/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led13/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led14/brightness 2>/dev/null");
                            system("echo 255 > /sys/class/leds/key_led15/brightness 2>/dev/null");
                            system("echo 255 > /sys/class/leds/key_led16/brightness 2>/dev/null");
                            system("echo 255 > /sys/class/leds/key_led17/brightness 2>/dev/null");
                            system("echo 255 > /sys/class/leds/key_led18/brightness 2>/dev/null");
                            system("echo 255 > /sys/class/leds/key_led19/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led20/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led21/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led22/brightness 2>/dev/null");
                            system("echo 0 > /sys/class/leds/key_led23/brightness 2>/dev/null");
                            SDL_Delay(1000); // attendi 1 secondo prima di lanciare il gioco
#endif
                            {
                                const bool hdmi_script_exists = (access("/sdcard/start_local_sd_HDMI.sh", F_OK) == 0);
                                const std::string launch_script = (hdmi_connected && hdmi_script_exists)
                                    ? "sh /sdcard/start_local_sd_HDMI.sh"
                                    : "sh /sdcard/start_local_sd.sh";
                            if (in_favorites && selected < (int)favorites_list.size()) {
                                // Lancio dal sistema FAVORITES
                                const FavoriteGame& fav = favorites_list[selected];
                                std::string full_path = roms_base + "/" + fav.game_id;
                                std::cerr << "[DEBUG] Lancio FAVORITES: " << full_path << std::endl;
                                add_to_lastplayed(fav.game_id, fav.system, fav.display_name);
                                system((launch_script + " \"\" \"\" \"\" \"" + full_path + "\"").c_str());
                            } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                                // Lancio dal sistema LAST PLAYED
                                // Copia preventiva: add_to_lastplayed modifica lastplayed_list
                                // (remove_if sposta elementi) rendendo lp una dangling reference.
                                std::string lp_game_id  = lastplayed_list[selected].game_id;
                                std::string lp_system   = lastplayed_list[selected].system;
                                std::string lp_display  = lastplayed_list[selected].display_name;
                                std::string full_path = roms_base + "/" + lp_game_id;
                                std::cerr << "[DEBUG] Lancio LAST PLAYED: " << full_path << std::endl;
                                add_to_lastplayed(lp_game_id, lp_system, lp_display);
                                system((launch_script + " \"\" \"\" \"\" \"" + full_path + "\"").c_str());
                            } else if (in_collection && collection_games.count(current_collection_name)) {
                                // Lancio da COLLECTION
                                const auto& gids = collection_games.at(current_collection_name);
                                if (selected < (int)gids.size()) {
                                    const std::string& gid = gids[selected];
                                    std::string full_path = roms_base + "/" + gid;
                                    size_t sl = gid.find('/');
                                    std::string col_sys = (sl != std::string::npos) ? gid.substr(0, sl) : "";
                                    std::string lp_disp = gid.substr(gid.rfind('/') + 1);
                                    { size_t d = lp_disp.rfind('.'); if (d != std::string::npos) lp_disp = lp_disp.substr(0, d); }
                                    const GameInfo* gi = find_game_info(lp_disp);
                                    if (gi && !gi->name.empty()) lp_disp = gi->name;
                                    std::cerr << "[DEBUG] Lancio COLLECTION: " << full_path << std::endl;
                                    add_to_lastplayed(gid, col_sys, lp_disp);
                                    system((launch_script + " \"\" \"\" \"\" \"" + full_path + "\"").c_str());
                                }
                            } else {
                                // Lancio dal sistema normale
                                std::string full_path = roms_base + current_rel + "/" + items[selected].name;
                                std::cerr << "[DEBUG] Lancio SISTEMA: " << full_path << std::endl;
                                std::string rel_path = current_rel.substr(("/" + current_sys).length());
                                std::string lp_game_id = current_sys + rel_path + "/" + items[selected].name;
                                std::string lp_disp = items[selected].name;
                                { size_t d = lp_disp.find_last_of('.'); if (d != std::string::npos) lp_disp = lp_disp.substr(0, d); }
                                const GameInfo* lp_gi = find_game_info(lp_disp);
                                if (lp_gi && !lp_gi->name.empty()) lp_disp = lp_gi->name;
                                add_to_lastplayed(lp_game_id, current_sys, lp_disp);
                                system((launch_script + " \"\" \"\" \"\" \"" + full_path + "\"").c_str());
                            }
                            }

                            // Re-initialize audio subsystem after the emulator exits.
                            SDL_InitSubSystem(SDL_INIT_AUDIO);
                            // Re-open the UI sound effects device (closed by SDL_QuitSubSystem).
                            if (audio_device) { SDL_CloseAudioDevice(audio_device); audio_device = 0; }
                            audio_device = SDL_OpenAudioDevice(NULL, 0, &target_spec, NULL, 0);
                            if (music_enabled && !music_tracks.empty())
                                start_music(music_tracks[music_track_index]);
                            // Joystick subsystem è rimasto attivo durante il gioco:
                            // non serve reinizializzarlo. SDL_FlushEvents (sotto) pulisce
                            // gli eventi joystick accumulati durante la partita.
                            // Se nuovi device sono stati aggiunti, SDL_JOYDEVICEADDED li
                            // gestisce automaticamente nel loop eventi principale.

                            // --- RIPRISTINO LED LAUNCHER ---
                            // Restore the launcher LED layout immediately on return.
                            // The low-battery blink thread will be restarted automatically
                            // by the battery monitoring loop on the next tick if needed.
                            system("/sdcard/bin/gsgparm -k 21 -0 all -1 dpad,A,B,X,Y,1,3,7,9,L1,R1 2>/dev/null");
                            // --------------------------------

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
                            // Reset held flags: SDL_FlushEvents discards pending JOYBUTTONUP events,
                            // so flags would stay true causing phantom fast-scroll after game exit
                            left_held = right_held = up_held = down_held = false;
                            fast_scroll_next = UINT32_MAX;
                            needs_art_update = true;
                            // Refresh items se siamo in last played: l'ordine è cambiato
                            if (in_lastplayed) {
                                items = load_lastplayed_as_items();
                                selected = 0; scroll = 0;
                                selection_timer = last_tick;
                            }
                            // ----------------------------------------
                        }
                    }

                    if (btn == 3) { 
                        play_sound(s_back); 
                        
                        if (in_favorites || in_lastplayed || in_collection) {
                            // Tornare dal carousel corrente (main o nested superfolder)
                            { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                            in_games = false;
                            in_favorites = false;
                            in_lastplayed = false;
                            in_collection = false;
                            current_collection_name = "";
                            current_rel = "";
                            current_sys = "";
                            items = current_sf_node ? items_from_sfnode(*current_sf_node) : build_main_carousel();
                            rebuild_system_list();
                            selected = last_main_sel;
                            if (!items.empty()) selected = std::max(0, std::min(selected, (int)items.size() - 1));
                            last_bg = "";
                            stop_video_preview();
                            pending_video_path = "";
                            video_fading = false;
                            video_fade_alpha = 1.0f;
                            if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                            if (box_art) SDL_DestroyTexture(box_art);
                            box_art = nullptr;
                            if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                            update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                            if (!items.empty() && selected < (int)items.size()) {
                                if (items[selected].name == "FAVORITES") g_count = (int)favorites_list.size();
                                else if (items[selected].name == "LAST PLAYED") g_count = (int)lastplayed_list.size();
                                else if (items[selected].is_superfolder) g_count = 0;
                                else if (items[selected].is_collection) g_count = (int)collection_games.at(items[selected].name).size();
                                else g_count = count_games_recursive(roms_base + "/" + items[selected].name, items[selected].name);
                            }
                            if (music_enabled && !music_tracks.empty()) start_music(music_tracks[music_track_index]);
                        } else {
                            // Tornare normale (dalle cartelle o dalla lista giochi)
                            size_t ls = current_rel.find_last_of("/");
                            std::string parent = (ls == 0 || ls == std::string::npos) ? "" : current_rel.substr(0, ls);
                            if (parent.empty()) { 
                                { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                                subfolder_nav_stack.clear(); // torno al carousel: azzera la storia
                                in_games = false; 
                                current_rel = ""; 
                                items = current_sf_node ? items_from_sfnode(*current_sf_node) : build_main_carousel();
                                rebuild_system_list();
                                selected = last_main_sel; 
                                if (!items.empty()) selected = std::max(0, std::min(selected, (int)items.size() - 1));
                                last_bg = ""; 
                                stop_video_preview();
                                pending_video_path = "";
                                video_fading = false;
                                video_fade_alpha = 1.0f;
                                if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                                if (box_art) SDL_DestroyTexture(box_art); 
                                box_art = nullptr; 
                                if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                                update_carousel_textures(renderer, items, selected, &bg_cur, &dev_cur, &dev_prev, &dev_next, &ctrl_cur);
                                if (!items.empty() && selected < (int)items.size()) {
                                    if (items[selected].name == "FAVORITES") g_count = (int)favorites_list.size();
                                    else if (items[selected].name == "LAST PLAYED") g_count = (int)lastplayed_list.size();
                                    else if (items[selected].is_superfolder) g_count = 0;
                                    else g_count = count_games_recursive(roms_base + "/" + items[selected].name, items[selected].name);
                                }
                                if (music_enabled && !music_tracks.empty()) start_music(music_tracks[music_track_index]);
                            } else {
                                { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
                                current_rel = parent;
                                items = scan_directory(roms_base + current_rel, false, current_sys);
                                // Ricarica il gamelist per il livello a cui torniamo:
                                // se siamo di nuovo alla root del sistema passa solo system_path,
                                // altrimenti fa merge con il gamelist della subfolder parent.
                                if (current_rel == "/" + current_sys)
                                    load_gamelist(roms_base + "/" + current_sys);
                                else
                                    load_gamelist(roms_base + "/" + current_sys, roms_base + current_rel);
                                if (show_gamelist_names) sort_items_by_display_name(items, current_gamelist);
                                stop_video_preview();
                                pending_video_path = "";
                                video_fading = false;
                                video_fade_alpha = 1.0f;
                                if (video_texture) { SDL_DestroyTexture(video_texture); video_texture = nullptr; }
                                if (box_art) { SDL_DestroyTexture(box_art); box_art = nullptr; }
                                if (marquee_tex) { SDL_DestroyTexture(marquee_tex); marquee_tex = nullptr; }
                                // Ripristina il focus sull'ultimo folder aperto
                                if (!subfolder_nav_stack.empty()) {
                                    selected = subfolder_nav_stack.back();
                                    subfolder_nav_stack.pop_back();
                                } else {
                                    selected = 0;
                                }
                                selected = std::max(0, std::min(selected, (int)items.size() - 1));
                                selection_timer = tick;
                            }
                        }
                        
                        scroll = 0;
                        needs_art_update = true;
                    }

                    if (btn == 7 && !items.empty() && !items[selected].is_dir) {
                        if (!collection_names.empty()) {
                            stop_video_preview();
                            std::string game_id, game_name;
                            if (in_favorites && selected < (int)favorites_list.size()) {
                                game_id   = favorites_list[selected].game_id;
                                game_name = favorites_list[selected].display_name;
                            } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                                game_id   = lastplayed_list[selected].game_id;
                                game_name = lastplayed_list[selected].display_name;
                            } else if (in_collection && collection_games.count(current_collection_name)) {
                                const auto& gids = collection_games.at(current_collection_name);
                                if (selected < (int)gids.size()) {
                                    game_id = gids[selected];
                                    game_name = game_id.substr(game_id.rfind('/') + 1);
                                    { size_t d = game_name.rfind('.'); if (d != std::string::npos) game_name = game_name.substr(0, d); }
                                    std::string col_sys, col_fn; split_game_id(game_id, col_sys, col_fn); size_t d2 = col_fn.rfind('.'); if (d2 != std::string::npos) col_fn = col_fn.substr(0, d2);
                                    const GameInfo* gi = find_game_info_for_system(col_sys, col_fn); if (!gi) gi = find_game_info(col_fn);
                                    if (gi && !gi->name.empty()) game_name = gi->name;
                                }
                            } else if (!current_sys.empty()) {
                                std::string rel_path = current_rel.substr(("/" + current_sys).length());
                                game_id   = current_sys + rel_path + "/" + items[selected].name;
                                game_name = items[selected].name;
                                { size_t d = game_name.rfind('.'); if (d != std::string::npos) game_name = game_name.substr(0, d); }
                                const GameInfo* gi = find_game_info(game_name);
                                if (gi && !gi->name.empty()) game_name = gi->name;
                            }
                            if (!game_id.empty())
                                show_collection_picker(renderer, font_path, list_bg_tex, game_id, game_name);
                            input_delay = tick + 300;
                        }
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
                // Game list: up/down fast scroll (and left/right in 3dbox mode)
                if (up_held && (tick - up_hold_start > 2000)) {
                    selected--;
                    needs_art_update = true; selection_timer = tick;
                    if (theme_cfg.gamelist_3dbox_enabled) { box3d_slide_dir = -1; box3d_slide_pos = 0.0f; }
                    play_sound(s_click);
                    fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
                } else if (down_held && (tick - down_hold_start > 2000)) {
                    selected++;
                    needs_art_update = true; selection_timer = tick;
                    if (theme_cfg.gamelist_3dbox_enabled) { box3d_slide_dir = 1; box3d_slide_pos = 0.0f; }
                    play_sound(s_click);
                    fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
                } else if (theme_cfg.gamelist_3dbox_enabled && left_held && (tick - left_hold_start > 2000)) {
                    selected--;
                    needs_art_update = true; selection_timer = tick;
                    box3d_slide_dir = -1; box3d_slide_pos = 0.0f;
                    play_sound(s_click);
                    fast_scroll_next = tick + (Uint32)theme_cfg.fast_scroll_interval;
                } else if (theme_cfg.gamelist_3dbox_enabled && right_held && (tick - right_hold_start > 2000)) {
                    selected++;
                    needs_art_update = true; selection_timer = tick;
                    box3d_slide_dir = 1; box3d_slide_pos = 0.0f;
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
            if (!in_games || !theme_cfg.gamelist_3dbox_enabled) {
                if (selected < scroll) {
                    scroll = selected;
                }
                if (selected >= scroll + theme_cfg.list_rows) {
                    scroll = selected - (theme_cfg.list_rows - 1);
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
     // fine 3° parte   
        if (!in_games) {
            if (bg_cur) {
                // Parallax: bg si sposta leggermente nella stessa direzione della slide ma più lentamente
                float slide_max_par = theme_cfg.carousel_vertical ? 600.0f : 1024.0f;
                float par_raw = (slide_pos < slide_max_par) ? (slide_max_par - slide_pos) * (float)slide_dir : 0.0f;
                int par_px = !theme_cfg.carousel_vertical ? (int)(par_raw * 0.06f) : 0;
                int par_py = theme_cfg.carousel_vertical  ? (int)(par_raw * 0.06f) : 0;
                int ovr_x = std::abs(par_px) + 4;
                int ovr_y = std::abs(par_py) + 4;
                SDL_Rect r = { par_px - ovr_x, par_py - ovr_y, 1024 + ovr_x * 2, 600 + ovr_y * 2 };
                SDL_RenderCopy(renderer, bg_cur, NULL, &r);
            }
            // --- VIGNETTE ai bordi ---
            if (theme_cfg.carousel_vignette) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                int vw = theme_cfg.carousel_vignette_w;
                int steps = 6;
                int sw = std::max(1, vw / steps);
                for (int s = 0; s < steps; s++) {
                    Uint8 a = (Uint8)(theme_cfg.carousel_vignette_alpha * (steps - s) / steps);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, a);
                    SDL_Rect rl = { s * sw, 0, sw, 600 };
                    SDL_Rect rr = { 1024 - (s + 1) * sw, 0, sw, 600 };
                    SDL_RenderFillRect(renderer, &rl);
                    SDL_RenderFillRect(renderer, &rr);
                }
            }
            auto draw_device = [&](SDL_Texture* d, float x_center, float y_center, float scale, float alpha) {
                if (!d) return;
                int dw, dh; SDL_QueryTexture(d, NULL, NULL, &dw, &dh);
                float scale_w = (float)theme_cfg.carousel_device_base_w / dw;
                float scale_h = (theme_cfg.carousel_device_base_h > 0) ? ((float)theme_cfg.carousel_device_base_h / dh) : scale_w;
                float final_scale = std::min(scale_w, scale_h) * scale;
                float tw = dw * final_scale, th = dh * final_scale;
                float dx = x_center - tw * 0.5f, dy = y_center - th * 0.5f;
                // Device shadow
                if (theme_cfg.carousel_device_shadow_alpha > 0) {
                    SDL_SetTextureColorMod(d, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                    SDL_SetTextureAlphaMod(d, (Uint8)(255 * alpha * theme_cfg.carousel_device_shadow_alpha / 255.0f));
                    SDL_FRect sr = { dx + theme_cfg.shadow_offset_x, dy + theme_cfg.shadow_offset_y, tw, th };
                    SDL_RenderCopyF(renderer, d, NULL, &sr);
                    SDL_SetTextureColorMod(d, 255, 255, 255);
                }
                SDL_SetTextureAlphaMod(d, (Uint8)(255 * alpha));
                SDL_FRect dr = { dx, dy, tw, th };
                SDL_RenderCopyF(renderer, d, NULL, &dr);
            };
            // --- FLOATING IDLE offset + ZOOM PULSE ---
            float float_off = 0.0f;
            if (theme_cfg.carousel_idle_float) {
                float_off = sinf(carousel_idle_time * theme_cfg.carousel_idle_float_speed * 2.0f * 3.14159265f) * theme_cfg.carousel_idle_float_amp;
            }
            float idle_scale = 1.0f;
            if (theme_cfg.carousel_idle_zoom) {
                idle_scale = 1.0f + sinf(carousel_idle_time * theme_cfg.carousel_idle_zoom_speed * 2.0f * 3.14159265f) * theme_cfg.carousel_idle_zoom_amp;
            }
            if (theme_cfg.carousel_vertical) {
                float slide_max = 600.0f;
                float anim_off = (slide_max - slide_pos) * slide_dir;
                draw_device(dev_prev, (float)theme_cfg.carousel_cur_x, (float)theme_cfg.carousel_prev_y + anim_off, theme_cfg.carousel_side_scale, theme_cfg.carousel_side_alpha);
                draw_device(dev_next, (float)theme_cfg.carousel_cur_x, (float)theme_cfg.carousel_next_y + anim_off, theme_cfg.carousel_side_scale, theme_cfg.carousel_side_alpha);
                draw_device(dev_cur,  (float)theme_cfg.carousel_cur_x, (float)theme_cfg.carousel_y_center + anim_off + float_off, idle_scale * bounce_scale, 1.0f);
            } else {
                float anim_off = (1024.0f - slide_pos) * slide_dir;
                draw_device(dev_prev, (float)theme_cfg.carousel_prev_x + anim_off, (float)theme_cfg.carousel_y_center, theme_cfg.carousel_side_scale, theme_cfg.carousel_side_alpha);
                draw_device(dev_next, (float)theme_cfg.carousel_next_x + anim_off, (float)theme_cfg.carousel_y_center, theme_cfg.carousel_side_scale, theme_cfg.carousel_side_alpha);
                draw_device(dev_cur,  (float)theme_cfg.carousel_cur_x  + anim_off, (float)theme_cfg.carousel_y_center + float_off, idle_scale * bounce_scale, 1.0f);
            }
            // Mostra solo il nome del sistema in alto a sinistra
            if (show_system_name && !items.empty()) draw_text(renderer, font_24, get_system_fullname(items[selected].name), theme_cfg.carousel_sys_name_x, theme_cfg.carousel_sys_name_y, theme_cfg.font_large, white);
            // Mostra il numero di giochi centrato a y=400 (non per i superfolders)
            if (!items.empty() && !items[selected].is_superfolder) {
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
                    int max_w    = theme_cfg.carousel_desc_max_w;
                    int max_h    = theme_cfg.carousel_desc_max_h;
                    int lh       = theme_cfg.carousel_desc_line_h;
                    int dy       = theme_cfg.carousel_desc_y;
                    std::vector<std::string> lines = wrap_text(font_20, desc, max_w);
                    int total_lines  = (int)lines.size();
                    int max_visible  = (lh > 0) ? max_h / lh : total_lines;
                    int scroll_offset = 0;
                    if (total_lines > max_visible) {
                        int extra = total_lines - max_visible;
                        carousel_desc_marquee_pos += theme_cfg.desc_scroll_speed * dt;
                        float cycle = (float)extra + 3.0f;
                        if (carousel_desc_marquee_pos > cycle) carousel_desc_marquee_pos = -2.0f;
                        float eff = carousel_desc_marquee_pos < 0 ? 0 : carousel_desc_marquee_pos;
                        if (eff > (float)extra) eff = (float)extra;
                        scroll_offset = (int)(eff * lh);
                    }
                    SDL_Rect clip = { theme_cfg.carousel_desc_x, dy, max_w, max_h };
                    SDL_RenderSetClipRect(renderer, &clip);
                    for (int li = 0; li < total_lines; li++) {
                        int ly = dy + li * lh - scroll_offset;
                        if (ly + lh < dy || ly > dy + max_h) continue;
                        draw_text(renderer, font_20, lines[li], theme_cfg.carousel_desc_x, ly, theme_cfg.font_medium, dc);
                    }
                    SDL_RenderSetClipRect(renderer, NULL);
                }
            }
        } else {
            if (list_bg_tex) SDL_RenderCopy(renderer, list_bg_tex, NULL, NULL);

            // Mostra logo del sistema o fallback testo
            std::string system_label = in_favorites ? "FAVORITES"
                : in_lastplayed  ? "LAST PLAYED"
                : in_collection  ? current_collection_name
                : get_system_fullname(current_sys);
            std::string logo_key = in_favorites ? "FAVORITES"
                : in_lastplayed  ? "LAST PLAYED"
                : in_collection  ? current_collection_name
                : current_sys;
            std::string logo_path = show_system_logo ? find_file_ignore_case(theme_p() + "logos", logo_key) : "";
            if (!logo_path.empty()) {
                SDL_Texture* logo_tex = load_texture(renderer, logo_path);
                if (logo_tex) {
                    int lw, lh; SDL_QueryTexture(logo_tex, NULL, NULL, &lw, &lh);
                    float scale = std::min((float)theme_cfg.logo_max_h / lh, (float)theme_cfg.logo_max_w / lw);
                    int dw = (int)(lw * scale), dh = (int)(lh * scale);
                    int draw_x = (theme_cfg.logo_x < 0) ? 512 - dw / 2 : theme_cfg.logo_x;
                    // Shadow
                    if (theme_cfg.shadows) {
                        SDL_SetTextureColorMod(logo_tex, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                        SDL_SetTextureAlphaMod(logo_tex, (Uint8)theme_cfg.logo_shadow_alpha);
                        SDL_Rect lr_sh = { draw_x + theme_cfg.shadow_offset_x, theme_cfg.logo_y + theme_cfg.shadow_offset_y, dw, dh };
                        SDL_RenderCopy(renderer, logo_tex, NULL, &lr_sh);
                    }
                    // Logo
                    SDL_SetTextureColorMod(logo_tex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(logo_tex, 255);
                    SDL_Rect lr = { draw_x, theme_cfg.logo_y, dw, dh };
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
            if (items.empty() || !items[selected].is_dir) {
            {
                // pre_video_art: screenshot shown before/during video fade
                // Fallback chain: screenshot → (video) → no_art
                SDL_Texture* pre_video_art = screenshot_tex;
                // Calculate destination rect based on texture aspect ratio
                auto calc_art_rect = [&](SDL_Texture* tex, int& dx, int& dy, int& tw, int& th) {
                    if (theme_cfg.gamelist_3dbox_enabled) {
                        // In modalità 3dbox: forza esattamente max_w × max_h
                        (void)tex;
                        tw = theme_cfg.boxart_max_w;
                        th = theme_cfg.boxart_max_h;
                        dx = theme_cfg.boxart_area_x;
                        dy = theme_cfg.boxart_area_y;
                    } else {
                        // In modalità lista: rispetta aspect ratio
                        int aw, ah; SDL_QueryTexture(tex, NULL, NULL, &aw, &ah);
                        float asp = (float)aw/ah;
                        if (asp > 1.33f) { tw = theme_cfg.boxart_max_w; th = (int)(theme_cfg.boxart_max_w/asp); }
                        else             { th = theme_cfg.boxart_max_h; tw = (int)(theme_cfg.boxart_max_h*asp); }
                        dx = theme_cfg.boxart_area_x + (theme_cfg.boxart_max_w - tw) / 2;
                        dy = theme_cfg.boxart_area_y + (theme_cfg.boxart_max_h - th) / 2;
                    }
                };

                int bp = theme_cfg.boxart_border_padding;
                auto draw_border = [&](int dx, int dy, int tw, int th) {
                    if (!theme_cfg.boxart_border_enabled) return;
                    SDL_SetRenderDrawColor(renderer, theme_cfg.color_boxart_border.r, theme_cfg.color_boxart_border.g, theme_cfg.color_boxart_border.b, theme_cfg.color_boxart_border.a);
                    SDL_Rect br = { dx - bp, dy - bp, tw + bp*2, th + bp*2 };
                    SDL_RenderFillRect(renderer, &br);
                };

                if (video_texture && video_running) {
                    // Video is ready: border always follows VIDEO dimensions
                    int vdx, vdy, vtw, vth;
                    calc_art_rect(video_texture, vdx, vdy, vtw, vth);
                    draw_border(vdx, vdy, vtw, vth);

                    // If fading: static image fades out (at its own rect, behind the video)
                    if (video_fading && pre_video_art) {
                        int bdx, bdy, btw, bth;
                        calc_art_rect(pre_video_art, bdx, bdy, btw, bth);
                        SDL_SetTextureAlphaMod(pre_video_art, (Uint8)(video_fade_alpha * 255));
                        SDL_Rect rba = { bdx, bdy, btw, bth };
                        SDL_RenderCopy(renderer, pre_video_art, NULL, &rba);
                        SDL_SetTextureAlphaMod(pre_video_art, 255);
                    }

                    // Video fades in or is fully shown
                    Uint8 va = video_fading ? (Uint8)((1.0f - video_fade_alpha) * 255) : 255;
                    SDL_SetTextureAlphaMod(video_texture, va);
                    SDL_Rect rvid = { vdx, vdy, vtw, vth };
                    SDL_RenderCopy(renderer, video_texture, NULL, &rvid);
                    SDL_SetTextureAlphaMod(video_texture, 255);
                    // CRT overlay sopra il video
                    if (crt_tex) {
                        SDL_Rect cr = (theme_cfg.crt_overlay_x >= 0 || theme_cfg.crt_overlay_y >= 0 || theme_cfg.crt_overlay_w > 0 || theme_cfg.crt_overlay_h > 0)
                            ? SDL_Rect{ theme_cfg.crt_overlay_x >= 0 ? theme_cfg.crt_overlay_x : rvid.x,
                                        theme_cfg.crt_overlay_y >= 0 ? theme_cfg.crt_overlay_y : rvid.y,
                                        theme_cfg.crt_overlay_w > 0  ? theme_cfg.crt_overlay_w : rvid.w,
                                        theme_cfg.crt_overlay_h > 0  ? theme_cfg.crt_overlay_h : rvid.h }
                            : rvid;
                        SDL_RenderCopy(renderer, crt_tex, NULL, &cr);
                    }

                } else if (!video_running && !video_fading) {
                    // No video at all: show static image or no_art with matching border
                    SDL_Texture* art = pre_video_art ? pre_video_art : no_art_tex;
                    if (art) {
                        int dx, dy, tw, th;
                        calc_art_rect(art, dx, dy, tw, th);
                        draw_border(dx, dy, tw, th);
                        SDL_Rect rb = { dx, dy, tw, th };
                        SDL_RenderCopy(renderer, art, NULL, &rb);
                        // CRT overlay sopra la boxart
                        if (crt_tex) {
                            SDL_Rect cr = (theme_cfg.crt_overlay_x >= 0 || theme_cfg.crt_overlay_y >= 0 || theme_cfg.crt_overlay_w > 0 || theme_cfg.crt_overlay_h > 0)
                                ? SDL_Rect{ theme_cfg.crt_overlay_x >= 0 ? theme_cfg.crt_overlay_x : rb.x,
                                            theme_cfg.crt_overlay_y >= 0 ? theme_cfg.crt_overlay_y : rb.y,
                                            theme_cfg.crt_overlay_w > 0  ? theme_cfg.crt_overlay_w : rb.w,
                                            theme_cfg.crt_overlay_h > 0  ? theme_cfg.crt_overlay_h : rb.h }
                                : rb;
                            SDL_RenderCopy(renderer, crt_tex, NULL, &cr);
                        }
                    }
                } else if (pre_video_art) {
                    // Video starting but first frame not yet decoded: show static image
                    int dx, dy, tw, th;
                    calc_art_rect(pre_video_art, dx, dy, tw, th);
                    draw_border(dx, dy, tw, th);
                    SDL_Rect rb = { dx, dy, tw, th };
                    SDL_RenderCopy(renderer, pre_video_art, NULL, &rb);
                    // CRT overlay sopra la boxart
                    if (crt_tex) {
                        SDL_Rect cr = (theme_cfg.crt_overlay_x >= 0 || theme_cfg.crt_overlay_y >= 0 || theme_cfg.crt_overlay_w > 0 || theme_cfg.crt_overlay_h > 0)
                            ? SDL_Rect{ theme_cfg.crt_overlay_x >= 0 ? theme_cfg.crt_overlay_x : rb.x,
                                        theme_cfg.crt_overlay_y >= 0 ? theme_cfg.crt_overlay_y : rb.y,
                                        theme_cfg.crt_overlay_w > 0  ? theme_cfg.crt_overlay_w : rb.w,
                                        theme_cfg.crt_overlay_h > 0  ? theme_cfg.crt_overlay_h : rb.h }
                            : rb;
                        SDL_RenderCopy(renderer, crt_tex, NULL, &cr);
                    }
                }
            }
            } // end if not dir (nasconde boxart/CRT/video per subfolder)

            // --- BOXART OVERLAY (shown when art is visible: after art_delay_ms or when video running) ---
            if (theme_cfg.boxart_overlay_enabled && box_art && art_visible && !items.empty() && !items[selected].is_dir) {
                int aw, ah; SDL_QueryTexture(box_art, NULL, NULL, &aw, &ah);
                float asp = (float)aw / ah;
                int tw, th;
                if (asp >= 1.0f) { tw = theme_cfg.boxart_overlay_max_w; th = (int)(tw / asp); }
                else             { th = theme_cfg.boxart_overlay_max_h; tw = (int)(th * asp); }
                if (tw > theme_cfg.boxart_overlay_max_w) { tw = theme_cfg.boxart_overlay_max_w; th = (int)(tw / asp); }
                if (th > theme_cfg.boxart_overlay_max_h) { th = theme_cfg.boxart_overlay_max_h; tw = (int)(th * asp); }
                int dx = theme_cfg.boxart_overlay_x;
                int dy = theme_cfg.boxart_overlay_y;
                if (theme_cfg.boxart_overlay_shadow) {
                    SDL_SetTextureAlphaMod(box_art, (Uint8)theme_cfg.boxart_overlay_shadow_alpha);
                    SDL_SetTextureColorMod(box_art, 0, 0, 0);
                    SDL_Rect rs = { dx + 3, dy + 3, tw, th };
                    SDL_RenderCopy(renderer, box_art, NULL, &rs);
                    SDL_SetTextureColorMod(box_art, 255, 255, 255);
                }
                SDL_SetTextureAlphaMod(box_art, (Uint8)theme_cfg.boxart_overlay_alpha);
                SDL_Rect rb = { dx, dy, tw, th };
                SDL_RenderCopy(renderer, box_art, NULL, &rb);
                SDL_SetTextureAlphaMod(box_art, 255);
            }

            // --- RENDERING GAME NAME + DESCRIPTION ---
            if (!items.empty() && !items[selected].is_dir) {
                std::string gname = items[selected].name;
                // Per i favoriti usa il nome reale dal game_id (include l'estensione ROM),
                // come fa il blocco di caricamento art. Questo evita troncamenti errati
                // per nomi che contengono un punto (es. "Dr. Mario", "Pac-Man Jr.").
                if (in_favorites && selected < (int)favorites_list.size()) {
                    const std::string& gid = favorites_list[selected].game_id;
                    size_t sl = gid.find_last_of('/');
                    gname = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                    const std::string& gid = lastplayed_list[selected].game_id;
                    size_t sl = gid.find_last_of('/');
                    gname = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                } else if (in_collection && collection_games.count(current_collection_name)) {
                    const auto& gids = collection_games.at(current_collection_name);
                    if (selected < (int)gids.size()) { size_t sl = gids[selected].rfind('/'); gname = (sl != std::string::npos) ? gids[selected].substr(sl + 1) : gids[selected]; }
                }
                size_t dot = gname.find_last_of('.');
                if (dot != std::string::npos) gname = gname.substr(0, dot);
                const GameInfo* gi = nullptr;
                if (in_favorites && selected < (int)favorites_list.size()) {
                    gi = find_game_info_for_system(favorites_list[selected].system, gname);
                } else if (in_lastplayed && selected < (int)lastplayed_list.size()) {
                    gi = find_game_info_for_system(lastplayed_list[selected].system, gname);
                } else if (in_collection && collection_games.count(current_collection_name)) {
                    const auto& gids = collection_games.at(current_collection_name);
                    if (selected < (int)gids.size()) { std::string col_sys, col_fn; split_game_id(gids[selected], col_sys, col_fn); size_t d = col_fn.rfind('.'); if (d != std::string::npos) col_fn = col_fn.substr(0, d); gi = find_game_info_for_system(col_sys, col_fn); if (!gi) gi = find_game_info(col_fn); }
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

                if (theme_cfg.game_meta_enabled) {
                    // Game metadata lines from gamelist.xml (fully theme-configurable)
                    int meta_x = (theme_cfg.game_meta_x >= 0) ? theme_cfg.game_meta_x : theme_cfg.desc_x;
                    int meta_y = (theme_cfg.game_meta_y >= 0) ? theme_cfg.game_meta_y : (theme_cfg.desc_y + theme_cfg.desc_area_h + 6);
                    int meta_lh = std::max(10, theme_cfg.game_meta_line_h);
                    int meta_fs = std::max(10, theme_cfg.game_meta_font_size);
                    TTF_Font* meta_font = font_16;
                    if (ttf_available) {
                        if (font_cache.find(meta_fs) == font_cache.end()) font_cache[meta_fs] = ttf_open_font(font_path, meta_fs);
                        if (font_cache.count(meta_fs) && font_cache[meta_fs]) meta_font = font_cache[meta_fs];
                    }
                    std::string rel = gi->releasedate.empty() ? "N/A" : format_release_date(gi->releasedate);
                    std::string genre = gi->genre.empty() ? "N/A" : gi->genre;
                    std::string dev = gi->developer.empty() ? "N/A" : gi->developer;
                    std::string pub = gi->publisher.empty() ? "N/A" : gi->publisher;
                    std::string players = gi->players.empty() ? "N/A" : gi->players;
                    std::string ext_rating = format_external_rating(gi->rating);
                    std::string meta_row1 = "Release: " + rel + "  |  Genre: " + genre;
                    std::string meta_row2 = "Developer: " + dev + "  |  Publisher: " + pub;
                    std::string meta_row3 = "Players: " + players + "  |  Rating: " + ext_rating;
                    draw_text(renderer, meta_font, meta_row1, meta_x, meta_y + meta_lh * 0, meta_fs, theme_cfg.game_meta_color);
                    draw_text(renderer, meta_font, meta_row2, meta_x, meta_y + meta_lh * 1, meta_fs, theme_cfg.game_meta_color);
                    draw_text(renderer, meta_font, meta_row3, meta_x, meta_y + meta_lh * 2, meta_fs, theme_cfg.game_meta_color);
                }
                } // end if (gi)

                // Personal rating overlay (stelle immagine, theme-configurable)
                if (theme_cfg.personal_rating_enabled) {
                    std::string gid = build_selected_game_id(items, selected, in_favorites, in_lastplayed, current_sys, current_rel);
                    int pr = get_user_rating(gid);
                    TTF_Font* pr_font = font_16;
                    if (ttf_available) {
                        int psz = std::max(10, theme_cfg.personal_rating_font_size);
                        if (font_cache.find(psz) == font_cache.end()) font_cache[psz] = ttf_open_font(font_path, psz);
                        if (font_cache.count(psz) && font_cache[psz]) pr_font = font_cache[psz];
                    }
                    // Etichetta "Personal:"
                    draw_text(renderer, pr_font, "Personal:",
                              theme_cfg.personal_rating_x, theme_cfg.personal_rating_y,
                              theme_cfg.personal_rating_font_size, theme_cfg.personal_rating_color);
                    // Stelline a destra dell'etichetta
                    int label_w = 0, label_h = 0;
                    if (pr_font && ttf_available) ttf_size_utf8(pr_font, "Personal:", &label_w, &label_h);
                    else label_w = 9 * theme_cfg.personal_rating_font_size / 2; // approssimazione bitmap
                    int star_sz = theme_cfg.personal_rating_star_size;
                    draw_star_rating(renderer, pr,
                                     theme_cfg.personal_rating_x + label_w + 6,
                                     theme_cfg.personal_rating_y + (theme_cfg.personal_rating_font_size - star_sz) / 2,
                                     star_sz);
                }
            }
            if (theme_cfg.gamelist_3dbox_enabled) {
                // --- 3DBOX CAROUSEL ---
                // Update slide animation
                int  box3d_slot_w = theme_cfg.gamelist_3dbox_w + theme_cfg.gamelist_3dbox_spacing;
                if (box3d_slide_pos < (float)box3d_slot_w) {
                    box3d_slide_pos += theme_cfg.gamelist_3dbox_slide_speed * dt;
                    if (box3d_slide_pos > (float)box3d_slot_w) box3d_slide_pos = (float)box3d_slot_w;
                }
                float box3d_anim_off = ((float)box3d_slot_w - box3d_slide_pos) * (float)box3d_slide_dir;
                int   total3d = (int)items.size();
                int   bvis = theme_cfg.gamelist_3dbox_visible_sides;
                int   bcx  = theme_cfg.gamelist_3dbox_center_x;
                int   bcy  = theme_cfg.gamelist_3dbox_center_y;
                int   bboxw = theme_cfg.gamelist_3dbox_w;
                int   bboxh = theme_cfg.gamelist_3dbox_h;
                float bss  = theme_cfg.gamelist_3dbox_side_scale;
                float bsa  = theme_cfg.gamelist_3dbox_side_alpha;
                // Pre-compute which slots are valid: assign each bidx to its closest-to-center
                // slot so the same game is never drawn twice (e.g. with 1 game only bo=0 is shown).
                std::set<int> valid_bos;
                if (total3d > 0) {
                    std::set<int> used_bidx;
                    valid_bos.insert(0);
                    used_bidx.insert(((selected) % total3d + total3d) % total3d);
                    for (int d = 1; d <= bvis; ++d) {
                        for (int sign : {-1, 1}) {
                            int bo = sign * d;
                            int bidx = ((selected + bo) % total3d + total3d) % total3d;
                            if (!used_bidx.count(bidx)) {
                                valid_bos.insert(bo);
                                used_bidx.insert(bidx);
                            }
                        }
                    }
                }
                // Draw outer items first (pass 0), center last (pass 1)
                for (int bpass = 0; bpass <= 1 && total3d > 0; ++bpass) {
                    for (int bo = -bvis; bo <= bvis; ++bo) {
                        bool is_center = (bo == 0);
                        if (bpass == 0 && is_center) continue;
                        if (bpass == 1 && !is_center) continue;
                        if (!valid_bos.count(bo)) continue; // skip duplicate items
                        int bidx = ((selected + bo) % total3d + total3d) % total3d;
                        int box_cx = bcx + bo * box3d_slot_w + (int)box3d_anim_off;
                        // Scale e alpha uniformi per tutte le box laterali
                        float bscale = is_center ? 1.0f : bss;
                        int biw = (int)(bboxw * bscale);
                        int bih = (int)(bboxh * bscale);
                        int bix = box_cx - biw / 2;
                        int biy = bcy - bih / 2;
                        // Highlight ring behind selected box
                        if (is_center && theme_cfg.gamelist_3dbox_highlight_alpha > 0) {
                            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                            SDL_SetRenderDrawColor(renderer, 255, 255, 255, (Uint8)theme_cfg.gamelist_3dbox_highlight_alpha);
                            SDL_Rect bhr = { bix - 4, biy - 4, biw + 8, bih + 8 };
                            SDL_RenderFillRect(renderer, &bhr);
                        }
                        // Alpha uniforme per tutte le box laterali
                        float balpha = is_center ? 1.0f : bsa;
                        SDL_Texture* btex = box3d_cache.count(bidx) ? box3d_cache.at(bidx) : nullptr;
                        // Per le cartelle senza immagine specifica usa box3d_folder_tex
                        bool b_is_dir = bidx < (int)items.size() && items[bidx].is_dir;
                        if (!btex && b_is_dir && box3d_folder_tex)
                            btex = box3d_folder_tex;
                        if (btex) {
                            SDL_SetTextureAlphaMod(btex, (Uint8)(balpha * 255.0f));
                            SDL_Rect bdr = { bix, biy, biw, bih };
                            SDL_RenderCopy(renderer, btex, NULL, &bdr);
                            SDL_SetTextureAlphaMod(btex, 255);
                        } else if (box3d_default_tex) {
                            SDL_SetTextureAlphaMod(box3d_default_tex, (Uint8)(balpha * 255.0f));
                            SDL_Rect bdr = { bix, biy, biw, bih };
                            SDL_RenderCopy(renderer, box3d_default_tex, NULL, &bdr);
                            SDL_SetTextureAlphaMod(box3d_default_tex, 255);
                        } else {
                            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                            SDL_SetRenderDrawColor(renderer, 50, 50, 80, (Uint8)(balpha * 180.0f));
                            SDL_Rect bdr = { bix, biy, biw, bih };
                            SDL_RenderFillRect(renderer, &bdr);
                        }
                        // Favorite overlay (angolo in basso a destra della box centrale)
                        if (is_center && favorite_tex && bidx < (int)items.size() && !items[bidx].is_dir) {
                            bool b_is_fav = false;
                            if (in_favorites) {
                                b_is_fav = true;
                            } else if (in_lastplayed) {
                                if (bidx < (int)lastplayed_list.size()) {
                                    const std::string& lp_id = lastplayed_list[bidx].game_id;
                                    for (const auto& fav : favorites_list)
                                        if (fav.game_id == lp_id) { b_is_fav = true; break; }
                                }
                            } else {
                                std::string brel = current_rel.length() > ("/" + current_sys).length()
                                    ? current_rel.substr(("/" + current_sys).length()) : "";
                                std::string b_game_id = current_sys + brel + "/" + items[bidx].name;
                                for (const auto& fav : favorites_list)
                                    if (fav.game_id == b_game_id) { b_is_fav = true; break; }
                            }
                            if (b_is_fav) {
                                int fw, fh; SDL_QueryTexture(favorite_tex, NULL, NULL, &fw, &fh);
                                float fs = (float)biw * 0.25f / fw;  // 25% della larghezza della box
                                int fdw = (int)(fw * fs), fdh = (int)(fh * fs);
                                int fx = bix + biw - fdw - 4;
                                int fy = biy + bih - fdh - 4;
                                SDL_Rect fav_r = { fx, fy, fdw, fdh };
                                SDL_RenderCopy(renderer, favorite_tex, NULL, &fav_r);
                            }
                        }
                        // Game/folder name below center box
                        if (is_center && theme_cfg.gamelist_3dbox_show_name
                            && bidx < (int)items.size()) {
                            std::string bn = items[bidx].name;
                            if (in_favorites && bidx < (int)favorites_list.size()) {
                                const std::string& gid = favorites_list[bidx].game_id;
                                size_t sl = gid.find_last_of('/');
                                bn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                            } else if (in_lastplayed && bidx < (int)lastplayed_list.size()) {
                                const std::string& gid = lastplayed_list[bidx].game_id;
                                size_t sl = gid.find_last_of('/');
                                bn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                            } else if (in_collection && collection_games.count(current_collection_name)) {
                                const auto& gids = collection_games.at(current_collection_name);
                                if (bidx < (int)gids.size()) {
                                    const std::string& gid = gids[bidx];
                                    size_t sl = gid.find_last_of('/');
                                    bn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                                }
                            }
                            size_t bdot = bn.find_last_of('.');
                            if (bdot != std::string::npos) bn = bn.substr(0, bdot);
                            if (show_gamelist_names || in_favorites || in_lastplayed || in_collection) {
                                const GameInfo* gi_b = nullptr;
                                if (in_favorites && bidx < (int)favorites_list.size())
                                    gi_b = find_game_info_for_system(favorites_list[bidx].system, bn);
                                else if (in_lastplayed && bidx < (int)lastplayed_list.size())
                                    gi_b = find_game_info_for_system(lastplayed_list[bidx].system, bn);
                                else if (in_collection && collection_games.count(current_collection_name)) {
                                    const auto& gids = collection_games.at(current_collection_name);
                                    if (bidx < (int)gids.size()) {
                                        std::string col_sys, col_fn;
                                        split_game_id(gids[bidx], col_sys, col_fn);
                                        size_t d = col_fn.rfind('.'); if (d != std::string::npos) col_fn = col_fn.substr(0, d);
                                        gi_b = find_game_info_for_system(col_sys, col_fn);
                                    }
                                } else
                                    gi_b = find_game_info(bn);
                                if (gi_b && !gi_b->name.empty()) bn = gi_b->name;
                            }
                            if (in_favorites && bidx < (int)favorites_list.size()) {
                                std::string sys = get_system_fullname(favorites_list[bidx].system);
                                bn = "[" + sys + "] " + bn;
                            } else if (in_lastplayed && bidx < (int)lastplayed_list.size()) {
                                std::string sys = get_system_fullname(lastplayed_list[bidx].system);
                                bn = "[" + sys + "] " + bn;
                            }
                            int bny = (theme_cfg.gamelist_3dbox_name_y >= 0)
                                ? theme_cfg.gamelist_3dbox_name_y
                                : (bcy + bboxh / 2 + 15);
                            int bntw = 0;
                            if (font_20 && ttf_available) {
                                SDL_Surface* nsurf = ttf_render_text_blended(font_20, bn, theme_cfg.gamelist_3dbox_name_color);
                                if (nsurf) { bntw = nsurf->w; SDL_FreeSurface(nsurf); }
                            }
                            // Trofeo a sinistra del nome (solo se il gioco ha cheevos)
                            bool b_has_cheevos = false;
                            if (trophy_tex && bidx < (int)items.size() && !items[bidx].is_dir) {
                                std::string bkey = items[bidx].name;
                                size_t bdotk = bkey.find_last_of('.'); if (bdotk != std::string::npos) bkey = bkey.substr(0, bdotk);
                                const GameInfo* gi_t = nullptr;
                                if (in_favorites && bidx < (int)favorites_list.size())
                                    gi_t = find_game_info_for_system(favorites_list[bidx].system, bkey);
                                else if (in_lastplayed && bidx < (int)lastplayed_list.size())
                                    gi_t = find_game_info_for_system(lastplayed_list[bidx].system, bkey);
                                else if (in_collection && collection_games.count(current_collection_name) && bidx < (int)collection_games.at(current_collection_name).size()) {
                                    std::string col_sys, col_fn;
                                    split_game_id(collection_games.at(current_collection_name)[bidx], col_sys, col_fn);
                                    size_t d = col_fn.rfind('.'); if (d != std::string::npos) col_fn = col_fn.substr(0, d);
                                    gi_t = find_game_info_for_system(col_sys, col_fn);
                                } else
                                    gi_t = find_game_info(bkey);
                                if (gi_t) b_has_cheevos = gi_t->cheevos;
                            }
                            draw_text(renderer, font_20, bn, bcx - bntw / 2, bny, theme_cfg.font_medium, theme_cfg.gamelist_3dbox_name_color);
                            if (b_has_cheevos && trophy_tex) {
                                int tsz = theme_cfg.font_medium;
                                SDL_Rect tr = { bcx + bntw / 2 + 4, bny, tsz, tsz };
                                SDL_RenderCopy(renderer, trophy_tex, NULL, &tr);
                            }
                        }
                    }
                }
            } else {
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
                if (!items[idx].is_dir) {
                    // Per i favoriti usa il nome dal game_id (con estensione ROM) per evitare
                    // troncamenti errati su nomi con punto (es. "Dr. Mario").
                    if (in_favorites && idx < (int)favorites_list.size()) {
                        const std::string& gid = favorites_list[idx].game_id;
                        size_t sl = gid.find_last_of('/');
                        dn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                    } else if (in_lastplayed && idx < (int)lastplayed_list.size()) {
                        const std::string& gid = lastplayed_list[idx].game_id;
                        size_t sl = gid.find_last_of('/');
                        dn = (sl != std::string::npos) ? gid.substr(sl + 1) : gid;
                    }
                    size_t dot = dn.find_last_of("."); if (dot != std::string::npos) dn = dn.substr(0, dot);
                    // Favoriti/lastplayed/collection cercano sempre il nome da gamelist
                    if (show_gamelist_names || in_favorites || in_lastplayed || in_collection) {
                        const GameInfo* gi_l = nullptr;
                        if (in_favorites && idx < (int)favorites_list.size())
                            gi_l = find_game_info_for_system(favorites_list[idx].system, dn);
                        else if (in_lastplayed && idx < (int)lastplayed_list.size())
                            gi_l = find_game_info_for_system(lastplayed_list[idx].system, dn);
                        else if (in_collection && collection_games.count(current_collection_name)) {
                            const auto& gids = collection_games.at(current_collection_name);
                            if (idx < (int)gids.size()) {
                                std::string col_sys, col_fn;
                                split_game_id(gids[idx], col_sys, col_fn);
                                size_t d = col_fn.rfind('.'); if (d != std::string::npos) col_fn = col_fn.substr(0, d);
                                gi_l = find_game_info_for_system(col_sys, col_fn);
                            }
                        } else
                            gi_l = find_game_info(dn);
                        if (gi_l && !gi_l->name.empty()) dn = gi_l->name;
                    }
                }
                if (in_favorites && idx < (int)favorites_list.size()) {
                    std::string sys = get_system_fullname(favorites_list[idx].system);
                    dn = "[" + sys + "] " + dn;
                } else if (in_lastplayed && idx < (int)lastplayed_list.size()) {
                    std::string sys = get_system_fullname(lastplayed_list[idx].system);
                    dn = "[" + sys + "] " + dn;
                }

                // --- CONTROLLO PREFERITO E COLORE ---
                bool is_fav = false;
                if (in_favorites) {
                    is_fav = true;
                } else if (in_lastplayed) {
                    if (idx < (int)lastplayed_list.size()) {
                        const std::string& lp_id = lastplayed_list[idx].game_id;
                        for (const auto& fav : favorites_list)
                            if (fav.game_id == lp_id) { is_fav = true; break; }
                    }
                } else if (in_collection) {
                    if (collection_games.count(current_collection_name)) {
                        const auto& gids = collection_games.at(current_collection_name);
                        if (idx < (int)gids.size())
                            for (const auto& fav : favorites_list)
                                if (fav.game_id == gids[idx]) { is_fav = true; break; }
                    }
                } else if (!current_sys.empty()) {
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

                // Icona sinistra (Favorito > Cartella/Controller)
                bool has_cheevos = false;
                if (!items[idx].is_dir) {
                    std::string dn_key = items[idx].name;
                    size_t dot_c = dn_key.find_last_of('.'); if (dot_c != std::string::npos) dn_key = dn_key.substr(0, dot_c);
                    const GameInfo* gi_c = nullptr;
                    if (in_favorites && idx < (int)favorites_list.size())
                        gi_c = find_game_info_for_system(favorites_list[idx].system, dn_key);
                    else if (in_lastplayed && idx < (int)lastplayed_list.size())
                        gi_c = find_game_info_for_system(lastplayed_list[idx].system, dn_key);
                    else if (in_collection && collection_games.count(current_collection_name) && idx < (int)collection_games.at(current_collection_name).size()) {
                        std::string col_sys, col_fn;
                        split_game_id(collection_games.at(current_collection_name)[idx], col_sys, col_fn);
                        size_t d = col_fn.rfind('.'); if (d != std::string::npos) col_fn = col_fn.substr(0, d);
                        gi_c = find_game_info_for_system(col_sys, col_fn);
                    } else
                        gi_c = find_game_info(dn_key);
                    if (gi_c) has_cheevos = gi_c->cheevos;
                }
                if (is_fav && favorite_tex && !items[idx].is_dir) {
                    int fav_w = 36, fav_h = 32;
                    float scale = 0.6f;
                    int draw_w = (int)(fav_w * scale), draw_h = (int)(fav_h * scale);
                    int icon_x = 32;
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
                int drawn_tw = 0;
                if (idx == selected) {
                    SDL_Surface* surf = ttf_render_text_blended(font_24, dn.c_str(), textColor);
                    if (surf) { drawn_tw = surf->w; SDL_FreeSurface(surf); }
                    if (drawn_tw > lim_w) {
                        draw_text_scissored(renderer, font_24, dn, text_x, text_y, text_size, lim_w, (marquee_pos < 0 ? 0 : (int)marquee_pos), textColor);
                        drawn_tw = lim_w;
                    } else {
                        draw_text(renderer, font_24, f_dn, text_x, text_y, text_size, textColor);
                    }
                } else {
                    draw_text(renderer, font_24, f_dn, text_x, text_y, text_size, textColor);
                    SDL_Surface* surf = ttf_render_text_blended(font_24, f_dn.c_str(), textColor);
                    if (surf) { drawn_tw = surf->w; SDL_FreeSurface(surf); }
                }
                // Trofeo a destra del nome per giochi con RetroAchievements
                if (has_cheevos && trophy_tex) {
                    int tsz = text_size;
                    int tx = text_x + drawn_tw + 6;
                    int ty = text_y;
                    SDL_Rect tr = { tx, ty, tsz, tsz };
                    SDL_RenderCopy(renderer, trophy_tex, NULL, &tr);
                }

            } // end normal list
            } // end gamelist_3dbox if/else
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
                           {"SELECT", "Options  ", true}};
                if (home_mode == 1) entries.push_back({"HOME", "Exit", true});
                else if (home_mode == 2 || home_mode == 3) entries.push_back({"HOME", "Back  ", true});
            } else {
                entries = {{"L", " ", false}, {"R", "Change System  ", false},
                           {"L1", " ", false}, {"R1", "\xc2\xb1" "5  ", false},
                           {"A", "Enter  ", false}, {"SELECT", "Options  ", true}};
                if (home_mode == 1) entries.push_back({"HOME", "Exit", true});
                else if (home_mode == 3) entries.push_back({"HOME", "Favorites", true});
                // home_mode == 0: nessuna voce HOME; home_mode == 2 in carousel: non fa nulla, non mostrata
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

        // Pre-calcola stringhe ora/data (disegnate 50px a sinistra dell'icona wifi)
        std::string hud_time_str;
        std::string hud_date_str;
        int hud_time_tw = 0, hud_date_tw = 0;
        if (show_clock || show_date) {
            if (clock_tz_index > 0) { setenv("TZ", clock_tz_name(clock_tz_index), 1); tzset(); }
            time_t clk_now = time(nullptr);
            struct tm* clk_tm = localtime(&clk_now);
            if (show_clock) {
                char clk_buf[16];
                const char* clk_fmt = (clock_format == 0) ? "%H:%M" : "%l:%M %p";
                strftime(clk_buf, sizeof(clk_buf), clk_fmt, clk_tm);
                hud_time_str = clk_buf;
                int clk_th = 0;
                if (ttf_size_utf8(font_16, hud_time_str.c_str(), &hud_time_tw, &clk_th) < 0)
                    hud_time_tw = (int)(hud_time_str.size() * 9);
            }
            if (show_date) {
                char date_buf[16];
                strftime(date_buf, sizeof(date_buf), "%m/%d/%Y", clk_tm);
                hud_date_str = date_buf;
                int date_th = 0;
                if (ttf_size_utf8(font_16, hud_date_str.c_str(), &hud_date_tw, &date_th) < 0)
                    hud_date_tw = (int)(hud_date_str.size() * 9);
            }
        }

        // Offset verso sinistra: solo percentuale batteria
        int hud_pct_offset = 0;
        if (show_battery && show_battery_percent && battery_pct >= 0) {
            std::string pct_str = std::to_string(battery_pct) + "%";
            int pct_tw = 0, pct_th = 0;
            if (ttf_size_utf8(font_16, pct_str.c_str(), &pct_tw, &pct_th) < 0)
                pct_tw = (int)(pct_str.size() * 10);
            hud_pct_offset = pct_tw + 4;
        }

        // Posizione di riferimento per ora/data: angolo destra HUD
        // Se wifi visibile: 20px a sinistra dell'icona wifi
        // Altrimenti: usa wifi_x come riferimento (sposta a destra)
        {
            int ref_x = theme_cfg.wifi_x - hud_pct_offset;
            int ref_y = theme_cfg.wifi_y;
            int ref_h = (int)(theme_cfg.wifi_src_h * theme_cfg.wifi_scale);
            if (!hud_time_str.empty() || !hud_date_str.empty()) {
                int total_tw = hud_time_tw;
                if (!hud_time_str.empty() && !hud_date_str.empty()) total_tw += 10;
                total_tw += hud_date_tw;
                int gap = show_wifi ? 20 : 8;
                int clk_x = ref_x - gap - total_tw;
                int clk_y = ref_y + (ref_h / 2) - (theme_cfg.font_small / 2);
                if (!hud_time_str.empty()) {
                    draw_text(renderer, font_16, hud_time_str, clk_x, clk_y, theme_cfg.font_small, {255, 255, 255, 255}, false);
                    clk_x += hud_time_tw + 10;
                }
                if (!hud_date_str.empty()) {
                    draw_text(renderer, font_16, hud_date_str, clk_x, clk_y, theme_cfg.font_small, {255, 255, 255, 255}, false);
                }
            }
        }

        if (show_wifi && w_idx < (int)wifi_icons.size() && wifi_icons[w_idx]) {
           int ww = (int)(theme_cfg.wifi_src_w * theme_cfg.wifi_scale);
           int wh = (int)(theme_cfg.wifi_src_h * theme_cfg.wifi_scale);
           int wx = theme_cfg.wifi_x - hud_pct_offset;
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
            
            int bx = theme_cfg.battery_x - hud_pct_offset;
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

            // 3. Percentuale batteria a destra dell'icona
            int pct_right_x = bx + bw; // X subito dopo l'icona
            if (show_battery_percent && battery_pct >= 0) {
                std::string pct_str = std::to_string(battery_pct) + "%";
                int pct_tw2 = 0, pct_th2 = 0;
                if (ttf_size_utf8(font_16, pct_str.c_str(), &pct_tw2, &pct_th2) < 0)
                    pct_tw2 = (int)(pct_str.size() * 10);
                int px_txt = bx + bw + 7;
                int py_txt = by + (bh / 2) - (theme_cfg.font_small / 2);
                draw_text(renderer, font_16, pct_str, px_txt, py_txt, theme_cfg.font_small, {255, 255, 255, 255}, false);
                pct_right_x = px_txt + pct_tw2;
            }
            (void)pct_right_x;
        }
        } // end show_battery

        // --- RENDERING MARQUEE (drawn last in game list to stay on top) ---
        // Visible after art_delay_ms (or video_delay_ms if not set), or immediately in 3dbox mode
        if (in_games && theme_cfg.marquee_enabled && marquee_tex && art_visible) {
            Uint8 eff_alpha = (Uint8)theme_cfg.marquee_alpha;
            if (eff_alpha > 0) {
                int mw, mh; SDL_QueryTexture(marquee_tex, NULL, NULL, &mw, &mh);
                float s = theme_cfg.marquee_scale;
                if (theme_cfg.marquee_max_w > 0 && (int)(mw * s) > theme_cfg.marquee_max_w)
                    s = (float)theme_cfg.marquee_max_w / mw;
                if (theme_cfg.marquee_max_h > 0 && (int)(mh * s) > theme_cfg.marquee_max_h)
                    s = (float)theme_cfg.marquee_max_h / mh;
                int dw = (int)(mw * s);
                int dh = (int)(mh * s);
                int mx = theme_cfg.marquee_x;
                int my = theme_cfg.marquee_y;
                if (theme_cfg.marquee_shadow && theme_cfg.shadows) {
                    Uint8 sh_alpha = (Uint8)theme_cfg.marquee_shadow_alpha;
                    SDL_SetTextureColorMod(marquee_tex, theme_cfg.shadow_color.r, theme_cfg.shadow_color.g, theme_cfg.shadow_color.b);
                    SDL_SetTextureAlphaMod(marquee_tex, sh_alpha);
                    SDL_Rect r_sh = { mx + theme_cfg.shadow_offset_x, my + theme_cfg.shadow_offset_y, dw, dh };
                    SDL_RenderCopy(renderer, marquee_tex, NULL, &r_sh);
                }
                SDL_SetTextureColorMod(marquee_tex, 255, 255, 255);
                SDL_SetTextureAlphaMod(marquee_tex, eff_alpha);
                SDL_Rect r_mq = { mx, my, dw, dh };
                SDL_RenderCopy(renderer, marquee_tex, NULL, &r_mq);
                SDL_SetTextureAlphaMod(marquee_tex, 255);
            }
        }

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
            ttf_size_utf8(font_20, music_track_name_overlay.c_str(), &tw, &th);
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
#ifndef CROSS_PLFORM
 
#endif
    stop_video_preview();
    if (video_texture) SDL_DestroyTexture(video_texture);
    if (box_art) SDL_DestroyTexture(box_art);
    if (marquee_tex) SDL_DestroyTexture(marquee_tex);
    if (box3d_default_tex) SDL_DestroyTexture(box3d_default_tex);
    if (box3d_folder_tex)  SDL_DestroyTexture(box3d_folder_tex);
    if (crt_tex)           SDL_DestroyTexture(crt_tex);
    { for (auto& kv : box3d_cache) SDL_DestroyTexture(kv.second); box3d_cache.clear(); }
    if (ctrl_cur) SDL_DestroyTexture(ctrl_cur);
    if (help_game_tex) SDL_DestroyTexture(help_game_tex);
    if (help_menu_tex) SDL_DestroyTexture(help_menu_tex);
    if (arrow_up_tex) SDL_DestroyTexture(arrow_up_tex);
    if (arrow_down_tex) SDL_DestroyTexture(arrow_down_tex);
    if (arrow_left_tex) SDL_DestroyTexture(arrow_left_tex);
    if (arrow_right_tex) SDL_DestroyTexture(arrow_right_tex);
    stop_music();
#ifndef CROSS_PLATFORM
    // Reset LED allo stato di default all'uscita dal launcher
    system("echo 0 > /sys/class/leds/key_led1/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led2/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led3/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led4/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led5/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led6/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led7/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led8/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led9/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led10/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led11/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led12/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led13/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led14/brightness 2>/dev/null");
    system("echo 255 > /sys/class/leds/key_led15/brightness 2>/dev/null");
    system("echo 255 > /sys/class/leds/key_led16/brightness 2>/dev/null");
    system("echo 255 > /sys/class/leds/key_led17/brightness 2>/dev/null");
    system("echo 255 > /sys/class/leds/key_led18/brightness 2>/dev/null");
    system("echo 255 > /sys/class/leds/key_led19/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led20/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led21/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led22/brightness 2>/dev/null");
    system("echo 0 > /sys/class/leds/key_led23/brightness 2>/dev/null");
#endif
    preload_stop_join();   // stop background preloader before freeing SDL resources
    clear_surface_cache(); // free preloaded surfaces before SDL_Quit
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