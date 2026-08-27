// layer_a.cpp
#include <windows.h>

#if !defined(_M_IX86)
#error "sh3vr proxy must be built for Win32 (x86)."
#endif

#define SH3VR_SYSTEM_DINPUT8 "C:\\Windows\\SysWOW64\\dinput8"

#pragma comment(linker, "/export:DirectInput8Create="   SH3VR_SYSTEM_DINPUT8 ".DirectInput8Create,PRIVATE")
#pragma comment(linker, "/export:DllCanUnloadNow="      SH3VR_SYSTEM_DINPUT8 ".DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/export:DllGetClassObject="    SH3VR_SYSTEM_DINPUT8 ".DllGetClassObject,PRIVATE")
#pragma comment(linker, "/export:DllRegisterServer="    SH3VR_SYSTEM_DINPUT8 ".DllRegisterServer,PRIVATE")
#pragma comment(linker, "/export:DllUnregisterServer="  SH3VR_SYSTEM_DINPUT8 ".DllUnregisterServer,PRIVATE")
#pragma comment(linker, "/export:GetdfDIJoystick="      SH3VR_SYSTEM_DINPUT8 ".GetdfDIJoystick,PRIVATE")

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}