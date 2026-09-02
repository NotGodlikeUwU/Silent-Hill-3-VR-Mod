#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <vector>

#include "shared_frame.h"

using Microsoft::WRL::ComPtr;

namespace
{

std::atomic_bool g_keepRunning = true;
FILE* g_log = nullptr;

void Log(const char* format, ...);

struct HostSettings
{
    std::uint32_t eyeWidth = 0;
    std::uint32_t eyeHeight = 0;
    std::uint32_t eyeSamples = 1;
    bool enableFxaa = false;
    std::uint32_t targetRefreshRate = 90;
    bool requestRefreshRate = true;
    bool enableColorCorrection = true;
    float exposureStops = -1.5f;
    float contrast = 1.15f;
    float saturation = 0.95f;
    float vignetteStrength = 0.30f;
    bool enableGamePostProcess = false;
    float gamePostProcessStrength = 0.80f;
    float gamePostProcessContrast = 0.95f;
    float gamePostProcessExposureStops = 0.18f;
    // Drawn by the D3D11 OpenXR compositor, after the game image for each eye.
    // This deliberately never touches the legacy D3D8 state used by hands.
    bool controllerOrientationDebug = false;
};

HostSettings g_settings = {};

float ReadIniFloat(const wchar_t* path, const wchar_t* section,
    const wchar_t* key, float defaultValue, float minimum, float maximum)
{
    wchar_t defaultText[32] = {};
    swprintf_s(defaultText, L"%.4f", defaultValue);
    wchar_t valueText[64] = {};
    GetPrivateProfileStringW(section, key, defaultText, valueText,
        static_cast<DWORD>(std::size(valueText)), path);
    wchar_t* end = nullptr;
    const float value = wcstof(valueText, &end);
    if (end == valueText || !std::isfinite(value))
    {
        Log("Ignoring invalid floating-point value in sh3vr.ini");
        return defaultValue;
    }
    return std::clamp(value, minimum, maximum);
}

bool GetHostDirectory(wchar_t* path, std::size_t capacity)
{
    if (!path || capacity == 0 || GetModuleFileNameW(nullptr, path,
        static_cast<DWORD>(capacity)) == 0)
    {
        return false;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return false;
    slash[1] = L'\0';
    return true;
}

void LoadSettings()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetHostDirectory(path, std::size(path)))
        return;
    wcscat_s(path, L"sh3vr.ini");

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    {
        WritePrivateProfileStringW(L"OpenXR", L"EyeWidth", L"0", path);
        WritePrivateProfileStringW(L"OpenXR", L"EyeHeight", L"0", path);
    }

    const UINT width = GetPrivateProfileIntW(L"OpenXR", L"EyeWidth", 0, path);
    const UINT height = GetPrivateProfileIntW(L"OpenXR", L"EyeHeight", 0, path);
    const UINT samples = GetPrivateProfileIntW(L"OpenXR", L"MSAASamples", 1,
        path);
    const UINT enableFxaa = GetPrivateProfileIntW(L"OpenXR", L"EnableFXAA", 0,
        path);
    const UINT targetRefreshRate = GetPrivateProfileIntW(L"OpenXR",
        L"TargetRefreshRate", 90, path);
    const UINT requestRefreshRate = GetPrivateProfileIntW(L"OpenXR",
        L"RequestRefreshRate", 1, path);
    const UINT enableColorCorrection = GetPrivateProfileIntW(L"Image",
        L"EnableColorCorrection", 1, path);
    const UINT enableGamePostProcess = GetPrivateProfileIntW(L"Image",
        L"EnableGamePostProcess", 0, path);
    const UINT controllerOrientationDebug = GetPrivateProfileIntW(L"Debug",
        L"ControllerOrientationOverlay", 0, path);
    if ((width == 0 && height == 0) ||
        (width >= 64 && width <= 4096 && height >= 64 && height <= 4096))
    {
        g_settings.eyeWidth = width;
        g_settings.eyeHeight = height;
    }
    else
    {
        Log("Ignoring invalid sh3vr.ini EyeWidth/EyeHeight values: %u x %u",
            width, height);
    }
    if (samples == 1 || samples == 2 || samples == 4)
        g_settings.eyeSamples = samples;
    else
        Log("Ignoring invalid sh3vr.ini MSAASamples value: %u", samples);
    g_settings.enableFxaa = enableFxaa != 0;
    if (targetRefreshRate == 0 ||
        (targetRefreshRate >= 60 && targetRefreshRate <= 240))
    {
        g_settings.targetRefreshRate = targetRefreshRate;
    }
    else
    {
        Log("Ignoring invalid sh3vr.ini TargetRefreshRate value: %u",
            targetRefreshRate);
    }
    g_settings.requestRefreshRate = requestRefreshRate != 0;
    g_settings.enableColorCorrection = enableColorCorrection != 0;
    g_settings.enableGamePostProcess = enableGamePostProcess != 0;
    g_settings.controllerOrientationDebug = controllerOrientationDebug != 0;
    g_settings.exposureStops = ReadIniFloat(path, L"Image", L"ExposureStops",
        -1.5f, -4.0f, 2.0f);
    g_settings.contrast = ReadIniFloat(path, L"Image", L"Contrast",
        1.15f, 0.5f, 2.0f);
    g_settings.saturation = ReadIniFloat(path, L"Image", L"Saturation",
        0.95f, 0.0f, 2.0f);
    g_settings.vignetteStrength = ReadIniFloat(path, L"Image",
        L"VignetteStrength", 0.30f, 0.0f, 1.0f);
    g_settings.gamePostProcessStrength = ReadIniFloat(path, L"Image",
        L"GamePostProcessStrength", 1.00f, 0.0f, 1.0f);
    g_settings.gamePostProcessContrast = ReadIniFloat(path, L"Image",
        L"GamePostProcessContrast", 0.88f, 0.5f, 1.5f);
    g_settings.gamePostProcessExposureStops = ReadIniFloat(path, L"Image",
        L"GamePostProcessExposureStops", 0.00f, -1.0f, 1.0f);

    char narrowPath[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrowPath,
        static_cast<int>(std::size(narrowPath)), nullptr, nullptr);
    Log("Loaded settings from %s: EyeWidth=%u EyeHeight=%u MSAASamples=%u "
        "EnableFXAA=%u TargetRefreshRate=%u RequestRefreshRate=%u",
        narrowPath, g_settings.eyeWidth,
        g_settings.eyeHeight, g_settings.eyeSamples,
        g_settings.enableFxaa ? 1u : 0u, g_settings.targetRefreshRate,
        g_settings.requestRefreshRate ? 1u : 0u);
    Log("Image correction: Enabled=%u ExposureStops=%.3f Contrast=%.3f "
        "Saturation=%.3f VignetteStrength=%.3f",
        g_settings.enableColorCorrection ? 1u : 0u,
        g_settings.exposureStops, g_settings.contrast,
        g_settings.saturation, g_settings.vignetteStrength);
    Log("Dynamic game post-process transfer: Enabled=%u Strength=%.3f "
        "Contrast=%.3f ExposureStops=%.3f",
        g_settings.enableGamePostProcess ? 1u : 0u,
        g_settings.gamePostProcessStrength,
        g_settings.gamePostProcessContrast,
        g_settings.gamePostProcessExposureStops);
    Log("Debug: ControllerOrientationOverlay=%u (host compositor)",
        g_settings.controllerOrientationDebug ? 1u : 0u);
}

void Log(const char* format, ...)
{
    char message[2048] = {};
    va_list arguments;
    va_start(arguments, format);
    vsprintf_s(message, format, arguments);
    va_end(arguments);

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    char line[2300] = {};
    sprintf_s(line, "[%02u:%02u:%02u.%03u] %s\n", time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, message);

    fputs(line, stdout);
    fflush(stdout);
    OutputDebugStringA(line);
    if (g_log)
    {
        fputs(line, g_log);
        fflush(g_log);
    }
}

bool CheckXr(XrResult result, const char* operation)
{
    if (XR_SUCCEEDED(result))
        return true;
    Log("%s failed with XrResult %d", operation, static_cast<int>(result));
    return false;
}

XrQuaternionf MultiplyQuaternions(const XrQuaternionf& first,
    const XrQuaternionf& second)
{
    return {
        first.w * second.x + first.x * second.w +
            first.y * second.z - first.z * second.y,
        first.w * second.y - first.x * second.z +
            first.y * second.w + first.z * second.x,
        first.w * second.z + first.x * second.y -
            first.y * second.x + first.z * second.w,
        first.w * second.w - first.x * second.x -
            first.y * second.y - first.z * second.z
    };
}

XrVector3f RotateVector(const XrQuaternionf& rotation,
    const XrVector3f& vector)
{
    const XrQuaternionf vectorQuaternion{
        vector.x, vector.y, vector.z, 0.0f };
    const XrQuaternionf inverse{
        -rotation.x, -rotation.y, -rotation.z, rotation.w };
    const XrQuaternionf rotated = MultiplyQuaternions(
        MultiplyQuaternions(rotation, vectorQuaternion), inverse);
    return { rotated.x, rotated.y, rotated.z };
}

BOOL WINAPI ConsoleHandler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT)
    {
        g_keepRunning = false;
        return TRUE;
    }
    return FALSE;
}

class SharedFrameConsumer
{
public:
    ~SharedFrameConsumer()
    {
        Close();
    }

    bool TryConnect()
    {
        if (m_header)
            return true;

        m_section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SH3VR_SECTION_NAME);
        if (!m_section)
            return false;

        m_header = static_cast<Sh3VrFrameHeader*>(
            MapViewOfFile(m_section, FILE_MAP_ALL_ACCESS, 0, 0, SH3VR_SECTION_BYTES));
        if (!m_header)
        {
            CloseHandle(m_section);
            m_section = nullptr;
            return false;
        }

        m_event = OpenEventW(SYNCHRONIZE, FALSE, SH3VR_EVENT_NAME);
        m_slots = reinterpret_cast<const std::uint8_t*>(m_header) + SH3VR_HEADER_BYTES;
        if (!ValidateHeader())
        {
            Log("Shared frame header is incompatible");
            Close();
            return false;
        }

        Log("Connected to frame producer PID %u", m_header->producerPid);
        return true;
    }

    bool ReadLatest(std::array<std::vector<std::uint8_t>, 2>& pixels,
        Sh3VrFrameHeader& metadata, bool copyPixels)
    {
        if (!m_header || !ValidateHeader() || m_header->producerAlive == 0)
            return false;

        const std::int32_t frameBefore = m_header->publishedFrame;
        if (frameBefore <= 0 || frameBefore == m_lastFrame)
            return false;

        const std::int32_t stereoSequenceBefore =
            m_header->stereoPairSequence;
        MemoryBarrier();
        std::memcpy(&metadata, m_header, sizeof(metadata));
        if (metadata.width == 0 || metadata.height == 0 ||
            metadata.width > SH3VR_MAX_WIDTH || metadata.height > SH3VR_MAX_HEIGHT ||
            metadata.pitch < metadata.width * SH3VR_BYTES_PER_PX ||
            static_cast<std::uint64_t>(metadata.pitch) * metadata.height > SH3VR_SLOT_BYTES)
        {
            Log("Rejected invalid frame metadata: %ux%u pitch %u",
                metadata.width, metadata.height, metadata.pitch);
            return false;
        }

        const bool synchronizedStereo =
            metadata.renderMode == SH3VR_RENDER_IMMERSIVE_STEREO;
        // A zero native-eye sequence means the proxy is using the complete
        // CPU stereo pair after a replay overflow.  The D3D12 backbuffer can
        // remain valid at the same time, so force a CPU copy for this frame
        // even when the GPU shared-source path is enabled.
        const bool cpuStereoFallback = synchronizedStereo &&
            metadata.d3d12EyeTextureFrameSequence == 0;
        const bool shouldCopyPixels = copyPixels || cpuStereoFallback;
        if (shouldCopyPixels)
        {
            const std::size_t byteCount =
                static_cast<std::size_t>(metadata.pitch) * metadata.height;
            if (synchronizedStereo)
            {
                if ((stereoSequenceBefore & 1) != 0 ||
                    metadata.stereoReadyMask != 3)
                {
                    return false;
                }
                for (std::size_t eye = 0; eye < pixels.size(); ++eye)
                {
                    pixels[eye].resize(byteCount);
                    std::memcpy(pixels[eye].data(),
                        m_slots + eye * SH3VR_SLOT_BYTES, byteCount);
                }
            }
            else
            {
                const std::uint32_t slot =
                    static_cast<std::uint32_t>(frameBefore) % SH3VR_SLOT_COUNT;
                pixels[0].resize(byteCount);
                pixels[1].clear();
                std::memcpy(pixels[0].data(),
                    m_slots + static_cast<std::size_t>(slot) * SH3VR_SLOT_BYTES,
                    byteCount);
            }
        }
        else
        {
            pixels[1].clear();
        }
        MemoryBarrier();
        if (m_header->publishedFrame != frameBefore ||
            (synchronizedStereo &&
                m_header->stereoPairSequence != stereoSequenceBefore))
            return false;

        m_lastFrame = frameBefore;
        return true;
    }

    void Wait(DWORD milliseconds) const
    {
        if (m_event)
            WaitForSingleObject(m_event, milliseconds);
        else
            Sleep(milliseconds);
    }

    void PublishRequestedEyeResolution(std::uint32_t width,
        std::uint32_t height, std::uint32_t sampleCount)
    {
        if (!m_header || width == 0 || height == 0 || sampleCount == 0)
            return;
        if (m_header->requestedEyeWidth == width &&
            m_header->requestedEyeHeight == height &&
            m_header->requestedEyeSampleCount == sampleCount)
        {
            return;
        }

        m_header->requestedEyeWidth = width;
        m_header->requestedEyeHeight = height;
        m_header->requestedEyeSampleCount = sampleCount;
        MemoryBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &m_header->requestedEyeGeneration));
        Log("Published requested eye render size %ux%u, samples %u",
            width, height, sampleCount);
    }

    void PublishHeadPose(XrTime predictedDisplayTime, const XrSpaceLocation& location)
    {
        if (!m_header)
            return;

        Sh3VrHeadPose pose = {};
        pose.predictedDisplayTime = predictedDisplayTime;
        if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
            pose.flags |= SH3VR_POSE_ORIENTATION_VALID;
        if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0)
            pose.flags |= SH3VR_POSE_POSITION_VALID;
        if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0)
            pose.flags |= SH3VR_POSE_ORIENTATION_TRACKED;
        if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0)
            pose.flags |= SH3VR_POSE_POSITION_TRACKED;

        pose.position[0] = location.pose.position.x;
        pose.position[1] = location.pose.position.y;
        pose.position[2] = location.pose.position.z;
        pose.orientation[0] = location.pose.orientation.x;
        pose.orientation[1] = location.pose.orientation.y;
        pose.orientation[2] = location.pose.orientation.z;
        pose.orientation[3] = location.pose.orientation.w;

        // Odd sequence means a write is in progress; even means stable data.
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &m_header->headPoseSequence));
        MemoryBarrier();
        std::memcpy(&m_header->headPose, &pose, sizeof(pose));
        MemoryBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &m_header->headPoseSequence));
    }

    void PublishControllerState(const Sh3VrControllerState& state)
    {
        if (!m_header)
            return;

        // Odd sequence means a write is in progress; even means stable data.
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &m_header->controllerStateSequence));
        MemoryBarrier();
        std::memcpy(&m_header->controllerState, &state, sizeof(state));
        MemoryBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &m_header->controllerStateSequence));
    }

    void PublishProjectionUvRects(
        const std::array<std::array<float, 4>, 2>& eyeRects)
    {
        if (!m_header)
            return;

        auto* state = reinterpret_cast<Sh3VrProjectionUvState*>(
            m_header->reserved + SH3VR_PROJECTION_UV_RESERVED_OFFSET);
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->sequence));
        MemoryBarrier();
        state->magic = SH3VR_PROJECTION_UV_MAGIC;
        std::memcpy(state->eyeRect, eyeRects.data(), sizeof(state->eyeRect));
        MemoryBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->sequence));
    }

private:
    bool ValidateHeader() const
    {
        return m_header && m_header->magic == SH3VR_MAGIC &&
            m_header->version == SH3VR_VERSION &&
            m_header->headerBytes == SH3VR_HEADER_BYTES &&
            m_header->slotCount == SH3VR_SLOT_COUNT &&
            m_header->slotBytes == SH3VR_SLOT_BYTES &&
            m_header->pixelLayout == SH3VR_PIXEL_BGRA8;
    }

    void Close()
    {
        if (m_header)
            UnmapViewOfFile(m_header);
        if (m_event)
            CloseHandle(m_event);
        if (m_section)
            CloseHandle(m_section);
        m_header = nullptr;
        m_slots = nullptr;
        m_event = nullptr;
        m_section = nullptr;
    }

    HANDLE m_section = nullptr;
    HANDLE m_event = nullptr;
    Sh3VrFrameHeader* m_header = nullptr;
    const std::uint8_t* m_slots = nullptr;
    std::int32_t m_lastFrame = 0;
};

struct EyeSwapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ComPtr<ID3D11RenderTargetView>> renderTargets;
};

class OpenXrHost
{
public:
    ~OpenXrHost()
    {
        Shutdown();
    }

    bool Initialize()
    {
        if (!CreateInstance() || !CreateSystem())
            return false;

        if (!CreateGraphicsDevice() || !CreateSession() || !CreateReferenceSpace())
        {
            m_retryAllowed = false;
            return false;
        }
        if (!InitializeControllerInput())
        {
            Log("OpenXR controller input is unavailable; keyboard input remains active");
            ShutdownControllerInput();
        }
        if (!CreateSwapchains() || !CreateBlitPipeline())
        {
            m_retryAllowed = false;
            return false;
        }
        Log("OpenXR host initialized");
        return true;
    }

    bool RetryAllowed() const
    {
        return m_retryAllowed;
    }

    std::uint32_t EyeWidth() const
    {
        return m_eyes[0].width;
    }

    std::uint32_t EyeHeight() const
    {
        return m_eyes[0].height;
    }

    bool PumpFrame(SharedFrameConsumer& consumer)
    {
        PollEvents();
        if (m_exitRequested)
            return false;
        if (!m_sessionRunning)
        {
            consumer.Wait(10);
            return true;
        }

        XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frameState{ XR_TYPE_FRAME_STATE };
        if (!CheckXr(xrWaitFrame(m_session, &waitInfo, &frameState), "xrWaitFrame"))
            return false;
        XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
        if (!CheckXr(xrBeginFrame(m_session, &beginInfo), "xrBeginFrame"))
            return false;

        SyncControllerInput(consumer, frameState.predictedDisplayTime);

        XrSpaceLocation headLocation{ XR_TYPE_SPACE_LOCATION };
        const XrResult headResult = xrLocateSpace(m_viewSpace, m_appSpace,
            frameState.predictedDisplayTime, &headLocation);
        if (XR_SUCCEEDED(headResult))
        {
            consumer.PublishHeadPose(frameState.predictedDisplayTime, headLocation);
        }
        else if (!m_headLocateFailureLogged)
        {
            Log("xrLocateSpace(VIEW) failed with XrResult %d",
                static_cast<int>(headResult));
            m_headLocateFailureLogged = true;
        }

        bool submitLayer = false;
        bool submitProjection = false;
        std::size_t activeSourceBase = 0;
        bool nativeEyeSourcesReady = false;
        std::array<XrView, 2> locatedViews = {
            XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW }
        };
        if (frameState.shouldRender && m_shouldRender)
        {
            UploadLatestFrame(consumer);
            activeSourceBase =
                static_cast<std::size_t>(m_d3d12EyeActiveSet) * 2;
            nativeEyeSourcesReady =
                m_d3d12EyeFrameSequence != 0 &&
                activeSourceBase + 1 < m_d3d12EyeViews.size() &&
                m_d3d12EyeViews[activeSourceBase] &&
                m_d3d12EyeViews[activeSourceBase + 1];
            if (m_renderMode != SH3VR_RENDER_CINEMA)
            {
                XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
                locateInfo.viewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locateInfo.displayTime = frameState.predictedDisplayTime;
                locateInfo.space = m_appSpace;
                XrViewState viewState{ XR_TYPE_VIEW_STATE };
                std::uint32_t viewCount = 0;
                const XrResult locateResult = xrLocateViews(m_session, &locateInfo,
                    &viewState, static_cast<std::uint32_t>(locatedViews.size()),
                    &viewCount, locatedViews.data());
                if (XR_SUCCEEDED(locateResult) && viewCount == locatedViews.size())
                {
                    submitProjection = true;
                    std::array<std::array<float, 4>, 2> projectionUvRects = {};
                    for (std::size_t eye = 0; eye < locatedViews.size(); ++eye)
                    {
                        const XrFovf guardBandFov = BuildGuardBandFov(
                            locatedViews[eye].fov, nativeEyeSourcesReady);
                        projectionUvRects[eye] = BuildProjectionUvRect(
                            guardBandFov, nativeEyeSourcesReady);
                        if (!RenderEye(eye, projectionUvRects[eye]))
                        {
                            submitProjection = false;
                            break;
                        }
                    }
                    if (submitProjection && nativeEyeSourcesReady)
                        consumer.PublishProjectionUvRects(projectionUvRects);
                    submitLayer = submitProjection;
                }
                else
                {
                    Log("xrLocateViews failed or returned %u views: XrResult %d",
                        viewCount, static_cast<int>(locateResult));
                }
            }
            else
            {
                submitLayer = RenderEye(0, m_uvRect);
            }
        }

        XrCompositionLayerQuad quadLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        quadLayer.space = m_viewSpace;
        quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quadLayer.subImage.swapchain = m_eyes[0].handle;
        quadLayer.subImage.imageRect.offset = { 0, 0 };
        quadLayer.subImage.imageRect.extent = {
            static_cast<std::int32_t>(m_eyes[0].width),
            static_cast<std::int32_t>(m_eyes[0].height)
        };
        quadLayer.pose.orientation.w = 1.0f;
        quadLayer.pose.position.z = -2.0f;
        quadLayer.size.height = 1.4f;
        quadLayer.size.width = quadLayer.size.height * m_frameAspect;

        std::array<XrCompositionLayerProjectionView, 2> projectionViews = {
            XrCompositionLayerProjectionView{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
            XrCompositionLayerProjectionView{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
        };
        for (std::size_t eye = 0; eye < projectionViews.size(); ++eye)
        {
            if (m_haveFrameRenderPose)
            {
                const XrPosef renderHeadPose{
                    {
                        m_frameRenderPose.orientation[0],
                        m_frameRenderPose.orientation[1],
                        m_frameRenderPose.orientation[2],
                        m_frameRenderPose.orientation[3]
                    },
                    {
                        m_frameRenderPose.position[0],
                        m_frameRenderPose.position[1],
                        m_frameRenderPose.position[2]
                    }
                };
                if (nativeEyeSourcesReady)
                {
                    // The game hook renders a parallel 64 mm stereo pair: the
                    // two cameras differ only by +/-32 mm on head-local X.
                    // Do not describe those textures with the runtime's eye
                    // poses, which can contain small Y/Z offsets and per-eye
                    // rotations. That pose mismatch is negligible at a
                    // distance but creates strong vertical disparity for a
                    // hand, weapon, or wall close to the lower lens area.
                    constexpr float renderedHalfIpdMeters = 0.032f;
                    const float eyeSign = eye == 0 ? -1.0f : 1.0f;
                    const XrVector3f localRenderedEyeOffset{
                        eyeSign * renderedHalfIpdMeters, 0.0f, 0.0f
                    };
                    const XrVector3f renderedEyeOffset = RotateVector(
                        renderHeadPose.orientation, localRenderedEyeOffset);
                    projectionViews[eye].pose.position = {
                        renderHeadPose.position.x + renderedEyeOffset.x,
                        renderHeadPose.position.y + renderedEyeOffset.y,
                        renderHeadPose.position.z + renderedEyeOffset.z
                    };
                    // Preserve the eye-relative orientation that belongs to
                    // the runtime's asymmetric FOV. Removing it makes the
                    // center look correct while the headset is level, but
                    // after a menu transition and a larger head rotation the
                    // two images diverge and distant geometry leaves the
                    // submitted frustum. Only the positional Y/Z mismatch is
                    // absent from the game's parallel camera pair.
                    const XrQuaternionf inverseCurrentHead{
                        -headLocation.pose.orientation.x,
                        -headLocation.pose.orientation.y,
                        -headLocation.pose.orientation.z,
                        headLocation.pose.orientation.w
                    };
                    const XrQuaternionf eyeRelativeOrientation =
                        MultiplyQuaternions(inverseCurrentHead,
                            locatedViews[eye].pose.orientation);
                    projectionViews[eye].pose.orientation =
                        MultiplyQuaternions(renderHeadPose.orientation,
                            eyeRelativeOrientation);
                    if (!m_nativeEyePoseMatchLogged && eye == 1)
                    {
                        m_nativeEyePoseMatchLogged = true;
                        Log("Native eye submission uses the rendered parallel "
                            "64 mm camera positions and runtime FOV-relative "
                            "eye orientations");
                    }
                }
                else
                {
                    const XrVector3f currentEyeOffset{
                        locatedViews[eye].pose.position.x -
                            headLocation.pose.position.x,
                        locatedViews[eye].pose.position.y -
                            headLocation.pose.position.y,
                        locatedViews[eye].pose.position.z -
                            headLocation.pose.position.z
                    };
                    const XrQuaternionf inverseCurrentHead{
                        -headLocation.pose.orientation.x,
                        -headLocation.pose.orientation.y,
                        -headLocation.pose.orientation.z,
                        headLocation.pose.orientation.w
                    };
                    const XrVector3f localEyeOffset = RotateVector(
                        inverseCurrentHead, currentEyeOffset);
                    const XrVector3f renderEyeOffset = RotateVector(
                        renderHeadPose.orientation, localEyeOffset);
                    projectionViews[eye].pose.position = {
                        renderHeadPose.position.x + renderEyeOffset.x,
                        renderHeadPose.position.y + renderEyeOffset.y,
                        renderHeadPose.position.z + renderEyeOffset.z
                    };

                    const XrQuaternionf eyeRelativeOrientation =
                        MultiplyQuaternions(inverseCurrentHead,
                            locatedViews[eye].pose.orientation);
                    projectionViews[eye].pose.orientation =
                        MultiplyQuaternions(renderHeadPose.orientation,
                            eyeRelativeOrientation);
                }
            }
            else
            {
                projectionViews[eye].pose = locatedViews[eye].pose;
            }
            projectionViews[eye].fov = BuildGuardBandFov(
                locatedViews[eye].fov, nativeEyeSourcesReady);
            projectionViews[eye].subImage.swapchain = m_eyes[eye].handle;
            projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
            projectionViews[eye].subImage.imageRect.extent = {
                static_cast<std::int32_t>(m_eyes[eye].width),
                static_cast<std::int32_t>(m_eyes[eye].height)
            };
        }

        XrCompositionLayerProjection projectionLayer{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        projectionLayer.space = m_appSpace;
        projectionLayer.viewCount = static_cast<std::uint32_t>(projectionViews.size());
        projectionLayer.views = projectionViews.data();

        const XrCompositionLayerBaseHeader* layers[] = {
            submitProjection
                ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                    &projectionLayer)
                : reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer)
        };

        XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = submitLayer ? 1u : 0u;
        endInfo.layers = submitLayer ? layers : nullptr;
        return CheckXr(xrEndFrame(m_session, &endInfo), "xrEndFrame");
    }

private:
    bool HasInstanceExtension(const char* extensionName)
    {
        std::uint32_t extensionCount = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0,
            &extensionCount, nullptr)))
        {
            return false;
        }

        std::vector<XrExtensionProperties> properties(extensionCount);
        for (XrExtensionProperties& property : properties)
            property.type = XR_TYPE_EXTENSION_PROPERTIES;

        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr,
            extensionCount, &extensionCount, properties.data())))
        {
            return false;
        }

        for (const XrExtensionProperties& property : properties)
        {
            if (strcmp(property.extensionName, extensionName) == 0)
                return true;
        }
        return false;
    }

    bool CreateInstance()
    {
        const char* extensions[2] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
            XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME };
        std::uint32_t extensionCount = 1;
        m_refreshRateExtensionEnabled = g_settings.requestRefreshRate &&
            g_settings.targetRefreshRate != 0 &&
            HasInstanceExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        if (m_refreshRateExtensionEnabled)
        {
            extensionCount++;
            Log("OpenXR extension %s is available; 90 Hz request is enabled",
                XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        }
        else if (g_settings.requestRefreshRate &&
            g_settings.targetRefreshRate != 0)
        {
            Log("OpenXR extension %s is unavailable; runtime refresh rate "
                "will not be changed", XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        }
        XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy_s(createInfo.applicationInfo.applicationName, "Silent Hill 3 VR");
        createInfo.applicationInfo.applicationVersion = 1;
        strcpy_s(createInfo.applicationInfo.engineName, "SH3VR");
        createInfo.applicationInfo.engineVersion = 1;
        // VDXR 1.0.x rejects applications that request the OpenXR 1.1 API.
        // This host only uses OpenXR 1.0 core functions and extensions.
        createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        createInfo.enabledExtensionCount = extensionCount;
        createInfo.enabledExtensionNames = extensions;
        return CheckXr(xrCreateInstance(&createInfo, &m_instance), "xrCreateInstance");
    }

    bool CreateSystem()
    {
        XrSystemGetInfo getInfo{ XR_TYPE_SYSTEM_GET_INFO };
        getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        return CheckXr(xrGetSystem(m_instance, &getInfo, &m_systemId), "xrGetSystem");
    }

    bool CreateGraphicsDevice()
    {
        PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
        if (!CheckXr(xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
            "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)") || !getRequirements)
        {
            return false;
        }

        XrGraphicsRequirementsD3D11KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR
        };
        if (!CheckXr(getRequirements(m_instance, m_systemId, &requirements),
            "xrGetD3D11GraphicsRequirementsKHR"))
        {
            return false;
        }

        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            Log("CreateDXGIFactory1 failed");
            return false;
        }

        ComPtr<IDXGIAdapter1> selectedAdapter;
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 description = {};
            adapter->GetDesc1(&description);
            if (description.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
                description.AdapterLuid.LowPart == requirements.adapterLuid.LowPart)
            {
                selectedAdapter = adapter;
                Log("Using OpenXR adapter: %ls", description.Description);
                break;
            }
        }
        if (!selectedAdapter)
        {
            Log("No DXGI adapter matches the OpenXR runtime LUID");
            return false;
        }

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_9_1;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT result = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, flags, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &m_device, &createdLevel, &m_context);
#if defined(_DEBUG)
        if (result == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            result = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                nullptr, flags, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                &m_device, &createdLevel, &m_context);
        }
#endif
        if (FAILED(result) || createdLevel < requirements.minFeatureLevel)
        {
            Log("D3D11CreateDevice failed or returned an insufficient feature level");
            return false;
        }
        return true;
    }

    bool CreateSession()
    {
        XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
        binding.device = m_device.Get();
        XrSessionCreateInfo createInfo{ XR_TYPE_SESSION_CREATE_INFO };
        createInfo.next = &binding;
        createInfo.systemId = m_systemId;
        if (!CheckXr(xrCreateSession(m_instance, &createInfo, &m_session),
            "xrCreateSession"))
        {
            return false;
        }
        ConfigureDisplayRefreshRate();
        return true;
    }

    void ConfigureDisplayRefreshRate()
    {
        if (!m_refreshRateExtensionEnabled ||
            !g_settings.requestRefreshRate || g_settings.targetRefreshRate == 0)
        {
            return;
        }

        if (!CheckXr(xrGetInstanceProcAddr(m_instance,
            "xrEnumerateDisplayRefreshRatesFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&m_enumerateRefreshRates)),
            "xrGetInstanceProcAddr(xrEnumerateDisplayRefreshRatesFB)"))
        {
            return;
        }
        if (!CheckXr(xrGetInstanceProcAddr(m_instance,
            "xrGetDisplayRefreshRateFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&m_getRefreshRate)),
            "xrGetInstanceProcAddr(xrGetDisplayRefreshRateFB)"))
        {
            return;
        }
        if (!CheckXr(xrGetInstanceProcAddr(m_instance,
            "xrRequestDisplayRefreshRateFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&m_requestRefreshRate)),
            "xrGetInstanceProcAddr(xrRequestDisplayRefreshRateFB)"))
        {
            return;
        }

        std::uint32_t rateCount = 0;
        if (XR_FAILED(m_enumerateRefreshRates(m_session, 0, &rateCount,
            nullptr)) || rateCount == 0)
        {
            Log("OpenXR refresh-rate enumeration returned no rates");
            return;
        }

        std::vector<float> rates(rateCount, 0.0f);
        if (XR_FAILED(m_enumerateRefreshRates(m_session, rateCount,
            &rateCount, rates.data())))
        {
            Log("OpenXR refresh-rate enumeration failed");
            return;
        }

        bool targetSupported = false;
        for (float rate : rates)
        {
            Log("OpenXR display refresh rate available: %.2f Hz", rate);
            if (std::fabs(rate - static_cast<float>(g_settings.targetRefreshRate)) <
                0.1f)
            {
                targetSupported = true;
            }
        }
        if (!targetSupported)
        {
            Log("OpenXR target refresh rate %u Hz is not supported by the "
                "runtime", g_settings.targetRefreshRate);
            return;
        }

        const XrResult requestResult = m_requestRefreshRate(m_session,
            static_cast<float>(g_settings.targetRefreshRate));
        if (XR_FAILED(requestResult))
        {
            Log("OpenXR display refresh-rate request for %u Hz failed with "
                "XrResult %d", g_settings.targetRefreshRate,
                static_cast<int>(requestResult));
            return;
        }

        float currentRate = 0.0f;
        const XrResult currentResult = m_getRefreshRate(m_session, &currentRate);
        if (XR_SUCCEEDED(currentResult))
        {
            Log("OpenXR display refresh-rate request accepted: current rate "
                "%.2f Hz", currentRate);
        }
        else
        {
            Log("OpenXR display refresh-rate request accepted; current rate "
                "query returned XrResult %d", static_cast<int>(currentResult));
        }
    }

    bool CreateInputAction(const char* name, const char* localizedName,
        XrActionType type, const XrPath* subactionPaths,
        std::uint32_t subactionPathCount, XrAction* action)
    {
        XrActionCreateInfo createInfo{ XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(createInfo.actionName, name);
        strcpy_s(createInfo.localizedActionName, localizedName);
        createInfo.actionType = type;
        createInfo.countSubactionPaths = subactionPathCount;
        createInfo.subactionPaths = subactionPaths;
        return CheckXr(xrCreateAction(m_controllerActionSet, &createInfo, action),
            name);
    }

    bool InitializeControllerInput()
    {
        XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
        strcpy_s(setInfo.actionSetName, "gameplay");
        strcpy_s(setInfo.localizedActionSetName, "Silent Hill 3 gameplay");
        setInfo.priority = 0;
        if (!CheckXr(xrCreateActionSet(m_instance, &setInfo,
            &m_controllerActionSet), "xrCreateActionSet(gameplay)"))
        {
            return false;
        }

        if (!CheckXr(xrStringToPath(m_instance, "/user/hand/left",
            &m_handPaths[0]), "xrStringToPath(left hand)") ||
            !CheckXr(xrStringToPath(m_instance, "/user/hand/right",
                &m_handPaths[1]), "xrStringToPath(right hand)"))
        {
            return false;
        }

        if (!CreateInputAction("thumbstick", "Thumbstick",
            XR_ACTION_TYPE_VECTOR2F_INPUT, m_handPaths.data(), 2,
            &m_thumbstickAction) ||
            !CreateInputAction("trigger", "Trigger",
                XR_ACTION_TYPE_FLOAT_INPUT, m_handPaths.data(), 2,
                &m_triggerAction) ||
            !CreateInputAction("squeeze", "Grip",
                XR_ACTION_TYPE_FLOAT_INPUT, m_handPaths.data(), 2,
                &m_squeezeAction) ||
            !CreateInputAction("stick_click", "Thumbstick click",
                XR_ACTION_TYPE_BOOLEAN_INPUT, m_handPaths.data(), 2,
                &m_stickClickAction) ||
            !CreateInputAction("grip_pose", "Grip pose",
                XR_ACTION_TYPE_POSE_INPUT, m_handPaths.data(), 2,
                &m_gripPoseAction) ||
            !CreateInputAction("aim_pose", "Aim pose",
                XR_ACTION_TYPE_POSE_INPUT, m_handPaths.data(), 2,
                &m_aimPoseAction) ||
            !CreateInputAction("button_a", "A button",
                XR_ACTION_TYPE_BOOLEAN_INPUT, &m_handPaths[1], 1,
                &m_buttonAAction) ||
            !CreateInputAction("button_b", "B button",
                XR_ACTION_TYPE_BOOLEAN_INPUT, &m_handPaths[1], 1,
                &m_buttonBAction) ||
            !CreateInputAction("button_x", "X button",
                XR_ACTION_TYPE_BOOLEAN_INPUT, &m_handPaths[0], 1,
                &m_buttonXAction) ||
            !CreateInputAction("button_y", "Y button",
                XR_ACTION_TYPE_BOOLEAN_INPUT, &m_handPaths[0], 1,
                &m_buttonYAction) ||
            !CreateInputAction("menu", "Menu button",
                XR_ACTION_TYPE_BOOLEAN_INPUT, &m_handPaths[0], 1,
                &m_menuAction))
        {
            return false;
        }

        XrPath touchProfile = XR_NULL_PATH;
        if (!CheckXr(xrStringToPath(m_instance,
            "/interaction_profiles/oculus/touch_controller", &touchProfile),
            "xrStringToPath(Oculus Touch profile)"))
        {
            return false;
        }

        std::vector<XrActionSuggestedBinding> bindings;
        const auto bind = [&](XrAction action, const char* path)
        {
            XrPath bindingPath = XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(m_instance, path, &bindingPath)))
                bindings.push_back({ action, bindingPath });
        };

        bind(m_thumbstickAction, "/user/hand/left/input/thumbstick");
        bind(m_thumbstickAction, "/user/hand/right/input/thumbstick");
        bind(m_triggerAction, "/user/hand/left/input/trigger/value");
        bind(m_triggerAction, "/user/hand/right/input/trigger/value");
        bind(m_squeezeAction, "/user/hand/left/input/squeeze/value");
        bind(m_squeezeAction, "/user/hand/right/input/squeeze/value");
        bind(m_stickClickAction, "/user/hand/left/input/thumbstick/click");
        bind(m_stickClickAction, "/user/hand/right/input/thumbstick/click");
        bind(m_buttonAAction, "/user/hand/right/input/a/click");
        bind(m_buttonBAction, "/user/hand/right/input/b/click");
        bind(m_buttonXAction, "/user/hand/left/input/x/click");
        bind(m_buttonYAction, "/user/hand/left/input/y/click");
        bind(m_menuAction, "/user/hand/left/input/menu/click");
        bind(m_gripPoseAction, "/user/hand/left/input/grip/pose");
        bind(m_gripPoseAction, "/user/hand/right/input/grip/pose");
        bind(m_aimPoseAction, "/user/hand/left/input/aim/pose");
        bind(m_aimPoseAction, "/user/hand/right/input/aim/pose");

        XrInteractionProfileSuggestedBinding suggested{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile = touchProfile;
        suggested.countSuggestedBindings =
            static_cast<std::uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        if (!CheckXr(xrSuggestInteractionProfileBindings(m_instance, &suggested),
            "xrSuggestInteractionProfileBindings(Oculus Touch)"))
        {
            return false;
        }

        XrSessionActionSetsAttachInfo attachInfo{
            XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &m_controllerActionSet;
        if (!CheckXr(xrAttachSessionActionSets(m_session, &attachInfo),
            "xrAttachSessionActionSets"))
        {
            return false;
        }

        for (std::size_t hand = 0; hand < m_handPaths.size(); ++hand)
        {
            XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            spaceInfo.poseInActionSpace.orientation.w = 1.0f;
            spaceInfo.subactionPath = m_handPaths[hand];
            spaceInfo.action = m_gripPoseAction;
            if (!CheckXr(xrCreateActionSpace(m_session, &spaceInfo,
                &m_gripSpaces[hand]), "xrCreateActionSpace(grip)"))
            {
                return false;
            }
            spaceInfo.action = m_aimPoseAction;
            if (!CheckXr(xrCreateActionSpace(m_session, &spaceInfo,
                &m_aimSpaces[hand]), "xrCreateActionSpace(aim)"))
            {
                return false;
            }
        }

        m_controllerInputInitialized = true;
        Log("OpenXR Quest Touch controller actions initialized");
        return true;
    }

    void ReadBooleanAction(XrAction action, XrPath subactionPath,
        std::uint32_t buttonMask, std::uint32_t& buttons)
    {
        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = action;
        getInfo.subactionPath = subactionPath;
        XrActionStateBoolean actionState{ XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo,
            &actionState)) && actionState.isActive && actionState.currentState)
        {
            buttons |= buttonMask;
        }
    }

    void ReadControllerPose(XrSpace space, XrTime displayTime,
        Sh3VrControllerPose& output)
    {
        if (space == XR_NULL_HANDLE)
            return;
        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace(space, m_appSpace, displayTime, &location)))
            return;
        if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
            output.flags |= SH3VR_POSE_ORIENTATION_VALID;
        if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0)
            output.flags |= SH3VR_POSE_POSITION_VALID;
        if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0)
            output.flags |= SH3VR_POSE_ORIENTATION_TRACKED;
        if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0)
            output.flags |= SH3VR_POSE_POSITION_TRACKED;
        output.position[0] = location.pose.position.x;
        output.position[1] = location.pose.position.y;
        output.position[2] = location.pose.position.z;
        output.orientation[0] = location.pose.orientation.x;
        output.orientation[1] = location.pose.orientation.y;
        output.orientation[2] = location.pose.orientation.z;
        output.orientation[3] = location.pose.orientation.w;
    }

    void SyncControllerInput(SharedFrameConsumer& consumer, XrTime displayTime)
    {
        Sh3VrControllerState state = {};
        state.predictedDisplayTime = displayTime;
        if (!m_controllerInputInitialized ||
            m_sessionState != XR_SESSION_STATE_FOCUSED)
        {
            consumer.PublishControllerState(state);
            return;
        }

        XrActiveActionSet activeSet{};
        activeSet.actionSet = m_controllerActionSet;
        XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeSet;
        const XrResult syncResult = xrSyncActions(m_session, &syncInfo);
        if (XR_FAILED(syncResult))
        {
            if (!m_controllerSyncFailureLogged)
            {
                Log("xrSyncActions failed with XrResult %d",
                    static_cast<int>(syncResult));
                m_controllerSyncFailureLogged = true;
            }
            consumer.PublishControllerState(state);
            return;
        }

        state.active = 1;
        for (std::size_t hand = 0; hand < m_handPaths.size(); ++hand)
        {
            XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.subactionPath = m_handPaths[hand];

            getInfo.action = m_thumbstickAction;
            XrActionStateVector2f stickState{ XR_TYPE_ACTION_STATE_VECTOR2F };
            if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &getInfo,
                &stickState)) && stickState.isActive)
            {
                state.thumbstick[hand][0] = stickState.currentState.x;
                state.thumbstick[hand][1] = stickState.currentState.y;
            }

            getInfo.action = m_triggerAction;
            XrActionStateFloat triggerState{ XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo,
                &triggerState)) && triggerState.isActive)
            {
                state.trigger[hand] = triggerState.currentState;
            }

            getInfo.action = m_squeezeAction;
            XrActionStateFloat squeezeState{ XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo,
                &squeezeState)) && squeezeState.isActive)
            {
                state.squeeze[hand] = squeezeState.currentState;
            }

            ReadBooleanAction(m_stickClickAction, m_handPaths[hand],
                hand == 0 ? SH3VR_BUTTON_LEFT_STICK :
                    SH3VR_BUTTON_RIGHT_STICK, state.buttons);
            ReadControllerPose(m_gripSpaces[hand], displayTime,
                state.gripPose[hand]);
            ReadControllerPose(m_aimSpaces[hand], displayTime,
                state.aimPose[hand]);
        }

        if (state.trigger[0] > 0.55f)
            state.buttons |= SH3VR_BUTTON_LEFT_TRIGGER;
        if (state.trigger[1] > 0.55f)
            state.buttons |= SH3VR_BUTTON_RIGHT_TRIGGER;
        if (state.squeeze[0] > 0.55f)
            state.buttons |= SH3VR_BUTTON_LEFT_GRIP;
        if (state.squeeze[1] > 0.55f)
            state.buttons |= SH3VR_BUTTON_RIGHT_GRIP;
        ReadBooleanAction(m_buttonAAction, m_handPaths[1],
            SH3VR_BUTTON_A, state.buttons);
        ReadBooleanAction(m_buttonBAction, m_handPaths[1],
            SH3VR_BUTTON_B, state.buttons);
        ReadBooleanAction(m_buttonXAction, m_handPaths[0],
            SH3VR_BUTTON_X, state.buttons);
        ReadBooleanAction(m_buttonYAction, m_handPaths[0],
            SH3VR_BUTTON_Y, state.buttons);
        ReadBooleanAction(m_menuAction, m_handPaths[0],
            SH3VR_BUTTON_MENU, state.buttons);
        consumer.PublishControllerState(state);
    }

    void ShutdownControllerInput()
    {
        for (XrSpace& space : m_gripSpaces)
        {
            if (space != XR_NULL_HANDLE)
                xrDestroySpace(space);
            space = XR_NULL_HANDLE;
        }
        for (XrSpace& space : m_aimSpaces)
        {
            if (space != XR_NULL_HANDLE)
                xrDestroySpace(space);
            space = XR_NULL_HANDLE;
        }
        if (m_controllerActionSet != XR_NULL_HANDLE)
            xrDestroyActionSet(m_controllerActionSet);
        m_controllerActionSet = XR_NULL_HANDLE;
        m_controllerInputInitialized = false;
    }

    bool CreateReferenceSpace()
    {
        XrReferenceSpaceCreateInfo createInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        createInfo.poseInReferenceSpace.orientation.w = 1.0f;
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        XrResult result = xrCreateReferenceSpace(m_session, &createInfo, &m_appSpace);
        if (XR_FAILED(result))
        {
            Log("STAGE space unavailable; falling back to LOCAL space");
            createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            result = xrCreateReferenceSpace(m_session, &createInfo, &m_appSpace);
        }
        if (!CheckXr(result, "xrCreateReferenceSpace(STAGE/LOCAL)"))
            return false;

        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        return CheckXr(xrCreateReferenceSpace(m_session, &createInfo, &m_viewSpace),
            "xrCreateReferenceSpace(VIEW)");
    }

    bool CreateSwapchains()
    {
        std::uint32_t viewCount = 0;
        if (!CheckXr(xrEnumerateViewConfigurationViews(m_instance, m_systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
            "xrEnumerateViewConfigurationViews(count)") || viewCount != 2)
        {
            Log("The runtime did not report exactly two primary stereo views");
            return false;
        }

        std::array<XrViewConfigurationView, 2> viewConfigs = {
            XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW },
            XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW }
        };
        if (!CheckXr(xrEnumerateViewConfigurationViews(m_instance, m_systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            static_cast<std::uint32_t>(viewConfigs.size()), &viewCount, viewConfigs.data()),
            "xrEnumerateViewConfigurationViews"))
        {
            return false;
        }

        std::uint32_t formatCount = 0;
        if (!CheckXr(xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr),
            "xrEnumerateSwapchainFormats(count)"))
            return false;
        std::vector<std::int64_t> formats(formatCount);
        if (!CheckXr(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount,
            formats.data()), "xrEnumerateSwapchainFormats"))
            return false;

        const DXGI_FORMAT preferences[] = {
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM
        };
        DXGI_FORMAT selectedFormat = DXGI_FORMAT_UNKNOWN;
        for (DXGI_FORMAT preference : preferences)
        {
            for (std::int64_t format : formats)
            {
                if (format == preference)
                {
                    selectedFormat = preference;
                    break;
                }
            }
            if (selectedFormat != DXGI_FORMAT_UNKNOWN)
                break;
        }
        if (selectedFormat == DXGI_FORMAT_UNKNOWN)
        {
            Log("The runtime offers no supported RGBA/BGRA8 swapchain format");
            return false;
        }
        m_swapchainUsesSrgb =
            selectedFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            selectedFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        Log("OpenXR swapchain transfer function: %s",
            m_swapchainUsesSrgb ? "sRGB" : "linear UNORM");

        for (std::size_t eye = 0; eye < m_eyes.size(); ++eye)
        {
            EyeSwapchain& target = m_eyes[eye];
            target.width = g_settings.eyeWidth != 0
                ? g_settings.eyeWidth : viewConfigs[eye].recommendedImageRectWidth;
            target.height = g_settings.eyeHeight != 0
                ? g_settings.eyeHeight : viewConfigs[eye].recommendedImageRectHeight;
            XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            createInfo.format = selectedFormat;
            createInfo.sampleCount = 1;
            createInfo.width = target.width;
            createInfo.height = target.height;
            createInfo.faceCount = 1;
            createInfo.arraySize = 1;
            createInfo.mipCount = 1;
            if (!CheckXr(xrCreateSwapchain(m_session, &createInfo, &target.handle),
                "xrCreateSwapchain"))
                return false;

            std::uint32_t imageCount = 0;
            xrEnumerateSwapchainImages(target.handle, 0, &imageCount, nullptr);
            target.images.assign(imageCount,
                XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            if (!CheckXr(xrEnumerateSwapchainImages(target.handle, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(target.images.data())),
                "xrEnumerateSwapchainImages"))
                return false;

            target.renderTargets.resize(imageCount);
            for (std::size_t image = 0; image < imageCount; ++image)
            {
                D3D11_TEXTURE2D_DESC textureDescription = {};
                target.images[image].texture->GetDesc(&textureDescription);

                D3D11_RENDER_TARGET_VIEW_DESC viewDescription = {};
                viewDescription.Format = selectedFormat;
                viewDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                viewDescription.Texture2D.MipSlice = 0;

                const HRESULT result = m_device->CreateRenderTargetView(
                    target.images[image].texture, &viewDescription,
                    &target.renderTargets[image]);
                if (FAILED(result))
                {
                    Log("CreateRenderTargetView failed for eye %zu image %zu: "
                        "hr=0x%08X textureFormat=%u requestedFormat=%u bindFlags=0x%08X",
                        eye, image, static_cast<unsigned>(result),
                        static_cast<unsigned>(textureDescription.Format),
                        static_cast<unsigned>(selectedFormat),
                        textureDescription.BindFlags);
                    return false;
                }
            }
            Log("Eye %zu swapchain: %ux%u, %u images, format %u%s", eye,
                target.width, target.height, imageCount,
                static_cast<unsigned>(selectedFormat),
                g_settings.eyeWidth != 0 ? " (sh3vr.ini override)" :
                " (runtime recommended)");
        }
        return true;
    }

    bool CreateBlitPipeline()
    {
        static constexpr char shaderSource[] = R"(
Texture2D gameFrame : register(t0);
Texture2D desktopColorStats : register(t1);
Texture2D eyeColorStats : register(t2);
SamplerState frameSampler : register(s0);
cbuffer BlitConstants : register(b0)
{
    float4 uvRect;
    float2 sourceTexelSize;
    float fxaaEnabled;
    float decodeSourceSrgb;
};
cbuffer ColorConstants : register(b1)
{
    float exposureScale;
    float contrast;
    float saturation;
    float vignetteStrength;
    float gamePostProcessTransferEnabled;
    float gamePostProcessTransferStrength;
    float gamePostProcessContrast;
    float gamePostProcessExposureScale;
};
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VertexOutput VsMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    float2 position = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = lerp(uvRect.xy, uvRect.zw, position);
    return output;
}
float3 DecodeSrgb(float3 color)
{
    float3 low = color / 12.92;
    float3 high = pow((color + 0.055) / 1.055, 2.4);
    return lerp(low, high, step(0.04045, color));
}
float3 ApplyImageCorrection(float3 color, float2 uv)
{
    color *= exposureScale;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = lerp(luminance.xxx, color, saturation);
    color = max((color - 0.18) * contrast + 0.18, 0.0);

    float2 uvSize = max(uvRect.zw - uvRect.xy, float2(0.0001, 0.0001));
    float2 localUv = (uv - uvRect.xy) / uvSize;
    float edgeDistance = length((localUv - 0.5) * float2(1.0, 0.9)) * 1.4142;
    float vignette = 1.0 - vignetteStrength * smoothstep(0.55, 1.0, edgeDistance);
    return color * vignette;
}
float3 ApplyGamePostProcessTransfer(float3 color)
{
    if (gamePostProcessTransferEnabled < 0.5)
        return color;

    float4 desktopStats = desktopColorStats.Load(int3(0, 0, 0));
    float4 eyeStats = eyeColorStats.Load(int3(0, 0, 0));
    static const float3 luminanceWeights = float3(0.299, 0.587, 0.114);

    float desktopLuminance = dot(desktopStats.rgb, luminanceWeights);
    float eyeLuminance = dot(eyeStats.rgb, luminanceWeights);
    if (desktopLuminance < 0.06 || eyeLuminance < 0.06)
        return color;

    float3 colorScale = clamp(
        desktopStats.rgb / max(eyeStats.rgb, float3(0.04, 0.04, 0.04)),
        float3(0.70, 0.70, 0.70), float3(2.00, 2.00, 2.00));
    float3 corrected = color * colorScale;
    corrected = (corrected - desktopStats.rgb) *
        gamePostProcessContrast + desktopStats.rgb;
    float correctedLuminance = dot(corrected, luminanceWeights);
    float saturationScale = clamp(
        desktopStats.a / max(eyeStats.a, 0.01), 0.60, 1.25);
    corrected = lerp(correctedLuminance.xxx, corrected, saturationScale);
    corrected = saturate(corrected * gamePostProcessExposureScale);
    return lerp(color, corrected,
        saturate(gamePostProcessTransferStrength));
}
float4 PsMain(VertexOutput input) : SV_Target
{
    float4 center = gameFrame.Sample(frameSampler, input.uv);
    if (fxaaEnabled < 0.5)
    {
        float3 outputColor = center.rgb;
        outputColor = ApplyGamePostProcessTransfer(outputColor);
        if (decodeSourceSrgb > 0.5)
            outputColor = DecodeSrgb(outputColor);
        outputColor = ApplyImageCorrection(outputColor, input.uv);
        return float4(outputColor, center.a);
    }

    static const float3 lumaWeights = float3(0.299, 0.587, 0.114);
    float3 rgbNW = gameFrame.Sample(frameSampler,
        input.uv + sourceTexelSize * float2(-1.0, -1.0)).rgb;
    float3 rgbNE = gameFrame.Sample(frameSampler,
        input.uv + sourceTexelSize * float2(1.0, -1.0)).rgb;
    float3 rgbSW = gameFrame.Sample(frameSampler,
        input.uv + sourceTexelSize * float2(-1.0, 1.0)).rgb;
    float3 rgbSE = gameFrame.Sample(frameSampler,
        input.uv + sourceTexelSize * float2(1.0, 1.0)).rgb;
    float lumaNW = dot(rgbNW, lumaWeights);
    float lumaNE = dot(rgbNE, lumaWeights);
    float lumaSW = dot(rgbSW, lumaWeights);
    float lumaSE = dot(rgbSE, lumaWeights);
    float lumaM = dot(center.rgb, lumaWeights);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 direction;
    direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    direction.y = (lumaNW + lumaSW) - (lumaNE + lumaSE);
    float directionReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);
    float inverseDirectionMinimum = 1.0 /
        (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * inverseDirectionMinimum, -8.0, 8.0) *
        sourceTexelSize;

    float3 rgbA = 0.5 * (
        gameFrame.Sample(frameSampler,
            input.uv + direction * (1.0 / 3.0 - 0.5)).rgb +
        gameFrame.Sample(frameSampler,
            input.uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        gameFrame.Sample(frameSampler, input.uv + direction * -0.5).rgb +
        gameFrame.Sample(frameSampler, input.uv + direction * 0.5).rgb);
    float lumaB = dot(rgbB, lumaWeights);
    float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    result = ApplyGamePostProcessTransfer(result);
    if (decodeSourceSrgb > 0.5)
        result = DecodeSrgb(result);
    result = ApplyImageCorrection(result, input.uv);
    return float4(result, center.a);
}
float4 StatsPsMain(VertexOutput input) : SV_Target
{
    float3 colorSum = 0.0;
    float saturationSum = 0.0;
    [unroll]
    for (uint y = 0; y < 8; ++y)
    {
        [unroll]
        for (uint x = 0; x < 8; ++x)
        {
            float2 gridUv = (float2(x, y) + 0.5) / 8.0;
            gridUv = lerp(float2(0.15, 0.15), float2(0.85, 0.85), gridUv);
            float2 sampleUv = lerp(uvRect.xy, uvRect.zw, gridUv);
            float3 color = gameFrame.SampleLevel(frameSampler, sampleUv, 0).rgb;
            float maximumChannel = max(color.r, max(color.g, color.b));
            float minimumChannel = min(color.r, min(color.g, color.b));
            float saturation = maximumChannel > 0.0001
                ? (maximumChannel - minimumChannel) / maximumChannel : 0.0;
            colorSum += color;
            saturationSum += saturation;
        }
    }
    return float4(colorSum / 64.0, saturationSum / 64.0);
}
struct DebugVertexInput
{
    float2 position : POSITION;
    float4 color : COLOR;
};
struct DebugVertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};
DebugVertexOutput DebugVsMain(DebugVertexInput input)
{
    DebugVertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}
float4 DebugPsMain(DebugVertexOutput input) : SV_Target
{
    return input.color;
}
)";

        ComPtr<ID3DBlob> vertexCode;
        ComPtr<ID3DBlob> pixelCode;
        ComPtr<ID3DBlob> statsPixelCode;
        ComPtr<ID3DBlob> debugVertexCode;
        ComPtr<ID3DBlob> debugPixelCode;
        ComPtr<ID3DBlob> errors;
        HRESULT result = D3DCompile(shaderSource, sizeof(shaderSource), "sh3vr_blit.hlsl",
            nullptr, nullptr, "VsMain", "vs_4_0", 0, 0, &vertexCode, &errors);
        if (FAILED(result))
        {
            Log("Vertex shader compilation failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
            return false;
        }
        errors.Reset();
        result = D3DCompile(shaderSource, sizeof(shaderSource), "sh3vr_blit.hlsl",
            nullptr, nullptr, "PsMain", "ps_4_0", 0, 0, &pixelCode, &errors);
        if (FAILED(result))
        {
            Log("Pixel shader compilation failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
            return false;
        }
        errors.Reset();
        result = D3DCompile(shaderSource, sizeof(shaderSource), "sh3vr_blit.hlsl",
            nullptr, nullptr, "DebugVsMain", "vs_4_0", 0, 0,
            &debugVertexCode, &errors);
        if (FAILED(result))
        {
            Log("Debug overlay vertex shader compilation failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) :
                "unknown error");
            return false;
        }
        errors.Reset();
        result = D3DCompile(shaderSource, sizeof(shaderSource), "sh3vr_blit.hlsl",
            nullptr, nullptr, "DebugPsMain", "ps_4_0", 0, 0,
            &debugPixelCode, &errors);
        if (FAILED(result))
        {
            Log("Debug overlay pixel shader compilation failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) :
                "unknown error");
            return false;
        }
        errors.Reset();
        result = D3DCompile(shaderSource, sizeof(shaderSource), "sh3vr_blit.hlsl",
            nullptr, nullptr, "StatsPsMain", "ps_4_0", 0, 0,
            &statsPixelCode, &errors);
        if (FAILED(result))
        {
            Log("Color-statistics shader compilation failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) :
                "unknown error");
            return false;
        }

        if (FAILED(m_device->CreateVertexShader(vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(), nullptr, &m_vertexShader)) ||
            FAILED(m_device->CreatePixelShader(pixelCode->GetBufferPointer(),
                pixelCode->GetBufferSize(), nullptr, &m_pixelShader)) ||
            FAILED(m_device->CreatePixelShader(statsPixelCode->GetBufferPointer(),
                statsPixelCode->GetBufferSize(), nullptr,
                &m_statsPixelShader)) ||
            FAILED(m_device->CreateVertexShader(debugVertexCode->GetBufferPointer(),
                debugVertexCode->GetBufferSize(), nullptr,
                &m_debugVertexShader)) ||
            FAILED(m_device->CreatePixelShader(debugPixelCode->GetBufferPointer(),
                debugPixelCode->GetBufferSize(), nullptr,
                &m_debugPixelShader)))
        {
            Log("Failed to create blit shaders");
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC debugElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,
                D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        if (FAILED(m_device->CreateInputLayout(debugElements,
            static_cast<UINT>(std::size(debugElements)),
            debugVertexCode->GetBufferPointer(), debugVertexCode->GetBufferSize(),
            &m_debugInputLayout)))
        {
            Log("Failed to create debug overlay input layout");
            return false;
        }
        D3D11_BUFFER_DESC debugVertexDesc = {};
        debugVertexDesc.ByteWidth = 4096u * 6u * sizeof(float);
        debugVertexDesc.Usage = D3D11_USAGE_DYNAMIC;
        debugVertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        debugVertexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateBuffer(&debugVertexDesc, nullptr,
            &m_debugVertexBuffer)))
        {
            Log("Failed to create debug overlay vertex buffer");
            return false;
        }
        D3D11_BLEND_DESC debugBlendDesc = {};
        debugBlendDesc.RenderTarget[0].BlendEnable = TRUE;
        debugBlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        debugBlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        debugBlendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        debugBlendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        debugBlendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        debugBlendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        debugBlendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(m_device->CreateBlendState(&debugBlendDesc,
            &m_debugBlendState)))
        {
            Log("Failed to create debug overlay blend state");
            return false;
        }

        D3D11_BUFFER_DESC constantDesc = {};
        constantDesc.ByteWidth = 32;
        constantDesc.Usage = D3D11_USAGE_DEFAULT;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(m_device->CreateBuffer(&constantDesc, nullptr, &m_blitConstants)))
            return false;
        constantDesc.ByteWidth = 32;
        if (FAILED(m_device->CreateBuffer(&constantDesc, nullptr,
            &m_colorConstants)))
        {
            Log("Failed to create image-correction constant buffer");
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(m_device->CreateSamplerState(&samplerDesc, &m_sampler)))
            return false;

        D3D11_TEXTURE2D_DESC statsTextureDesc = {};
        statsTextureDesc.Width = 1;
        statsTextureDesc.Height = 1;
        statsTextureDesc.MipLevels = 1;
        statsTextureDesc.ArraySize = 1;
        statsTextureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        statsTextureDesc.SampleDesc.Count = 1;
        statsTextureDesc.Usage = D3D11_USAGE_DEFAULT;
        statsTextureDesc.BindFlags = D3D11_BIND_RENDER_TARGET |
            D3D11_BIND_SHADER_RESOURCE;
        for (std::size_t index = 0; index < m_colorStatsTextures.size(); ++index)
        {
            if (FAILED(m_device->CreateTexture2D(&statsTextureDesc, nullptr,
                    &m_colorStatsTextures[index])) ||
                FAILED(m_device->CreateRenderTargetView(
                    m_colorStatsTextures[index].Get(), nullptr,
                    &m_colorStatsRenderTargets[index])) ||
                FAILED(m_device->CreateShaderResourceView(
                    m_colorStatsTextures[index].Get(), nullptr,
                    &m_colorStatsViews[index])))
            {
                Log("Failed to create post-process color-statistics resource %zu",
                    index);
                return false;
            }
        }

        D3D11_BLEND_DESC statsBlendDesc = {};
        statsBlendDesc.RenderTarget[0].BlendEnable = TRUE;
        statsBlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        statsBlendDesc.RenderTarget[0].DestBlend =
            D3D11_BLEND_INV_BLEND_FACTOR;
        statsBlendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        statsBlendDesc.RenderTarget[0].SrcBlendAlpha =
            D3D11_BLEND_BLEND_FACTOR;
        statsBlendDesc.RenderTarget[0].DestBlendAlpha =
            D3D11_BLEND_INV_BLEND_FACTOR;
        statsBlendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        statsBlendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(m_device->CreateBlendState(&statsBlendDesc,
            &m_statsBlendState));
    }

    void PollEvents()
    {
        XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
        {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto* changed =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                m_sessionState = changed->state;
                Log("OpenXR session state changed to %d", static_cast<int>(m_sessionState));
                switch (m_sessionState)
                {
                case XR_SESSION_STATE_READY:
                {
                    XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                    beginInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (CheckXr(xrBeginSession(m_session, &beginInfo), "xrBeginSession"))
                        m_sessionRunning = true;
                    break;
                }
                case XR_SESSION_STATE_SYNCHRONIZED:
                    m_shouldRender = false;
                    break;
                case XR_SESSION_STATE_VISIBLE:
                case XR_SESSION_STATE_FOCUSED:
                    m_shouldRender = true;
                    break;
                case XR_SESSION_STATE_STOPPING:
                    if (m_sessionRunning)
                        xrEndSession(m_session);
                    m_sessionRunning = false;
                    m_shouldRender = false;
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    m_exitRequested = true;
                    break;
                default:
                    break;
                }
            }
            else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            {
                m_exitRequested = true;
            }
            event = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    void UploadLatestFrame(SharedFrameConsumer& consumer)
    {
        Sh3VrFrameHeader metadata = {};
        std::array<std::vector<std::uint8_t>, 2> pixels;
        const bool copyCpuPixels = !m_usingD3D12SharedSource;
        if (!consumer.ReadLatest(pixels, metadata, copyCpuPixels))
            return;

        UpdateD3D12BackBufferSource(metadata);
        UpdateD3D12EyeSources(metadata);

        Sh3VrWeaponDebugState weaponDebug = {};
        std::memcpy(&weaponDebug, metadata.reserved, sizeof(weaponDebug));
        m_debugWeaponOrientationValid =
            weaponDebug.magic == SH3VR_WEAPON_DEBUG_MAGIC &&
            weaponDebug.weaponValid != 0 &&
            std::isfinite(weaponDebug.pitchDegrees) &&
            std::isfinite(weaponDebug.yawDegrees) &&
            std::isfinite(weaponDebug.rollDegrees);
        if (m_debugWeaponOrientationValid)
        {
            m_debugWeaponPitch = weaponDebug.pitchDegrees;
            m_debugWeaponYaw = weaponDebug.yawDegrees;
            m_debugWeaponRoll = weaponDebug.rollDegrees;
            m_debugWeaponProfileIndex = weaponDebug.profileIndex;
        }
        m_debugLeftHandOrientationValid =
            weaponDebug.magic == SH3VR_WEAPON_DEBUG_MAGIC &&
            weaponDebug.leftHandValid != 0 &&
            std::isfinite(weaponDebug.leftHandPitchDegrees) &&
            std::isfinite(weaponDebug.leftHandYawDegrees) &&
            std::isfinite(weaponDebug.leftHandRollDegrees);
        if (m_debugLeftHandOrientationValid)
        {
            m_debugLeftHandPitch = weaponDebug.leftHandPitchDegrees;
            m_debugLeftHandYaw = weaponDebug.leftHandYawDegrees;
            m_debugLeftHandRoll = weaponDebug.leftHandRollDegrees;
        }

        if (metadata.renderMode != m_renderMode)
        {
            const std::int32_t previousRenderMode = m_renderMode;
            m_renderMode = metadata.renderMode;
            Log("Presentation mode changed to %s",
                m_renderMode == SH3VR_RENDER_IMMERSIVE_STEREO
                ? "immersive synchronized stereo"
                : (m_renderMode == SH3VR_RENDER_IMMERSIVE_MONO
                    ? "immersive mono" : "cinema"));
            if (previousRenderMode == SH3VR_RENDER_CINEMA &&
                m_renderMode != SH3VR_RENDER_CINEMA)
            {
                BeginColorStatsCalibration("immersive scene transition");
            }
        }

        m_gamePostProcessEnabled = metadata.gamePostProcessEnabled != 0;
        for (std::size_t channel = 0;
            channel < m_gamePostProcessScale.size(); ++channel)
        {
            const float value = metadata.gamePostProcessScale[channel];
            m_gamePostProcessScale[channel] = std::isfinite(value)
                ? std::clamp(value, 0.0f, 4.0f) : 1.0f;
        }
        m_gamePostProcessBlend = std::isfinite(
            metadata.gamePostProcessBlend)
            ? std::clamp(metadata.gamePostProcessBlend, 0.0f, 1.0f)
            : 0.0f;
        if (m_gamePostProcessEnabled && !m_gamePostProcessLogged)
        {
            m_gamePostProcessLogged = true;
            Log("Per-eye SH3 post-processing enabled: source %u,%u,%u,%u, "
                "RGB scale %.4f,%.4f,%.4f, blend %.4f",
                metadata.gamePostProcessSource[0],
                metadata.gamePostProcessSource[1],
                metadata.gamePostProcessSource[2],
                metadata.gamePostProcessSource[3],
                m_gamePostProcessScale[0], m_gamePostProcessScale[1],
                m_gamePostProcessScale[2], m_gamePostProcessBlend);
        }

        const bool cpuPixelsAvailable = !pixels[0].empty();
        if (cpuPixelsAvailable &&
            (!m_sourceTextures[0] || metadata.width != m_sourceWidth ||
                metadata.height != m_sourceHeight))
        {
            for (std::size_t eye = 0; eye < m_sourceTextures.size(); ++eye)
            {
                m_sourceViews[eye].Reset();
                m_sourceTextures[eye].Reset();
            }
            D3D11_TEXTURE2D_DESC description = {};
            description.Width = metadata.width;
            description.Height = metadata.height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            for (std::size_t eye = 0; eye < m_sourceTextures.size(); ++eye)
            {
                if (FAILED(m_device->CreateTexture2D(&description, nullptr,
                    &m_sourceTextures[eye])) ||
                    FAILED(m_device->CreateShaderResourceView(
                        m_sourceTextures[eye].Get(), nullptr, &m_sourceViews[eye])))
                {
                    Log("Failed to create %ux%u source texture for eye %zu",
                        metadata.width, metadata.height, eye);
                    return;
                }
            }
            m_sourceWidth = metadata.width;
            m_sourceHeight = metadata.height;
            Log("Created source texture %ux%u", m_sourceWidth, m_sourceHeight);
        }

        if (cpuPixelsAvailable)
        {
            const bool synchronizedStereo =
                metadata.renderMode == SH3VR_RENDER_IMMERSIVE_STEREO;
            const std::size_t uploadCount = synchronizedStereo ? 2 : 1;
            for (std::size_t sourceEye = 0; sourceEye < uploadCount; ++sourceEye)
            {
                if (!pixels[sourceEye].empty())
                {
                    m_context->UpdateSubresource(m_sourceTextures[sourceEye].Get(), 0,
                        nullptr, pixels[sourceEye].data(), metadata.pitch, 0);
                }
            }
        }
        const float inverseWidth = 1.0f / static_cast<float>(metadata.width);
        const float inverseHeight = 1.0f / static_cast<float>(metadata.height);
        m_uvRect = {
            metadata.contentLeft * inverseWidth,
            metadata.contentTop * inverseHeight,
            (metadata.contentLeft + metadata.contentWidth) * inverseWidth,
            (metadata.contentTop + metadata.contentHeight) * inverseHeight
        };
        if (metadata.contentWidth != 0 && metadata.contentHeight != 0)
        {
            m_frameAspect = static_cast<float>(metadata.contentWidth) /
                static_cast<float>(metadata.contentHeight);
        }
        const std::uint32_t requiredPoseFlags =
            SH3VR_POSE_ORIENTATION_VALID | SH3VR_POSE_POSITION_VALID;
        const std::uint32_t nativeSet =
            metadata.d3d12EyeTextureActiveSet < 2
                ? metadata.d3d12EyeTextureActiveSet : 0;
        const std::size_t nativeBase = static_cast<std::size_t>(nativeSet) * 2;
        const bool nativeFrameAvailable =
            metadata.d3d12EyeTextureFrameSequence != 0 &&
            nativeBase + 1 < m_d3d12EyeViews.size() &&
            m_d3d12EyeViews[nativeBase] && m_d3d12EyeViews[nativeBase + 1];
        const Sh3VrHeadPose& renderPose = nativeFrameAvailable
            ? metadata.d3d12EyeFrameRenderPoses[nativeSet]
            : metadata.frameRenderPose;
        if ((renderPose.flags & requiredPoseFlags) ==
            requiredPoseFlags)
        {
            m_frameRenderPose = renderPose;
            m_haveFrameRenderPose = true;
            if (!m_frameRenderPoseLogged)
            {
                m_frameRenderPoseLogged = true;
                Log("Frame render-pose submission enabled for OpenXR timewarp");
            }
        }
        m_context->UpdateSubresource(m_blitConstants.Get(), 0, nullptr,
            m_uvRect.data(), 0, 0);
    }

    void UpdateD3D12BackBufferSource(const Sh3VrFrameHeader& metadata)
    {
        if (metadata.d3d12BackBufferCount == 0 ||
            metadata.d3d12BackBufferCount > m_d3d12SourceViews.size())
        {
            m_usingD3D12SharedSource = false;
            return;
        }

        m_d3d12BackBufferIndex = metadata.d3d12BackBufferIndex <
            metadata.d3d12BackBufferCount ? metadata.d3d12BackBufferIndex : 0;

        if (metadata.d3d12BackBufferGeneration == m_lastD3D12HandleGeneration)
        {
            m_usingD3D12SharedSource =
                m_d3d12SourceViews[m_d3d12BackBufferIndex] != nullptr;
            return;
        }

        m_lastD3D12HandleGeneration = metadata.d3d12BackBufferGeneration;
        m_usingD3D12SharedSource = false;
        for (std::size_t index = 0; index < m_d3d12SourceViews.size(); ++index)
        {
            m_d3d12SourceViews[index].Reset();
            m_d3d12SourceTextures[index].Reset();
        }

        HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE,
            metadata.producerPid);
        if (!producer)
        {
            Log("D3D12 interop: OpenProcess(%u) failed, error %u",
                metadata.producerPid, GetLastError());
            return;
        }

        ComPtr<ID3D11Device1> device1;
        const HRESULT deviceResult = m_device.As(&device1);
        if (FAILED(deviceResult) || !device1)
        {
            Log("D3D12 interop: ID3D11Device1 query failed, hr 0x%08X",
                static_cast<unsigned>(deviceResult));
            CloseHandle(producer);
            return;
        }

        for (std::uint32_t index = 0; index < metadata.d3d12BackBufferCount;
            ++index)
        {
            if (metadata.d3d12BackBufferHandles[index] == 0)
                continue;

            HANDLE localHandle = nullptr;
            const HANDLE producerHandle = reinterpret_cast<HANDLE>(
                static_cast<UINT_PTR>(metadata.d3d12BackBufferHandles[index]));
            if (!DuplicateHandle(producer, producerHandle, GetCurrentProcess(),
                &localHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
            {
                Log("D3D12 interop: DuplicateHandle(%u) failed, error %u",
                    index, GetLastError());
                continue;
            }

            ComPtr<ID3D11Texture2D> texture;
            const HRESULT openResult = device1->OpenSharedResource1(localHandle,
                IID_PPV_ARGS(&texture));
            Log("D3D12-to-D3D11 shared-resource %u: hr 0x%08X, texture %p",
                index, static_cast<unsigned>(openResult), texture.Get());
            CloseHandle(localHandle);
            if (FAILED(openResult) || !texture)
                continue;

            D3D11_TEXTURE2D_DESC description = {};
            texture->GetDesc(&description);
            Log("D3D12-to-D3D11 shared texture %u: %ux%u, format %u, bind 0x%X, misc 0x%X",
                index, description.Width, description.Height,
                static_cast<unsigned>(description.Format), description.BindFlags,
                description.MiscFlags);

            ComPtr<ID3D11ShaderResourceView> view;
            const HRESULT viewResult = m_device->CreateShaderResourceView(
                texture.Get(), nullptr, &view);
            Log("D3D12-to-D3D11 shared SRV %u: hr 0x%08X, view %p",
                index, static_cast<unsigned>(viewResult), view.Get());
            if (SUCCEEDED(viewResult) && view)
            {
                m_d3d12SourceTextures[index] = texture;
                m_d3d12SourceViews[index] = view;
            }
        }
        CloseHandle(producer);

        m_usingD3D12SharedSource =
            m_d3d12SourceViews[m_d3d12BackBufferIndex] != nullptr;
        if (m_usingD3D12SharedSource)
        {
            Log("GPU shared backbuffer source enabled, current index %u",
                m_d3d12BackBufferIndex);
        }
        else
        {
            Log("GPU shared backbuffer source unavailable; CPU upload remains active");
        }
    }

    void UpdateD3D12EyeSources(const Sh3VrFrameHeader& metadata)
    {
        m_d3d12EyeActiveSet = metadata.d3d12EyeTextureActiveSet < 2
            ? metadata.d3d12EyeTextureActiveSet : 0;
        m_d3d12EyeFrameSequence = metadata.d3d12EyeTextureFrameSequence;
        m_d3d12EyeSourceWidth = metadata.d3d12EyeTextureWidth;
        m_d3d12EyeSourceHeight = metadata.d3d12EyeTextureHeight;
        if (metadata.d3d12EyeTextureGeneration ==
            m_lastD3D12EyeTextureGeneration)
        {
            return;
        }
        m_lastD3D12EyeTextureGeneration =
            metadata.d3d12EyeTextureGeneration;
        for (std::size_t eye = 0; eye < m_d3d12EyeViews.size(); ++eye)
        {
            m_d3d12EyeViews[eye].Reset();
            m_d3d12EyeTextures[eye].Reset();
        }

        bool haveAllHandles = true;
        for (std::size_t index = 0; index < m_d3d12EyeViews.size(); ++index)
            haveAllHandles = haveAllHandles &&
                metadata.d3d12EyeTextureHandles[index] != 0;
        if (!haveAllHandles)
        {
            return;
        }

        HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE,
            metadata.producerPid);
        if (!producer)
        {
            Log("Native eye interop: OpenProcess(%u) failed, error %u",
                metadata.producerPid, GetLastError());
            return;
        }

        ComPtr<ID3D11Device1> device1;
        const HRESULT deviceResult = m_device.As(&device1);
        if (FAILED(deviceResult) || !device1)
        {
            Log("Native eye interop: ID3D11Device1 query failed, hr 0x%08X",
                static_cast<unsigned>(deviceResult));
            CloseHandle(producer);
            return;
        }

        for (std::size_t index = 0; index < m_d3d12EyeViews.size(); ++index)
        {
            HANDLE localHandle = nullptr;
            const HANDLE producerHandle = reinterpret_cast<HANDLE>(
                static_cast<UINT_PTR>(metadata.d3d12EyeTextureHandles[index]));
            if (!DuplicateHandle(producer, producerHandle, GetCurrentProcess(),
                &localHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
            {
                Log("Native eye set %zu eye %zu DuplicateHandle failed, "
                    "error %u", index / 2, index % 2,
                    GetLastError());
                continue;
            }

            ComPtr<ID3D11Texture2D> texture;
            const HRESULT openResult = device1->OpenSharedResource1(localHandle,
                IID_PPV_ARGS(&texture));
            CloseHandle(localHandle);
            Log("Native eye set %zu eye %zu shared-resource open: hr "
                "0x%08X, texture %p", index / 2, index % 2,
                static_cast<unsigned>(openResult), texture.Get());
            if (FAILED(openResult) || !texture)
                continue;

            D3D11_TEXTURE2D_DESC description = {};
            texture->GetDesc(&description);
            Log("Native eye set %zu eye %zu shared texture: %ux%u, format "
                "%u, bind 0x%X, misc 0x%X", index / 2, index % 2,
                description.Width, description.Height,
                static_cast<unsigned>(description.Format), description.BindFlags,
                description.MiscFlags);

            ComPtr<ID3D11ShaderResourceView> view;
            const HRESULT viewResult = m_device->CreateShaderResourceView(
                texture.Get(), nullptr, &view);
            Log("Native eye set %zu eye %zu shared SRV: hr 0x%08X, view %p",
                index / 2, index % 2,
                static_cast<unsigned>(viewResult), view.Get());
            if (SUCCEEDED(viewResult) && view)
            {
                m_d3d12EyeTextures[index] = texture;
                m_d3d12EyeViews[index] = view;
            }
        }
        CloseHandle(producer);

        bool allReady = true;
        for (const auto& view : m_d3d12EyeViews)
            allReady = allReady && view;
        if (allReady)
            Log("Double-buffered native per-eye GPU sources are ready");
    }

    XrFovf BuildGuardBandFov(const XrFovf& runtimeFov,
        bool nativeEyeSource) const
    {
        constexpr float sourceHalfVertical = 60.0f *
            3.14159265358979323846f / 180.0f;
        constexpr float guardBand = 13.0f *
            3.14159265358979323846f / 180.0f;
        constexpr float sourceTanHalfVertical = 1.0f / 0.5773502692f;
        const float sourceAspect = nativeEyeSource && m_eyes[0].height != 0
            ? static_cast<float>(m_eyes[0].width) /
                static_cast<float>(m_eyes[0].height)
            : m_frameAspect;
        const float sourceHalfHorizontal = std::atan(
            sourceTanHalfVertical * sourceAspect);

        XrFovf result = runtimeFov;
        result.angleLeft = (std::max)(-sourceHalfHorizontal,
            runtimeFov.angleLeft - guardBand);
        result.angleRight = (std::min)(sourceHalfHorizontal,
            runtimeFov.angleRight + guardBand);
        result.angleDown = (std::max)(-sourceHalfVertical,
            runtimeFov.angleDown - guardBand);
        result.angleUp = (std::min)(sourceHalfVertical,
            runtimeFov.angleUp + guardBand);
        return result;
    }

    std::array<float, 4> BuildProjectionUvRect(const XrFovf& fov,
        bool fullSource) const
    {
        // The game hook renders a symmetric 120-degree vertical projection.
        // Select the rays required by each runtime-provided asymmetric eye FOV.
        constexpr float sourceTanHalfVertical = 1.0f / 0.5773502692f;
        const float sourceAspect = fullSource && m_eyes[0].height != 0
            ? static_cast<float>(m_eyes[0].width) /
                static_cast<float>(m_eyes[0].height)
            : m_frameAspect;
        const float sourceTanHalfHorizontal =
            sourceTanHalfVertical * sourceAspect;
        const float left = 0.5f *
            (std::tan(fov.angleLeft) / sourceTanHalfHorizontal + 1.0f);
        const float right = 0.5f *
            (std::tan(fov.angleRight) / sourceTanHalfHorizontal + 1.0f);
        const float top = 0.5f *
            (1.0f - std::tan(fov.angleUp) / sourceTanHalfVertical);
        const float bottom = 0.5f *
            (1.0f - std::tan(fov.angleDown) / sourceTanHalfVertical);

        const float contentLeft = fullSource ? 0.0f : m_uvRect[0];
        const float contentTop = fullSource ? 0.0f : m_uvRect[1];
        const float contentRight = fullSource ? 1.0f : m_uvRect[2];
        const float contentBottom = fullSource ? 1.0f : m_uvRect[3];
        const float contentWidth = contentRight - contentLeft;
        const float contentHeight = contentBottom - contentTop;
        return {
            contentLeft + contentWidth * left,
            contentTop + contentHeight * top,
            contentLeft + contentWidth * right,
            contentTop + contentHeight * bottom
        };
    }

    bool CaptureEyeDebugTexture(ID3D11Texture2D* source)
    {
        if (!source || m_eyeDebugCaptured)
            return false;

        D3D11_TEXTURE2D_DESC sourceDesc = {};
        source->GetDesc(&sourceDesc);
        D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
            return false;

        m_context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;

        wchar_t path[MAX_PATH] = {};
        if (!GetHostDirectory(path, std::size(path)))
        {
            m_context->Unmap(staging.Get(), 0);
            return false;
        }
        wcscat_s(path, L"sh3vr_eye_debug.bmp");
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            m_context->Unmap(staging.Get(), 0);
            return false;
        }

        const DWORD rowBytes = sourceDesc.Width * 4;
        const DWORD pixelBytes = rowBytes * sourceDesc.Height;
        BITMAPFILEHEADER fileHeader = {};
        BITMAPINFOHEADER infoHeader = {};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
        fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
        infoHeader.biSize = sizeof(infoHeader);
        infoHeader.biWidth = static_cast<LONG>(sourceDesc.Width);
        infoHeader.biHeight = -static_cast<LONG>(sourceDesc.Height);
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biCompression = BI_RGB;
        infoHeader.biSizeImage = pixelBytes;

        DWORD written = 0;
        bool saved = WriteFile(file, &fileHeader, sizeof(fileHeader), &written,
            nullptr) != FALSE;
        saved = saved && WriteFile(file, &infoHeader, sizeof(infoHeader),
            &written, nullptr) != FALSE;
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData);
        for (UINT y = 0; saved && y < sourceDesc.Height; ++y)
        {
            saved = WriteFile(file, row + static_cast<std::size_t>(y) *
                mapped.RowPitch, rowBytes, &written, nullptr) != FALSE &&
                written == rowBytes;
        }
        CloseHandle(file);
        m_context->Unmap(staging.Get(), 0);
        if (saved)
        {
            m_eyeDebugCaptured = true;
            Log("Captured raw native eye texture to sh3vr_eye_debug.bmp: %ux%u",
                sourceDesc.Width, sourceDesc.Height);
        }
        return saved;
    }

    void BeginColorStatsCalibration(const char* reason)
    {
        if (!g_settings.enableGamePostProcess)
            return;
        m_colorStatsInitialized.fill(false);
        m_colorStatsCalibrationFramesRemaining = 90;
        m_colorStatsCalibrationPending = true;
        m_colorStatsFrozenLogged = false;
        Log("Post-process color calibration started immediately for "
            "90 frames: %s",
            reason ? reason : "unspecified");
    }

    void UpdateColorStats(std::size_t statsIndex,
        ID3D11ShaderResourceView* sourceView,
        const std::array<float, 4>& sourceUvRect)
    {
        if (!sourceView || statsIndex >= m_colorStatsRenderTargets.size() ||
            !m_statsPixelShader || !m_statsBlendState)
        {
            return;
        }

        ID3D11RenderTargetView* statsTarget =
            m_colorStatsRenderTargets[statsIndex].Get();
        m_context->OMSetRenderTargets(1, &statsTarget, nullptr);
        D3D11_VIEWPORT statsViewport = {};
        statsViewport.Width = 1.0f;
        statsViewport.Height = 1.0f;
        statsViewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &statsViewport);

        struct BlitConstantsData
        {
            float uvRect[4];
            float sourceTexelSize[2];
            float fxaaEnabled;
            float decodeSourceSrgb;
        };
        BlitConstantsData constantsData = {};
        std::memcpy(constantsData.uvRect, sourceUvRect.data(),
            sizeof(constantsData.uvRect));
        m_context->UpdateSubresource(m_blitConstants.Get(), 0, nullptr,
            &constantsData, 0, 0);

        const float blendAmount = m_colorStatsInitialized[statsIndex]
            ? 0.08f : 1.0f;
        const float blendFactor[4] = {
            blendAmount, blendAmount, blendAmount, blendAmount
        };
        m_context->OMSetBlendState(m_statsBlendState.Get(), blendFactor,
            0xFFFFFFFFu);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        ID3D11Buffer* constants = m_blitConstants.Get();
        m_context->VSSetConstantBuffers(0, 1, &constants);
        m_context->PSSetShader(m_statsPixelShader.Get(), nullptr, 0);
        m_context->PSSetConstantBuffers(0, 1, &constants);
        ID3D11SamplerState* sampler = m_sampler.Get();
        m_context->PSSetSamplers(0, 1, &sampler);
        m_context->PSSetShaderResources(0, 1, &sourceView);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullView = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullView);
        m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        m_colorStatsInitialized[statsIndex] = true;
    }

    struct DebugOverlayVertex
    {
        float x;
        float y;
        float r;
        float g;
        float b;
        float a;
    };

    bool ReadDebugWeaponEuler(float& pitch, float& yaw, float& roll) const
    {
        if (!m_debugWeaponOrientationValid)
            return false;
        pitch = m_debugWeaponPitch;
        yaw = m_debugWeaponYaw;
        roll = m_debugWeaponRoll;
        return true;
    }

    bool ReadDebugLeftHandEuler(float& pitch, float& yaw, float& roll) const
    {
        if (!m_debugLeftHandOrientationValid)
            return false;
        pitch = m_debugLeftHandPitch;
        yaw = m_debugLeftHandYaw;
        roll = m_debugLeftHandRoll;
        return true;
    }

    static std::uint8_t DebugSegmentMask(char character)
    {
        switch (character)
        {
        case '0': return 0x3Fu;
        case '1': return 0x06u;
        case '2': return 0x5Bu;
        case '3': return 0x4Fu;
        case '4': return 0x66u;
        case '5': return 0x6Du;
        case '6': return 0x7Du;
        case '7': return 0x07u;
        case '8': return 0x7Fu;
        case '9': return 0x6Fu;
        case '-': return 0x40u;
        case 'P': return 0x73u;
        case 'Y': return 0x6Eu;
        case 'R': return 0x50u;
        default: return 0u;
        }
    }

    static void AddDebugRect(std::vector<DebugOverlayVertex>& vertices,
        float left, float top, float right, float bottom,
        float width, float height, const std::array<float, 4>& color)
    {
        const float x0 = left / width * 2.0f - 1.0f;
        const float x1 = right / width * 2.0f - 1.0f;
        const float y0 = 1.0f - top / height * 2.0f;
        const float y1 = 1.0f - bottom / height * 2.0f;
        const DebugOverlayVertex a = { x0, y0, color[0], color[1],
            color[2], color[3] };
        const DebugOverlayVertex b = { x1, y0, color[0], color[1],
            color[2], color[3] };
        const DebugOverlayVertex c = { x1, y1, color[0], color[1],
            color[2], color[3] };
        const DebugOverlayVertex d = { x0, y1, color[0], color[1],
            color[2], color[3] };
        vertices.insert(vertices.end(), { a, b, c, a, c, d });
    }

    static void AddDebugGlyph(std::vector<DebugOverlayVertex>& vertices,
        char character, float x, float y, float scale, float width,
        float height, const std::array<float, 4>& color)
    {
        const std::uint8_t mask = DebugSegmentMask(character);
        const float t = 2.5f * scale;
        const float w = 13.0f * scale;
        const float h = 22.0f * scale;
        const auto rect = [&](float left, float top, float right, float bottom)
        {
            AddDebugRect(vertices, x + left, y + top, x + right, y + bottom,
                width, height, color);
        };
        if (mask & 0x01u) rect(t, 0.0f, w - t, t);
        if (mask & 0x02u) rect(w - t, t, w, h * 0.5f - t * 0.5f);
        if (mask & 0x04u) rect(w - t, h * 0.5f + t * 0.5f, w, h - t);
        if (mask & 0x08u) rect(t, h - t, w - t, h);
        if (mask & 0x10u) rect(0.0f, h * 0.5f + t * 0.5f, t, h - t);
        if (mask & 0x20u) rect(0.0f, t, t, h * 0.5f - t * 0.5f);
        if (mask & 0x40u) rect(t, h * 0.5f - t * 0.5f,
            w - t, h * 0.5f + t * 0.5f);
    }

    void DrawControllerOrientationDebug(std::uint32_t width,
        std::uint32_t height)
    {
        if (!g_settings.controllerOrientationDebug || !m_debugVertexBuffer ||
            width == 0 || height == 0)
        {
            return;
        }
        std::vector<DebugOverlayVertex> vertices;
        vertices.reserve(2048);
        const float scale = std::clamp(static_cast<float>(height) / 1080.0f,
            0.75f, 1.5f);
        const float panelTop = static_cast<float>(height) * 0.40f;
        const float panelWidth = 155.0f * scale;
        const float panelHeight = 112.0f * scale;
        const char labels[3] = { 'P', 'Y', 'R' };
        const std::array<std::array<float, 4>, 3> colors = {
            std::array<float, 4>{ 0.25f, 0.85f, 1.0f, 1.0f },
            std::array<float, 4>{ 0.40f, 1.0f, 0.45f, 1.0f },
            std::array<float, 4>{ 1.0f, 0.65f, 0.20f, 1.0f }
        };
        const auto addPanel = [&](float panelLeft, const float values[3],
            const std::array<float, 4>& markerColor)
        {
            AddDebugRect(vertices, panelLeft, panelTop,
                panelLeft + panelWidth, panelTop + panelHeight,
                static_cast<float>(width), static_cast<float>(height),
                { 0.01f, 0.01f, 0.015f, 0.78f });
            AddDebugRect(vertices, panelLeft, panelTop,
                panelLeft + panelWidth, panelTop + 4.0f * scale,
                static_cast<float>(width), static_cast<float>(height),
                markerColor);
            for (std::size_t line = 0; line < 3; ++line)
            {
                char text[8] = {};
                snprintf(text, std::size(text), "%c%4d", labels[line],
                    static_cast<int>(std::lround(values[line])));
                const float lineY = panelTop +
                    (10.0f + 33.0f * line) * scale;
                for (std::size_t index = 0; text[index] != '\0'; ++index)
                {
                    AddDebugGlyph(vertices, text[index],
                        panelLeft + (10.0f + 25.0f * index) * scale, lineY,
                        scale, static_cast<float>(width),
                        static_cast<float>(height), colors[line]);
                }
            }
        };

        float weaponValues[3] = {};
        if (ReadDebugWeaponEuler(weaponValues[0], weaponValues[1],
            weaponValues[2]))
        {
            addPanel(static_cast<float>(width) * 0.24f, weaponValues,
                { 0.35f, 0.45f, 1.0f, 1.0f });
        }
        float leftHandValues[3] = {};
        if (ReadDebugLeftHandEuler(leftHandValues[0], leftHandValues[1],
            leftHandValues[2]))
        {
            addPanel(static_cast<float>(width) * 0.54f, leftHandValues,
                { 1.0f, 0.25f, 0.35f, 1.0f });
        }
        if (vertices.empty())
            return;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_debugVertexBuffer.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            return;
        }
        std::memcpy(mapped.pData, vertices.data(),
            vertices.size() * sizeof(DebugOverlayVertex));
        m_context->Unmap(m_debugVertexBuffer.Get(), 0);
        const UINT stride = sizeof(DebugOverlayVertex);
        const UINT offset = 0;
        ID3D11Buffer* vertexBuffer = m_debugVertexBuffer.Get();
        m_context->IASetInputLayout(m_debugInputLayout.Get());
        m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_debugVertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_debugPixelShader.Get(), nullptr, 0);
        m_context->OMSetBlendState(m_debugBlendState.Get(), nullptr,
            0xFFFFFFFFu);
        m_context->Draw(static_cast<UINT>(vertices.size()), 0);
        m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        if (!m_debugOverlayDrawLogged)
        {
            m_debugOverlayDrawLogged = true;
            Log("Matrix P/Y/R overlay active: weapon panel plus left-hand panel (profile %d)",
                m_debugWeaponProfileIndex);
        }
    }

    bool RenderEye(std::size_t eye, const std::array<float, 4>& uvRect)
    {
        EyeSwapchain& target = m_eyes[eye];
        XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        std::uint32_t imageIndex = 0;
        if (!CheckXr(xrAcquireSwapchainImage(target.handle, &acquireInfo, &imageIndex),
            "xrAcquireSwapchainImage"))
            return false;
        XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (!CheckXr(xrWaitSwapchainImage(target.handle, &waitInfo),
            "xrWaitSwapchainImage"))
            return false;

        const float clearColor[] = { 0.02f, 0.02f, 0.02f, 1.0f };
        ID3D11RenderTargetView* renderTarget = target.renderTargets[imageIndex].Get();
        m_context->OMSetRenderTargets(1, &renderTarget, nullptr);
        m_context->ClearRenderTargetView(renderTarget, clearColor);
        const std::size_t sourceEye = m_renderMode == SH3VR_RENDER_IMMERSIVE_STEREO
            ? eye : 0;
        ID3D11ShaderResourceView* sourceView = nullptr;
        const std::size_t nativeSourceIndex =
            static_cast<std::size_t>(m_d3d12EyeActiveSet) * 2 + eye;
        // A non-zero sequence identifies a completed native-eye copy.
        const bool usingNativeEye = m_renderMode != SH3VR_RENDER_CINEMA &&
            m_d3d12EyeFrameSequence != 0 &&
            nativeSourceIndex < m_d3d12EyeViews.size() &&
            m_d3d12EyeViews[nativeSourceIndex];
        if (usingNativeEye)
        {
            sourceView = m_d3d12EyeViews[nativeSourceIndex].Get();
            if (eye == 0 && !m_projectionMappingLogged)
            {
                m_projectionMappingLogged = true;
                Log("Native eye projection mapping: UV %.6f,%.6f to "
                    "%.6f,%.6f; sampled source region %.0fx%.0f of %ux%u",
                    uvRect[0], uvRect[1], uvRect[2], uvRect[3],
                    (uvRect[2] - uvRect[0]) * target.width,
                    (uvRect[3] - uvRect[1]) * target.height,
                    target.width, target.height);
            }
            if (eye == 0 && nativeSourceIndex < m_d3d12EyeTextures.size())
                CaptureEyeDebugTexture(
                    m_d3d12EyeTextures[nativeSourceIndex].Get());
            if (!m_nativeEyeRenderingLogged)
            {
                m_nativeEyeRenderingLogged = true;
                Log("Native per-eye GPU sources selected for projection test");
            }
        }
        else if (m_renderMode != SH3VR_RENDER_IMMERSIVE_STEREO &&
            m_usingD3D12SharedSource &&
            m_d3d12BackBufferIndex < m_d3d12SourceViews.size())
        {
            // The synchronized full-pass path publishes two CPU eye frames.
            // Do not replace them with the identical shared backbuffer.
            sourceView = m_d3d12SourceViews[m_d3d12BackBufferIndex].Get();
        }
        if (!sourceView)
            sourceView = m_sourceViews[sourceEye].Get();

        ID3D11ShaderResourceView* desktopReferenceView = nullptr;
        if (m_usingD3D12SharedSource &&
            m_d3d12BackBufferIndex < m_d3d12SourceViews.size())
        {
            desktopReferenceView =
                m_d3d12SourceViews[m_d3d12BackBufferIndex].Get();
        }
        if (!desktopReferenceView)
            desktopReferenceView = m_sourceViews[0].Get();

        if (usingNativeEye && g_settings.enableGamePostProcess &&
            desktopReferenceView)
        {
            if (m_colorStatsCalibrationPending && eye == 0 &&
                m_colorStatsCalibrationFramesRemaining == 0 &&
                m_colorStatsInitialized[0] &&
                m_colorStatsInitialized[1] &&
                m_colorStatsInitialized[2])
            {
                m_colorStatsCalibrationPending = false;
                if (!m_colorStatsFrozenLogged)
                {
                    m_colorStatsFrozenLogged = true;
                    Log("Post-process color calibration frozen until "
                        "the next immersive scene transition");
                }
            }
            if (!m_colorStatsCalibrationPending &&
                !m_colorStatsInitialized[0])
            {
                BeginColorStatsCalibration("native eye sources became ready");
            }
            if (m_colorStatsCalibrationPending &&
                m_colorStatsCalibrationFramesRemaining != 0)
            {
                if (eye == 0)
                    UpdateColorStats(0, desktopReferenceView, m_uvRect);
                UpdateColorStats(1 + eye, sourceView, uvRect);
                if (eye == 1)
                {
                    --m_colorStatsCalibrationFramesRemaining;
                }
            }
        }

        if (sourceView)
        {
            m_context->OMSetRenderTargets(1, &renderTarget, nullptr);
            D3D11_VIEWPORT viewport = {};
            viewport.Width = static_cast<float>(target.width);
            viewport.Height = static_cast<float>(target.height);
            viewport.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &viewport);
            ID3D11SamplerState* sampler = m_sampler.Get();
            ID3D11Buffer* constants = m_blitConstants.Get();
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
            m_context->VSSetConstantBuffers(0, 1, &constants);
            m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
            m_context->PSSetSamplers(0, 1, &sampler);
            ID3D11Buffer* colorConstants = m_colorConstants.Get();
            m_context->PSSetConstantBuffers(1, 1, &colorConstants);

            const auto drawSource = [&](ID3D11ShaderResourceView* view,
                const std::array<float, 4>& sourceUvRect,
                std::uint32_t sourceWidth, std::uint32_t sourceHeight,
                bool enableFxaa)
            {
                if (!view)
                    return;
                struct BlitConstantsData
                {
                    float uvRect[4];
                    float sourceTexelSize[2];
                    float fxaaEnabled;
                    float decodeSourceSrgb;
                };
                BlitConstantsData constantsData = {};
                std::memcpy(constantsData.uvRect, sourceUvRect.data(),
                    sizeof(constantsData.uvRect));
                const float width = static_cast<float>(sourceWidth != 0
                    ? sourceWidth : target.width);
                const float height = static_cast<float>(sourceHeight != 0
                    ? sourceHeight : target.height);
                constantsData.sourceTexelSize[0] = 1.0f / width;
                constantsData.sourceTexelSize[1] = 1.0f / height;
                constantsData.fxaaEnabled = enableFxaa ? 1.0f : 0.0f;
                constantsData.decodeSourceSrgb = m_swapchainUsesSrgb
                    ? 1.0f : 0.0f;
                struct ColorConstantsData
                {
                    float exposureScale;
                    float contrast;
                    float saturation;
                    float vignetteStrength;
                    float gamePostProcessTransferEnabled;
                    float gamePostProcessTransferStrength;
                    float gamePostProcessContrast;
                    float gamePostProcessExposureScale;
                };
                ColorConstantsData colorData = {};
                if (g_settings.enableColorCorrection)
                {
                    colorData.exposureScale =
                        std::exp2(g_settings.exposureStops);
                    colorData.contrast = g_settings.contrast;
                    colorData.saturation = g_settings.saturation;
                    colorData.vignetteStrength =
                        g_settings.vignetteStrength;
                }
                else
                {
                    colorData.exposureScale = 1.0f;
                    colorData.contrast = 1.0f;
                    colorData.saturation = 1.0f;
                    colorData.vignetteStrength = 0.0f;
                }
                const bool applyGamePostProcess =
                    g_settings.enableGamePostProcess && usingNativeEye &&
                    m_colorStatsInitialized[0] &&
                    m_colorStatsInitialized[1 + eye];
                colorData.gamePostProcessTransferEnabled =
                    applyGamePostProcess ? 1.0f : 0.0f;
                colorData.gamePostProcessTransferStrength =
                    applyGamePostProcess
                    ? g_settings.gamePostProcessStrength : 0.0f;
                colorData.gamePostProcessContrast =
                    g_settings.gamePostProcessContrast;
                colorData.gamePostProcessExposureScale = std::exp2(
                    g_settings.gamePostProcessExposureStops);
                if (applyGamePostProcess && !m_gamePostProcessTransferLogged)
                {
                    m_gamePostProcessTransferLogged = true;
                    Log("Dynamic per-eye SH3 post-process color transfer enabled");
                }
                m_context->UpdateSubresource(m_blitConstants.Get(), 0, nullptr,
                    &constantsData, 0, 0);
                m_context->UpdateSubresource(m_colorConstants.Get(), 0,
                    nullptr, &colorData, 0, 0);
                ID3D11ShaderResourceView* source = view;
                m_context->PSSetShaderResources(0, 1, &source);
                ID3D11ShaderResourceView* statsViews[2] = {
                    applyGamePostProcess ? m_colorStatsViews[0].Get() : nullptr,
                    applyGamePostProcess
                        ? m_colorStatsViews[1 + eye].Get() : nullptr
                };
                m_context->PSSetShaderResources(1, 2, statsViews);
                m_context->Draw(3, 0);
                ID3D11ShaderResourceView* nullViews[3] = {};
                m_context->PSSetShaderResources(0, 3, nullViews);
            };

            drawSource(sourceView, uvRect,
                usingNativeEye ? m_d3d12EyeSourceWidth : m_sourceWidth,
                usingNativeEye ? m_d3d12EyeSourceHeight : m_sourceHeight,
                usingNativeEye && g_settings.enableFxaa);
        }

        // The debug display is composed only after the complete eye image.
        // It cannot modify D3D8 hand/weapon state or either native eye depth.
        DrawControllerOrientationDebug(target.width, target.height);

        m_context->Flush();
        XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        return CheckXr(xrReleaseSwapchainImage(target.handle, &releaseInfo),
            "xrReleaseSwapchainImage");
    }

    void Shutdown()
    {
        for (std::size_t eye = 0; eye < m_sourceTextures.size(); ++eye)
        {
            m_sourceViews[eye].Reset();
            m_sourceTextures[eye].Reset();
        }
        for (std::size_t index = 0; index < m_d3d12SourceTextures.size(); ++index)
        {
            m_d3d12SourceViews[index].Reset();
            m_d3d12SourceTextures[index].Reset();
        }
        for (std::size_t eye = 0; eye < m_d3d12EyeTextures.size(); ++eye)
        {
            m_d3d12EyeViews[eye].Reset();
            m_d3d12EyeTextures[eye].Reset();
        }
        m_vertexShader.Reset();
        m_pixelShader.Reset();
        m_statsPixelShader.Reset();
        m_debugVertexShader.Reset();
        m_debugPixelShader.Reset();
        m_debugInputLayout.Reset();
        m_debugVertexBuffer.Reset();
        m_debugBlendState.Reset();
        m_sampler.Reset();
        m_statsBlendState.Reset();
        for (std::size_t index = 0; index < m_colorStatsTextures.size(); ++index)
        {
            m_colorStatsViews[index].Reset();
            m_colorStatsRenderTargets[index].Reset();
            m_colorStatsTextures[index].Reset();
            m_colorStatsInitialized[index] = false;
        }
        m_blitConstants.Reset();
        m_colorConstants.Reset();
        for (EyeSwapchain& eye : m_eyes)
        {
            eye.renderTargets.clear();
            eye.images.clear();
            if (eye.handle != XR_NULL_HANDLE)
                xrDestroySwapchain(eye.handle);
            eye.handle = XR_NULL_HANDLE;
        }
        ShutdownControllerInput();
        if (m_appSpace != XR_NULL_HANDLE)
            xrDestroySpace(m_appSpace);
        if (m_viewSpace != XR_NULL_HANDLE)
            xrDestroySpace(m_viewSpace);
        if (m_session != XR_NULL_HANDLE)
            xrDestroySession(m_session);
        m_context.Reset();
        m_device.Reset();
        if (m_instance != XR_NULL_HANDLE)
            xrDestroyInstance(m_instance);
        m_appSpace = XR_NULL_HANDLE;
        m_viewSpace = XR_NULL_HANDLE;
        m_session = XR_NULL_HANDLE;
        m_instance = XR_NULL_HANDLE;
    }

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_appSpace = XR_NULL_HANDLE;
    XrSpace m_viewSpace = XR_NULL_HANDLE;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
    XrActionSet m_controllerActionSet = XR_NULL_HANDLE;
    XrAction m_thumbstickAction = XR_NULL_HANDLE;
    XrAction m_triggerAction = XR_NULL_HANDLE;
    XrAction m_squeezeAction = XR_NULL_HANDLE;
    XrAction m_stickClickAction = XR_NULL_HANDLE;
    XrAction m_gripPoseAction = XR_NULL_HANDLE;
    XrAction m_aimPoseAction = XR_NULL_HANDLE;
    XrAction m_buttonAAction = XR_NULL_HANDLE;
    XrAction m_buttonBAction = XR_NULL_HANDLE;
    XrAction m_buttonXAction = XR_NULL_HANDLE;
    XrAction m_buttonYAction = XR_NULL_HANDLE;
    XrAction m_menuAction = XR_NULL_HANDLE;
    std::array<XrPath, 2> m_handPaths = { XR_NULL_PATH, XR_NULL_PATH };
    std::array<XrSpace, 2> m_gripSpaces = {
        XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::array<XrSpace, 2> m_aimSpaces = {
        XR_NULL_HANDLE, XR_NULL_HANDLE };
    bool m_controllerInputInitialized = false;
    bool m_controllerSyncFailureLogged = false;
    bool m_debugWeaponOrientationValid = false;
    float m_debugWeaponPitch = 0.0f;
    float m_debugWeaponYaw = 0.0f;
    float m_debugWeaponRoll = 0.0f;
    std::int32_t m_debugWeaponProfileIndex = -1;
    bool m_debugLeftHandOrientationValid = false;
    float m_debugLeftHandPitch = 0.0f;
    float m_debugLeftHandYaw = 0.0f;
    float m_debugLeftHandRoll = 0.0f;
    bool m_debugOverlayDrawLogged = false;
    bool m_sessionRunning = false;
    bool m_shouldRender = false;
    bool m_exitRequested = false;
    bool m_retryAllowed = true;
    bool m_refreshRateExtensionEnabled = false;
    bool m_headLocateFailureLogged = false;
    PFN_xrEnumerateDisplayRefreshRatesFB m_enumerateRefreshRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB m_getRefreshRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB m_requestRefreshRate = nullptr;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    std::array<EyeSwapchain, 2> m_eyes;
    std::array<ComPtr<ID3D11Texture2D>, 2> m_sourceTextures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> m_sourceViews;
    std::array<ComPtr<ID3D11Texture2D>, 2> m_d3d12SourceTextures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> m_d3d12SourceViews;
    std::array<ComPtr<ID3D11Texture2D>, 4> m_d3d12EyeTextures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> m_d3d12EyeViews;
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11PixelShader> m_statsPixelShader;
    ComPtr<ID3D11VertexShader> m_debugVertexShader;
    ComPtr<ID3D11PixelShader> m_debugPixelShader;
    ComPtr<ID3D11InputLayout> m_debugInputLayout;
    ComPtr<ID3D11Buffer> m_debugVertexBuffer;
    ComPtr<ID3D11BlendState> m_debugBlendState;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11BlendState> m_statsBlendState;
    std::array<ComPtr<ID3D11Texture2D>, 3> m_colorStatsTextures;
    std::array<ComPtr<ID3D11RenderTargetView>, 3>
        m_colorStatsRenderTargets;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> m_colorStatsViews;
    std::array<bool, 3> m_colorStatsInitialized = {};
    std::uint32_t m_colorStatsCalibrationFramesRemaining = 0;
    bool m_colorStatsCalibrationPending = false;
    bool m_colorStatsFrozenLogged = false;
    ComPtr<ID3D11Buffer> m_blitConstants;
    ComPtr<ID3D11Buffer> m_colorConstants;
    std::uint32_t m_sourceWidth = 0;
    std::uint32_t m_sourceHeight = 0;
    std::array<float, 4> m_uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    float m_frameAspect = 16.0f / 9.0f;
    std::int32_t m_renderMode = SH3VR_RENDER_CINEMA;
    Sh3VrHeadPose m_frameRenderPose = {};
    bool m_haveFrameRenderPose = false;
    bool m_frameRenderPoseLogged = false;
    std::uint32_t m_lastD3D12HandleGeneration = 0;
    std::uint32_t m_lastD3D12EyeTextureGeneration = 0;
    std::uint32_t m_d3d12EyeActiveSet = 0;
    std::uint32_t m_d3d12EyeFrameSequence = 0;
    std::uint32_t m_d3d12EyeSourceWidth = 0;
    std::uint32_t m_d3d12EyeSourceHeight = 0;
    bool m_swapchainUsesSrgb = false;
    std::uint32_t m_d3d12BackBufferIndex = 0;
    bool m_usingD3D12SharedSource = false;
    bool m_nativeEyeRenderingLogged = false;
    bool m_projectionMappingLogged = false;
    bool m_nativeEyePoseMatchLogged = false;
    bool m_eyeDebugCaptured = false;
    bool m_gamePostProcessEnabled = false;
    bool m_gamePostProcessLogged = false;
    bool m_gamePostProcessTransferLogged = false;
    std::array<float, 3> m_gamePostProcessScale = { 1.0f, 1.0f, 1.0f };
    float m_gamePostProcessBlend = 0.0f;
};

void OpenLogFile()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetHostDirectory(path, std::size(path)))
        return;
    wcscat_s(path, L"sh3vr_host64.log");
    _wfopen_s(&g_log, path, L"w");
}

} // namespace

int wmain(int argumentCount, wchar_t** arguments)
{
    OpenLogFile();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    Log("SH3VR x64 host starting");
    LoadSettings();

    DWORD parentPid = 0;
    for (int index = 1; index + 1 < argumentCount; ++index)
    {
        if (wcscmp(arguments[index], L"--parent-pid") == 0)
            parentPid = wcstoul(arguments[index + 1], nullptr, 10);
    }

    HANDLE parentProcess = nullptr;
    if (parentPid != 0)
    {
        parentProcess = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        if (parentProcess)
            Log("Monitoring game process PID %u", parentPid);
        else
            Log("Could not open game process PID %u, error %u", parentPid, GetLastError());
    }

    const auto parentIsAlive = [&]()
    {
        return !parentProcess || WaitForSingleObject(parentProcess, 0) == WAIT_TIMEOUT;
    };

    std::unique_ptr<OpenXrHost> host;
    while (g_keepRunning && parentIsAlive() && !host)
    {
        auto candidate = std::make_unique<OpenXrHost>();
        if (candidate->Initialize())
        {
            host = std::move(candidate);
            break;
        }

        if (!candidate->RetryAllowed())
        {
            Log("OpenXR initialization encountered a permanent graphics error; "
                "automatic retries are disabled");
            break;
        }

        Log("OpenXR initialization failed; retrying in 2 seconds");
        for (int step = 0; step < 20 && g_keepRunning && parentIsAlive(); ++step)
            Sleep(100);
    }

    if (!host)
    {
        Log("SH3VR x64 host stopped before OpenXR initialized");
        if (g_log)
            fclose(g_log);
        return 1;
    }

    SharedFrameConsumer consumer;
    bool reportedWaiting = false;
    while (g_keepRunning && parentIsAlive())
    {
        const bool connected = consumer.TryConnect();
        if (!connected && !reportedWaiting)
        {
            Log("Waiting for the 32-bit game frame producer");
            reportedWaiting = true;
        }
        if (connected)
        {
            consumer.PublishRequestedEyeResolution(host->EyeWidth(),
                host->EyeHeight(), g_settings.eyeSamples);
        }
        if (!host->PumpFrame(consumer))
            break;
    }

    Log("SH3VR x64 host stopping");
    if (parentProcess)
        CloseHandle(parentProcess);
    if (g_log)
        fclose(g_log);
    return 0;
}
