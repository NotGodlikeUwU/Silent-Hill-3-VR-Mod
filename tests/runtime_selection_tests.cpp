#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../sh3vr_host64/sh3vr_host64/runtime_selection.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    namespace fs = std::filesystem;
    wchar_t temporary[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temporary);
    // Disposable generated fixtures, never a user's configuration.
    const fs::path directory = fs::path(temporary) /
        (L"sh3vr-runtime-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directory(directory);
    const auto ini = directory / L"test.ini";
    const auto manifest = directory / L"steamxr_win64.json";
    std::ofstream(manifest) << "{}";
    try
    {
        using sh3vr::AutoRuntime;
        for (int bits = 0; bits < 16; ++bits)
        {
            const bool server = (bits & 1) != 0, compositor = (bits & 2) != 0;
            const bool monitor = (bits & 4) != 0, streamer = (bits & 8) != 0;
            const auto expected = server && (compositor || monitor) ? AutoRuntime::SteamVR :
                streamer ? AutoRuntime::VirtualDesktop : AutoRuntime::System;
            Require(sh3vr::ChooseAutoRuntime(false, server, compositor, monitor, streamer) == expected,
                "Automatic process selection failed");
            Require(sh3vr::ChooseAutoRuntime(true, server, compositor, monitor, streamer) == AutoRuntime::Inherited,
                "Explicit launcher runtime must override process detection");
        }
        bool steam = false;
        std::wstring diagnostic;
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", L"inherited-test-runtime");
        Require(sh3vr::ConfigureRuntime(ini.c_str(), diagnostic, steam) && !steam,
            "Default Auto must honor the launcher's inherited runtime");
        wchar_t env[32768] = {};
        GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, 32768);
        Require(wcscmp(env, L"inherited-test-runtime") == 0, "System overwrote inherited selection");

        WritePrivateProfileStringW(L"OpenXR", L"Runtime", L"System", ini.c_str());
        Require(sh3vr::ConfigureRuntime(ini.c_str(), diagnostic, steam) && !steam,
            "Manual System selection failed");
        GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, 32768);
        Require(wcscmp(env, L"inherited-test-runtime") == 0, "Manual System overwrote inherited selection");

        WritePrivateProfileStringW(L"OpenXR", L"Runtime", L"invalid", ini.c_str());
        Require(!sh3vr::ConfigureRuntime(ini.c_str(), diagnostic, steam), "Unknown mode accepted");
        WritePrivateProfileStringW(L"OpenXR", L"Runtime", L"sTeAmVr", ini.c_str());
        WritePrivateProfileStringW(L"OpenXR", L"SteamVRRuntimePath", L"steamxr_win64.json", ini.c_str());
        Require(!sh3vr::ConfigureRuntime(ini.c_str(), diagnostic, steam), "Relative manifest accepted");
        const auto missing = directory / L"missing" / L"steamxr_win64.json";
        WritePrivateProfileStringW(L"OpenXR", L"SteamVRRuntimePath", missing.c_str(), ini.c_str());
        Require(!sh3vr::ConfigureRuntime(ini.c_str(), diagnostic, steam), "Missing manifest accepted");
        WritePrivateProfileStringW(L"OpenXR", L"SteamVRRuntimePath", manifest.c_str(), ini.c_str());
        Require(sh3vr::ConfigureRuntime(ini.c_str(), diagnostic, steam) && steam, "Valid SteamVR selection failed");
        GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, 32768);
        Require(manifest.wstring() == env, "Selected manifest was not forwarded to loader");
        Require(!sh3vr::IsSteamManifest(directory.wstring()), "Directory accepted as runtime");
        Require(!sh3vr::IsSteamManifest(ini.wstring()), "Wrong manifest filename accepted");
        std::cout << "PASS: 32 automatic selection cases, runtime default, inherited selection, invalid mode, relative/missing path, "
            "explicit SteamVR selection, file validation\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    // Remove only the exact generated files, not a recursive directory tree.
    fs::remove(ini);
    fs::remove(manifest);
    fs::remove(directory);
    return 0;
}
