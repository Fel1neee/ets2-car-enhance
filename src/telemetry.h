#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include "scssdk_telemetry.h"

enum class CarClass : int {
    Unknown  = 0,
    Civilian = 1,
    Sport    = 2,
    Supercar = 3,
    Truck    = 4
};

enum class TireType : int {
    Slick        = 0,
    SemiSlick    = 1,
    Street       = 2,
    NarrowStreet = 3
};

enum class BrakeType : int {
    Standard      = 0,
    Sport         = 1,
    CarbonCeramic = 2
};

inline const char* tire_type_name_ru(TireType t) {
    switch (t) {
        case TireType::Slick:        return "Слик";
        case TireType::SemiSlick:    return "Полуслик";
        case TireType::Street:       return "Дорожная";
        case TireType::NarrowStreet: return "Узкая дорожная";
        default:                     return "Полуслик";
    }
}

inline const char* car_class_name_ru(CarClass c) {
    switch (c) {
        case CarClass::Civilian: return "Гражданский";
        case CarClass::Sport:    return "Спорт";
        case CarClass::Supercar: return "Спорткар";
        case CarClass::Truck:    return "Тягач (Выкл)";
        default:                 return "Не определён";
    }
}

struct TelemetryState {
    std::atomic<float> speed_mps{0.0f};
    std::atomic<float> steer{0.0f};
    std::atomic<float> throttle{0.0f};
    std::atomic<float> brake{0.0f};
    std::atomic<bool>  paused{true};
    std::atomic<bool>  running{false};

    // Detected vehicle info from SCS_TELEMETRY_EVENT_configuration ("truck")
    std::atomic<int>      detected_class{0};
    std::atomic<float>    car_rpm_limit{0.0f};
    std::atomic<uint32_t> car_wheel_count{0};
    CRITICAL_SECTION      car_cs{};
    bool                  car_cs_init{false};
    char                  car_name[128]{"Не загружено"};

    // Live physics diagnostics (updated every physics tick, shown in ImGui overlay)
    std::atomic<uint32_t> dbg_ticks{0};
    std::atomic<float>    dbg_raw_mass{0.0f};
    std::atomic<float>    dbg_downforce_g{0.0f};
    std::atomic<float>    dbg_yaw_rate{0.0f};
    std::atomic<float>    dbg_slip_deg{0.0f};
    std::atomic<float>    dbg_esp_decel{0.0f};
    std::atomic<bool>     dbg_esp_active{false};
    std::atomic<bool>     dbg_abs_active{false};
    std::atomic<float>    dbg_tire_grip{1.0f};
    std::atomic<int>      dbg_kamm_state{0}; // 0=ok, 1=edge(6-11), 2=slide(12+), 3=trail, 4=understeer, 5=oversteer
    std::atomic<float>    ambient_temp{22.0f};
    std::atomic<bool>     reset_thermals{true};
    std::atomic<float>    dbg_g_lat{0.0f};  // + = right (inertia in left turn)
    std::atomic<float>    dbg_g_long{0.0f}; // + = forward (braking weight transfer)
    // 0=FL, 1=FR, 2=RL, 3=RR
    std::atomic<float>    dbg_tire_temp[4]{{22.0f}, {22.0f}, {22.0f}, {22.0f}};
    std::atomic<float>    dbg_brake_temp[4]{{22.0f}, {22.0f}, {22.0f}, {22.0f}};
};

inline TelemetryState g_tel;
inline scs_log_t g_scs_log = nullptr;

inline void init_telemetry_sync() {
    if (!g_tel.car_cs_init) {
        InitializeCriticalSection(&g_tel.car_cs);
        g_tel.car_cs_init = true;
    }
}

inline void destroy_telemetry_sync() {
    if (g_tel.car_cs_init) {
        DeleteCriticalSection(&g_tel.car_cs);
        g_tel.car_cs_init = false;
    }
}

inline void set_car_name(const char* name) {
    if (!g_tel.car_cs_init) return;
    EnterCriticalSection(&g_tel.car_cs);
    std::strncpy(g_tel.car_name, name ? name : "Unknown", sizeof(g_tel.car_name) - 1);
    g_tel.car_name[sizeof(g_tel.car_name) - 1] = '\0';
    LeaveCriticalSection(&g_tel.car_cs);
}

inline void get_car_name(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    if (!g_tel.car_cs_init) {
        std::strncpy(out, g_tel.car_name, outSize - 1);
        out[outSize - 1] = '\0';
        return;
    }
    EnterCriticalSection(&g_tel.car_cs);
    std::strncpy(out, g_tel.car_name, outSize - 1);
    out[outSize - 1] = '\0';
    LeaveCriticalSection(&g_tel.car_cs);
}

inline void plugin_log(scs_log_type_t type, const char* msg) {
    if (g_scs_log) {
        g_scs_log(type, msg);
    }
}
