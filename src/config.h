#pragma once
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "telemetry.h"

struct PluginConfig {
    // [Blur]
    bool  blur_enabled  = true;
    float start_kmh     = 85.0f;
    float max_kmh       = 290.0f;
    float strength      = 0.036f;
    float clear_radius  = 0.34f;
    int   samples       = 12;
    float vignette      = 0.12f;

    // [Downforce] — mass-normalized aero downforce + front/rear aero balance + anti-bounce
    bool  downforce_enabled   = true;
    float downforce_start_kmh = 90.0f;
    float downforce_g_at_300  = 0.22f;
    float downforce_max_g     = 0.38f;
    float aero_front_pct      = 38.0f; // 38% front / 62% rear (realistic supercar aero balance)
    float anti_bounce         = 0.22f;

    // [Assist] — Yaw + Slip-Angle ESP / ABS-Brake / TC-Throttle stabilizer
    bool  assist_enabled    = true;
    float assist_start_kmh  = 50.0f;
    float esp_strength      = 0.80f;
    float slip_esp_strength = 0.85f;
    float abs_brake_boost   = 1.50f;
    float tc_throttle_boost = 1.25f;
    float max_yaw_decel     = 2.2f;
    float speed_steer_damp  = 1.00f; // 0..2: tightens allowed yaw rate above 140 km/h
    float speed_rumble      = 0.22f; // 0..1: subtle road/chassis micro-shake above 160 km/h
    float esp_brake_drag    = 0.18f; // 0..0.5 G: longitudinal drag when ESP intervenes
    float brake_dive        = 0.25f; // 0..0.6: nose-down pitch torque during ABS braking

    // [Tire] — Pacejka-lite progressive slip + Kamm circle weight transfer
    bool  tire_enabled      = true;
    int   tire_type         = 1;     // 0=Slick, 1=SemiSlick, 2=Street, 3=NarrowStreet
    int   brake_type        = 1;     // 0=Standard, 1=Sport, 2=CarbonCeramic
    float tire_grip         = 0.98f; // 0..1.5: peak lateral tire grip & cornering bite
    float kamm_strength     = 0.80f; // 0..1.5: trail-braking turn-in / brake understeer / power oversteer

    // [UI]
    int   menu_joy_btn      = 25; // 1..32 (0 = keyboard Insert/F6 only)
    bool  auto_preset       = true;
    int   active_preset     = 0;  // CarClass int
    int   abs_level         = 3;  // 0=OFF, 1..5
    int   esp_level         = 3;  // 0=OFF, 1..5
    bool  ui_advanced       = false;
    bool  mini_hud_enabled  = true;
    float mini_hud_x        = -1.0f;
    float mini_hud_y        = -1.0f;
    float mini_hud_scale    = 1.0f;
    int   ui_lang           = 0;    // 0=EN, 1=RU
};

inline PluginConfig g_cfg;
inline char g_ini_path[MAX_PATH] = {0};

inline float read_ini_float(const char* section, const char* key, float def_val, const char* path) {
    char buf[64] = {0};
    char def_str[64] = {0};
    std::snprintf(def_str, sizeof(def_str), "%.6f", def_val);
    GetPrivateProfileStringA(section, key, def_str, buf, sizeof(buf), path);
    return static_cast<float>(std::atof(buf));
}

inline int read_ini_int(const char* section, const char* key, int def_val, const char* path) {
    return static_cast<int>(GetPrivateProfileIntA(section, key, def_val, path));
}

inline void write_ini_int(const char* section, const char* key, int val, const char* path) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", val);
    WritePrivateProfileStringA(section, key, buf, path);
}

inline void write_ini_float(const char* section, const char* key, float val, const char* path) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", val);
    WritePrivateProfileStringA(section, key, buf, path);
}

inline void init_ini_path(HMODULE hMod) {
    char mod_path[MAX_PATH] = {0};
    GetModuleFileNameA(hMod, mod_path, MAX_PATH);
    char* slash = std::strrchr(mod_path, '\\');
    if (slash) {
        *(slash + 1) = '\0';
        std::snprintf(g_ini_path, sizeof(g_ini_path), "%sets2_car_enhance.ini", mod_path);
    } else {
        std::snprintf(g_ini_path, sizeof(g_ini_path), "ets2_car_enhance.ini");
    }
}

inline void apply_abs_level(int lvl) {
    if (lvl < 0) lvl = 0;
    if (lvl > 5) lvl = 5;
    g_cfg.abs_level = lvl;
    switch (lvl) {
        case 0: g_cfg.abs_brake_boost = 0.00f; g_cfg.brake_dive = 0.10f; break;
        case 1: g_cfg.abs_brake_boost = 1.12f; g_cfg.brake_dive = 0.18f; break;
        case 2: g_cfg.abs_brake_boost = 1.28f; g_cfg.brake_dive = 0.22f; break;
        case 3: g_cfg.abs_brake_boost = 1.45f; g_cfg.brake_dive = 0.25f; break;
        case 4: g_cfg.abs_brake_boost = 1.65f; g_cfg.brake_dive = 0.28f; break;
        case 5: g_cfg.abs_brake_boost = 1.95f; g_cfg.brake_dive = 0.32f; break;
    }
    g_cfg.assist_enabled = (g_cfg.abs_level > 0 || g_cfg.esp_level > 0);
}

inline void apply_esp_level(int lvl) {
    if (lvl < 0) lvl = 0;
    if (lvl > 5) lvl = 5;
    g_cfg.esp_level = lvl;
    switch (lvl) {
        case 0:
            g_cfg.esp_strength = 0.00f; g_cfg.slip_esp_strength = 0.00f;
            g_cfg.tc_throttle_boost = 1.00f; g_cfg.esp_brake_drag = 0.00f; g_cfg.max_yaw_decel = 1.0f;
            break;
        case 1:
            g_cfg.esp_strength = 0.25f; g_cfg.slip_esp_strength = 0.25f;
            g_cfg.tc_throttle_boost = 1.05f; g_cfg.esp_brake_drag = 0.06f; g_cfg.max_yaw_decel = 1.2f;
            break;
        case 2:
            g_cfg.esp_strength = 0.45f; g_cfg.slip_esp_strength = 0.45f;
            g_cfg.tc_throttle_boost = 1.12f; g_cfg.esp_brake_drag = 0.12f; g_cfg.max_yaw_decel = 1.6f;
            break;
        case 3:
            g_cfg.esp_strength = 0.70f; g_cfg.slip_esp_strength = 0.75f;
            g_cfg.tc_throttle_boost = 1.20f; g_cfg.esp_brake_drag = 0.22f; g_cfg.max_yaw_decel = 2.0f;
            break;
        case 4:
            g_cfg.esp_strength = 0.95f; g_cfg.slip_esp_strength = 1.05f;
            g_cfg.tc_throttle_boost = 1.35f; g_cfg.esp_brake_drag = 0.32f; g_cfg.max_yaw_decel = 2.5f;
            break;
        case 5:
            g_cfg.esp_strength = 1.25f; g_cfg.slip_esp_strength = 1.35f;
            g_cfg.tc_throttle_boost = 1.60f; g_cfg.esp_brake_drag = 0.42f; g_cfg.max_yaw_decel = 3.2f;
            break;
    }
    g_cfg.assist_enabled = (g_cfg.abs_level > 0 || g_cfg.esp_level > 0);
}

inline void apply_tire_type(int type) {
    if (type < 0) type = 0;
    if (type > 3) type = 3;
    g_cfg.tire_type = type;
    g_cfg.tire_enabled = true;
    switch (type) {
        case 0: g_cfg.tire_grip = 1.22f; g_cfg.kamm_strength = 0.85f; break; // Slick
        case 1: g_cfg.tire_grip = 0.98f; g_cfg.kamm_strength = 0.80f; break; // Semi-Slick
        case 2: g_cfg.tire_grip = 0.78f; g_cfg.kamm_strength = 0.65f; break; // Street
        case 3: g_cfg.tire_grip = 0.56f; g_cfg.kamm_strength = 0.55f; break; // Narrow Street
    }
}

inline void apply_brake_type(int btype) {
    if (btype < 0) btype = 0;
    if (btype > 2) btype = 2;
    g_cfg.brake_type = btype;
}

inline void apply_car_preset(CarClass cls) {
    g_cfg.active_preset = static_cast<int>(cls);
    switch (cls) {
        case CarClass::Civilian:
            g_cfg.downforce_enabled   = true;
            g_cfg.downforce_start_kmh = 100.0f;
            g_cfg.downforce_g_at_300  = 0.08f;
            g_cfg.downforce_max_g     = 0.15f;
            g_cfg.aero_front_pct      = 50.0f;
            g_cfg.anti_bounce         = 0.16f;
            apply_abs_level(3);
            apply_esp_level(3);
            apply_tire_type(static_cast<int>(TireType::Street));
            apply_brake_type(static_cast<int>(BrakeType::Standard));
            break;

        case CarClass::Sport:
            g_cfg.downforce_enabled   = true;
            g_cfg.downforce_start_kmh = 90.0f;
            g_cfg.downforce_g_at_300  = 0.15f;
            g_cfg.downforce_max_g     = 0.25f;
            g_cfg.aero_front_pct      = 44.0f;
            g_cfg.anti_bounce         = 0.20f;
            apply_abs_level(3);
            apply_esp_level(2);
            apply_tire_type(static_cast<int>(TireType::SemiSlick));
            apply_brake_type(static_cast<int>(BrakeType::Sport));
            break;

        case CarClass::Supercar:
            g_cfg.downforce_enabled   = true;
            g_cfg.downforce_start_kmh = 85.0f;
            g_cfg.downforce_g_at_300  = 0.22f;
            g_cfg.downforce_max_g     = 0.38f;
            g_cfg.aero_front_pct      = 38.0f;
            g_cfg.anti_bounce         = 0.22f;
            apply_abs_level(4);
            apply_esp_level(3);
            apply_tire_type(static_cast<int>(TireType::Slick));
            apply_brake_type(static_cast<int>(BrakeType::CarbonCeramic));
            break;

        case CarClass::Truck:
            g_cfg.downforce_enabled = false;
            g_cfg.assist_enabled    = false;
            g_cfg.tire_enabled      = false;
            break;

        default:
            break;
    }
}

inline bool contains_ci(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}

inline CarClass classify_vehicle(
    const char* brand_id, const char* brand, const char* id, const char* name,
    float rpm_limit, uint32_t wheel_count)
{
    char combined[512];
    std::snprintf(combined, sizeof(combined), " %s %s %s %s ",
                  brand_id ? brand_id : "",
                  brand ? brand : "",
                  id ? id : "",
                  name ? name : "");
    for (size_t i = 0; combined[i]; ++i) {
        combined[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(combined[i])));
    }

    // 1. Trucks (by >4 wheels, diesel low RPM limit, or truck brand/model)
    const char* truckKeys[] = {
        "scania", "volvo", "daf", "iveco", "renault", "freightliner", "kenworth",
        "peterbilt", "mack", "western", "international", "actros", "arocs", "antos",
        "axor", "tgx", "tgs", "man ", "stralis", "s-way", "s_way", "kamaz", "maz", "ural"
    };
    for (const char* k : truckKeys) {
        if (contains_ci(combined, k)) return CarClass::Truck;
    }
    if (wheel_count > 4) return CarClass::Truck;
    if (rpm_limit > 100.0f && rpm_limit < 3600.0f) return CarClass::Truck;

    // 2. Supercars / Hypercars
    const char* superKeys[] = {
        "lamborghini", "countach", "huracan", "aventador", "revuelto", "gallardo",
        "ferrari", "mclaren", "bugatti", "pagani", "koenigsegg", "porsche", "911",
        "992", "gt3", "gt2", "aston", "db11", "db12", "vantage", "corvette",
        "audi r8", "r8 ", "gtr", "gt-r", "viper", "amg gt", "ford gt", "nsx", "supercar"
    };
    for (const char* k : superKeys) {
        if (contains_ci(combined, k)) return CarClass::Supercar;
    }
    if (rpm_limit >= 7400.0f) return CarClass::Supercar;

    // 3. Sport sedans / Hot hatches / Tuned
    const char* sportKeys[] = {
        "gti", "golf r", "golf 7.5", "r8.5", "m3", "m4", "m5", "m8", "rs3", "rs4", "rs5", "rs6", "rs7",
        "mansory", "p780", "brabus", "amg", "f-sport", "fsport", "type r", "sti", "evo",
        "supra", "cayenne turbo", "panamera", "urus", "sport"
    };
    for (const char* k : sportKeys) {
        if (contains_ci(combined, k)) return CarClass::Sport;
    }
    if (rpm_limit >= 6400.0f) return CarClass::Sport;

    // 4. Default for 2..4 wheeled vehicles: Civilian car / sedan / classic
    return CarClass::Civilian;
}

inline void reset_defaults() {
    bool keepAuto = g_cfg.auto_preset;
    int  keepBtn  = g_cfg.menu_joy_btn;
    g_cfg = PluginConfig{};
    g_cfg.auto_preset  = keepAuto;
    g_cfg.menu_joy_btn = keepBtn;
    int det = g_tel.detected_class.load();
    if (det != 0) apply_car_preset(static_cast<CarClass>(det));
}

inline void load_config() {
    if (g_ini_path[0] == '\0') return;
    g_cfg.blur_enabled = read_ini_int("Blur", "enabled", 1, g_ini_path) != 0;
    g_cfg.start_kmh    = read_ini_float("Blur", "start_kmh", 85.0f, g_ini_path);
    g_cfg.max_kmh      = read_ini_float("Blur", "max_kmh", 290.0f, g_ini_path);
    g_cfg.strength     = read_ini_float("Blur", "strength", 0.036f, g_ini_path);
    g_cfg.clear_radius = read_ini_float("Blur", "clear_radius", 0.34f, g_ini_path);
    g_cfg.samples      = read_ini_int("Blur", "samples", 12, g_ini_path);
    if (g_cfg.samples < 4)  g_cfg.samples = 4;
    if (g_cfg.samples > 16) g_cfg.samples = 16;
    g_cfg.vignette     = read_ini_float("Blur", "vignette", 0.12f, g_ini_path);

    g_cfg.downforce_enabled   = read_ini_int("Downforce", "enabled", 1, g_ini_path) != 0;
    g_cfg.downforce_start_kmh = read_ini_float("Downforce", "start_kmh", 90.0f, g_ini_path);
    g_cfg.downforce_g_at_300  = read_ini_float("Downforce", "g_at_300kmh", 0.22f, g_ini_path);
    g_cfg.downforce_max_g     = read_ini_float("Downforce", "max_g", 0.38f, g_ini_path);
    g_cfg.aero_front_pct      = read_ini_float("Downforce", "aero_front_pct", 38.0f, g_ini_path);
    if (g_cfg.aero_front_pct < 20.0f) g_cfg.aero_front_pct = 20.0f;
    if (g_cfg.aero_front_pct > 80.0f) g_cfg.aero_front_pct = 80.0f;
    g_cfg.anti_bounce         = read_ini_float("Downforce", "anti_bounce", 0.22f, g_ini_path);
    if (g_cfg.downforce_max_g < 0.0f) g_cfg.downforce_max_g = 0.0f;
    if (g_cfg.downforce_max_g > 1.5f) g_cfg.downforce_max_g = 1.5f;

    g_cfg.assist_enabled    = read_ini_int("Assist", "enabled", 1, g_ini_path) != 0;
    g_cfg.assist_start_kmh  = read_ini_float("Assist", "start_kmh", 50.0f, g_ini_path);
    g_cfg.esp_strength      = read_ini_float("Assist", "esp_strength", 0.80f, g_ini_path);
    g_cfg.slip_esp_strength = read_ini_float("Assist", "slip_esp_strength", 0.85f, g_ini_path);
    g_cfg.abs_brake_boost   = read_ini_float("Assist", "abs_brake_boost", 1.50f, g_ini_path);
    g_cfg.tc_throttle_boost = read_ini_float("Assist", "tc_throttle_boost", 1.25f, g_ini_path);
    g_cfg.max_yaw_decel     = read_ini_float("Assist", "max_yaw_decel", 2.2f, g_ini_path);
    if (g_cfg.max_yaw_decel < 0.0f) g_cfg.max_yaw_decel = 0.0f;
    if (g_cfg.max_yaw_decel > 6.0f) g_cfg.max_yaw_decel = 6.0f;
    g_cfg.speed_steer_damp  = read_ini_float("Assist", "speed_steer_damp", 1.00f, g_ini_path);
    if (g_cfg.speed_steer_damp < 0.0f) g_cfg.speed_steer_damp = 0.0f;
    if (g_cfg.speed_steer_damp > 2.0f) g_cfg.speed_steer_damp = 2.0f;
    g_cfg.speed_rumble      = read_ini_float("Assist", "speed_rumble", 0.22f, g_ini_path);
    if (g_cfg.speed_rumble < 0.0f) g_cfg.speed_rumble = 0.0f;
    if (g_cfg.speed_rumble > 1.0f) g_cfg.speed_rumble = 1.0f;
    g_cfg.esp_brake_drag    = read_ini_float("Assist", "esp_brake_drag", 0.18f, g_ini_path);
    if (g_cfg.esp_brake_drag < 0.0f) g_cfg.esp_brake_drag = 0.0f;
    if (g_cfg.esp_brake_drag > 0.5f) g_cfg.esp_brake_drag = 0.5f;
    g_cfg.brake_dive        = read_ini_float("Assist", "brake_dive", 0.25f, g_ini_path);
    if (g_cfg.brake_dive < 0.0f) g_cfg.brake_dive = 0.0f;
    if (g_cfg.brake_dive > 0.6f) g_cfg.brake_dive = 0.6f;

    g_cfg.tire_enabled      = read_ini_int("Tire", "enabled", 1, g_ini_path) != 0;
    g_cfg.tire_type         = read_ini_int("Tire", "tire_type", 1, g_ini_path);
    if (g_cfg.tire_type < 0) g_cfg.tire_type = 0;
    if (g_cfg.tire_type > 3) g_cfg.tire_type = 3;
    g_cfg.brake_type        = read_ini_int("Tire", "brake_type", 1, g_ini_path);
    if (g_cfg.brake_type < 0) g_cfg.brake_type = 0;
    if (g_cfg.brake_type > 2) g_cfg.brake_type = 2;
    g_cfg.tire_grip         = read_ini_float("Tire", "tire_grip", 0.98f, g_ini_path);
    if (g_cfg.tire_grip < 0.0f) g_cfg.tire_grip = 0.0f;
    if (g_cfg.tire_grip > 1.5f) g_cfg.tire_grip = 1.5f;
    g_cfg.kamm_strength     = read_ini_float("Tire", "kamm_strength", 0.80f, g_ini_path);
    if (g_cfg.kamm_strength < 0.0f) g_cfg.kamm_strength = 0.0f;
    if (g_cfg.kamm_strength > 1.5f) g_cfg.kamm_strength = 1.5f;

    g_cfg.menu_joy_btn = read_ini_int("UI", "menu_joy_btn", 25, g_ini_path);
    if (g_cfg.menu_joy_btn < 0)  g_cfg.menu_joy_btn = 0;
    if (g_cfg.menu_joy_btn > 32) g_cfg.menu_joy_btn = 32;
    g_cfg.auto_preset      = read_ini_int("UI", "auto_preset", 1, g_ini_path) != 0;
    g_cfg.abs_level        = read_ini_int("UI", "abs_level", 3, g_ini_path);
    if (g_cfg.abs_level < 0) g_cfg.abs_level = 0;
    if (g_cfg.abs_level > 5) g_cfg.abs_level = 5;
    g_cfg.esp_level        = read_ini_int("UI", "esp_level", 3, g_ini_path);
    if (g_cfg.esp_level < 0) g_cfg.esp_level = 0;
    if (g_cfg.esp_level > 5) g_cfg.esp_level = 5;
    g_cfg.ui_advanced      = read_ini_int("UI", "ui_advanced", 0, g_ini_path) != 0;
    g_cfg.mini_hud_enabled = read_ini_int("UI", "mini_hud_enabled", 1, g_ini_path) != 0;
    g_cfg.mini_hud_x       = read_ini_float("UI", "mini_hud_x", -1.0f, g_ini_path);
    g_cfg.mini_hud_y       = read_ini_float("UI", "mini_hud_y", -1.0f, g_ini_path);
    g_cfg.mini_hud_scale   = read_ini_float("UI", "mini_hud_scale", 1.0f, g_ini_path);
    if (g_cfg.mini_hud_scale < 0.70f) g_cfg.mini_hud_scale = 0.70f;
    if (g_cfg.mini_hud_scale > 1.80f) g_cfg.mini_hud_scale = 1.80f;
    g_cfg.ui_lang          = read_ini_int("UI", "ui_lang", 0, g_ini_path);
    if (g_cfg.ui_lang < 0 || g_cfg.ui_lang > 1) g_cfg.ui_lang = 0;
}

inline void save_config() {
    if (g_ini_path[0] == '\0') return;
    write_ini_int("Blur", "enabled", g_cfg.blur_enabled ? 1 : 0, g_ini_path);
    write_ini_float("Blur", "start_kmh", g_cfg.start_kmh, g_ini_path);
    write_ini_float("Blur", "max_kmh", g_cfg.max_kmh, g_ini_path);
    write_ini_float("Blur", "strength", g_cfg.strength, g_ini_path);
    write_ini_float("Blur", "clear_radius", g_cfg.clear_radius, g_ini_path);
    write_ini_int("Blur", "samples", g_cfg.samples, g_ini_path);
    write_ini_float("Blur", "vignette", g_cfg.vignette, g_ini_path);

    write_ini_int("Downforce", "enabled", g_cfg.downforce_enabled ? 1 : 0, g_ini_path);
    write_ini_float("Downforce", "start_kmh", g_cfg.downforce_start_kmh, g_ini_path);
    write_ini_float("Downforce", "g_at_300kmh", g_cfg.downforce_g_at_300, g_ini_path);
    write_ini_float("Downforce", "max_g", g_cfg.downforce_max_g, g_ini_path);
    write_ini_float("Downforce", "aero_front_pct", g_cfg.aero_front_pct, g_ini_path);
    write_ini_float("Downforce", "anti_bounce", g_cfg.anti_bounce, g_ini_path);

    write_ini_int("Assist", "enabled", g_cfg.assist_enabled ? 1 : 0, g_ini_path);
    write_ini_float("Assist", "start_kmh", g_cfg.assist_start_kmh, g_ini_path);
    write_ini_float("Assist", "esp_strength", g_cfg.esp_strength, g_ini_path);
    write_ini_float("Assist", "slip_esp_strength", g_cfg.slip_esp_strength, g_ini_path);
    write_ini_float("Assist", "abs_brake_boost", g_cfg.abs_brake_boost, g_ini_path);
    write_ini_float("Assist", "tc_throttle_boost", g_cfg.tc_throttle_boost, g_ini_path);
    write_ini_float("Assist", "max_yaw_decel", g_cfg.max_yaw_decel, g_ini_path);
    write_ini_float("Assist", "speed_steer_damp", g_cfg.speed_steer_damp, g_ini_path);
    write_ini_float("Assist", "speed_rumble", g_cfg.speed_rumble, g_ini_path);
    write_ini_float("Assist", "esp_brake_drag", g_cfg.esp_brake_drag, g_ini_path);
    write_ini_float("Assist", "brake_dive", g_cfg.brake_dive, g_ini_path);

    write_ini_int("Tire", "enabled", g_cfg.tire_enabled ? 1 : 0, g_ini_path);
    write_ini_int("Tire", "tire_type", g_cfg.tire_type, g_ini_path);
    write_ini_int("Tire", "brake_type", g_cfg.brake_type, g_ini_path);
    write_ini_float("Tire", "tire_grip", g_cfg.tire_grip, g_ini_path);
    write_ini_float("Tire", "kamm_strength", g_cfg.kamm_strength, g_ini_path);

    write_ini_int("UI", "menu_joy_btn", g_cfg.menu_joy_btn, g_ini_path);
    write_ini_int("UI", "auto_preset", g_cfg.auto_preset ? 1 : 0, g_ini_path);
    write_ini_int("UI", "abs_level", g_cfg.abs_level, g_ini_path);
    write_ini_int("UI", "esp_level", g_cfg.esp_level, g_ini_path);
    write_ini_int("UI", "ui_advanced", g_cfg.ui_advanced ? 1 : 0, g_ini_path);
    write_ini_int("UI", "mini_hud_enabled", g_cfg.mini_hud_enabled ? 1 : 0, g_ini_path);
    write_ini_float("UI", "mini_hud_x", g_cfg.mini_hud_x, g_ini_path);
    write_ini_float("UI", "mini_hud_y", g_cfg.mini_hud_y, g_ini_path);
    write_ini_float("UI", "mini_hud_scale", g_cfg.mini_hud_scale, g_ini_path);
    write_ini_int("UI", "ui_lang", g_cfg.ui_lang, g_ini_path);
}
