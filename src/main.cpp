#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "scssdk.h"
#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

#include "config.h"
#include "telemetry.h"
#include "dx11_blur.h"
#include "physics_downforce.h"
#include "MinHook.h"

static HMODULE g_hModule = nullptr;

static SCSAPI_VOID on_telemetry_paused(const scs_event_t, const void*, const scs_context_t) {
    g_tel.paused.store(true, std::memory_order_relaxed);
}

static SCSAPI_VOID on_telemetry_started(const scs_event_t, const void*, const scs_context_t) {
    g_tel.paused.store(false, std::memory_order_relaxed);
}

static SCSAPI_VOID on_configuration(const scs_event_t, const void* const event_info, const scs_context_t) {
    const auto* cfg = reinterpret_cast<const scs_telemetry_configuration_t*>(event_info);
    if (!cfg || !cfg->id || !cfg->attributes) return;
    if (std::strcmp(cfg->id, SCS_TELEMETRY_CONFIG_truck) != 0) return;

    const char* brand_id = "";
    const char* brand    = "";
    const char* veh_id   = "";
    const char* name     = "";
    float rpm_limit      = 0.0f;
    uint32_t wheel_count = 0;

    for (const scs_named_value_t* attr = cfg->attributes; attr->name; ++attr) {
        if (std::strcmp(attr->name, "brand_id") == 0 && attr->value.type == SCS_VALUE_TYPE_string) {
            brand_id = attr->value.value_string.value ? attr->value.value_string.value : "";
        } else if (std::strcmp(attr->name, "brand") == 0 && attr->value.type == SCS_VALUE_TYPE_string) {
            brand = attr->value.value_string.value ? attr->value.value_string.value : "";
        } else if (std::strcmp(attr->name, "id") == 0 && attr->value.type == SCS_VALUE_TYPE_string) {
            veh_id = attr->value.value_string.value ? attr->value.value_string.value : "";
        } else if (std::strcmp(attr->name, "name") == 0 && attr->value.type == SCS_VALUE_TYPE_string) {
            name = attr->value.value_string.value ? attr->value.value_string.value : "";
        } else if (std::strcmp(attr->name, "rpm_limit") == 0 && attr->value.type == SCS_VALUE_TYPE_float) {
            rpm_limit = attr->value.value_float.value;
        } else if (std::strcmp(attr->name, "wheel_count") == 0 && attr->value.type == SCS_VALUE_TYPE_u32) {
            wheel_count = attr->value.value_u32.value;
        }
    }

    if (brand[0] == '\0' && name[0] == '\0' && veh_id[0] == '\0') return;

    char fullName[128];
    if (brand[0] && name[0]) {
        std::snprintf(fullName, sizeof(fullName), "%s %s", brand, name);
    } else {
        std::snprintf(fullName, sizeof(fullName), "%s", name[0] ? name : (brand[0] ? brand : veh_id));
    }

    CarClass cls = classify_vehicle(brand_id, brand, veh_id, name, rpm_limit, wheel_count);
    set_car_name(fullName);
    g_tel.car_rpm_limit.store(rpm_limit, std::memory_order_relaxed);
    g_tel.car_wheel_count.store(wheel_count, std::memory_order_relaxed);
    g_tel.detected_class.store(static_cast<int>(cls), std::memory_order_relaxed);
    g_tel.reset_thermals.store(true, std::memory_order_relaxed);

    if (g_cfg.auto_preset) {
        apply_car_preset(cls);
    }

    char logBuf[256];
    std::snprintf(logBuf, sizeof(logBuf),
        "[ets2_car_enhance] Vehicle: '%s' (wheels=%u, rpm=%.0f) -> %s (auto=%d)",
        fullName, wheel_count, rpm_limit, car_class_name_ru(cls), g_cfg.auto_preset ? 1 : 0);
    plugin_log(SCS_LOG_TYPE_message, logBuf);
}

static SCSAPI_VOID on_speed_channel(
    const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    if (value && value->type == SCS_VALUE_TYPE_float) {
        g_tel.speed_mps.store(value->value_float.value, std::memory_order_relaxed);
    }
}

static SCSAPI_VOID on_steer_channel(
    const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    if (value && value->type == SCS_VALUE_TYPE_float) {
        g_tel.steer.store(value->value_float.value, std::memory_order_relaxed);
    }
}

static SCSAPI_VOID on_throttle_channel(
    const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    if (value && value->type == SCS_VALUE_TYPE_float) {
        g_tel.throttle.store(value->value_float.value, std::memory_order_relaxed);
    }
}

static SCSAPI_VOID on_brake_channel(
    const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    if (value && value->type == SCS_VALUE_TYPE_float) {
        g_tel.brake.store(value->value_float.value, std::memory_order_relaxed);
    }
}

static SCSAPI_VOID on_game_time_channel(
    const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    if (value && value->type == SCS_VALUE_TYPE_u32) {
        uint32_t mins = value->value_u32.value % 1440u;
        float hour = static_cast<float>(mins) / 60.0f;
        // ~16 C at 03:00 night -> ~27 C at 15:00 day
        float amb = 21.5f + 5.5f * std::sin((hour - 9.0f) * 0.26179939f);
        g_tel.ambient_temp.store(amb, std::memory_order_relaxed);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

extern "C" {

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params) {
    if (version != SCS_TELEMETRY_VERSION_1_01) {
        return SCS_RESULT_unsupported;
    }

    const auto* vp = reinterpret_cast<const scs_telemetry_init_params_v101_t*>(params);
    if (!vp) return SCS_RESULT_generic_error;

    init_telemetry_sync();
    g_scs_log = vp->common.log;
    init_ini_path(g_hModule);
    load_config();

    vp->register_for_event(SCS_TELEMETRY_EVENT_paused,        on_telemetry_paused,  nullptr);
    vp->register_for_event(SCS_TELEMETRY_EVENT_started,       on_telemetry_started, nullptr);
    vp->register_for_event(SCS_TELEMETRY_EVENT_configuration, on_configuration,     nullptr);

    vp->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_speed,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_float,
        SCS_TELEMETRY_CHANNEL_FLAG_none,
        on_speed_channel,
        nullptr);
    vp->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_effective_steering,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_float,
        SCS_TELEMETRY_CHANNEL_FLAG_none,
        on_steer_channel,
        nullptr);
    vp->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_effective_throttle,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_float,
        SCS_TELEMETRY_CHANNEL_FLAG_none,
        on_throttle_channel,
        nullptr);
    vp->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_effective_brake,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_float,
        SCS_TELEMETRY_CHANNEL_FLAG_none,
        on_brake_channel,
        nullptr);
    vp->register_for_channel(
        SCS_TELEMETRY_CHANNEL_game_time,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_u32,
        SCS_TELEMETRY_CHANNEL_FLAG_none,
        on_game_time_channel,
        nullptr);

    g_tel.running.store(true);
    MH_Initialize();
    dx11_blur_start();
    physics_downforce_start();

    plugin_log(SCS_LOG_TYPE_message, "[ets2_car_enhance] v1.7 initialized (Axle Aero Balance + Slip-Angle ESP)");
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    g_tel.running.store(false);
    physics_downforce_stop();
    dx11_blur_stop();
    MH_Uninitialize();
    plugin_log(SCS_LOG_TYPE_message, "[ets2_car_enhance] shutdown complete");
    g_scs_log = nullptr;
    destroy_telemetry_sync();
}

} // extern "C"
