#pragma once

// Select only this host's runtime. Never change the system OpenXR registration.
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>
#include <iterator>
#include <string>

namespace sh3vr {
enum class AutoRuntime { Inherited, SteamVR, VirtualDesktop, System };

inline AutoRuntime ChooseAutoRuntime(bool inherited, bool steamServer,
    bool steamCompositor, bool steamMonitor, bool desktopStreamer)
{
    // Explicit launcher selection wins. Streamer often stays running while
    // Virtual Desktop transports SteamVR, so its presence alone cannot win.
    if (inherited) return AutoRuntime::Inherited;
    if (steamServer && (steamCompositor || steamMonitor)) return AutoRuntime::SteamVR;
    if (desktopStreamer) return AutoRuntime::VirtualDesktop;
    return AutoRuntime::System;
}

struct RuntimeProcesses
{
    bool server = false, compositor = false, monitor = false, streamer = false;
    std::wstring streamerDirectory;
};

inline RuntimeProcesses DetectRuntimeProcesses()
{
    RuntimeProcesses result;
    DWORD ourSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ourSession)) return result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do
    {
        DWORD session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != ourSession)
            continue;
        if (_wcsicmp(entry.szExeFile, L"vrserver.exe") == 0) result.server = true;
        if (_wcsicmp(entry.szExeFile, L"vrcompositor.exe") == 0) result.compositor = true;
        if (_wcsicmp(entry.szExeFile, L"vrmonitor.exe") == 0) result.monitor = true;
        if (_wcsicmp(entry.szExeFile, L"VirtualDesktop.Streamer.exe") == 0)
        {
            result.streamer = true;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process)
            {
                wchar_t path[32768] = {};
                DWORD size = static_cast<DWORD>(std::size(path));
                if (QueryFullProcessImageNameW(process, 0, path, &size))
                {
                    result.streamerDirectory = path;
                    const auto slash = result.streamerDirectory.find_last_of(L"\\/");
                    if (slash != std::wstring::npos) result.streamerDirectory.resize(slash);
                    else result.streamerDirectory.clear();
                }
                CloseHandle(process);
            }
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return result;
}

inline bool IsFile(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::wstring ReadRegistryPath(HKEY root, const wchar_t* key, const wchar_t* name)
{
    wchar_t value[32768] = {};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
        nullptr, value, &bytes) != ERROR_SUCCESS)
        return {};
    return value;
}

inline bool IsSteamManifest(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    return _wcsicmp(path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1),
        L"steamxr_win64.json") == 0 && IsFile(path);
}

inline std::wstring FindRegisteredRuntime(const wchar_t* filename)
{
    const auto matches = [&](const std::wstring& path)
    {
        const auto slash = path.find_last_of(L"\\/");
        return _wcsicmp(path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1),
            filename) == 0 && IsFile(path);
    };
    const auto active = ReadRegistryPath(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
    if (matches(active))
        return active;
    HKEY runtimes = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0,
        KEY_READ | KEY_WOW64_64KEY, &runtimes) == ERROR_SUCCESS)
    {
        std::wstring found;
        for (DWORD index = 0;; ++index)
        {
            wchar_t name[32768] = {};
            DWORD length = static_cast<DWORD>(std::size(name));
            const auto result = RegEnumValueW(runtimes, index, name, &length,
                nullptr, nullptr, nullptr, nullptr);
            if (result != ERROR_SUCCESS)
                break;
            if (matches(name))
            {
                found = name;
                break;
            }
        }
        RegCloseKey(runtimes);
        if (!found.empty())
            return found;
    }
    return {};
}

inline std::wstring FindSteamRuntime()
{
    const auto registered = FindRegisteredRuntime(L"steamxr_win64.json");
    if (!registered.empty()) return registered;
    const auto steam = ReadRegistryPath(HKEY_CURRENT_USER,
        L"Software\\Valve\\Steam", L"SteamPath");
    const auto candidate = steam + L"\\steamapps\\common\\SteamVR\\steamxr_win64.json";
    return !steam.empty() && IsSteamManifest(candidate) ? candidate : L"";
}

inline bool ConfigureRuntime(const wchar_t* ini, std::wstring& diagnostic, bool& requireSteam)
{
    wchar_t mode[64] = {};
    GetPrivateProfileStringW(L"OpenXR", L"Runtime", L"Auto", mode, 64, ini);
    requireSteam = _wcsicmp(mode, L"SteamVR") == 0;
    std::wstring reason;
    if (_wcsicmp(mode, L"Auto") == 0)
    {
        wchar_t inherited[32768] = {};
        const DWORD length = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", inherited,
            static_cast<DWORD>(std::size(inherited)));
        const auto processes = DetectRuntimeProcesses();
        const auto selected = ChooseAutoRuntime(length != 0, processes.server,
            processes.compositor, processes.monitor, processes.streamer);
        const std::wstring signals = L" [server=" + std::to_wstring(processes.server) +
            L", compositor=" + std::to_wstring(processes.compositor) +
            L", monitor=" + std::to_wstring(processes.monitor) +
            L", streamer=" + std::to_wstring(processes.streamer) + L"]";
        if (selected == AutoRuntime::Inherited)
        {
            diagnostic = L"Auto: honoring inherited XR_RUNTIME_JSON: " +
                std::wstring(length < std::size(inherited) ? inherited : L"(too long)") + signals;
            return true;
        }
        if (selected == AutoRuntime::System)
        {
            diagnostic = L"Auto: no active VR application detected; using system OpenXR runtime" + signals;
            return true;
        }
        if (selected == AutoRuntime::VirtualDesktop)
        {
            auto path = processes.streamerDirectory.empty() ? std::wstring{} :
                processes.streamerDirectory + L"\\OpenXR\\virtualdesktop-openxr.json";
            if (!IsFile(path)) path = FindRegisteredRuntime(L"virtualdesktop-openxr.json");
            if (path.empty() || !IsFile(path))
            {
                diagnostic = L"Auto: Virtual Desktop is running but its OpenXR manifest was not found" + signals;
                return false;
            }
            if (!SetEnvironmentVariableW(L"XR_RUNTIME_JSON", path.c_str()))
            {
                diagnostic = L"Cannot set this process's XR_RUNTIME_JSON.";
                return false;
            }
            diagnostic = L"Auto: Virtual Desktop / VDXR (no active SteamVR): " + path + signals;
            return true;
        }
        requireSteam = true;
        reason = L"Auto: active SteamVR" + signals + L"; ";
    }
    if (!requireSteam)
    {
        if (_wcsicmp(mode, L"System") != 0)
        {
            diagnostic = L"Invalid [OpenXR] Runtime. Use Auto, System or SteamVR.";
            return false;
        }
        diagnostic = L"System: using the active OpenXR runtime (or inherited XR_RUNTIME_JSON).";
        return true;
    }
    wchar_t configured[32768] = {};
    GetPrivateProfileStringW(L"OpenXR", L"SteamVRRuntimePath", L"", configured,
        static_cast<DWORD>(std::size(configured)), ini);
    const std::wstring path = configured[0] ? configured : FindSteamRuntime();
    // Require an absolute x64 manifest path. Do not silently launch VDXR on a typo.
    const bool absolute = path.size() > 2 &&
        ((path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
         (path[0] == L'\\' && path[1] == L'\\'));
    if (!absolute || !IsSteamManifest(path))
    {
        diagnostic = L"SteamVR x64 runtime not found. Set SteamVRRuntimePath to the absolute "
            L"SteamVR/steamxr_win64.json path, or register SteamVR as the OpenXR runtime.";
        return false;
    }
    if (!SetEnvironmentVariableW(L"XR_RUNTIME_JSON", path.c_str()))
    {
        diagnostic = L"Cannot set this process's XR_RUNTIME_JSON.";
        return false;
    }
    diagnostic = reason + L"SteamVR (process-local): " + path;
    return true;
}
} // namespace sh3vr
