// =============================================================================
//  sh3vr - bootstrap_stub.cpp
//  Environment scan and D3D9 hook installation, executed on VrThread.
// =============================================================================

#include <windows.h>
#include <dinput.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "dxguid.lib")

#include "MinHook.h"
#include "shared_frame.h"

// Provided by dllmain.cpp
extern void Log(const char* format, ...);

// Provided by d3d9_hook.cpp
extern bool D3D9Hook_Install();
extern void D3D9Hook_Remove();
extern bool Interop8_ReadControllerState(Sh3VrControllerState* state);
extern bool Interop8_ReadHeadPose(Sh3VrHeadPose* pose);
extern std::uint32_t D3D9Hook_GetRoomscaleMovementMask();
extern bool D3D9Hook_GetCameraModCharacterAlignForward();
extern bool D3D9Hook_GetMeleeWeaponHitbox(std::uint8_t gameWeapon,
    float gripWorld[4], float tipWorld[4], float* radiusGameUnits);
extern bool D3D9Hook_GetFirearmAimRay(std::uint8_t gameWeapon,
    float muzzleWorld[4], float rayEndWorld[4]);
extern bool D3D9Hook_GetActiveFirearmAimRay(float muzzleWorld[4],
    float rayEndWorld[4]);

using DirectInputCreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*,
    REFGUID, void**, IUnknown*);
using DirectInputGetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void*,
    DWORD, LPVOID);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);

static DirectInputCreateDeviceFn g_originalCreateDevice = nullptr;
static DirectInputGetDeviceStateFn g_originalKeyboardGetDeviceState = nullptr;
static DirectInputGetDeviceStateFn g_originalMouseGetDeviceState = nullptr;
static void* g_keyboardGetDeviceStateTarget = nullptr;
static void* g_mouseGetDeviceStateTarget = nullptr;
static void* volatile g_keyboardDevice = nullptr;
static void* volatile g_mouseDevice = nullptr;
static volatile LONG g_controllerInputLogged = 0;
static volatile LONG g_cameraModStrafeLogged = 0;
static GetKeyStateFn g_originalGetKeyState = nullptr;
static GetAsyncKeyStateFn g_originalGetAsyncKeyState = nullptr;
static bool g_rightTriggerAutoAim = true;
static bool g_enableRightHandAim = false;
static float g_aimMouseCountsPerDegree = 6.0f;
static bool g_rightHandAimActive = false;
static std::int64_t g_rightHandAimLastDisplayTime = 0;
static float g_rightHandAimVirtualYaw = 0.0f;
static float g_rightHandAimVirtualPitch = 0.0f;
static LONG g_rightHandAimLogCount = 0;
static bool g_enableMeleeMotion = true;
static float g_meleeSwingSpeedMetersPerSecond = 2.80f;
static float g_meleeSwingMinTravelMeters = 0.30f;
static float g_meleeSwingMinNetTravelMeters = 0.22f;
static DWORD g_meleeSwingCooldownMilliseconds = 280;
static bool g_meleeHandPositionValid = false;
static int g_meleeLastWeaponProfile = -1;
static std::int64_t g_meleeLastDisplayTime = 0;
static float g_meleeLastHandPosition[3] = {};
static float g_meleeSmoothedVelocity[3] = {};
static float g_meleeSwingTravelMeters = 0.0f;
static float g_meleeSwingStartPosition[3] = {};
static bool g_meleeSwingPathActive = false;
static bool g_meleeSwingArmed = true;
static DWORD g_meleeLastSwingTick = 0;
static DWORD g_meleeAimPulseUntilTick = 0;
static DWORD g_meleeAttackPulseStartTick = 0;
static DWORD g_meleeAttackPulseUntilTick = 0;
static LONG g_meleeSwingLogCount = 0;
static bool g_meleeWorldHitboxValid = false;
static float g_meleeLastWeaponGripWorld[4] = {};
static float g_meleeLastWeaponTipWorld[4] = {};
static float g_meleeSwingStartGripWorld[4] = {};
static float g_meleeSwingStartTipWorld[4] = {};
static LONG g_meleeNativeHitboxLogCount = 0;
static LONG g_firearmNativeAimLogCount = 0;
static volatile LONG g_meleePendingCollision = 0;
static float g_meleePendingPreviousGripWorld[4] = {};
static float g_meleePendingPreviousTipWorld[4] = {};
static float g_meleePendingCurrentGripWorld[4] = {};
static float g_meleePendingCurrentTipWorld[4] = {};
static float g_meleePendingRadiusGameUnits = 0.0f;
static std::uint8_t g_meleePendingWeapon = 0;
static DWORD g_meleePendingCollisionUntilTick = 0;
// The PS2-derived character layout is not valid for the PC executable. Keep
// this legacy read-only probe disabled unless a developer is investigating a
// specific build; it can otherwise scan a large amount of unrelated memory.
static bool g_enableWeaponScanDiagnostics = false;
static volatile LONG g_weaponScanStarted = 0;
static volatile LONG g_gameFlashlightEnabled = 0;
static volatile LONG g_gameFlashlightInputStateKnown = 0;
static bool g_flashlightKeyWasDown = false;

bool InputBridge_IsFlashlightEnabled()
{
    return InterlockedCompareExchange(&g_gameFlashlightEnabled, 0, 0) != 0;
}

void InputBridge_ObserveFlashlightEnabled()
{
    // Saves can load with the flashlight already enabled, so there may be no
    // F-key edge for the input bridge to observe.  Let the renderer establish
    // that initial state from a clear flashlight hotspot.  Once an explicit
    // key edge has been seen, input remains authoritative and a bright scene
    // cannot accidentally switch the tracked state back on.
    if (InterlockedCompareExchange(&g_gameFlashlightInputStateKnown, 0, 0) != 0)
        return;
    if (InterlockedCompareExchange(&g_gameFlashlightEnabled, 1, 0) == 0)
        Log("InputBridge: SH3 flashlight inferred on from its rendered hotspot");
}

static bool ReadMotionControlSetting(const char* key, bool defaultValue)
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return defaultValue;
    }

    char* finalSlash = std::strrchr(iniPath, '\\');
    if (!finalSlash)
        return defaultValue;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("MotionControls", key,
        defaultValue ? 1 : 0, iniPath) != 0;
}

static int ReadMotionControlIntSetting(const char* key, int defaultValue)
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return defaultValue;
    }

    char* finalSlash = std::strrchr(iniPath, '\\');
    if (!finalSlash)
        return defaultValue;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("MotionControls", key, defaultValue,
        iniPath);
}

static bool NormalizeQuaternion(float quaternion[4])
{
    const float lengthSquared =
        quaternion[0] * quaternion[0] +
        quaternion[1] * quaternion[1] +
        quaternion[2] * quaternion[2] +
        quaternion[3] * quaternion[3];
    if (!std::isfinite(lengthSquared) || lengthSquared < 0.000001f)
        return false;

    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    for (int component = 0; component < 4; ++component)
        quaternion[component] *= inverseLength;
    return true;
}

static void MultiplyQuaternions(const float left[4], const float right[4],
    float output[4])
{
    output[0] = left[3] * right[0] + left[0] * right[3] +
        left[1] * right[2] - left[2] * right[1];
    output[1] = left[3] * right[1] - left[0] * right[2] +
        left[1] * right[3] + left[2] * right[0];
    output[2] = left[3] * right[2] + left[0] * right[1] -
        left[1] * right[0] + left[2] * right[3];
    output[3] = left[3] * right[3] - left[0] * right[0] -
        left[1] * right[1] - left[2] * right[2];
}

static void RotateVectorByQuaternion(const float quaternion[4],
    const float input[3], float output[3])
{
    const float vectorQuaternion[4] = {
        input[0], input[1], input[2], 0.0f
    };
    const float inverse[4] = {
        -quaternion[0], -quaternion[1], -quaternion[2], quaternion[3]
    };
    float temporary[4] = {};
    float rotated[4] = {};
    MultiplyQuaternions(quaternion, vectorQuaternion, temporary);
    MultiplyQuaternions(temporary, inverse, rotated);
    output[0] = rotated[0];
    output[1] = rotated[1];
    output[2] = rotated[2];
}

static bool ReadRightHandAimAngles(const Sh3VrControllerState& controller,
    float* yawDegrees, float* pitchDegrees)
{
    if (!yawDegrees || !pitchDegrees ||
        (controller.aimPose[1].flags & SH3VR_POSE_ORIENTATION_VALID) == 0)
    {
        return false;
    }

    Sh3VrHeadPose headPose = {};
    if (!Interop8_ReadHeadPose(&headPose) ||
        (headPose.flags & SH3VR_POSE_ORIENTATION_VALID) == 0)
    {
        return false;
    }

    float headOrientation[4] = {
        headPose.orientation[0], headPose.orientation[1],
        headPose.orientation[2], headPose.orientation[3]
    };
    float aimOrientation[4] = {
        controller.aimPose[1].orientation[0],
        controller.aimPose[1].orientation[1],
        controller.aimPose[1].orientation[2],
        controller.aimPose[1].orientation[3]
    };
    if (!NormalizeQuaternion(headOrientation) ||
        !NormalizeQuaternion(aimOrientation))
    {
        return false;
    }

    const float inverseHead[4] = {
        -headOrientation[0], -headOrientation[1],
        -headOrientation[2], headOrientation[3]
    };
    float relativeAim[4] = {};
    MultiplyQuaternions(inverseHead, aimOrientation, relativeAim);
    if (!NormalizeQuaternion(relativeAim))
        return false;

    const float openXrForward[3] = { 0.0f, 0.0f, -1.0f };
    float headLocalForward[3] = {};
    RotateVectorByQuaternion(relativeAim, openXrForward, headLocalForward);
    const float horizontalLength = std::sqrt(
        headLocalForward[0] * headLocalForward[0] +
        headLocalForward[2] * headLocalForward[2]);
    if (!std::isfinite(horizontalLength) || horizontalLength < 0.0001f)
        return false;

    constexpr float radiansToDegrees = 57.295779513082320876f;
    *yawDegrees = std::atan2(headLocalForward[0],
        -headLocalForward[2]) * radiansToDegrees;
    *pitchDegrees = std::atan2(headLocalForward[1],
        horizontalLength) * radiansToDegrees;
    return std::isfinite(*yawDegrees) && std::isfinite(*pitchDegrees);
}

static void InjectRightHandAim(DIMOUSESTATE* mouse,
    const Sh3VrControllerState& controller)
{
    const bool attackHeld =
        (controller.buttons & SH3VR_BUTTON_RIGHT_TRIGGER) != 0;
    const bool manualAimHeld =
        (controller.buttons & SH3VR_BUTTON_LEFT_TRIGGER) != 0;
    if (!g_enableRightHandAim || (!attackHeld && !manualAimHeld))
    {
        g_rightHandAimActive = false;
        g_rightHandAimLastDisplayTime = 0;
        return;
    }

    float targetYaw = 0.0f;
    float targetPitch = 0.0f;
    if (!ReadRightHandAimAngles(controller, &targetYaw, &targetPitch))
    {
        g_rightHandAimActive = false;
        g_rightHandAimLastDisplayTime = 0;
        return;
    }

    targetYaw = std::clamp(targetYaw, -100.0f, 100.0f);
    targetPitch = std::clamp(targetPitch, -70.0f, 70.0f);
    if (!g_rightHandAimActive)
    {
        // SH3 starts a fresh aim action from the character's forward axis.
        // The virtual angles track the mouse deltas already submitted during
        // this action so polling the same target cannot accumulate rotation.
        g_rightHandAimVirtualYaw = 0.0f;
        g_rightHandAimVirtualPitch = 0.0f;
        g_rightHandAimActive = true;
        g_rightHandAimLastDisplayTime = 0;
    }
    if (controller.predictedDisplayTime == g_rightHandAimLastDisplayTime)
        return;
    g_rightHandAimLastDisplayTime = controller.predictedDisplayTime;

    const float yawError = targetYaw - g_rightHandAimVirtualYaw;
    const float pitchError = targetPitch - g_rightHandAimVirtualPitch;
    constexpr LONG maximumCountsPerUpdate = 240;
    const LONG yawCounts = std::clamp(static_cast<LONG>(std::lround(
        yawError * g_aimMouseCountsPerDegree)),
        -maximumCountsPerUpdate, maximumCountsPerUpdate);
    const LONG pitchCounts = std::clamp(static_cast<LONG>(std::lround(
        -pitchError * g_aimMouseCountsPerDegree)),
        -maximumCountsPerUpdate, maximumCountsPerUpdate);
    mouse->lX += yawCounts;
    mouse->lY += pitchCounts;
    g_rightHandAimVirtualYaw +=
        static_cast<float>(yawCounts) / g_aimMouseCountsPerDegree;
    g_rightHandAimVirtualPitch -=
        static_cast<float>(pitchCounts) / g_aimMouseCountsPerDegree;

    const LONG logCount = InterlockedIncrement(&g_rightHandAimLogCount);
    if (logCount <= 12 || logCount % 900 == 0)
    {
        Log("MotionControls: right-hand aim %d, target yaw x10 %d, "
            "pitch x10 %d, mouse delta %ld/%ld", logCount,
            static_cast<int>(std::lround(targetYaw * 10.0f)),
            static_cast<int>(std::lround(targetPitch * 10.0f)),
            yawCounts, pitchCounts);
    }
}

static bool IsCameraModLoaded()
{
    return GetModuleHandleA("OTSMod.dll") != nullptr;
}

static bool InstallInputHook(void* target, void* detour, void** original,
    const char* name)
{
    const MH_STATUS createStatus = MH_CreateHook(target, detour, original);
    if (createStatus != MH_OK)
    {
        Log("InputBridge: MH_CreateHook(%s) failed with status %d",
            name, static_cast<int>(createStatus));
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
    {
        Log("InputBridge: MH_EnableHook(%s) failed with status %d",
            name, static_cast<int>(enableStatus));
        return false;
    }
    Log("InputBridge: hooked %s at 0x%08X", name,
        static_cast<unsigned>(reinterpret_cast<UINT_PTR>(target)));
    return true;
}

static void SetVirtualKey(BYTE* keyboardState, DWORD stateBytes,
    DWORD dik, bool pressed)
{
    if (pressed && dik < stateBytes)
        keyboardState[dik] |= 0x80;
}

static bool ReadFreshControllerState(Sh3VrControllerState& state)
{
    static std::int64_t lastDisplayTime = 0;
    static DWORD lastUpdateTick = 0;

    if (!Interop8_ReadControllerState(&state))
        return false;
    if (state.predictedDisplayTime != lastDisplayTime)
    {
        lastDisplayTime = state.predictedDisplayTime;
        lastUpdateTick = GetTickCount();
    }
    if (state.active == 0 || lastUpdateTick == 0 ||
        GetTickCount() - lastUpdateTick > 250)
    {
        return false;
    }
    return true;
}

static bool TickIsBefore(DWORD now, DWORD end)
{
    return end != 0 && static_cast<LONG>(end - now) > 0;
}

#pragma pack(push, 1)
struct Sh3NativeBattleCollision
{
    std::uint16_t kind;
    std::uint16_t battleId;
    std::uint32_t attacker;
    // The PC port removes the PS2 alignment gap after attacker, but retains
    // all four vectors.  For a sword collision these form the swept weapon
    // segment: previous grip/tip followed by current grip/tip.
    float previousGrip[4];
    float previousTip[4];
    float currentGrip[4];
    float currentTip[4];
};
#pragma pack(pop)

static_assert(sizeof(Sh3NativeBattleCollision) == 0x48,
    "SH3 PC battle collision layout changed");

using Sh3NativeAddBattleCollisionFn = void(__cdecl*)(
    const Sh3NativeBattleCollision* collision);
using Sh3NativeSelectAttackFn = std::uint8_t(__cdecl*)(
    std::uint8_t weapon, std::uint8_t action);
using Sh3NativeBuildAttackRayFn = void(__cdecl*)(void* character,
    std::uint16_t attackId, float origin[4], float direction[4]);

static Sh3NativeAddBattleCollisionFn g_originalNativeAddBattleCollision =
    nullptr;
static Sh3NativeBuildAttackRayFn g_originalNativeBuildAttackRay = nullptr;

static bool ReadGameBytes(const void* address, void* output, SIZE_T size)
{
    SIZE_T bytesRead = 0;
    return address && output && size != 0 &&
        ReadProcessMemory(GetCurrentProcess(), address, output, size,
            &bytesRead) != FALSE && bytesRead == size;
}

static bool ReadNativePlayerWeaponContext(std::uint8_t* weapon,
    void** player)
{
    if (!weapon || !player)
        return false;

    const auto moduleBase = reinterpret_cast<std::uintptr_t>(
        GetModuleHandleA(nullptr));
    constexpr std::uintptr_t preferredBase = 0x00400000u;
    const void* weaponAddress = reinterpret_cast<const void*>(moduleBase +
        (0x0712C656u - preferredBase));
    const void* playerAddress = reinterpret_cast<const void*>(moduleBase +
        (0x0712C100u - preferredBase));
    if (!ReadGameBytes(weaponAddress, weapon, sizeof(*weapon)) ||
        !ReadGameBytes(playerAddress, player, sizeof(*player)) || !*player)
    {
        return false;
    }

    std::uint16_t characterKind = 0;
    if (!ReadGameBytes(static_cast<const BYTE*>(*player) + 0x80u,
        &characterKind, sizeof(characterKind)) || characterKind != 0x0100u)
    {
        return false;
    }
    // PC SH3 uses one compact weapon index: 0 Handgun, 1 Shotgun,
    // 2 Submachine Gun, 3 Stun Gun, then melee weapons 4..7.
    return *weapon <= 7u;
}

static bool ReadNativeMeleeContext(std::uint8_t* weapon, void** player)
{
    return ReadNativePlayerWeaponContext(weapon, player) &&
        *weapon >= 4u && *weapon <= 7u;
}

static void ExtendMeleeWeaponSegment(float grip[4], float tip[4],
    float radiusGameUnits)
{
    if (!grip || !tip)
        return;
    const float axisX = tip[0] - grip[0];
    const float axisY = tip[1] - grip[1];
    const float axisZ = tip[2] - grip[2];
    const float axisLength = std::sqrt(axisX * axisX + axisY * axisY +
        axisZ * axisZ);
    if (std::isfinite(axisLength) && axisLength > 0.001f &&
        std::isfinite(radiusGameUnits) && radiusGameUnits > 0.0f)
    {
        const float capScale = radiusGameUnits / axisLength;
        grip[0] -= axisX * capScale;
        grip[1] -= axisY * capScale;
        grip[2] -= axisZ * capScale;
        tip[0] += axisX * capScale;
        tip[1] += axisY * capScale;
        tip[2] += axisZ * capScale;
    }
}

static bool ArmNativeMeleeHitbox(std::uint8_t weapon,
    const float previousGrip[4], const float previousTip[4],
    const float currentGrip[4], const float currentTip[4],
    float radiusGameUnits)
{
    if (!g_originalNativeAddBattleCollision || !previousGrip ||
        !previousTip || !currentGrip || !currentTip)
        return false;

    InterlockedExchange(&g_meleePendingCollision, 0);
    std::memcpy(g_meleePendingPreviousGripWorld, previousGrip,
        sizeof(g_meleePendingPreviousGripWorld));
    std::memcpy(g_meleePendingPreviousTipWorld, previousTip,
        sizeof(g_meleePendingPreviousTipWorld));
    std::memcpy(g_meleePendingCurrentGripWorld, currentGrip,
        sizeof(g_meleePendingCurrentGripWorld));
    std::memcpy(g_meleePendingCurrentTipWorld, currentTip,
        sizeof(g_meleePendingCurrentTipWorld));
    g_meleePendingPreviousGripWorld[3] = 1.0f;
    g_meleePendingPreviousTipWorld[3] = 1.0f;
    g_meleePendingCurrentGripWorld[3] = 1.0f;
    g_meleePendingCurrentTipWorld[3] = 1.0f;
    g_meleePendingRadiusGameUnits = radiusGameUnits;
    g_meleePendingWeapon = weapon;
    g_meleePendingCollisionUntilTick = GetTickCount() + 1500u;
    MemoryBarrier();
    InterlockedExchange(&g_meleePendingCollision, 1);

    const LONG count = InterlockedIncrement(&g_meleeNativeHitboxLogCount);
    if (count <= 20 || count % 100 == 0)
    {
        const float x = currentTip[0] - previousTip[0];
        const float y = currentTip[1] - previousTip[1];
        const float z = currentTip[2] - previousTip[2];
        const float sweepLength = std::sqrt(x * x + y * y + z * z);
        Log("MotionControls: native melee replacement %d armed: weapon %u, "
            "sweep/radius game units %d/%d", count,
            static_cast<unsigned>(weapon),
            static_cast<int>(std::lround(sweepLength)),
            static_cast<int>(std::lround(radiusGameUnits)));
    }
    return true;
}

static void __cdecl HookedNativeAddBattleCollision(
    const Sh3NativeBattleCollision* source)
{
    if (!g_originalNativeAddBattleCollision || !source)
        return;

    Sh3NativeBattleCollision collision = *source;
    const DWORD now = GetTickCount();
    if (InterlockedCompareExchange(&g_meleePendingCollision, 0, 0) != 0)
    {
        if (!TickIsBefore(now, g_meleePendingCollisionUntilTick))
        {
            InterlockedExchange(&g_meleePendingCollision, 0);
            Log("MotionControls: pending native melee replacement expired");
        }
        else
        {
            std::uint8_t weapon = 0;
            void* player = nullptr;
            const bool playerMelee = ReadNativeMeleeContext(&weapon, &player);
            const bool nativeMeleeCollision = collision.kind == 2u ||
                collision.kind == 3u;
            if (playerMelee && weapon == g_meleePendingWeapon &&
                collision.attacker == static_cast<std::uint32_t>(
                    reinterpret_cast<std::uintptr_t>(player)) &&
                nativeMeleeCollision)
            {
                std::memcpy(collision.previousGrip,
                    g_meleePendingPreviousGripWorld,
                    sizeof(collision.previousGrip));
                std::memcpy(collision.previousTip,
                    g_meleePendingPreviousTipWorld,
                    sizeof(collision.previousTip));
                std::memcpy(collision.currentGrip,
                    g_meleePendingCurrentGripWorld,
                    sizeof(collision.currentGrip));
                std::memcpy(collision.currentTip,
                    g_meleePendingCurrentTipWorld,
                    sizeof(collision.currentTip));
                ExtendMeleeWeaponSegment(collision.previousGrip,
                    collision.previousTip, g_meleePendingRadiusGameUnits);
                ExtendMeleeWeaponSegment(collision.currentGrip,
                    collision.currentTip, g_meleePendingRadiusGameUnits);
                collision.previousGrip[3] = 1.0f;
                collision.previousTip[3] = 1.0f;
                collision.currentGrip[3] = 1.0f;
                collision.currentTip[3] = 1.0f;
                InterlockedExchange(&g_meleePendingCollision, 0);
                Log("MotionControls: SH3 native melee collision replaced: "
                    "weapon %u, attack %u, kind %u",
                    static_cast<unsigned>(weapon),
                    static_cast<unsigned>(collision.battleId),
                    static_cast<unsigned>(collision.kind));
            }
        }
    }

    g_originalNativeAddBattleCollision(&collision);
}

static void __cdecl HookedNativeBuildAttackRay(void* character,
    std::uint16_t attackId, float origin[4], float direction[4])
{
    if (!g_originalNativeBuildAttackRay)
        return;

    g_originalNativeBuildAttackRay(character, attackId, origin, direction);
    if (!character || !origin || !direction)
        return;

    std::uint16_t characterKind = 0;
    if (!ReadGameBytes(static_cast<const BYTE*>(character) + 0x80u,
        &characterKind, sizeof(characterKind)) || characterKind != 0x0100u)
    {
        return;
    }

    float muzzleWorld[4] = {};
    float rayEndWorld[4] = {};
    if (!D3D9Hook_GetActiveFirearmAimRay(muzzleWorld, rayEndWorld))
        return;

    float rayDirection[3] = {
        rayEndWorld[0] - muzzleWorld[0],
        rayEndWorld[1] - muzzleWorld[1],
        rayEndWorld[2] - muzzleWorld[2]
    };
    const float rayLength = std::sqrt(
        rayDirection[0] * rayDirection[0] +
        rayDirection[1] * rayDirection[1] +
        rayDirection[2] * rayDirection[2]);
    if (!std::isfinite(rayLength) || rayLength < 0.001f)
        return;

    for (int axis = 0; axis < 3; ++axis)
    {
        origin[axis] = muzzleWorld[axis];
        direction[axis] = rayDirection[axis] / rayLength;
    }
    origin[3] = 1.0f;
    direction[3] = 0.0f;

    const LONG count = InterlockedIncrement(&g_firearmNativeAimLogCount);
    if (count <= 24 || count % 120 == 0)
    {
        Log("MotionControls: native firearm origin/direction replaced %d: "
            "attack %u", count, static_cast<unsigned>(attackId));
    }
}

static bool InstallNativeMeleeCollisionHook()
{
    if (g_originalNativeAddBattleCollision)
        return true;
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(
        GetModuleHandleA(nullptr));
    constexpr std::uintptr_t preferredBase = 0x00400000u;
    void* target = reinterpret_cast<void*>(moduleBase +
        (0x004ACE10u - preferredBase));
    return InstallInputHook(target,
        reinterpret_cast<void*>(HookedNativeAddBattleCollision),
        reinterpret_cast<void**>(&g_originalNativeAddBattleCollision),
        "sh3!clBattleAddQue");
}

static bool InstallNativeFirearmRayHook()
{
    if (g_originalNativeBuildAttackRay)
        return true;
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(
        GetModuleHandleA(nullptr));
    constexpr std::uintptr_t preferredBase = 0x00400000u;
    void* target = reinterpret_cast<void*>(moduleBase +
        (0x004B3D50u - preferredBase));
    return InstallInputHook(target,
        reinterpret_cast<void*>(HookedNativeBuildAttackRay),
        reinterpret_cast<void**>(&g_originalNativeBuildAttackRay),
        "sh3!build_attack_origin_direction");
}

static void UpdateMeleeSwing(const Sh3VrControllerState& controller)
{
    std::uint8_t gameWeapon = 0;
    void* player = nullptr;
    const bool haveNativeMelee = ReadNativeMeleeContext(&gameWeapon, &player);
    const int meleeProfile = gameWeapon == 4u ? 0 :
        (gameWeapon == 5u ? 1 : (gameWeapon == 6u ? 3 :
            (gameWeapon == 7u ? 2 : -1)));
    float weaponGripWorld[4] = {};
    float weaponTipWorld[4] = {};
    float weaponRadiusGameUnits = 0.0f;
    const bool haveWorldHitbox = haveNativeMelee &&
        D3D9Hook_GetMeleeWeaponHitbox(gameWeapon, weaponGripWorld,
            weaponTipWorld, &weaponRadiusGameUnits);
    if (!g_enableMeleeMotion || meleeProfile < 0 || !haveWorldHitbox ||
        (controller.gripPose[1].flags & SH3VR_POSE_POSITION_VALID) == 0)
    {
        g_meleeHandPositionValid = false;
        g_meleeLastDisplayTime = 0;
        g_meleeSmoothedVelocity[0] = 0.0f;
        g_meleeSmoothedVelocity[1] = 0.0f;
        g_meleeSmoothedVelocity[2] = 0.0f;
        g_meleeSwingTravelMeters = 0.0f;
        g_meleeSwingPathActive = false;
        g_meleeLastWeaponProfile = -1;
        g_meleeSwingArmed = true;
        g_meleeWorldHitboxValid = false;
        return;
    }

    // Switching weapons changes the proxy endpoint length. Never interpret
    // that discontinuity as a high-speed physical swing.
    if (g_meleeLastWeaponProfile != meleeProfile)
    {
        g_meleeHandPositionValid = false;
        g_meleeLastDisplayTime = 0;
        g_meleeSmoothedVelocity[0] = 0.0f;
        g_meleeSmoothedVelocity[1] = 0.0f;
        g_meleeSmoothedVelocity[2] = 0.0f;
        g_meleeSwingTravelMeters = 0.0f;
        g_meleeSwingPathActive = false;
        g_meleeSwingArmed = true;
        g_meleeLastWeaponProfile = meleeProfile;
        g_meleeWorldHitboxValid = false;
    }

    // DirectInput and Win32 key APIs can query the same OpenXR sample many
    // times. Process its motion only once; attack pulse lifetime is measured
    // in milliseconds and is never consumed by polling.
    if (controller.predictedDisplayTime == g_meleeLastDisplayTime)
        return;

    float handPosition[3] = {
        controller.gripPose[1].position[0],
        controller.gripPose[1].position[1],
        controller.gripPose[1].position[2]
    };
    Sh3VrHeadPose headPose = {};
    if (Interop8_ReadHeadPose(&headPose) &&
        (headPose.flags & SH3VR_POSE_POSITION_VALID) != 0)
    {
        // Remove body motion from the hand signal so roomscale walking does
        // not register as a melee attack.
        for (int axis = 0; axis < 3; ++axis)
            handPosition[axis] -= headPose.position[axis];
    }

    if (g_meleeHandPositionValid && g_meleeLastDisplayTime != 0 &&
        controller.predictedDisplayTime > g_meleeLastDisplayTime)
    {
        const double elapsedSeconds = static_cast<double>(
            controller.predictedDisplayTime - g_meleeLastDisplayTime) * 1.0e-9;
        if (elapsedSeconds >= 0.002 && elapsedSeconds <= 0.100)
        {
            const float inverseElapsed = static_cast<float>(
                1.0 / elapsedSeconds);
            float velocity[3] = {};
            for (int axis = 0; axis < 3; ++axis)
            {
                velocity[axis] = (handPosition[axis] -
                    g_meleeLastHandPosition[axis]) * inverseElapsed;
                if (!std::isfinite(velocity[axis]))
                    velocity[axis] = 0.0f;
            }
            const float smoothing = std::clamp(
                static_cast<float>(elapsedSeconds) * 45.0f, 0.35f, 0.85f);
            for (int axis = 0; axis < 3; ++axis)
            {
                g_meleeSmoothedVelocity[axis] += smoothing *
                    (velocity[axis] - g_meleeSmoothedVelocity[axis]);
            }
            const float smoothedSpeedSquared =
                g_meleeSmoothedVelocity[0] * g_meleeSmoothedVelocity[0] +
                g_meleeSmoothedVelocity[1] * g_meleeSmoothedVelocity[1] +
                g_meleeSmoothedVelocity[2] * g_meleeSmoothedVelocity[2];
            const float rawSpeedSquared = velocity[0] * velocity[0] +
                velocity[1] * velocity[1] + velocity[2] * velocity[2];
            const float smoothedSpeed = smoothedSpeedSquared > 0.0f
                ? std::sqrt(smoothedSpeedSquared) : 0.0f;
            const float rawSpeed = rawSpeedSquared > 0.0f
                ? std::sqrt(rawSpeedSquared) : 0.0f;
            // Raw motion gives a prompt strike while smoothing rejects a
            // single tracking outlier. Requiring both retains responsiveness
            // without attacks from ordinary controller drift.
            const float speed = (std::min)(rawSpeed,
                smoothedSpeed * 1.35f);
            const float segmentX = handPosition[0] -
                g_meleeLastHandPosition[0];
            const float segmentY = handPosition[1] -
                g_meleeLastHandPosition[1];
            const float segmentZ = handPosition[2] -
                g_meleeLastHandPosition[2];
            const float segmentLength = std::sqrt(
                segmentX * segmentX + segmentY * segmentY +
                segmentZ * segmentZ);
            // Detect strength from real controller translation only. Using a
            // metre-long virtual endpoint amplified tiny orientation jitter
            // into multi-metre-per-second false swings. A valid strike now
            // needs sustained path length, net displacement, and peak speed.
            if (rawSpeed >= g_meleeSwingSpeedMetersPerSecond * 0.40f)
            {
                if (!g_meleeSwingPathActive)
                {
                    std::memcpy(g_meleeSwingStartPosition,
                        g_meleeLastHandPosition,
                        sizeof(g_meleeSwingStartPosition));
                    g_meleeSwingTravelMeters = 0.0f;
                    g_meleeSwingPathActive = true;
                    std::memcpy(g_meleeSwingStartTipWorld,
                        g_meleeWorldHitboxValid
                            ? g_meleeLastWeaponTipWorld : weaponTipWorld,
                        sizeof(g_meleeSwingStartTipWorld));
                    std::memcpy(g_meleeSwingStartGripWorld,
                        g_meleeWorldHitboxValid
                            ? g_meleeLastWeaponGripWorld : weaponGripWorld,
                        sizeof(g_meleeSwingStartGripWorld));
                }
                g_meleeSwingTravelMeters = (std::min)(0.75f,
                    g_meleeSwingTravelMeters + segmentLength);
            }
            else if (rawSpeed < g_meleeSwingSpeedMetersPerSecond * 0.25f)
            {
                g_meleeSwingTravelMeters = 0.0f;
                g_meleeSwingPathActive = false;
            }
            const DWORD now = GetTickCount();
            const DWORD weaponCooldown = meleeProfile == 0
                ? static_cast<DWORD>(260u) : (meleeProfile == 2
                    ? static_cast<DWORD>(560u)
                    : static_cast<DWORD>(380u));
            const bool cooldownElapsed = g_meleeLastSwingTick == 0 ||
                static_cast<DWORD>(now - g_meleeLastSwingTick) >=
                    (std::max)(g_meleeSwingCooldownMilliseconds,
                        weaponCooldown);
            if (speed <= g_meleeSwingSpeedMetersPerSecond * 0.25f)
            {
                g_meleeSwingArmed = true;
            }
            float netTravel = 0.0f;
            if (g_meleeSwingPathActive)
            {
                const float netX = handPosition[0] -
                    g_meleeSwingStartPosition[0];
                const float netY = handPosition[1] -
                    g_meleeSwingStartPosition[1];
                const float netZ = handPosition[2] -
                    g_meleeSwingStartPosition[2];
                netTravel = std::sqrt(netX * netX + netY * netY +
                    netZ * netZ);
            }
            if (g_meleeSwingArmed && cooldownElapsed &&
                speed >= g_meleeSwingSpeedMetersPerSecond &&
                g_meleeSwingTravelMeters >= g_meleeSwingMinTravelMeters &&
                netTravel >= g_meleeSwingMinNetTravelMeters)
            {
                const float completedTravel = g_meleeSwingTravelMeters;
                g_meleeSwingArmed = false;
                g_meleeSwingTravelMeters = 0.0f;
                g_meleeSwingPathActive = false;
                g_meleeLastSwingTick = now;
                // Arm a replacement for the collision that SH3 will create
                // in its own melee attack phase. Calling clBattleAddQue from
                // input polling is too early: the game clears that queue
                // before resolving damage.
                const bool nativeArmed = ArmNativeMeleeHitbox(gameWeapon,
                    g_meleeSwingStartGripWorld,
                    g_meleeSwingStartTipWorld,
                    weaponGripWorld, weaponTipWorld,
                    weaponRadiusGameUnits);
                // Let the game enqueue and process its normal attack. The
                // weapon mesh remains controller-driven, while damage, stun,
                // sounds and enemy reactions stay entirely native.
                g_meleeAimPulseUntilTick = now + 950u;
                g_meleeAttackPulseStartTick = now + 35u;
                g_meleeAttackPulseUntilTick = now + 240u;
                if (!nativeArmed)
                    Log("MotionControls: native melee replacement hook is "
                        "unavailable; using the attack pulse only");
                const LONG count = InterlockedIncrement(&g_meleeSwingLogCount);
                if (count <= 12 || count % 100 == 0)
                {
                    Log("MotionControls: strong translational melee swing %d, hand "
                        "speed mm/s %d, path mm %d, net mm %d, profile %d",
                        count, static_cast<int>(std::lround(speed * 1000.0f)),
                        static_cast<int>(std::lround(
                            completedTravel * 1000.0f)),
                        static_cast<int>(std::lround(netTravel * 1000.0f)),
                        meleeProfile);
                }
            }
        }
        else
        {
            g_meleeSmoothedVelocity[0] = 0.0f;
            g_meleeSmoothedVelocity[1] = 0.0f;
            g_meleeSmoothedVelocity[2] = 0.0f;
            g_meleeSwingTravelMeters = 0.0f;
            g_meleeSwingPathActive = false;
            g_meleeSwingArmed = true;
        }
    }

    std::memcpy(g_meleeLastHandPosition, handPosition,
        sizeof(g_meleeLastHandPosition));
    g_meleeLastDisplayTime = controller.predictedDisplayTime;
    g_meleeHandPositionValid = true;
    std::memcpy(g_meleeLastWeaponGripWorld, weaponGripWorld,
        sizeof(g_meleeLastWeaponGripWorld));
    std::memcpy(g_meleeLastWeaponTipWorld, weaponTipWorld,
        sizeof(g_meleeLastWeaponTipWorld));
    g_meleeWorldHitboxValid = true;

}

static bool IsMeleeAimPulseActive()
{
    return TickIsBefore(GetTickCount(), g_meleeAimPulseUntilTick);
}

static bool IsMeleeAttackPulseActive()
{
    const DWORD now = GetTickCount();
    return g_meleeAttackPulseStartTick != 0 &&
        static_cast<LONG>(now - g_meleeAttackPulseStartTick) >= 0 &&
        TickIsBefore(now, g_meleeAttackPulseUntilTick);
}

static bool IsReadableWritableRegion(const MEMORY_BASIC_INFORMATION& memory)
{
    if (memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    {
        return false;
    }

    const DWORD protection = memory.Protect & 0xFFu;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
}

static bool IsPlausibleCharacterVector(const BYTE* address)
{
    float values[4] = {};
    std::memcpy(values, address, sizeof(values));
    for (int index = 0; index < 3; ++index)
    {
        if (!std::isfinite(values[index]) ||
            std::fabs(values[index]) > 100000.0f)
            return false;
    }
    return std::isfinite(values[3]) && std::fabs(values[3]) <= 4.0f;
}

static bool IsExecutableModuleAddress(std::uintptr_t address,
    std::uintptr_t moduleStart, std::uintptr_t moduleEnd)
{
    if (address < moduleStart || address >= moduleEnd)
        return false;

    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
        sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT)
    {
        return false;
    }
    const DWORD protection = memory.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
}

static void LogCharacterLayoutCandidate(const char* label, int ordinal,
    const BYTE* candidate, std::uintptr_t moduleStart)
{
    std::int32_t index = -1;
    std::uint32_t status = 0;
    std::uint32_t step = 0;
    std::uint16_t kind = 0;
    std::uint16_t id = 0;
    std::uintptr_t function = 0;
    std::uintptr_t previous = 0;
    std::uintptr_t next = 0;
    float position[4] = {};
    float rotation[4] = {};
    std::memcpy(&index, candidate + 0x00u, sizeof(index));
    std::memcpy(&status, candidate + 0x04u, sizeof(status));
    std::memcpy(&step, candidate + 0x08u, sizeof(step));
    std::memcpy(&kind, candidate + 0x0Cu, sizeof(kind));
    std::memcpy(&id, candidate + 0x0Eu, sizeof(id));
    std::memcpy(&function, candidate + 0xA0u, sizeof(function));
    std::memcpy(&previous, candidate + 0xB8u, sizeof(previous));
    std::memcpy(&next, candidate + 0xBCu, sizeof(next));
    std::memcpy(position, candidate + 0x20u, sizeof(position));
    std::memcpy(rotation, candidate + 0x30u, sizeof(rotation));
    Log("MotionControls: %s %d: address 0x%08X, kind 0x%04X, id "
        "0x%04X, index %d, status 0x%08X, step 0x%08X, function RVA "
        "0x%08X, previous 0x%08X, next 0x%08X, position x1000 "
        "%d/%d/%d/%d, rotation x1000 %d/%d/%d/%d", label, ordinal,
        static_cast<unsigned>(reinterpret_cast<UINT_PTR>(candidate)), kind,
        id, index, status, step,
        static_cast<unsigned>(function - moduleStart),
        static_cast<unsigned>(previous), static_cast<unsigned>(next),
        static_cast<int>(std::lround(position[0] * 1000.0f)),
        static_cast<int>(std::lround(position[1] * 1000.0f)),
        static_cast<int>(std::lround(position[2] * 1000.0f)),
        static_cast<int>(std::lround(position[3] * 1000.0f)),
        static_cast<int>(std::lround(rotation[0] * 1000.0f)),
        static_cast<int>(std::lround(rotation[1] * 1000.0f)),
        static_cast<int>(std::lround(rotation[2] * 1000.0f)),
        static_cast<int>(std::lround(rotation[3] * 1000.0f)));
}

static bool IsReadableCharacterAddress(const BYTE* candidate)
{
    MEMORY_BASIC_INFORMATION memory = {};
    if (!candidate || VirtualQuery(candidate, &memory, sizeof(memory)) !=
        sizeof(memory) || !IsReadableWritableRegion(memory))
    {
        return false;
    }
    const std::uintptr_t candidateAddress =
        reinterpret_cast<std::uintptr_t>(candidate);
    const std::uintptr_t regionEnd = reinterpret_cast<std::uintptr_t>(
        memory.BaseAddress) + memory.RegionSize;
    return candidateAddress <= regionEnd &&
        regionEnd - candidateAddress >= 0x1D0u;
}

static void LogCharacterChain(const BYTE* player,
    std::uintptr_t moduleStart)
{
    const BYTE* current = player;
    for (int index = 0; current && index < 32; ++index)
    {
        if (!IsReadableCharacterAddress(current))
        {
            Log("MotionControls: character chain stopped at unreadable "
                "address 0x%08X", static_cast<unsigned>(
                    reinterpret_cast<UINT_PTR>(current)));
            return;
        }
        LogCharacterLayoutCandidate("character chain node", index, current,
            moduleStart);

        std::uintptr_t next = 0;
        std::memcpy(&next, current + 0xBCu, sizeof(next));
        if (!next || next == reinterpret_cast<std::uintptr_t>(current))
            return;
        current = reinterpret_cast<const BYTE*>(next);
    }
}

static void RunWeaponCharacterScan()
{
    if (!g_enableWeaponScanDiagnostics ||
        InterlockedCompareExchange(&g_weaponScanStarted, 1, 0) != 0)
    {
        return;
    }

    HMODULE executable = GetModuleHandleA(nullptr);
    auto* moduleBase = reinterpret_cast<BYTE*>(executable);
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(
        moduleBase);
    if (!moduleBase || dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        Log("MotionControls: character scan could not read the SH3 module");
        return;
    }
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        moduleBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
    {
        Log("MotionControls: character scan found an invalid SH3 PE header");
        return;
    }

    const std::uintptr_t moduleStart = reinterpret_cast<std::uintptr_t>(
        moduleBase);
    const std::uintptr_t moduleEnd = moduleStart +
        ntHeaders->OptionalHeader.SizeOfImage;
    constexpr SIZE_T characterBytesRequired = 0x1D0u;
    constexpr SIZE_T maximumScannedBytes = 512u * 1024u * 1024u;
    constexpr int maximumLoggedCandidates = 8;
    SIZE_T scannedBytes = 0;
    int candidateCount = 0;
    std::uintptr_t cursor = 0x10000u;

    Log("MotionControls: read-only Heather/weapon structure scan started");
    while (cursor < 0x7FFF0000u && scannedBytes < maximumScannedBytes &&
        candidateCount < maximumLoggedCandidates)
    {
        MEMORY_BASIC_INFORMATION memory = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory,
            sizeof(memory)) != sizeof(memory))
        {
            break;
        }

        const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(
            memory.BaseAddress);
        const std::uintptr_t regionEnd = regionStart + memory.RegionSize;
        if (regionEnd <= cursor)
            break;

        if (IsReadableWritableRegion(memory) &&
            memory.RegionSize >= characterBytesRequired)
        {
            scannedBytes += memory.RegionSize;
            const std::uintptr_t lastCandidate =
                regionEnd - characterBytesRequired;
            for (std::uintptr_t address = regionStart;
                address <= lastCandidate; address += sizeof(void*))
            {
                const BYTE* candidate = reinterpret_cast<const BYTE*>(
                    address);
                std::uint16_t kind = 0;
                std::memcpy(&kind, candidate + 0x0Cu, sizeof(kind));
                const bool isHeather = kind == 0x0100u;
                const bool isWeapon = kind >= 0x0801u && kind <= 0x0812u;
                if (!isHeather && !isWeapon)
                    continue;

                std::int32_t index = -1;
                std::uintptr_t function = 0;
                std::memcpy(&index, candidate + 0x00u, sizeof(index));
                std::memcpy(&function, candidate + 0xA0u,
                    sizeof(function));
                if (index < 0 || index >= 64 ||
                    !IsExecutableModuleAddress(function, moduleStart,
                        moduleEnd) ||
                    !IsPlausibleCharacterVector(candidate + 0x20u) ||
                    !IsPlausibleCharacterVector(candidate + 0x30u))
                {
                    continue;
                }

                ++candidateCount;
                LogCharacterLayoutCandidate("character candidate",
                    candidateCount, candidate, moduleStart);
                if (isHeather)
                    LogCharacterChain(candidate, moduleStart);
                if (candidateCount >= maximumLoggedCandidates)
                    break;
            }
        }
        cursor = regionEnd;
    }

    Log("MotionControls: character scan completed: %d candidate(s), %u "
        "MiB of writable memory inspected", candidateCount,
        static_cast<unsigned>(scannedBytes / (1024u * 1024u)));
}

static bool ControllerPressesVirtualKey(
    const Sh3VrControllerState& controller, int virtualKey)
{
    UpdateMeleeSwing(controller);
    constexpr float movementThreshold = 0.35f;
    const std::uint32_t buttons = controller.buttons;
    const bool leftStickPressed =
        (buttons & SH3VR_BUTTON_LEFT_STICK) != 0;
    const bool rightStickPressed =
        (buttons & SH3VR_BUTTON_RIGHT_STICK) != 0;
    const bool cameraModMenuChord = leftStickPressed && rightStickPressed;
    const bool cameraModLoaded = IsCameraModLoaded();

    switch (virtualKey)
    {
    case VK_LBUTTON:
        return (buttons & SH3VR_BUTTON_RIGHT_TRIGGER) != 0 ||
            IsMeleeAttackPulseActive();
    case VK_RBUTTON:
        return (buttons & SH3VR_BUTTON_LEFT_TRIGGER) != 0 ||
            (g_rightTriggerAutoAim &&
                (buttons & SH3VR_BUTTON_RIGHT_TRIGGER) != 0) ||
            IsMeleeAimPulseActive();
    case VK_F1:
        return cameraModMenuChord;
    case VK_TAB:
        return rightStickPressed && !cameraModMenuChord;
    case VK_SHIFT:
    case VK_LSHIFT:
        return (buttons & SH3VR_BUTTON_LEFT_GRIP) != 0 ||
            (leftStickPressed && !cameraModMenuChord);
    case VK_SPACE:
    case VK_RETURN:
        return (buttons & SH3VR_BUTTON_A) != 0;
    case VK_ESCAPE:
        return (buttons & (SH3VR_BUTTON_B | SH3VR_BUTTON_MENU)) != 0;
    case 'I':
        return (buttons & SH3VR_BUTTON_X) != 0;
    case 'M':
        return (buttons & SH3VR_BUTTON_Y) != 0;
    case 'W':
    case VK_UP:
        return controller.thumbstick[0][1] > movementThreshold;
    case 'S':
    case VK_DOWN:
        return controller.thumbstick[0][1] < -movementThreshold;
    case VK_LEFT:
        return controller.thumbstick[0][0] < -movementThreshold;
    case VK_RIGHT:
        return controller.thumbstick[0][0] > movementThreshold;
    case 'A':
        return !cameraModLoaded &&
            controller.thumbstick[0][0] < -movementThreshold;
    case 'D':
        return !cameraModLoaded &&
            controller.thumbstick[0][0] > movementThreshold;
    case 'Q':
        return cameraModLoaded &&
            controller.thumbstick[0][0] < -movementThreshold;
    case 'E':
        return cameraModLoaded &&
            controller.thumbstick[0][0] > movementThreshold;
    default:
        return false;
    }
}

static SHORT MergeControllerVirtualKeyState(SHORT state, int virtualKey)
{
    Sh3VrControllerState controller = {};
    if (!ReadFreshControllerState(controller) ||
        !ControllerPressesVirtualKey(controller, virtualKey))
    {
        return state;
    }

    // Preserve the native state and add both the documented high bit and the
    // low-byte representation used by SH3 for mouse-button polling.
    return static_cast<SHORT>(state | static_cast<SHORT>(0x8080));
}

static SHORT WINAPI HookedGetKeyState(int virtualKey)
{
    SHORT state = g_originalGetKeyState(virtualKey);
    return MergeControllerVirtualKeyState(state, virtualKey);
}

static SHORT WINAPI HookedGetAsyncKeyState(int virtualKey)
{
    SHORT state = g_originalGetAsyncKeyState(virtualKey);
    return MergeControllerVirtualKeyState(state, virtualKey);
}

static HRESULT InjectControllerState(void* device, DWORD stateBytes,
    LPVOID stateData, HRESULT result)
{
    if (FAILED(result) || !stateData)
        return result;

    Sh3VrControllerState controller = {};
    if (!ReadFreshControllerState(controller))
        return result;

    if (InterlockedCompareExchange(&g_controllerInputLogged, 1, 0) == 0)
        Log("InputBridge: live Quest Touch input is being injected");

    const std::uint32_t buttons = controller.buttons;
    if (device == InterlockedCompareExchangePointer(
        &g_mouseDevice, nullptr, nullptr))
    {
        if (stateBytes >= sizeof(DIMOUSESTATE))
        {
            DIMOUSESTATE* mouse = static_cast<DIMOUSESTATE*>(stateData);
            UpdateMeleeSwing(controller);
            const bool meleeSwingPulse = IsMeleeAttackPulseActive();
            const bool meleeAimPulse = IsMeleeAimPulseActive();
            InjectRightHandAim(mouse, controller);
            if ((buttons & SH3VR_BUTTON_RIGHT_TRIGGER) != 0 ||
                meleeSwingPulse)
                mouse->rgbButtons[0] |= 0x80;
            if ((buttons & SH3VR_BUTTON_LEFT_TRIGGER) != 0 ||
                (g_rightTriggerAutoAim &&
                    (buttons & SH3VR_BUTTON_RIGHT_TRIGGER) != 0) ||
                meleeAimPulse)
            {
                mouse->rgbButtons[1] |= 0x80;
            }
        }
        return result;
    }

    if (device != InterlockedCompareExchangePointer(
        &g_keyboardDevice, nullptr, nullptr) || stateBytes < 256)
    {
        return result;
    }

    BYTE* keys = static_cast<BYTE*>(stateData);
    constexpr float movementThreshold = 0.35f;
    constexpr float turnThreshold = 0.65f;
    static std::uint32_t previousButtons = 0;
    const std::uint32_t pressedButtons = buttons & ~previousButtons;
    if (pressedButtons != 0)
        Log("InputBridge: Quest button press mask 0x%08X", pressedButtons);
    if ((pressedButtons & SH3VR_BUTTON_RIGHT_TRIGGER) != 0)
        RunWeaponCharacterScan();
    previousButtons = buttons;

    const bool cameraModLoaded = IsCameraModLoaded();
    const bool movingWithLeftStick =
        std::fabs(controller.thumbstick[0][0]) > movementThreshold ||
        std::fabs(controller.thumbstick[0][1]) > movementThreshold;
    const std::uint32_t roomscaleMovement = movingWithLeftStick
        ? SH3VR_ROOMSCALE_NONE
        : D3D9Hook_GetRoomscaleMovementMask();
    const bool cameraModCharacterAlign = cameraModLoaded &&
        D3D9Hook_GetCameraModCharacterAlignForward();
    static LONG g_cameraModCharacterAlignLogCount = 0;
    if (cameraModCharacterAlign)
    {
        const LONG count = InterlockedIncrement(
            &g_cameraModCharacterAlignLogCount);
        if (count == 1)
        {
            Log("InputBridge: Camera Mod snap turn requests Heather "
                "alignment through the game's normal forward movement path");
        }
    }
    if (cameraModLoaded && movingWithLeftStick &&
        InterlockedCompareExchange(&g_cameraModStrafeLogged, 1, 0) == 0)
    {
        Log("InputBridge: Camera Mod 2D movement active: W/S/Q/E directions; "
            "arrow-key duplicates retained for menus");
    }

    SetVirtualKey(keys, stateBytes, DIK_W,
        controller.thumbstick[0][1] > movementThreshold ||
        (roomscaleMovement & SH3VR_ROOMSCALE_FORWARD) != 0 ||
        cameraModCharacterAlign);
    SetVirtualKey(keys, stateBytes, DIK_UP,
        controller.thumbstick[0][1] > movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_S,
        controller.thumbstick[0][1] < -movementThreshold ||
        (roomscaleMovement & SH3VR_ROOMSCALE_BACKWARD) != 0);
    SetVirtualKey(keys, stateBytes, DIK_DOWN,
        controller.thumbstick[0][1] < -movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_A,
        (!cameraModLoaded &&
            controller.thumbstick[0][0] < -movementThreshold));
    SetVirtualKey(keys, stateBytes, DIK_LEFT,
        controller.thumbstick[0][0] < -movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_D,
        (!cameraModLoaded &&
            controller.thumbstick[0][0] > movementThreshold));
    SetVirtualKey(keys, stateBytes, DIK_RIGHT,
        controller.thumbstick[0][0] > movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_Q,
        cameraModLoaded
            ? controller.thumbstick[0][0] < -movementThreshold ||
                (roomscaleMovement & SH3VR_ROOMSCALE_LEFT) != 0
            : controller.thumbstick[1][0] < -turnThreshold);
    SetVirtualKey(keys, stateBytes, DIK_E,
        cameraModLoaded
            ? controller.thumbstick[0][0] > movementThreshold ||
                (roomscaleMovement & SH3VR_ROOMSCALE_RIGHT) != 0
            : controller.thumbstick[1][0] > turnThreshold);

    SetVirtualKey(keys, stateBytes, DIK_SPACE,
        (buttons & SH3VR_BUTTON_A) != 0);
    SetVirtualKey(keys, stateBytes, DIK_RETURN,
        (buttons & SH3VR_BUTTON_A) != 0);
    SetVirtualKey(keys, stateBytes, DIK_NUMPADENTER,
        (buttons & SH3VR_BUTTON_A) != 0);
    SetVirtualKey(keys, stateBytes, DIK_ESCAPE,
        (buttons & (SH3VR_BUTTON_B | SH3VR_BUTTON_MENU)) != 0);
    SetVirtualKey(keys, stateBytes, DIK_I,
        (buttons & SH3VR_BUTTON_X) != 0);
    SetVirtualKey(keys, stateBytes, DIK_M,
        (buttons & SH3VR_BUTTON_Y) != 0);
    const bool leftStickPressed =
        (buttons & SH3VR_BUTTON_LEFT_STICK) != 0;
    const bool rightStickPressed =
        (buttons & SH3VR_BUTTON_RIGHT_STICK) != 0;
    const bool cameraModMenuChord =
        leftStickPressed && rightStickPressed;
    static bool previousCameraModMenuChord = false;
    if (cameraModMenuChord && !previousCameraModMenuChord)
        Log("InputBridge: Camera Mod F1 menu chord pressed");
    previousCameraModMenuChord = cameraModMenuChord;

    SetVirtualKey(keys, stateBytes, DIK_F1, cameraModMenuChord);
    SetVirtualKey(keys, stateBytes, DIK_TAB,
        rightStickPressed && !cameraModMenuChord);
    SetVirtualKey(keys, stateBytes, DIK_LSHIFT,
        (buttons & SH3VR_BUTTON_LEFT_GRIP) != 0 ||
        (leftStickPressed && !cameraModMenuChord));
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedKeyboardGetDeviceState(void* device,
    DWORD stateBytes, LPVOID stateData)
{
    const HRESULT result = g_originalKeyboardGetDeviceState(device,
        stateBytes, stateData);
    if (SUCCEEDED(result) && stateData && stateBytes >= 256u)
    {
        const BYTE* keys = static_cast<const BYTE*>(stateData);
        const bool flashlightKeyDown = (keys[DIK_F] & 0x80u) != 0;
        if (flashlightKeyDown && !g_flashlightKeyWasDown)
        {
            const LONG enabled = InterlockedCompareExchange(
                &g_gameFlashlightEnabled, 0, 0) == 0 ? 1 : 0;
            InterlockedExchange(&g_gameFlashlightEnabled, enabled);
            InterlockedExchange(&g_gameFlashlightInputStateKnown, 1);
            Log("InputBridge: tracked SH3 flashlight state is now %s",
                enabled != 0 ? "on" : "off");
        }
        g_flashlightKeyWasDown = flashlightKeyDown;
    }
    return InjectControllerState(device, stateBytes, stateData, result);
}

static HRESULT STDMETHODCALLTYPE HookedMouseGetDeviceState(void* device,
    DWORD stateBytes, LPVOID stateData)
{
    const HRESULT result = g_originalMouseGetDeviceState(device,
        stateBytes, stateData);
    return InjectControllerState(device, stateBytes, stateData, result);
}

static HRESULT STDMETHODCALLTYPE HookedCreateDevice(void* directInput,
    REFGUID deviceGuid, void** device, IUnknown* outer)
{
    const HRESULT result = g_originalCreateDevice(directInput, deviceGuid,
        device, outer);
    if (FAILED(result) || !device || !*device)
        return result;

    void** vtable = *reinterpret_cast<void***>(*device);
    void* getDeviceStateTarget = vtable[9];
    if (IsEqualGUID(deviceGuid, GUID_SysKeyboard))
    {
        InterlockedExchangePointer(&g_keyboardDevice, *device);
        if (!g_originalKeyboardGetDeviceState)
        {
            if (getDeviceStateTarget == g_mouseGetDeviceStateTarget &&
                g_originalMouseGetDeviceState)
            {
                g_originalKeyboardGetDeviceState =
                    g_originalMouseGetDeviceState;
            }
            else
            {
                g_keyboardGetDeviceStateTarget = getDeviceStateTarget;
                InstallInputHook(getDeviceStateTarget,
                    reinterpret_cast<void*>(HookedKeyboardGetDeviceState),
                    reinterpret_cast<void**>(&g_originalKeyboardGetDeviceState),
                    "keyboard IDirectInputDevice8::GetDeviceState");
            }
        }
        Log("InputBridge: captured keyboard device 0x%08X",
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(*device)));
    }
    else if (IsEqualGUID(deviceGuid, GUID_SysMouse))
    {
        InterlockedExchangePointer(&g_mouseDevice, *device);
        if (!g_originalMouseGetDeviceState)
        {
            if (getDeviceStateTarget == g_keyboardGetDeviceStateTarget &&
                g_originalKeyboardGetDeviceState)
            {
                g_originalMouseGetDeviceState =
                    g_originalKeyboardGetDeviceState;
            }
            else
            {
                g_mouseGetDeviceStateTarget = getDeviceStateTarget;
                InstallInputHook(getDeviceStateTarget,
                    reinterpret_cast<void*>(HookedMouseGetDeviceState),
                    reinterpret_cast<void**>(&g_originalMouseGetDeviceState),
                    "mouse IDirectInputDevice8::GetDeviceState");
            }
        }
        Log("InputBridge: captured mouse device 0x%08X",
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(*device)));
    }
    return result;
}

static void LogModulePresence(const char* name)
{
    const bool loaded = GetModuleHandleA(name) != nullptr;
    Log("  module %-28s : %s", name, loaded ? "LOADED" : "absent");
}

void VR_Bootstrap()
{
    Log("VR_Bootstrap: environment scan");

    LogModulePresence("d3d8.dll");
    LogModulePresence("d3d9.dll");
    LogModulePresence("Silent_Hill_3_PC_Fix.dll");
    LogModulePresence("d3d9on12.dll");
    LogModulePresence("d3d12.dll");
    LogModulePresence("dxgi.dll");
    LogModulePresence("DiscordHook.dll");
    LogModulePresence("quartz.dll");
    LogModulePresence("dsound.dll");

    if (GetModuleHandleA("DiscordHook.dll"))
        Log("WARNING: Discord overlay active, disable it before hooking Present");

    if (!D3D9Hook_Install())
    {
        Log("VR_Bootstrap: D3D9 hook installation FAILED, mod stays inactive");
        return;
    }

    Log("VR_Bootstrap: done");
}

void VR_Shutdown()
{
    D3D9Hook_Remove();
    Log("VR_Shutdown: complete");
}

void InputBridge_OnDirectInputCreated(IUnknown* pInterface)
{
    Log("InputBridge_OnDirectInputCreated: interface 0x%08X",
        (unsigned)(UINT_PTR)pInterface);
    if (!pInterface || g_originalCreateDevice)
        return;

    g_rightTriggerAutoAim = ReadMotionControlSetting(
        "RightTriggerAutoAim", true);
    g_enableRightHandAim = ReadMotionControlSetting(
        "EnableRightHandAim", false);
    g_enableWeaponScanDiagnostics = ReadMotionControlSetting(
        "WeaponScanDiagnostics", false);
    g_enableMeleeMotion = ReadMotionControlSetting(
        "MeleeMotion", true);
    const int aimMouseCountsPerDegree = std::clamp(
        ReadMotionControlIntSetting("AimMouseCountsPerDegree", 6), 1, 30);
    g_aimMouseCountsPerDegree =
        static_cast<float>(aimMouseCountsPerDegree);
    const int meleeSwingSpeedMmPerSecond = std::clamp(
        ReadMotionControlIntSetting("MeleeSwingSpeedMmPerSecond", 2800),
        400, 4000);
    g_meleeSwingSpeedMetersPerSecond = static_cast<float>(
        meleeSwingSpeedMmPerSecond) * 0.001f;
    const int meleeSwingMinTravelMm = std::clamp(
        ReadMotionControlIntSetting("MeleeSwingMinTravelMm", 300), 40, 500);
    g_meleeSwingMinTravelMeters = static_cast<float>(
        meleeSwingMinTravelMm) * 0.001f;
    const int meleeSwingMinNetTravelMm = std::clamp(
        ReadMotionControlIntSetting("MeleeSwingMinNetTravelMm", 220),
        30, 400);
    g_meleeSwingMinNetTravelMeters = static_cast<float>(
        meleeSwingMinNetTravelMm) * 0.001f;
    const int meleeSwingCooldown = std::clamp(
        ReadMotionControlIntSetting("MeleeSwingCooldownMs", 280), 80, 1000);
    g_meleeSwingCooldownMilliseconds = static_cast<DWORD>(
        meleeSwingCooldown);
    Log("InputBridge: [MotionControls] RightTriggerAutoAim=%s",
        g_rightTriggerAutoAim ? "1" : "0");
    Log("InputBridge: [MotionControls] EnableRightHandAim=%s; "
        "AimMouseCountsPerDegree=%d", g_enableRightHandAim ? "1" : "0",
        aimMouseCountsPerDegree);
    Log("InputBridge: [MotionControls] WeaponScanDiagnostics=%s",
        g_enableWeaponScanDiagnostics ? "1" : "0");
    Log("InputBridge: [MotionControls] MeleeMotion=%s; "
        "MeleeSwingSpeedMmPerSecond=%d; MeleeSwingMinTravelMm=%d; "
        "MeleeSwingMinNetTravelMm=%d; MeleeSwingCooldownMs=%d; "
        "native SH3 battle collision replacement enabled",
        g_enableMeleeMotion ? "1" : "0", meleeSwingSpeedMmPerSecond,
        meleeSwingMinTravelMm, meleeSwingMinNetTravelMm,
        meleeSwingCooldown);

    // Firearm motion aiming uses the same native combat queue hook and must
    // remain available even when the optional melee gesture detector is off.
    if (!InstallNativeMeleeCollisionHook())
        Log("InputBridge: native battle collision hook installation FAILED");
    if (!InstallNativeFirearmRayHook())
        Log("InputBridge: native firearm ray hook installation FAILED");

    void** vtable = *reinterpret_cast<void***>(pInterface);
    InstallInputHook(vtable[3], reinterpret_cast<void*>(HookedCreateDevice),
        reinterpret_cast<void**>(&g_originalCreateDevice),
        "IDirectInput8::CreateDevice");

    if (!g_originalGetKeyState)
    {
        void* getKeyStateTarget = reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetKeyState"));
        if (getKeyStateTarget)
        {
            InstallInputHook(getKeyStateTarget,
                reinterpret_cast<void*>(HookedGetKeyState),
                reinterpret_cast<void**>(&g_originalGetKeyState),
                "user32!GetKeyState");
        }
    }

    if (!g_originalGetAsyncKeyState)
    {
        void* getAsyncKeyStateTarget = reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                "GetAsyncKeyState"));
        if (getAsyncKeyStateTarget)
        {
            InstallInputHook(getAsyncKeyStateTarget,
                reinterpret_cast<void*>(HookedGetAsyncKeyState),
                reinterpret_cast<void**>(&g_originalGetAsyncKeyState),
                "user32!GetAsyncKeyState");
        }
    }
}
