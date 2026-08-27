// =============================================================================
//  sh3vr - bootstrap_stub.cpp
//  Environment scan and D3D9 hook installation, executed on VrThread.
// =============================================================================

#include <windows.h>
#include <dinput.h>
#include <cstdint>

#pragma comment(lib, "dxguid.lib")

#include "MinHook.h"
#include "shared_frame.h"

// Provided by dllmain.cpp
extern void Log(const char* format, ...);

// Provided by d3d9_hook.cpp
extern bool D3D9Hook_Install();
extern void D3D9Hook_Remove();
extern bool Interop8_ReadControllerState(Sh3VrControllerState* state);

using DirectInputCreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*,
    REFGUID, void**, IUnknown*);
using DirectInputGetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void*,
    DWORD, LPVOID);
using GetKeyStateFn = SHORT(WINAPI*)(int);

static DirectInputCreateDeviceFn g_originalCreateDevice = nullptr;
static DirectInputGetDeviceStateFn g_originalKeyboardGetDeviceState = nullptr;
static DirectInputGetDeviceStateFn g_originalMouseGetDeviceState = nullptr;
static void* g_keyboardGetDeviceStateTarget = nullptr;
static void* g_mouseGetDeviceStateTarget = nullptr;
static void* volatile g_keyboardDevice = nullptr;
static void* volatile g_mouseDevice = nullptr;
static volatile LONG g_controllerInputLogged = 0;
static GetKeyStateFn g_originalGetKeyState = nullptr;

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

static SHORT WINAPI HookedGetKeyState(int virtualKey)
{
    SHORT state = g_originalGetKeyState(virtualKey);
    if (virtualKey != VK_LBUTTON && virtualKey != VK_RBUTTON)
        return state;

    Sh3VrControllerState controller = {};
    if (!ReadFreshControllerState(controller))
        return state;

    const bool pressed = virtualKey == VK_LBUTTON
        ? (controller.buttons & SH3VR_BUTTON_RIGHT_TRIGGER) != 0
        : (controller.buttons & SH3VR_BUTTON_LEFT_TRIGGER) != 0;
    if (pressed)
    {
        // SH3 tests bit 0x80 in AL after GetKeyState instead of testing the
        // documented high bit of the returned SHORT. Set both representations.
        state = static_cast<SHORT>(state | static_cast<SHORT>(0x8080));
    }
    return state;
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
            if ((buttons & SH3VR_BUTTON_RIGHT_TRIGGER) != 0)
                mouse->rgbButtons[0] |= 0x80;
            if ((buttons & SH3VR_BUTTON_LEFT_TRIGGER) != 0)
                mouse->rgbButtons[1] |= 0x80;
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
    previousButtons = buttons;

    SetVirtualKey(keys, stateBytes, DIK_W,
        controller.thumbstick[0][1] > movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_UP,
        controller.thumbstick[0][1] > movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_S,
        controller.thumbstick[0][1] < -movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_DOWN,
        controller.thumbstick[0][1] < -movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_A,
        controller.thumbstick[0][0] < -movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_LEFT,
        controller.thumbstick[0][0] < -movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_D,
        controller.thumbstick[0][0] > movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_RIGHT,
        controller.thumbstick[0][0] > movementThreshold);
    SetVirtualKey(keys, stateBytes, DIK_Q,
        controller.thumbstick[1][0] < -turnThreshold);
    SetVirtualKey(keys, stateBytes, DIK_E,
        controller.thumbstick[1][0] > turnThreshold);

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
    SetVirtualKey(keys, stateBytes, DIK_TAB,
        (buttons & SH3VR_BUTTON_RIGHT_STICK) != 0);
    SetVirtualKey(keys, stateBytes, DIK_LSHIFT,
        (buttons & (SH3VR_BUTTON_LEFT_GRIP |
            SH3VR_BUTTON_LEFT_STICK)) != 0);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedKeyboardGetDeviceState(void* device,
    DWORD stateBytes, LPVOID stateData)
{
    const HRESULT result = g_originalKeyboardGetDeviceState(device,
        stateBytes, stateData);
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
}
