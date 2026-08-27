// =============================================================================
//  sh3vr - dllmain.cpp  (version 4)
//
//  Proxy DLL entry point. Loaded by sh3.exe as dinput8.dll, it:
//    1. Opens the log file with raw Win32 calls only (no CRT file IO, which is
//       unreliable this early in process startup).
//    2. Hooks Direct3DCreate8 in the local d3d8 wrapper immediately, while the
//       process is still single-threaded. The wrapper creates its device long
//       before any worker thread of ours could run.
//    3. Spawns VrThread, which starts the hidden 64-bit OpenXR companion and
//       installs the remaining hooks outside DllMain.
//    4. Forwards DirectInput8Create to the real system dinput8.dll.
// =============================================================================

#include <windows.h>
#include "exports.h"

// ---- provided by other translation units -----------------------------------

extern void EarlyHook_D3D8();                       // d3d9_hook.cpp
extern void VR_Bootstrap();                         // bootstrap_stub.cpp
extern void VR_Shutdown();                          // bootstrap_stub.cpp
extern void InputBridge_OnDirectInputCreated(IUnknown* device);  // bootstrap_stub.cpp

// ---- state -----------------------------------------------------------------

static HMODULE g_selfModule = nullptr;
static HANDLE  g_logHandle = INVALID_HANDLE_VALUE;
static HANDLE  g_vrThread = nullptr;
static HANDLE  g_hostProcess = nullptr;
static HMODULE g_realDInput8 = nullptr;

static volatile LONG g_shutdown = 0;
static volatile LONG g_realResolved = 0;
static volatile LONG g_bootstrapRan = 0;

static CRITICAL_SECTION g_logLock;
static volatile LONG     g_logLockReady = 0;

// =============================================================================
//  Logging: CreateFileW plus WriteFile only, flushed after every line
// =============================================================================

static void OpenLog()
{
    if (InterlockedCompareExchange(&g_logLockReady, 1, 0) == 0)
        InitializeCriticalSection(&g_logLock);

    if (g_logHandle != INVALID_HANDLE_VALUE)
        return;

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(g_selfModule, path, MAX_PATH) == 0)
        return;

    // Replace the file name with sh3vr.log, keeping the game directory.
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    slash[1] = L'\0';
    lstrcatW(path, L"sh3vr.log");

    g_logHandle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const char* format, ...)
{
    char body[1024];
    va_list args;
    va_start(args, format);
    wvsprintfA(body, format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[1200];
    wsprintfA(line, "[%02u:%02u:%02u.%03u][tid %u] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        GetCurrentThreadId(), body);

    OutputDebugStringA(line);

    if (g_logHandle == INVALID_HANDLE_VALUE)
        return;

    const bool locked = (InterlockedCompareExchange(&g_logLockReady, 1, 1) == 1);
    if (locked)
        EnterCriticalSection(&g_logLock);

    DWORD written = 0;
    WriteFile(g_logHandle, line, lstrlenA(line), &written, nullptr);
    FlushFileBuffers(g_logHandle);

    if (locked)
        LeaveCriticalSection(&g_logLock);
}

static void CloseLog()
{
    if (g_logHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_logHandle);
        g_logHandle = INVALID_HANDLE_VALUE;
    }
}

// =============================================================================
//  Hidden x64 OpenXR companion
// =============================================================================

static bool StartHostProcess()
{
    if (g_hostProcess)
        return true;

    wchar_t hostPath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_selfModule, hostPath, MAX_PATH) == 0)
    {
        Log("StartHostProcess: GetModuleFileNameW failed, error %u", GetLastError());
        return false;
    }

    wchar_t* slash = wcsrchr(hostPath, L'\\');
    if (!slash)
    {
        Log("StartHostProcess: proxy path has no directory");
        return false;
    }

    wchar_t workingDirectory[MAX_PATH] = {};
    const size_t directoryLength = static_cast<size_t>(slash - hostPath + 1);
    if (directoryLength >= MAX_PATH)
        return false;
    memcpy(workingDirectory, hostPath, directoryLength * sizeof(wchar_t));
    workingDirectory[directoryLength] = L'\0';
    lstrcpyW(slash + 1, L"sh3vr_host64.exe");

    if (GetFileAttributesW(hostPath) == INVALID_FILE_ATTRIBUTES)
    {
        Log("StartHostProcess: sh3vr_host64.exe is missing from the game directory");
        return false;
    }

    wchar_t commandLine[MAX_PATH + 80] = {};
    wsprintfW(commandLine, L"\"%s\" --parent-pid %u", hostPath, GetCurrentProcessId());

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessW(hostPath, commandLine, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, workingDirectory, &startup, &process);
    if (!created)
    {
        Log("StartHostProcess: CreateProcessW failed, error %u", GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    g_hostProcess = process.hProcess;
    Log("OpenXR host started hidden, pid %u", process.dwProcessId);
    return true;
}

// =============================================================================
//  Real dinput8.dll forwarding
// =============================================================================

static void LoadRealDInput8()
{
    if (InterlockedCompareExchange(&g_realResolved, 1, 0) != 0)
        return;

    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;

    lstrcatW(path, L"\\dinput8.dll");
    g_realDInput8 = LoadLibraryW(path);

    Log("Real dinput8.dll resolved at 0x%08X", (unsigned)(UINT_PTR)g_realDInput8);
}

typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, const IID&,
    LPVOID*, IUnknown*);

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version,
    const IID& iid, LPVOID* out,
    IUnknown* outer)
{
    Log("DirectInput8Create called, version 0x%08X", (unsigned)version);

    LoadRealDInput8();
    if (!g_realDInput8)
    {
        Log("DirectInput8Create: real dinput8.dll is unavailable");
        return E_FAIL;
    }

    PFN_DirectInput8Create real = reinterpret_cast<PFN_DirectInput8Create>(
        GetProcAddress(g_realDInput8, "DirectInput8Create"));
    if (!real)
    {
        Log("DirectInput8Create: export not found in the real dinput8.dll");
        return E_FAIL;
    }

    const HRESULT hr = real(instance, version, iid, out, outer);

    if (SUCCEEDED(hr) && out && *out)
    {
        Log("DirectInput8Create succeeded, interface 0x%08X",
            (unsigned)(UINT_PTR)*out);
        InputBridge_OnDirectInputCreated(static_cast<IUnknown*>(*out));
    }
    else
    {
        Log("DirectInput8Create failed, hr = 0x%08X", (unsigned)hr);
    }

    return hr;
}

// =============================================================================
//  Worker thread
// =============================================================================

static DWORD WINAPI VrThread(LPVOID)
{
    Log("VrThread started");

    if (InterlockedCompareExchange(&g_bootstrapRan, 1, 0) != 0)
    {
        Log("VrThread: bootstrap already executed, skipping");
        return 0;
    }

    if (!StartHostProcess())
        Log("VrThread: OpenXR host did not start; flat rendering remains available");

    if (InterlockedCompareExchange(&g_shutdown, 0, 0) == 0)
        VR_Bootstrap();

    Log("VrThread finished bootstrap");
    return 0;
}

// =============================================================================
//  DllMain
// =============================================================================

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        g_selfModule = module;
        DisableThreadLibraryCalls(module);

        OpenLog();
        Log("=== sh3vr proxy attached, pid %u ===", GetCurrentProcessId());

        // The d3d8 wrapper creates its device very early, long before VrThread
        // could ever run. Hook its factory export right now, while the process
        // is still single-threaded and MinHook cannot deadlock on anything.
        EarlyHook_D3D8();

        static LONG s_threadSpawned = 0;
        if (InterlockedCompareExchange(&s_threadSpawned, 1, 0) == 0)
            g_vrThread = CreateThread(nullptr, 0, VrThread, nullptr, 0, nullptr);

        break;
    }

    case DLL_PROCESS_DETACH:
    {
        InterlockedExchange(&g_shutdown, 1);

        // When reserved is non-null the process is terminating and no cleanup
        // that touches other DLLs or threads is safe.
        if (reserved == nullptr)
        {
            VR_Shutdown();

            if (g_vrThread)
            {
                WaitForSingleObject(g_vrThread, 2000);
                CloseHandle(g_vrThread);
                g_vrThread = nullptr;
            }

            if (g_realDInput8)
            {
                FreeLibrary(g_realDInput8);
                g_realDInput8 = nullptr;
            }

            if (g_hostProcess)
            {
                CloseHandle(g_hostProcess);
                g_hostProcess = nullptr;
            }
        }

        Log("=== sh3vr proxy detaching ===");
        CloseLog();
        break;
    }

    default:
        break;
    }

    // Never fail here: returning FALSE would abort process startup.
    return TRUE;
}
