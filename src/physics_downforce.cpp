#include "physics_downforce.h"
#include "config.h"
#include "telemetry.h"
#include "MinHook.h"

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <atomic>

namespace {

struct Vec3 {
    float x, y, z;
};

struct Quat {
    float w, x, y, z; // Prism3D quat_t order returned by PhysX +0x108
};

// Verified physics_actor_dynamic_t vtable slots (PhysX 3.4.3 & Bullet in ETS2 1.62.3.3):
//   +0x070: int   get_activation_state()         (2 == sleeping)
//   +0x088: float get_mass()                     (exact internal mass in PhysX units)
//   +0x090: float get_inv_mass()                 (PxRigidBody::getInvMass)
//   +0x108: Quat* get_rotation(Quat* out)        (Prism3D quat_t w,x,y,z)
//   +0x138: Vec3* get_linear_velocity(Vec3* out) (world space m/s)
//   +0x158: Vec3* get_angular_velocity(Vec3* out)(world space rad/s, .y = yaw rate)
//   +0x1B0: void  activate(int force)
//   +0x268: void  apply_central_force(const Vec3* force) (PxRigidBody::addForce eFORCE)
//   +0x2B0: void  apply_torque(const Vec3* torque)       (PxRigidBody::addTorque eFORCE)

typedef void  (__fastcall* VehicleStepFn)(void* vehicle, void* arg);
typedef int   (__fastcall* GetActStateFn)(void* actor);
typedef float (__fastcall* GetFloatFn)(void* actor);
typedef Quat* (__fastcall* GetQuatFn)(void* actor, Quat* out);
typedef Vec3* (__fastcall* GetVec3Fn)(void* actor, Vec3* out);
typedef void  (__fastcall* ActivateFn)(void* actor, int force);
typedef void  (__fastcall* ApplyVec3Fn)(void* actor, const Vec3* vec);

VehicleStepFn     oVehicleStep  = nullptr;
void*             g_vstepAddr   = nullptr;
HANDLE            g_physThread  = nullptr;
std::atomic<bool> g_stopPhys{false};
std::atomic<bool> g_physHooked{false};
std::atomic<bool> g_loggedFirst{false};
bool              g_prevF7      = false;
bool              g_prevF8      = false;

bool is_truckersmp_loaded() {
    return GetModuleHandleA("core_ets2mp.dll") != nullptr ||
           GetModuleHandleA("core_atsmp.dll") != nullptr;
}

bool get_text_section(HMODULE mod, uint8_t*& outBase, size_t& outSize) {
    if (!mod) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            outBase = reinterpret_cast<uint8_t*>(mod) + sec[i].VirtualAddress;
            outSize = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

void* find_vehicle_step() {
    HMODULE exe = GetModuleHandleA(nullptr);
    uint8_t* text = nullptr;
    size_t textSize = 0;
    if (!get_text_section(exe, text, textSize) || textSize < 0x1000) {
        return nullptr;
    }

    static const uint8_t kInnerPrologue[11] = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x53, 0x56, 0x57
    };

    void* found = nullptr;
    int hitCount = 0;

    for (size_t base = 0; base + 0x24 < textSize; ++base) {
        if (text[base + 0x12] != 0x48 ||
            text[base + 0x13] != 0x8B ||
            text[base + 0x14] != 0xF9 ||
            text[base + 0x15] != 0xE8) {
            continue;
        }
        if (text[base + 0x1A] != 0x48 ||
            text[base + 0x1B] != 0x83 ||
            text[base + 0x1C] != 0xBF ||
            text[base + 0x1F] != 0x00 ||
            text[base + 0x20] != 0x00 ||
            text[base + 0x21] != 0x00) {
            continue;
        }

        int32_t rel = 0;
        std::memcpy(&rel, text + base + 0x16, sizeof(rel));
        int64_t targetOff = static_cast<int64_t>(base) + 0x1A + rel;
        if (targetOff < 0 || static_cast<size_t>(targetOff) + sizeof(kInnerPrologue) >= textSize) {
            continue;
        }
        if (std::memcmp(text + targetOff, kInnerPrologue, sizeof(kInnerPrologue)) == 0) {
            found = text + base;
            ++hitCount;
        }
    }

    return (hitCount == 1) ? found : nullptr;
}

void __fastcall hkVehicleStep(void* vehicle, void* arg) {
    oVehicleStep(vehicle, arg);

    if (g_stopPhys.load(std::memory_order_relaxed) || !vehicle) {
        return;
    }

    bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !g_prevF7) {
        g_cfg.assist_enabled = !g_cfg.assist_enabled;
        plugin_log(SCS_LOG_TYPE_message, g_cfg.assist_enabled
            ? "[ets2_car_enhance] ESP/ABS/TC Stabilizer ON (F7)"
            : "[ets2_car_enhance] ESP/ABS/TC Stabilizer OFF (F7)");
    }
    g_prevF7 = f7;

    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !g_prevF8) {
        g_cfg.downforce_enabled = !g_cfg.downforce_enabled;
        plugin_log(SCS_LOG_TYPE_message, g_cfg.downforce_enabled
            ? "[ets2_car_enhance] Aero downforce ON (F8)"
            : "[ets2_car_enhance] Aero downforce OFF (F8)");
    }
    g_prevF8 = f8;

    if (g_tel.paused.load(std::memory_order_relaxed)) {
        g_tel.dbg_downforce_g.store(0.0f, std::memory_order_relaxed);
        g_tel.dbg_slip_deg.store(0.0f, std::memory_order_relaxed);
        g_tel.dbg_esp_active.store(false, std::memory_order_relaxed);
        g_tel.dbg_abs_active.store(false, std::memory_order_relaxed);
        g_tel.dbg_tire_grip.store(1.0f, std::memory_order_relaxed);
        g_tel.dbg_kamm_state.store(0, std::memory_order_relaxed);
        g_tel.dbg_g_lat.store(0.0f, std::memory_order_relaxed);
        g_tel.dbg_g_long.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // Verify 2..4 wheeled vehicle (car chassis; skips multi-axle trailers)
    auto* vbytes = static_cast<uint8_t*>(vehicle);
    void* vdata = *reinterpret_cast<void**>(vbytes + 0x200);
    if (!vdata) return;
    uint32_t wheelCount = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(vdata) + 0x678);
    if (wheelCount < 2 || wheelCount > 4) return;

    void* actor = *reinterpret_cast<void**>(vbytes + 0x30);
    if (!actor) return;
    void** vtable = *reinterpret_cast<void***>(actor);
    if (!vtable) return;

    auto getMass           = reinterpret_cast<GetFloatFn>(vtable[0x088 / 8]);
    auto getInvMass        = reinterpret_cast<GetFloatFn>(vtable[0x090 / 8]);
    auto getRotation       = reinterpret_cast<GetQuatFn>(vtable[0x108 / 8]);
    auto getLinVel         = reinterpret_cast<GetVec3Fn>(vtable[0x138 / 8]);
    auto getAngVel         = reinterpret_cast<GetVec3Fn>(vtable[0x158 / 8]);
    auto applyCentralForce = reinterpret_cast<ApplyVec3Fn>(vtable[0x268 / 8]);
    auto applyTorque       = reinterpret_cast<ApplyVec3Fn>(vtable[0x2B0 / 8]);
    if (!getRotation || !getLinVel || !getAngVel || !applyCentralForce || !applyTorque) return;

    float mass = getMass ? getMass(actor) : 0.0f;
    if (!std::isfinite(mass) || mass <= 1e-4f) {
        float invM = getInvMass ? getInvMass(actor) : 0.0f;
        if (std::isfinite(invM) && invM > 1e-6f) mass = 1.0f / invM;
    }
    if (!std::isfinite(mass) || mass <= 1e-4f || mass > 50000.0f) return;

    Vec3 linVel = {0.0f, 0.0f, 0.0f};
    getLinVel(actor, &linVel);
    if (!std::isfinite(linVel.x) || !std::isfinite(linVel.y) || !std::isfinite(linVel.z)) return;

    float actorHorizMps = std::sqrt(linVel.x * linVel.x + linVel.z * linVel.z);
    float playerSpeedMps = std::fabs(g_tel.speed_mps.load(std::memory_order_relaxed));
    if (std::fabs(actorHorizMps - playerSpeedMps) > 8.0f) return;

    // Real dt via QueryPerformanceCounter ONLY for filtered player vehicle (ignores AI traffic!)
    static LARGE_INTEGER s_qpcFreq{};
    static LARGE_INTEGER s_prevQpc{};
    static float s_tireTemp[4]  = { 22.0f, 22.0f, 22.0f, 22.0f };
    static float s_brakeTemp[4] = { 22.0f, 22.0f, 22.0f, 22.0f };
    static void* s_lastPlayerVeh = nullptr;
    float ambTemp = g_tel.ambient_temp.load(std::memory_order_relaxed);
    if (g_tel.reset_thermals.exchange(false, std::memory_order_relaxed)) {
        for (int i = 0; i < 4; ++i) {
            s_tireTemp[i]  = ambTemp;
            s_brakeTemp[i] = ambTemp;
            g_tel.dbg_tire_temp[i].store(ambTemp, std::memory_order_relaxed);
            g_tel.dbg_brake_temp[i].store(ambTemp, std::memory_order_relaxed);
        }
    }
    if (s_qpcFreq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_qpcFreq);
        QueryPerformanceCounter(&s_prevQpc);
    }
    LARGE_INTEGER nowQpc{};
    QueryPerformanceCounter(&nowQpc);
    float dt = static_cast<float>(nowQpc.QuadPart - s_prevQpc.QuadPart) / static_cast<float>(s_qpcFreq.QuadPart);
    s_prevQpc = nowQpc;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.05f)  dt = 0.05f;

    float speedMps = actorHorizMps;
    float speedKmh = speedMps * 3.6f;

    // Extract car world forward (-Z) and right (+X) axes from actor quaternion (w, x, y, z)
    Quat q = {1.0f, 0.0f, 0.0f, 0.0f};
    getRotation(actor, &q);
    float fwdX = -2.0f * (q.x * q.z + q.w * q.y);
    float fwdZ = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    float fLen = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
    if (fLen > 1e-4f) { fwdX /= fLen; fwdZ /= fLen; }
    else { fwdX = 0.0f; fwdZ = -1.0f; }

    float rightX = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    float rightZ = 2.0f * (q.x * q.z - q.w * q.y);
    float rLen = std::sqrt(rightX * rightX + rightZ * rightZ);
    if (rLen > 1e-4f) { rightX /= rLen; rightZ /= rLen; }
    else { rightX = 1.0f; rightZ = 0.0f; }

    if (!g_loggedFirst.exchange(true, std::memory_order_relaxed)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "[ets2_car_enhance] v2.1.4 first tick: wheels=%u physxMass=%.4f fwd=(%.2f,%.2f)",
            wheelCount, mass, fwdX, fwdZ);
        plugin_log(SCS_LOG_TYPE_message, buf);
    }

    g_tel.dbg_ticks.fetch_add(1, std::memory_order_relaxed);
    g_tel.dbg_raw_mass.store(mass, std::memory_order_relaxed);

    // Compute inertial G-force (weight transfer vector: +gLong = forward/braking, +gLat = right)
    static Vec3  s_prevLinVel{0.0f, 0.0f, 0.0f};
    static bool  s_hasPrevVel = false;
    static float s_gLatSmooth  = 0.0f;
    static float s_gLongSmooth = 0.0f;
    if (s_lastPlayerVeh != vehicle) {
        s_lastPlayerVeh = vehicle;
        s_hasPrevVel = false;
    }
    if (s_hasPrevVel && dt > 0.0005f) {
        float ax = (linVel.x - s_prevLinVel.x) / dt;
        float az = (linVel.z - s_prevLinVel.z) / dt;
        float aFwd   = ax * fwdX   + az * fwdZ;
        float aRight = ax * rightX + az * rightZ;
        float rawGLong = (-aFwd / 9.81f) * 1.25f;
        float rawGLat  = (-aRight / 9.81f) * 1.25f;
        if (rawGLong >  3.0f) rawGLong =  3.0f;
        if (rawGLong < -3.0f) rawGLong = -3.0f;
        if (rawGLat  >  3.0f) rawGLat  =  3.0f;
        if (rawGLat  < -3.0f) rawGLat  = -3.0f;
        float gAlpha = dt * 16.0f;
        if (gAlpha > 1.0f) gAlpha = 1.0f;
        s_gLongSmooth += (rawGLong - s_gLongSmooth) * gAlpha;
        s_gLatSmooth  += (rawGLat  - s_gLatSmooth)  * gAlpha;
    } else {
        s_hasPrevVel = true;
    }
    s_prevLinVel = linVel;
    g_tel.dbg_g_long.store(s_gLongSmooth, std::memory_order_relaxed);
    g_tel.dbg_g_lat.store(s_gLatSmooth, std::memory_order_relaxed);

    // Count grounded wheels
    int groundedWheels = 0;
    uint8_t* wheelArr = *reinterpret_cast<uint8_t**>(vbytes + 0x4D8 + 0x08);
    uint64_t wheelArrCount = *reinterpret_cast<uint64_t*>(vbytes + 0x4D8 + 0x10);
    if (wheelArr && wheelArrCount >= wheelCount && wheelArrCount <= 16) {
        for (uint32_t w = 0; w < wheelCount; ++w) {
            if (wheelArr[w * 0x28] != 0) ++groundedWheels;
        }
    } else {
        groundedWheels = static_cast<int>(wheelCount);
    }

    bool woke = false;
    auto ensure_awake = [&]() {
        if (woke) return;
        auto getActState = reinterpret_cast<GetActStateFn>(vtable[0x070 / 8]);
        if (getActState && getActState(actor) == 2) {
            auto activate = reinterpret_cast<ActivateFn>(vtable[0x1B0 / 8]);
            if (activate) activate(actor, 0);
        }
        woke = true;
    };

    // -------------------------------------------------------------------------
    // 1. AERO DOWNFORCE + FRONT/REAR AXLE BALANCE + ANTI-BOUNCE
    // -------------------------------------------------------------------------
    float appliedG = 0.0f;
    if (g_cfg.downforce_enabled && speedKmh >= g_cfg.downforce_start_kmh) {
        float v0 = g_cfg.downforce_start_kmh / 3.6f;
        float vRef = (300.0f / 3.6f) - v0;
        float dv = speedMps - v0;
        if (dv > 0.0f && vRef > 1.0f) {
            float ratio = dv / vRef;
            float gFactor = g_cfg.downforce_g_at_300 * (ratio * ratio);
            if (gFactor > g_cfg.downforce_max_g) gFactor = g_cfg.downforce_max_g;

            if (linVel.y < -0.5f) {
                float fade = 1.0f + (linVel.y + 0.5f) / 1.0f;
                if (fade < 0.0f) fade = 0.0f;
                gFactor *= fade;
            }
            if (groundedWheels == 0) {
                gFactor *= 0.35f;
            }

            float bounceG = 0.0f;
            if (g_cfg.anti_bounce > 0.001f && linVel.y > 0.25f) {
                bounceG = g_cfg.anti_bounce * (linVel.y - 0.25f);
                if (bounceG > 0.40f) bounceG = 0.40f;
            }

            appliedG = gFactor + bounceG;
            if (appliedG > 0.001f) {
                ensure_awake();
                float totalForce = mass * 9.81f * appliedG;
                Vec3 force = { 0.0f, -totalForce, 0.0f };
                applyCentralForce(actor, &force);

                // Front/rear aero balance pitch torque (aero_front_pct < 50% -> rear-biased downforce)
                // Pure pitch torque around car's right axis (zero collision bottoming risk)
                float rearBias = (50.0f - g_cfg.aero_front_pct) / 50.0f; // +0.24 for 38% front / 62% rear
                if (std::fabs(rearBias) > 0.01f && groundedWheels >= 2) {
                    float pitchNm = (mass * 9.81f * gFactor) * rearBias * 1.15f;
                    Vec3 pitchTorque = { pitchNm * rightX, 0.0f, pitchNm * rightZ };
                    applyTorque(actor, &pitchTorque);
                }
            }
        }
    }
    g_tel.dbg_downforce_g.store(appliedG, std::memory_order_relaxed);

    // -------------------------------------------------------------------------
    // 2. DUAL-LOOP ESP: YAW-RATE + SLIP-ANGLE (vector vs nose) + ABS / TC
    // -------------------------------------------------------------------------
    Vec3 angVel = {0.0f, 0.0f, 0.0f};
    getAngVel(actor, &angVel);
    float wy = std::isfinite(angVel.y) ? angVel.y : 0.0f;
    g_tel.dbg_yaw_rate.store(wy, std::memory_order_relaxed);

    // Compute longitudinal & lateral velocity in car frame -> true slip angle (works even in deep 80 deg slides!)
    float vLong = linVel.x * fwdX + linVel.z * fwdZ;
    float vLat  = linVel.x * rightX + linVel.z * rightZ;
    float slipDeg = 0.0f;
    if (speedMps > 5.0f && vLong > -1.0f) {
        float denom = (vLong > 0.5f) ? vLong : 0.5f;
        slipDeg = std::atan2(vLat, denom) * 57.29578f;
    }
    g_tel.dbg_slip_deg.store(slipDeg, std::memory_order_relaxed);

    bool espActive = false;
    bool absActive = false;
    float appliedDecel = 0.0f;

    float steerRaw = g_tel.steer.load(std::memory_order_relaxed);
    float steerAbs = std::fabs(steerRaw);
    float brake    = g_tel.brake.load(std::memory_order_relaxed);
    float throttle = g_tel.throttle.load(std::memory_order_relaxed);
    float absSlip  = std::fabs(slipDeg);

    // -------------------------------------------------------------------------
    // THERMAL MODEL (Wave 3): Tire (25..140 C) & Brake (35..950 C) with real dt
    // -------------------------------------------------------------------------
    {
        float tireHeatMult = 1.0f;
        float tireCoolMult = 1.0f;
        switch (g_cfg.tire_type) {
            case 0: tireHeatMult = 1.55f; tireCoolMult = 0.85f; break; // Slick: heats fast
            case 1: tireHeatMult = 1.20f; tireCoolMult = 0.95f; break; // Semi-slick
            case 2: tireHeatMult = 0.80f; tireCoolMult = 1.15f; break; // Street: lasts longest
            case 3: tireHeatMult = 0.92f; tireCoolMult = 1.10f; break; // Narrow street
        }

        float speedRatio = speedKmh / 200.0f;
        if (speedRatio > 1.8f) speedRatio = 1.8f;

        // Highway cruise stays inside optimal window (~68-85C); sliding & weaving drives heat to 110-130C
        float baseHeat  = (0.55f * speedRatio + 0.30f * absSlip * (0.4f + speedRatio) + 2.4f * steerAbs * speedRatio) * tireHeatMult;
        float frontLoad = 1.0f + brake * 0.85f;
        float rearLoad  = 1.0f + throttle * 0.65f;
        // Outside wheels work significantly harder in corners (steerRaw > 0 = left turn -> right wheels loaded)
        float leftLoad  = (steerRaw < 0.0f) ? (1.0f + steerAbs * 0.95f) : (1.0f - steerAbs * 0.30f);
        float rightLoad = (steerRaw > 0.0f) ? (1.0f + steerAbs * 0.95f) : (1.0f - steerAbs * 0.30f);

        float wheelLoads[4] = {
            frontLoad * leftLoad,  // FL
            frontLoad * rightLoad, // FR
            rearLoad  * leftLoad,  // RL
            rearLoad  * rightLoad  // RR
        };

        for (int i = 0; i < 4; ++i) {
            float cool = (s_tireTemp[i] - ambTemp) * (0.014f + 0.012f * speedRatio) * tireCoolMult;
            s_tireTemp[i] += (baseHeat * wheelLoads[i] - cool) * dt;
            if (s_tireTemp[i] < 10.0f)  s_tireTemp[i] = 10.0f;
            if (s_tireTemp[i] > 140.0f) s_tireTemp[i] = 140.0f;
            g_tel.dbg_tire_temp[i].store(s_tireTemp[i], std::memory_order_relaxed);
        }

        float brakeCoolMult = 1.0f;
        if (g_cfg.brake_type == 1) brakeCoolMult = 1.20f;      // Sport drilled/slotted
        else if (g_cfg.brake_type == 2) brakeCoolMult = 1.15f; // Carbon-ceramic

        float brakeHeatInput = (brake > 0.05f && speedKmh > 10.0f) ? (brake * speedKmh * 1.15f) : 0.0f;
        float brakeBias[4]   = { 1.25f, 1.25f, 0.72f, 0.72f }; // Front takes ~63% braking load
        for (int i = 0; i < 4; ++i) {
            float airCool = (s_brakeTemp[i] - ambTemp) * (0.008f + 0.024f * speedRatio * (1.0f - 0.6f * brake)) * brakeCoolMult;
            s_brakeTemp[i] += (brakeHeatInput * brakeBias[i] - airCool) * dt;
            if (s_brakeTemp[i] < 10.0f)  s_brakeTemp[i] = 10.0f;
            if (s_brakeTemp[i] > 950.0f) s_brakeTemp[i] = 950.0f;
            g_tel.dbg_brake_temp[i].store(s_brakeTemp[i], std::memory_order_relaxed);
        }
    }

    if (g_cfg.assist_enabled && speedKmh >= g_cfg.assist_start_kmh && groundedWheels >= 2 && speedMps > 5.0f && vLong > -1.0f) {
        float steer = steerAbs;

        float gainMult = 1.0f;
        float brakeTighten = 1.0f;
        if (brake > 0.25f) {
            brakeTighten = 0.85f; // Gentle tighten so ESP does not fight brake dive
            if (g_cfg.abs_brake_boost > 0.01f) gainMult *= (1.0f + (g_cfg.abs_brake_boost - 1.0f) * 0.5f);
        }
        if (throttle > 0.45f) {
            gainMult *= g_cfg.tc_throttle_boost;
        }

        float approxIyy = mass * 2.2f;
        float totalTorqueY = 0.0f;

        // Loop A: Yaw-rate damping (catches sudden spin initiation + high-speed steering stabilization)
        if (g_cfg.esp_strength > 0.001f) {
            float absWy = std::fabs(wy);
            float speedScale = 1.0f;
            if (g_cfg.speed_steer_damp > 0.001f && speedKmh > 140.0f) {
                speedScale = 1.0f / (1.0f + g_cfg.speed_steer_damp * ((speedKmh - 140.0f) / 140.0f));
            }
            float allowWy = (0.08f + steer * 1.20f) * brakeTighten * speedScale;
            if (absWy > allowWy) {
                float excess = absWy - allowWy;
                float decel = g_cfg.esp_strength * gainMult * excess * 1.8f;
                if (decel > g_cfg.max_yaw_decel) decel = g_cfg.max_yaw_decel;
                float signWy = (wy > 0.0f) ? 1.0f : -1.0f;
                totalTorqueY += -signWy * (approxIyy * decel);
            }
        }

        // Loop B: Slip-angle alignment (catches sideways drift even when angVel.y ~ 0)
        // Cross product (fwd x linVel).y = fwd.z * linVel.x - fwd.x * linVel.z
        // Torque around +Y with sign of crossY always rotates car nose toward velocity vector!
        if (g_cfg.slip_esp_strength > 0.001f) {
            float allowSlip = (4.8f + steer * 5.0f) * brakeTighten;
            if (absSlip > allowSlip) {
                float excessDeg = absSlip - allowSlip;
                float crossY = fwdZ * linVel.x - fwdX * linVel.z;
                float signAlign = (crossY > 0.0f) ? 1.0f : -1.0f;
                float slipDecel = g_cfg.slip_esp_strength * gainMult * (excessDeg * 0.14f);
                if (slipDecel > g_cfg.max_yaw_decel) slipDecel = g_cfg.max_yaw_decel;
                totalTorqueY += signAlign * (approxIyy * slipDecel);
            }
        }

        float maxTorque = approxIyy * g_cfg.max_yaw_decel;
        if (totalTorqueY >  maxTorque) totalTorqueY =  maxTorque;
        if (totalTorqueY < -maxTorque) totalTorqueY = -maxTorque;

        static float s_espDragTimer = 0.0f;
        static float s_espDragPeak  = 0.0f;
        if (s_espDragTimer > 0.0f) s_espDragTimer -= dt;

        if (std::fabs(totalTorqueY) > 1e-4f) {
            Vec3 torque = { 0.0f, totalTorqueY, 0.0f };
            ensure_awake();
            applyTorque(actor, &torque);
            espActive = true;
            appliedDecel = std::fabs(totalTorqueY) / approxIyy;

            // Realistic ESP speed/throttle cut with short 220ms hold so mistakes noticeably scrub speed
            if (g_cfg.esp_brake_drag > 0.001f && g_cfg.max_yaw_decel > 0.01f) {
                float espRatio = appliedDecel / g_cfg.max_yaw_decel;
                if (espRatio > 1.0f) espRatio = 1.0f;
                if (espRatio > 0.12f) {
                    s_espDragTimer = 0.28f;
                    if (espRatio > s_espDragPeak) s_espDragPeak = espRatio;
                }
            }
        }
        if (s_espDragTimer > 0.0f && g_cfg.esp_brake_drag > 0.001f) {
            float dragN = mass * 9.81f * (g_cfg.esp_brake_drag * s_espDragPeak);
            Vec3 dragF = { -fwdX * dragN, 0.0f, -fwdZ * dragN };
            ensure_awake();
            applyCentralForce(actor, &dragF);
        } else {
            s_espDragPeak = 0.0f;
        }

        // Real Brake Fade pedal softness ("soft pedal"): applies even if ABS assist is OFF
        float avgFrontBrake = 0.5f * (s_brakeTemp[0] + s_brakeTemp[1]);
        float fadeStart = 560.0f, fadeSpan = 200.0f;
        if (g_cfg.brake_type == 0) { fadeStart = 430.0f; fadeSpan = 190.0f; }
        else if (g_cfg.brake_type == 2) { fadeStart = 780.0f; fadeSpan = 150.0f; }
        float fadeNorm = 0.0f;
        if (avgFrontBrake > fadeStart) {
            fadeNorm = (avgFrontBrake - fadeStart) / fadeSpan;
            if (fadeNorm > 1.0f) fadeNorm = 1.0f;
        }
        if (fadeNorm > 0.02f && brake > 0.15f && vLong > 8.0f) {
            // Counteracts part of game's native braking so overheated brakes noticeably lengthen stopping distance
            float fadeCounterN = mass * 9.81f * (0.38f * fadeNorm * brake);
            Vec3 fadeF = { fwdX * fadeCounterN, 0.0f, fwdZ * fadeCounterN };
            ensure_awake();
            applyCentralForce(actor, &fadeF);
        }

        // Loop C: High-speed ABS longitudinal brake assist (> 36 km/h) + clamped nose brake dive
        // Gives real stopping power from 200-430 km/h with BrakeType cold/fade behavior
        if (brake > 0.15f && vLong > 10.0f && g_cfg.abs_brake_boost > 0.01f) {
            float brakeNorm = (brake - 0.15f) / 0.85f;
            if (brakeNorm > 1.0f) brakeNorm = 1.0f;
            float speedFade = (vLong - 10.0f) / 30.0f;
            if (speedFade > 1.0f) speedFade = 1.0f;

            float extraBrakeG = 0.45f * (g_cfg.abs_brake_boost / 1.50f) * brakeNorm * speedFade;
            if (extraBrakeG > 0.75f) extraBrakeG = 0.75f;

            if (g_cfg.brake_type == 2) { // Carbon-ceramic cold bite
                if (avgFrontBrake < 150.0f) {
                    float coldFrac = (150.0f - avgFrontBrake) / 110.0f;
                    if (coldFrac > 1.0f) coldFrac = 1.0f;
                    extraBrakeG *= (1.0f - 0.14f * coldFrac);
                } else {
                    extraBrakeG *= 1.04f;
                }
            }
            if (fadeNorm > 0.0f) {
                extraBrakeG *= (1.0f - 0.65f * fadeNorm);
            }

            if (extraBrakeG > 0.005f) {
                absActive = true;
                float brakeN = mass * 9.81f * extraBrakeG;
                Vec3 brakeF = { -fwdX * brakeN, 0.0f, -fwdZ * brakeN };
                ensure_awake();
                applyCentralForce(actor, &brakeF);

                // Nose-down brake dive (-right axis pitch torque), hard-clamped so low splitters never scrape
                if (g_cfg.brake_dive > 0.001f) {
                    float diveG = extraBrakeG * g_cfg.brake_dive;
                    if (diveG > 0.42f) diveG = 0.42f;
                    float diveNm = (mass * 9.81f * diveG) * 1.30f;
                    Vec3 diveTorque = { -diveNm * rightX, 0.0f, -diveNm * rightZ };
                    applyTorque(actor, &diveTorque);
                }
            }
        }
    }

    g_tel.dbg_esp_active.store(espActive, std::memory_order_relaxed);
    g_tel.dbg_abs_active.store(absActive, std::memory_order_relaxed);
    g_tel.dbg_esp_decel.store(appliedDecel, std::memory_order_relaxed);

    // -------------------------------------------------------------------------
    // 3. PACEJKA-LITE TIRE MODEL + KAMM CIRCLE (progressive slip & weight transfer)
    // -------------------------------------------------------------------------
    float reportGrip = 1.0f;
    int   kammState  = 0;

    if (g_cfg.tire_enabled && speedKmh >= 40.0f && groundedWheels >= 2 && speedMps > 5.0f && vLong > -1.0f) {
        // Temperature grip modifier (depends on TireType)
        float avgTireT = 0.25f * (s_tireTemp[0] + s_tireTemp[1] + s_tireTemp[2] + s_tireTemp[3]);
        float optLow = 60.0f, optHigh = 95.0f;
        if (g_cfg.tire_type == 0) { optLow = 80.0f; optHigh = 105.0f; }      // Slick
        else if (g_cfg.tire_type == 1) { optLow = 70.0f; optHigh = 100.0f; } // Semi-slick
        else if (g_cfg.tire_type == 2) { optLow = 50.0f; optHigh = 95.0f; }  // Street
        else { optLow = 45.0f; optHigh = 90.0f; }                            // Narrow

        float tempGripMult = 1.0f;
        if (avgTireT < optLow) {
            float cold = (optLow - avgTireT) / 35.0f;
            if (cold > 1.0f) cold = 1.0f;
            tempGripMult = 1.0f - cold * (g_cfg.tire_type == 0 ? 0.16f : 0.08f);
        } else if (avgTireT > optHigh) {
            float hot = (avgTireT - optHigh) / 25.0f;
            if (hot > 1.0f) hot = 1.0f;
            tempGripMult = 1.0f - hot * (g_cfg.tire_type == 0 ? 0.22f : 0.12f);
        }

        // Progressive slip-angle grip curve (wider, smoother warning window):
        // 0-3.5 deg: peak grip (1.00)
        // 3.5-11.5 deg: wide controllable edge slip (1.00 -> 0.68)
        // 11.5+ deg: tire breakaway / drop-off (0.68 -> 0.42)
        float tireGripScale = 1.0f;
        if (absSlip > 3.5f && absSlip <= 11.5f) {
            tireGripScale = 1.0f - ((absSlip - 3.5f) / 8.0f) * 0.32f;
            kammState = 1;
        } else if (absSlip > 11.5f) {
            float over = (absSlip - 11.5f) / 9.0f;
            if (over > 1.0f) over = 1.0f;
            tireGripScale = 0.68f - over * 0.26f;
            kammState = 2;
        }
        tireGripScale *= tempGripMult;

        // Smooth lateral steering buildup so heavy cars don't snap-turn instantaneously at 200+
        static float s_steerSmooth = 0.0f;
        float alpha = dt * 8.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        s_steerSmooth += (steerRaw - s_steerSmooth) * alpha;
        float steerEffAbs  = std::fabs(s_steerSmooth);
        float steerEffSign = (s_steerSmooth >= 0.0f) ? 1.0f : -1.0f;

        float loadBonus = 1.0f + (appliedG * 0.8f < 0.35f ? appliedG * 0.8f : 0.35f);
        float approxIyy = mass * 2.2f;
        float kamm      = g_cfg.kamm_strength;
        float steerSign = (steerRaw >= 0.0f) ? 1.0f : -1.0f; // +1 = left (+Y yaw, -right force)

        // Kamm circle weight transfer (turn-in trail braking / smooth brake understeer / power oversteer)
        if (steerAbs > 0.05f && speedKmh >= 45.0f && kamm > 0.001f) {
            float kammTorqueY = 0.0f;

            if (brake >= 0.10f && brake <= 0.68f) {
                // Trail braking: front weight transfer sharpens turn-in bite
                float trailShape = 1.0f - std::fabs(brake - 0.38f) / 0.30f;
                if (trailShape > 0.0f) {
                    float bite = kamm * steerAbs * trailShape * 0.55f;
                    kammTorqueY += steerSign * (approxIyy * bite);
                    kammState = 3;
                }
            } else if (brake > 0.72f) {
                // Excessive brake in corner: smoothly fade front lateral grip (no jerky reverse yaw snap!)
                float overBrake = (brake - 0.72f) / 0.28f;
                if (overBrake > 1.0f) overBrake = 1.0f;
                tireGripScale *= (1.0f - 0.32f * overBrake);
                kammState = 4;
            } else if (throttle > 0.65f && brake < 0.10f) {
                // Heavy throttle in corner: rear traction eats lateral grip -> controllable power oversteer
                float pwrNorm = (throttle - 0.65f) / 0.35f;
                if (pwrNorm > 1.0f) pwrNorm = 1.0f;
                float slide = kamm * steerAbs * pwrNorm * 0.72f;
                kammTorqueY += steerSign * (approxIyy * slide);
                kammState = 5;
            }

            if (std::fabs(kammTorqueY) > 1e-4f) {
                ensure_awake();
                Vec3 kt = { 0.0f, kammTorqueY, 0.0f };
                applyTorque(actor, &kt);
            }
        }

        // Lateral cornering bite & progressive tire friction along car right axis
        float latForceG = 0.0f;
        if (steerEffAbs > 0.03f && g_cfg.tire_grip > 0.001f) {
            float speedFactor = (speedKmh - 40.0f) / 100.0f;
            if (speedFactor > 1.0f) speedFactor = 1.0f;
            // Centripetal bite toward turn direction (-steerEffSign * right)
            latForceG += -steerEffSign * (g_cfg.tire_grip * tireGripScale * loadBonus * 0.22f * steerEffAbs * speedFactor);
        }

        if (std::fabs(vLat) > 0.4f && g_cfg.tire_grip > 0.001f) {
            float latNorm = vLat / 12.0f;
            if (latNorm >  1.0f) latNorm =  1.0f;
            if (latNorm < -1.0f) latNorm = -1.0f;
            latForceG += -latNorm * (g_cfg.tire_grip * tireGripScale * 0.22f);
        }

        if (std::fabs(latForceG) > 0.002f) {
            float latN = mass * 9.81f * latForceG;
            Vec3 latF = { rightX * latN, 0.0f, rightZ * latN };
            ensure_awake();
            applyCentralForce(actor, &latF);
        }

        reportGrip = tireGripScale;
    }

    g_tel.dbg_tire_grip.store(reportGrip, std::memory_order_relaxed);
    g_tel.dbg_kamm_state.store(kammState, std::memory_order_relaxed);

    // -------------------------------------------------------------------------
    // 4. HIGH-SPEED ROAD RUMBLE / CHASSIS MICRO-SHAKE (160+ km/h)
    // -------------------------------------------------------------------------
    if (g_cfg.speed_rumble > 0.001f && speedKmh >= 160.0f && groundedWheels >= 2) {
        static uint32_t s_rumbleTick = 0;
        ++s_rumbleTick;
        float t = static_cast<float>(s_rumbleTick);
        float norm = (speedKmh - 160.0f) / 260.0f;
        if (norm > 1.2f) norm = 1.2f;
        float amp = g_cfg.speed_rumble * (norm * norm);

        // Multi-frequency micro-impulses: vertical road texture + subtle pitch/roll tremor
        float vWave = std::sin(t * 0.63f) * 0.6f + std::sin(t * 1.47f) * 0.4f;
        float rWave = std::sin(t * 0.91f) * 0.55f + std::cos(t * 1.73f) * 0.45f;
        float pWave = std::cos(t * 0.77f);

        float vertForce = mass * 9.81f * (0.045f * amp) * vWave;
        Vec3 rumbleF = { 0.0f, vertForce, 0.0f };
        ensure_awake();
        applyCentralForce(actor, &rumbleF);

        float rollNm  = mass * 9.81f * (0.022f * amp) * rWave;
        float pitchNm = mass * 9.81f * (0.018f * amp) * pWave;
        Vec3 rumbleT = {
            pitchNm * rightX + rollNm * fwdX,
            0.0f,
            pitchNm * rightZ + rollNm * fwdZ
        };
        applyTorque(actor, &rumbleT);
    }
}

DWORD WINAPI phys_init_thread(LPVOID) {
    for (int i = 0; i < 25 && !g_stopPhys.load(); ++i) {
        Sleep(100);
    }
    if (g_stopPhys.load()) return 0;

    if (is_truckersmp_loaded()) {
        plugin_log(SCS_LOG_TYPE_warning, "[ets2_car_enhance] TruckersMP detected, physics hooks skipped");
        return 0;
    }

    g_vstepAddr = find_vehicle_step();
    if (!g_vstepAddr) {
        plugin_log(SCS_LOG_TYPE_warning, "[ets2_car_enhance] vehicle_step signature not found, physics disabled");
        return 0;
    }

    if (MH_CreateHook(g_vstepAddr, reinterpret_cast<void*>(&hkVehicleStep), reinterpret_cast<void**>(&oVehicleStep)) == MH_OK &&
        MH_EnableHook(g_vstepAddr) == MH_OK) {
        g_physHooked.store(true);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "[ets2_car_enhance] v2.1.4 physics hook active at %p (Axle Aero + Slip-Angle ESP)", g_vstepAddr);
        plugin_log(SCS_LOG_TYPE_message, buf);
    } else {
        plugin_log(SCS_LOG_TYPE_warning, "[ets2_car_enhance] Failed to enable vehicle_step hook");
    }
    return 0;
}

} // namespace

void physics_downforce_start() {
    g_stopPhys.store(false);
    g_physThread = CreateThread(nullptr, 0, phys_init_thread, nullptr, 0, nullptr);
}

void physics_downforce_stop() {
    g_stopPhys.store(true);
    if (g_physThread) {
        WaitForSingleObject(g_physThread, 3000);
        CloseHandle(g_physThread);
        g_physThread = nullptr;
    }
    if (g_physHooked.load() && g_vstepAddr) {
        MH_DisableHook(g_vstepAddr);
        MH_RemoveHook(g_vstepAddr);
        g_physHooked.store(false);
    }
}
