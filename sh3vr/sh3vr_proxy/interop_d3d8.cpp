// =============================================================================
//  sh3vr - interop_d3d8.cpp  (version 3)
//
//  Frame producer for a genuine Direct3D 8 device.
//
//  The process contains no D3D9, D3D11, DXGI or Vulkan module: the game renders
//  through the stock Direct3D 8 runtime and the NVIDIA user mode driver. D3D8
//  has no shared surfaces, so the frame is taken through system memory:
//
//    IDirect3DDevice8::GetBackBuffer      (slot 16)
//      -> IDirect3DDevice8::CopyRects     (slot 28)  into a SYSTEMMEM surface
//        -> IDirect3DSurface8::LockRect   (slot 9)
//          -> memcpy into a shared memory section
//            -> SetEvent, picked up by sh3vr_host64.exe
//
//  Nothing here touches OpenXR or D3D11: the 32 bit game process stays clean
//  and every headset facing call happens in the 64 bit host.
// =============================================================================

#include <windows.h>
#include "shared_frame.h"

extern void Log(const char* format, ...);

struct IDirect3DDevice8;
struct IDirect3DSurface8;

// -----------------------------------------------------------------------------
//  Direct3D 8 constants and structures (the modern SDK no longer ships d3d8.h)
// -----------------------------------------------------------------------------

enum
{
    VT8_GetBackBuffer = 16,
    VT8_CreateImageSurface = 27,
    VT8_CopyRects = 28,
    VT8_GetRenderTarget = 32
};

enum
{
    VTS8_Release = 2,
    VTS8_GetDesc = 8,
    VTS8_LockRect = 9,
    VTS8_UnlockRect = 10
};

enum { D3D8_BACKBUFFER_TYPE_MONO = 0 };
enum { D3D8_LOCK_READONLY = 0x00000010 };

// Direct3D 8 layout, which differs from Direct3D 9: Size sits between Pool and
// MultiSampleType, and Width and Height come last.
struct D3D8SurfaceDesc
{
    DWORD Format;
    DWORD Type;
    DWORD Usage;
    DWORD Pool;
    UINT  Size;
    DWORD MultiSampleType;
    UINT  Width;
    UINT  Height;
};

struct D3D8LockedRect
{
    INT   Pitch;
    void* pBits;
};

typedef HRESULT(WINAPI* PFN8_GetBackBuffer)(IDirect3DDevice8*, UINT, DWORD,
    IDirect3DSurface8**);
typedef HRESULT(WINAPI* PFN8_GetRenderTarget)(IDirect3DDevice8*, IDirect3DSurface8**);
typedef HRESULT(WINAPI* PFN8_CreateImageSurface)(IDirect3DDevice8*, UINT, UINT, DWORD,
    IDirect3DSurface8**);
typedef HRESULT(WINAPI* PFN8_CopyRects)(IDirect3DDevice8*, IDirect3DSurface8*,
    const RECT*, UINT, IDirect3DSurface8*,
    const POINT*);

typedef ULONG(WINAPI* PFNS8_Release)(IDirect3DSurface8*);
typedef HRESULT(WINAPI* PFNS8_GetDesc)(IDirect3DSurface8*, D3D8SurfaceDesc*);
typedef HRESULT(WINAPI* PFNS8_LockRect)(IDirect3DSurface8*, D3D8LockedRect*,
    const RECT*, DWORD);
typedef HRESULT(WINAPI* PFNS8_UnlockRect)(IDirect3DSurface8*);

// Calls a COM method by vtable slot, which avoids declaring every preceding
// method of these interfaces.
template <typename T>
static T Slot(void* object, int index)
{
    void** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<T>(vtable[index]);
}

static void ReleaseSurface(IDirect3DSurface8* surface)
{
    if (surface)
        Slot<PFNS8_Release>(surface, VTS8_Release)(surface);
}

static const char* FormatName(DWORD format)
{
    switch (format)
    {
    case 20: return "R8G8B8";
    case 21: return "A8R8G8B8";
    case 22: return "X8R8G8B8";
    case 23: return "R5G6B5";
    case 24: return "X1R5G5B5";
    case 25: return "A1R5G5B5";
    default: return "other";
    }
}

// -----------------------------------------------------------------------------
//  State
// -----------------------------------------------------------------------------

static IDirect3DSurface8* g_staging = nullptr;
static IDirect3DSurface8* g_stereoStaging[2] = {};
static UINT               g_width = 0;
static UINT               g_height = 0;
static DWORD              g_format = 0;
static bool               g_initialised = false;
static bool               g_broken = false;
static Sh3VrHeadPose       g_frameRenderPose = {};

static HANDLE            g_section = nullptr;
static HANDLE            g_frameEvent = nullptr;
static Sh3VrFrameHeader* g_header = nullptr;
static BYTE* g_slotBase = nullptr;
static std::uint64_t g_d3d12BackBufferHandles[2] = {};
static std::uint32_t g_d3d12BackBufferIndex = 0;
static std::uint32_t g_d3d12BackBufferCount = 0;
static std::uint64_t g_d3d12EyeTextureHandles[4] = {};
static std::uint32_t g_d3d12EyeTextureWidth = 0;
static std::uint32_t g_d3d12EyeTextureHeight = 0;
static std::uint32_t g_d3d12EyeTextureFormat = 0;
static std::uint32_t g_d3d12EyeTextureActiveSet = 0;
static std::uint32_t g_d3d12EyeTextureFrameSequence = 0;
static Sh3VrHeadPose g_d3d12EyeFrameRenderPoses[2] = {};
static std::uint32_t g_gamePostProcessEnabled = 0;
static float g_gamePostProcessScale[3] = { 1.0f, 1.0f, 1.0f };
static float g_gamePostProcessBlend = 0.0f;
static std::uint32_t g_gamePostProcessSource[4] = {};
static std::uint32_t g_gamePostProcessIntensity = 0;

static LONG   g_frameCounter = 0;
static LONG64 g_qpcFrequency = 0;
static LONG64 g_captureTicks = 0;   // accumulated cost, reported periodically
static LONG   g_captureCalls = 0;
static volatile LONG g_renderMode = SH3VR_RENDER_CINEMA;
static volatile LONG g_renderFlags = SH3VR_RENDER_FLAG_NONE;
static volatile LONG g_renderEye = 0;
static bool g_stereoPairOpen = false;
static LONG g_stereoPairCounter = 0;

void Interop8_SetRenderMode(std::uint32_t mode)
{
    InterlockedExchange(&g_renderMode, static_cast<LONG>(mode));
    if (g_header)
    {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_header->renderMode),
            static_cast<LONG>(mode));
    }
}

void Interop8_SetRenderFlags(std::uint32_t flags)
{
    InterlockedExchange(&g_renderFlags, static_cast<LONG>(flags));
    if (g_header)
    {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(
            &g_header->renderFlags), static_cast<LONG>(flags));
    }
}

void Interop8_SetRenderEye(std::uint32_t eye)
{
    InterlockedExchange(&g_renderEye, static_cast<LONG>(eye));
    if (g_header)
    {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_header->renderEye),
            static_cast<LONG>(eye));
    }
}

void Interop8_SetGamePostProcess(bool enabled, const float scale[3],
    float blend, const std::uint32_t source[4], std::uint32_t intensity)
{
    g_gamePostProcessEnabled = enabled ? 1u : 0u;
    for (std::uint32_t channel = 0; channel < 3; ++channel)
    {
        g_gamePostProcessScale[channel] = scale
            ? scale[channel] : 1.0f;
    }
    g_gamePostProcessBlend = blend;
    for (std::uint32_t channel = 0; channel < 4; ++channel)
    {
        g_gamePostProcessSource[channel] = source
            ? source[channel] : 0u;
    }
    g_gamePostProcessIntensity = intensity;

    if (g_header)
    {
        g_header->gamePostProcessEnabled = g_gamePostProcessEnabled;
        for (std::uint32_t channel = 0; channel < 3; ++channel)
        {
            g_header->gamePostProcessScale[channel] =
                g_gamePostProcessScale[channel];
        }
        g_header->gamePostProcessBlend = g_gamePostProcessBlend;
        for (std::uint32_t channel = 0; channel < 4; ++channel)
        {
            g_header->gamePostProcessSource[channel] =
                g_gamePostProcessSource[channel];
        }
        g_header->gamePostProcessIntensity = g_gamePostProcessIntensity;
    }
}

void Interop8_SetD3D12BackBufferHandles(HANDLE first, HANDLE second,
    std::uint32_t currentIndex, std::uint32_t bufferCount)
{
    const std::uint64_t handles[2] =
    {
        static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(first)),
        static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(second))
    };
    const std::uint32_t clampedCount = bufferCount > 2 ? 2 : bufferCount;
    const std::uint32_t clampedIndex = currentIndex < clampedCount
        ? currentIndex : 0;
    const bool handlesChanged =
        g_d3d12BackBufferHandles[0] != handles[0] ||
        g_d3d12BackBufferHandles[1] != handles[1] ||
        g_d3d12BackBufferCount != clampedCount;

    g_d3d12BackBufferHandles[0] = handles[0];
    g_d3d12BackBufferHandles[1] = handles[1];
    g_d3d12BackBufferIndex = clampedIndex;
    g_d3d12BackBufferCount = clampedCount;

    if (g_header)
    {
        g_header->d3d12BackBufferHandles[0] = g_d3d12BackBufferHandles[0];
        g_header->d3d12BackBufferHandles[1] = g_d3d12BackBufferHandles[1];
        g_header->d3d12BackBufferIndex = g_d3d12BackBufferIndex;
        g_header->d3d12BackBufferCount = g_d3d12BackBufferCount;
        if (handlesChanged)
        {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                &g_header->d3d12BackBufferGeneration));
        }
        MemoryBarrier();
    }
}

void Interop8_SetD3D12EyeTextureHandles(HANDLE set0Left, HANDLE set0Right,
    HANDLE set1Left, HANDLE set1Right, std::uint32_t width,
    std::uint32_t height, std::uint32_t format)
{
    const std::uint64_t handles[4] =
    {
        static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(set0Left)),
        static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(set0Right)),
        static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(set1Left)),
        static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(set1Right))
    };
    const bool changed =
        g_d3d12EyeTextureHandles[0] != handles[0] ||
        g_d3d12EyeTextureHandles[1] != handles[1] ||
        g_d3d12EyeTextureHandles[2] != handles[2] ||
        g_d3d12EyeTextureHandles[3] != handles[3] ||
        g_d3d12EyeTextureWidth != width ||
        g_d3d12EyeTextureHeight != height ||
        g_d3d12EyeTextureFormat != format;

    for (UINT index = 0; index < 4; ++index)
        g_d3d12EyeTextureHandles[index] = handles[index];
    g_d3d12EyeTextureWidth = width;
    g_d3d12EyeTextureHeight = height;
    g_d3d12EyeTextureFormat = format;

    if (g_header)
    {
        for (UINT index = 0; index < 4; ++index)
            g_header->d3d12EyeTextureHandles[index] = handles[index];
        g_header->d3d12EyeTextureWidth = width;
        g_header->d3d12EyeTextureHeight = height;
        g_header->d3d12EyeTextureFormat = format;
        if (changed)
        {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                &g_header->d3d12EyeTextureGeneration));
        }
        MemoryBarrier();
    }
}

void Interop8_SetD3D12EyeTextureFrame(std::uint32_t activeSet,
    std::uint32_t frameSequence, const Sh3VrHeadPose& renderPose)
{
    g_d3d12EyeTextureActiveSet = activeSet < 2 ? activeSet : 0;
    g_d3d12EyeTextureFrameSequence = frameSequence;
    g_d3d12EyeFrameRenderPoses[g_d3d12EyeTextureActiveSet] = renderPose;
    if (g_header)
    {
        g_header->d3d12EyeFrameRenderPoses[g_d3d12EyeTextureActiveSet] =
            renderPose;
        // Publish the completion sequence before switching the active set.
        // The host can sample this header while the producer is writing the
        // next shared eye pair. Keeping the old set visible until the new
        // sequence is observable prevents one frame of partially copied eye
        // textures from reaching OpenXR.
        MemoryBarrier();
        g_header->d3d12EyeTextureFrameSequence =
            g_d3d12EyeTextureFrameSequence;
        MemoryBarrier();
        g_header->d3d12EyeTextureActiveSet = g_d3d12EyeTextureActiveSet;
    }
}

bool Interop8_GetRequestedEyeResolution(std::uint32_t* width,
    std::uint32_t* height, std::uint32_t* sampleCount)
{
    if (!g_header || !width || !height || !sampleCount)
        return false;

    MemoryBarrier();
    const std::uint32_t requestedWidth = g_header->requestedEyeWidth;
    const std::uint32_t requestedHeight = g_header->requestedEyeHeight;
    const std::uint32_t requestedSamples = g_header->requestedEyeSampleCount;
    if (requestedWidth == 0 || requestedHeight == 0 ||
        requestedSamples == 0)
    {
        return false;
    }

    *width = requestedWidth;
    *height = requestedHeight;
    *sampleCount = requestedSamples;
    return true;
}

bool Interop8_ReadControllerState(Sh3VrControllerState* state)
{
    if (!g_header || !state || g_header->producerAlive == 0)
        return false;

    const std::int32_t sequenceBefore = g_header->controllerStateSequence;
    if ((sequenceBefore & 1) != 0)
        return false;

    MemoryBarrier();
    memcpy(state, &g_header->controllerState, sizeof(*state));
    MemoryBarrier();

    const std::int32_t sequenceAfter = g_header->controllerStateSequence;
    return sequenceBefore == sequenceAfter && (sequenceAfter & 1) == 0;
}

// -----------------------------------------------------------------------------
//  Shared memory
// -----------------------------------------------------------------------------

static bool SharedFrame_Init()
{
    if (g_header)
        return true;

    g_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, SH3VR_SECTION_BYTES, SH3VR_SECTION_NAME);
    if (!g_section)
    {
        Log("SharedFrame_Init: CreateFileMapping failed, error %u", GetLastError());
        return false;
    }

    const bool reused = (GetLastError() == ERROR_ALREADY_EXISTS);

    g_header = static_cast<Sh3VrFrameHeader*>(
        MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, 0, 0, SH3VR_SECTION_BYTES));
    if (!g_header)
    {
        Log("SharedFrame_Init: MapViewOfFile failed, error %u", GetLastError());
        CloseHandle(g_section);
        g_section = nullptr;
        return false;
    }

    g_slotBase = reinterpret_cast<BYTE*>(g_header) + SH3VR_HEADER_BYTES;

    g_frameEvent = CreateEventW(nullptr, FALSE, FALSE, SH3VR_EVENT_NAME);
    if (!g_frameEvent)
        Log("SharedFrame_Init: CreateEvent failed, error %u", GetLastError());

    ZeroMemory(g_header, sizeof(Sh3VrFrameHeader));
    g_header->magic = SH3VR_MAGIC;
    g_header->version = SH3VR_VERSION;
    g_header->headerBytes = SH3VR_HEADER_BYTES;
    g_header->slotCount = SH3VR_SLOT_COUNT;
    g_header->slotBytes = SH3VR_SLOT_BYTES;
    g_header->pixelLayout = SH3VR_PIXEL_BGRA8;
    g_header->producerPid = GetCurrentProcessId();
    g_header->producerAlive = 1;
    g_header->renderMode = g_renderMode;
    g_header->renderFlags = g_renderFlags;
    g_header->renderEye = g_renderEye;
    g_header->d3d12BackBufferHandles[0] = g_d3d12BackBufferHandles[0];
    g_header->d3d12BackBufferHandles[1] = g_d3d12BackBufferHandles[1];
    g_header->d3d12BackBufferIndex = g_d3d12BackBufferIndex;
    g_header->d3d12BackBufferCount = g_d3d12BackBufferCount;
    for (UINT index = 0; index < 4; ++index)
    {
        g_header->d3d12EyeTextureHandles[index] =
            g_d3d12EyeTextureHandles[index];
    }
    g_header->d3d12EyeTextureWidth = g_d3d12EyeTextureWidth;
    g_header->d3d12EyeTextureHeight = g_d3d12EyeTextureHeight;
    g_header->d3d12EyeTextureFormat = g_d3d12EyeTextureFormat;
    g_header->d3d12EyeTextureActiveSet = g_d3d12EyeTextureActiveSet;
    g_header->d3d12EyeTextureFrameSequence =
        g_d3d12EyeTextureFrameSequence;
    g_header->d3d12EyeFrameRenderPoses[0] =
        g_d3d12EyeFrameRenderPoses[0];
    g_header->d3d12EyeFrameRenderPoses[1] =
        g_d3d12EyeFrameRenderPoses[1];
    g_header->gamePostProcessEnabled = g_gamePostProcessEnabled;
    for (UINT channel = 0; channel < 3; ++channel)
    {
        g_header->gamePostProcessScale[channel] =
            g_gamePostProcessScale[channel];
    }
    g_header->gamePostProcessBlend = g_gamePostProcessBlend;
    for (UINT channel = 0; channel < 4; ++channel)
    {
        g_header->gamePostProcessSource[channel] =
            g_gamePostProcessSource[channel];
    }
    g_header->gamePostProcessIntensity = g_gamePostProcessIntensity;

    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&g_qpcFrequency));
    g_header->qpcFrequency = g_qpcFrequency;

    Log("SharedFrame_Init: section %s, %u bytes, %u slots of %u bytes",
        reused ? "reused" : "created",
        (unsigned)SH3VR_SECTION_BYTES, (unsigned)SH3VR_SLOT_COUNT,
        (unsigned)SH3VR_SLOT_BYTES);
    return true;
}

// Copies one locked surface into the free slot and publishes it.
static void SharedFrame_Publish(const BYTE* source, INT sourcePitch,
    UINT left, UINT top, UINT width, UINT height)
{
    if (!g_header)
        return;

    const UINT slot = (g_header->publishedFrame + 1) % SH3VR_SLOT_COUNT;
    const UINT destPitch = g_width * SH3VR_BYTES_PER_PX;
    BYTE* dest = g_slotBase + (SIZE_T)slot * SH3VR_SLOT_BYTES;

    if ((SIZE_T)destPitch * g_height > SH3VR_SLOT_BYTES)
    {
        Log("SharedFrame_Publish: frame %ux%u exceeds the slot capacity",
            g_width, g_height);
        g_broken = true;
        return;
    }

    // The staging pitch equals width * 4 in practice, but a single memcpy is
    // only valid when the pitches match exactly.
    if (sourcePitch == (INT)destPitch)
    {
        memcpy(dest, source, (SIZE_T)destPitch * g_height);
    }
    else
    {
        for (UINT y = 0; y < g_height; ++y)
            memcpy(dest + (SIZE_T)y * destPitch,
                source + (SIZE_T)y * sourcePitch,
                destPitch);
    }

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);

    g_header->width = g_width;
    g_header->height = g_height;
    g_header->pitch = destPitch;
    g_header->contentLeft = left;
    g_header->contentTop = top;
    g_header->contentWidth = width;
    g_header->contentHeight = height;
    g_header->qpcCapture = now.QuadPart;
    g_header->frameRenderPose = g_frameRenderPose;

    // Publish last, with a barrier, so the consumer never sees a partial slot.
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_header->publishedFrame),
        (LONG)(g_header->publishedFrame + 1));

    if (g_frameEvent)
        SetEvent(g_frameEvent);
}

// Publishes timing and pose metadata without reading the D3D8 render target.
// The x64 host uses this path when it can sample the D3D12On9 swapchain
// backbuffer directly. Cinema mode keeps the CPU path because it still needs
// black-border detection for the source content rectangle.
static void SharedFrame_PublishGpuMetadata()
{
    if (!g_header || g_width == 0 || g_height == 0)
        return;

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);

    g_header->width = g_width;
    g_header->height = g_height;
    g_header->pitch = g_width * SH3VR_BYTES_PER_PX;
    g_header->contentLeft = 0;
    g_header->contentTop = 0;
    g_header->contentWidth = g_width;
    g_header->contentHeight = g_height;
    g_header->qpcCapture = now.QuadPart;
    g_header->frameRenderPose = g_frameRenderPose;

    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_header->publishedFrame),
        static_cast<LONG>(g_header->publishedFrame + 1));

    if (g_frameEvent)
        SetEvent(g_frameEvent);
}

static bool SharedFrame_CopyStereoEye(const BYTE* source, INT sourcePitch,
    UINT eye, UINT left, UINT top, UINT width, UINT height)
{
    if (!g_header || eye > 1)
        return false;

    volatile LONG* pairSequence = reinterpret_cast<volatile LONG*>(
        &g_header->stereoPairSequence);
    volatile LONG* readyMask = reinterpret_cast<volatile LONG*>(
        &g_header->stereoReadyMask);
    volatile LONG* publishedFrame = reinterpret_cast<volatile LONG*>(
        &g_header->publishedFrame);

    if (eye == 0)
    {
        const LONG sequence = InterlockedCompareExchange(pairSequence, 0, 0);
        if ((sequence & 1) != 0)
            InterlockedIncrement(pairSequence);
        InterlockedExchange(readyMask, 0);
        InterlockedIncrement(pairSequence);
        g_stereoPairOpen = true;
    }
    else if (!g_stereoPairOpen)
    {
        return false;
    }

    const UINT destPitch = g_width * SH3VR_BYTES_PER_PX;
    BYTE* dest = g_slotBase + static_cast<SIZE_T>(eye) * SH3VR_SLOT_BYTES;
    if (static_cast<SIZE_T>(destPitch) * g_height > SH3VR_SLOT_BYTES)
        return false;

    if (sourcePitch == static_cast<INT>(destPitch))
    {
        memcpy(dest, source, static_cast<SIZE_T>(destPitch) * g_height);
    }
    else
    {
        for (UINT y = 0; y < g_height; ++y)
        {
            memcpy(dest + static_cast<SIZE_T>(y) * destPitch,
                source + static_cast<SIZE_T>(y) * sourcePitch, destPitch);
        }
    }

    InterlockedOr(readyMask, 1 << eye);
    if (eye == 0)
        return true;

    g_header->width = g_width;
    g_header->height = g_height;
    g_header->pitch = destPitch;
    g_header->contentLeft = left;
    g_header->contentTop = top;
    g_header->contentWidth = width;
    g_header->contentHeight = height;
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    g_header->qpcCapture = now.QuadPart;
    g_header->frameRenderPose = g_frameRenderPose;
    g_header->renderMode = SH3VR_RENDER_IMMERSIVE_STEREO;
    g_header->renderEye = 1;

    MemoryBarrier();
    InterlockedIncrement(pairSequence);
    InterlockedIncrement(publishedFrame);
    g_stereoPairOpen = false;
    const LONG pair = InterlockedIncrement(&g_stereoPairCounter);
    if (pair == 1 || pair == 60 || pair % 600 == 0)
    {
        Log("Synchronized stereo pair %d published at %ux%u", pair,
            g_width, g_height);
    }
    if (g_frameEvent)
        SetEvent(g_frameEvent);
    return true;
}

static void SharedFrame_Shutdown()
{
    if (g_header)
    {
        g_header->producerAlive = 0;
        MemoryBarrier();
        if (g_frameEvent)
            SetEvent(g_frameEvent);   // let the host notice immediately
        UnmapViewOfFile(g_header);
        g_header = nullptr;
    }

    if (g_frameEvent)
    {
        CloseHandle(g_frameEvent);
        g_frameEvent = nullptr;
    }

    if (g_section)
    {
        CloseHandle(g_section);
        g_section = nullptr;
    }

    g_slotBase = nullptr;
}

bool Interop8_ReadHeadPose(Sh3VrHeadPose* output)
{
    if (!g_header || !output)
        return false;

    volatile LONG* sequence = reinterpret_cast<volatile LONG*>(
        &g_header->headPoseSequence);

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const LONG before = InterlockedCompareExchange(sequence, 0, 0);
        if ((before & 1) != 0)
            continue;

        MemoryBarrier();
        Sh3VrHeadPose sample = {};
        memcpy(&sample, &g_header->headPose, sizeof(sample));
        MemoryBarrier();

        const LONG after = InterlockedCompareExchange(sequence, 0, 0);
        if (before == after && (after & 1) == 0)
        {
            *output = sample;
            return sample.flags != SH3VR_POSE_NONE;
        }
    }

    return false;
}

void Interop8_SetFrameRenderPose(const Sh3VrHeadPose& pose)
{
    g_frameRenderPose = pose;
}

// -----------------------------------------------------------------------------
//  Frame source
// -----------------------------------------------------------------------------

// Returns the current frame source, preferring the back buffer and falling back
// to render target zero.
static IDirect3DSurface8* AcquireFrameSource(IDirect3DDevice8* device)
{
    IDirect3DSurface8* surface = nullptr;

    HRESULT hr = Slot<PFN8_GetBackBuffer>(device, VT8_GetBackBuffer)(
        device, 0, D3D8_BACKBUFFER_TYPE_MONO, &surface);
    if (SUCCEEDED(hr) && surface)
        return surface;

    hr = Slot<PFN8_GetRenderTarget>(device, VT8_GetRenderTarget)(device, &surface);
    if (SUCCEEDED(hr) && surface)
        return surface;

    Log("AcquireFrameSource: no frame source available, hr = 0x%08X", (unsigned)hr);
    return nullptr;
}

bool Interop8_Init(IDirect3DDevice8* device)
{
    if (g_initialised || g_broken || !device)
        return g_initialised;

    IDirect3DSurface8* source = AcquireFrameSource(device);
    if (!source)
    {
        g_broken = true;
        return false;
    }

    D3D8SurfaceDesc desc = {};
    HRESULT hr = Slot<PFNS8_GetDesc>(source, VTS8_GetDesc)(source, &desc);
    ReleaseSurface(source);

    if (FAILED(hr))
    {
        Log("Interop8_Init: GetDesc failed, hr = 0x%08X", (unsigned)hr);
        g_broken = true;
        return false;
    }

    Log("--- D3D8 frame source ---");
    Log("  size            : %ux%u", desc.Width, desc.Height);
    Log("  format          : %u (%s)", (unsigned)desc.Format, FormatName(desc.Format));
    Log("  usage           : 0x%08X, pool %u, multisample %u",
        (unsigned)desc.Usage, (unsigned)desc.Pool, (unsigned)desc.MultiSampleType);

    if (desc.MultiSampleType != 0)
        Log("  WARNING: multisampled source, CopyRects may refuse it");

    if (desc.Width > SH3VR_MAX_WIDTH || desc.Height > SH3VR_MAX_HEIGHT)
    {
        Log("  ERROR: %ux%u exceeds the shared section limit of %ux%u. Raise "
            "SH3VR_MAX_WIDTH and SH3VR_MAX_HEIGHT and rebuild both sides.",
            desc.Width, desc.Height,
            (unsigned)SH3VR_MAX_WIDTH, (unsigned)SH3VR_MAX_HEIGHT);
        g_broken = true;
        return false;
    }

    hr = Slot<PFN8_CreateImageSurface>(device, VT8_CreateImageSurface)(
        device, desc.Width, desc.Height, desc.Format, &g_staging);
    if (FAILED(hr) || !g_staging)
    {
        Log("Interop8_Init: CreateImageSurface failed, hr = 0x%08X", (unsigned)hr);
        g_broken = true;
        return false;
    }

    for (UINT eye = 0; eye < 2; ++eye)
    {
        hr = Slot<PFN8_CreateImageSurface>(device, VT8_CreateImageSurface)(
            device, desc.Width, desc.Height, desc.Format,
            &g_stereoStaging[eye]);
        if (FAILED(hr) || !g_stereoStaging[eye])
        {
            Log("Interop8_Init: stereo staging surface %u failed, hr = 0x%08X",
                eye, static_cast<unsigned>(hr));
            ReleaseSurface(g_staging);
            g_staging = nullptr;
            for (UINT releaseEye = 0; releaseEye < 2; ++releaseEye)
            {
                ReleaseSurface(g_stereoStaging[releaseEye]);
                g_stereoStaging[releaseEye] = nullptr;
            }
            g_broken = true;
            return false;
        }
    }

    g_width = desc.Width;
    g_height = desc.Height;
    g_format = desc.Format;

    if (!SharedFrame_Init())
    {
        ReleaseSurface(g_staging);
        g_staging = nullptr;
        for (UINT eye = 0; eye < 2; ++eye)
        {
            ReleaseSurface(g_stereoStaging[eye]);
            g_stereoStaging[eye] = nullptr;
        }
        g_broken = true;
        return false;
    }

    g_initialised = true;
    Log("Interop8_Init: staging surface at 0x%08X, producer ready",
        (unsigned)(UINT_PTR)g_staging);
    Log("--- frame source end ---");
    return true;
}

// Detects the pillarboxed content rectangle once, so the host can crop the
// black bars the game leaves at 16:9 resolutions.
static void DetectContentRect(const BYTE* pixels, INT pitch,
    UINT* left, UINT* top, UINT* width, UINT* height)
{
    *left = 0;
    *top = 0;
    *width = g_width;
    *height = g_height;

    const UINT row = g_height / 2;
    const DWORD* line = reinterpret_cast<const DWORD*>(pixels + (SIZE_T)row * pitch);

    UINT first = 0;
    while (first < g_width && (line[first] & 0x00FFFFFF) == 0)
        ++first;

    UINT last = g_width;
    while (last > first && (line[last - 1] & 0x00FFFFFF) == 0)
        --last;

    if (first < last && (last - first) >= g_width / 4)
    {
        *left = first;
        *width = last - first;
    }
}

bool Interop8_CaptureStereoEye(IDirect3DDevice8* device,
    IDirect3DSurface8* source, std::uint32_t eye)
{
    if (g_broken || !device || !source || eye > 1)
        return false;
    if (!g_initialised && !Interop8_Init(device))
        return false;

    IDirect3DSurface8* staging = g_stereoStaging[eye];
    HRESULT hr = Slot<PFN8_CopyRects>(device, VT8_CopyRects)(
        device, source, nullptr, 0, staging, nullptr);
    if (FAILED(hr))
    {
        Log("Interop8_CaptureStereoEye: CopyRects eye %u failed, hr = 0x%08X",
            eye, static_cast<unsigned>(hr));
        return false;
    }

    D3D8LockedRect locked = {};
    hr = Slot<PFNS8_LockRect>(staging, VTS8_LockRect)(
        staging, &locked, nullptr, D3D8_LOCK_READONLY);
    if (FAILED(hr) || !locked.pBits)
    {
        Log("Interop8_CaptureStereoEye: LockRect eye %u failed, hr = 0x%08X",
            eye, static_cast<unsigned>(hr));
        return false;
    }

    const BYTE* pixels = static_cast<const BYTE*>(locked.pBits);
    UINT left = 0, top = 0, width = g_width, height = g_height;
    DetectContentRect(pixels, locked.Pitch, &left, &top, &width, &height);
    const bool published = SharedFrame_CopyStereoEye(pixels, locked.Pitch,
        eye, left, top, width, height);
    Slot<PFNS8_UnlockRect>(staging, VTS8_UnlockRect)(staging);
    return published;
}

void Interop8_ProbeSurface(IDirect3DDevice8* device,
    IDirect3DSurface8* source, const char* label)
{
    if (g_broken || !device || !source)
        return;
    if (!g_initialised && !Interop8_Init(device))
        return;

    IDirect3DSurface8* staging = g_stereoStaging[0];
    HRESULT hr = Slot<PFN8_CopyRects>(device, VT8_CopyRects)(
        device, source, nullptr, 0, staging, nullptr);
    if (FAILED(hr))
    {
        Log("Surface probe %s: CopyRects failed, hr = 0x%08X",
            label ? label : "unnamed", static_cast<unsigned>(hr));
        return;
    }

    D3D8LockedRect locked = {};
    hr = Slot<PFNS8_LockRect>(staging, VTS8_LockRect)(
        staging, &locked, nullptr, D3D8_LOCK_READONLY);
    if (FAILED(hr) || !locked.pBits)
    {
        Log("Surface probe %s: LockRect failed, hr = 0x%08X",
            label ? label : "unnamed", static_cast<unsigned>(hr));
        return;
    }

    std::uint64_t blue = 0;
    std::uint64_t green = 0;
    std::uint64_t red = 0;
    std::uint32_t samples = 0;
    std::uint8_t minimum = 255;
    std::uint8_t maximum = 0;
    for (UINT y = 0; y < g_height; y += 32)
    {
        const BYTE* row = static_cast<const BYTE*>(locked.pBits) +
            static_cast<SIZE_T>(y) * locked.Pitch;
        for (UINT x = 0; x < g_width; x += 32)
        {
            const BYTE* pixel = row + static_cast<SIZE_T>(x) * 4;
            blue += pixel[0];
            green += pixel[1];
            red += pixel[2];
            minimum = min(minimum, min(pixel[0], min(pixel[1], pixel[2])));
            maximum = max(maximum, max(pixel[0], max(pixel[1], pixel[2])));
            ++samples;
        }
    }

    Slot<PFNS8_UnlockRect>(staging, VTS8_UnlockRect)(staging);
    if (samples != 0)
    {
        Log("Surface probe %s: average BGR %u %u %u, range %u..%u, "
            "samples %u", label ? label : "unnamed",
            static_cast<unsigned>(blue / samples),
            static_cast<unsigned>(green / samples),
            static_cast<unsigned>(red / samples),
            static_cast<unsigned>(minimum), static_cast<unsigned>(maximum),
            static_cast<unsigned>(samples));
    }
}

void Interop8_CompareSurfaces(IDirect3DDevice8* device,
    IDirect3DSurface8* first, IDirect3DSurface8* second, const char* label)
{
    if (g_broken || !device || !first || !second)
        return;
    if (!g_initialised && !Interop8_Init(device))
        return;

    HRESULT hr = Slot<PFN8_CopyRects>(device, VT8_CopyRects)(
        device, first, nullptr, 0, g_stereoStaging[0], nullptr);
    if (FAILED(hr))
    {
        Log("Surface comparison %s: first CopyRects failed, hr = 0x%08X",
            label ? label : "unnamed", static_cast<unsigned>(hr));
        return;
    }
    hr = Slot<PFN8_CopyRects>(device, VT8_CopyRects)(
        device, second, nullptr, 0, g_stereoStaging[1], nullptr);
    if (FAILED(hr))
    {
        Log("Surface comparison %s: second CopyRects failed, hr = 0x%08X",
            label ? label : "unnamed", static_cast<unsigned>(hr));
        return;
    }

    D3D8LockedRect firstLocked = {};
    D3D8LockedRect secondLocked = {};
    hr = Slot<PFNS8_LockRect>(g_stereoStaging[0], VTS8_LockRect)(
        g_stereoStaging[0], &firstLocked, nullptr, D3D8_LOCK_READONLY);
    if (FAILED(hr) || !firstLocked.pBits)
    {
        Log("Surface comparison %s: first LockRect failed, hr = 0x%08X",
            label ? label : "unnamed", static_cast<unsigned>(hr));
        return;
    }
    hr = Slot<PFNS8_LockRect>(g_stereoStaging[1], VTS8_LockRect)(
        g_stereoStaging[1], &secondLocked, nullptr, D3D8_LOCK_READONLY);
    if (FAILED(hr) || !secondLocked.pBits)
    {
        Slot<PFNS8_UnlockRect>(g_stereoStaging[0], VTS8_UnlockRect)(
            g_stereoStaging[0]);
        Log("Surface comparison %s: second LockRect failed, hr = 0x%08X",
            label ? label : "unnamed", static_cast<unsigned>(hr));
        return;
    }

    std::uint64_t absoluteDifference[3] = {};
    std::uint32_t changedSamples = 0;
    std::uint32_t samples = 0;
    std::uint8_t maximumDifference = 0;
    for (UINT y = 0; y < g_height; y += 16)
    {
        const BYTE* firstRow = static_cast<const BYTE*>(firstLocked.pBits) +
            static_cast<SIZE_T>(y) * firstLocked.Pitch;
        const BYTE* secondRow = static_cast<const BYTE*>(secondLocked.pBits) +
            static_cast<SIZE_T>(y) * secondLocked.Pitch;
        for (UINT x = 0; x < g_width; x += 16)
        {
            const BYTE* firstPixel = firstRow + static_cast<SIZE_T>(x) * 4;
            const BYTE* secondPixel = secondRow + static_cast<SIZE_T>(x) * 4;
            bool changed = false;
            for (UINT channel = 0; channel < 3; ++channel)
            {
                const int signedDifference =
                    static_cast<int>(firstPixel[channel]) -
                    static_cast<int>(secondPixel[channel]);
                const std::uint8_t difference = static_cast<std::uint8_t>(
                    signedDifference < 0 ? -signedDifference : signedDifference);
                absoluteDifference[channel] += difference;
                maximumDifference = max(maximumDifference, difference);
                changed = changed || difference > 2;
            }
            if (changed)
                ++changedSamples;
            ++samples;
        }
    }

    Slot<PFNS8_UnlockRect>(g_stereoStaging[1], VTS8_UnlockRect)(
        g_stereoStaging[1]);
    Slot<PFNS8_UnlockRect>(g_stereoStaging[0], VTS8_UnlockRect)(
        g_stereoStaging[0]);
    if (samples != 0)
    {
        Log("Surface comparison %s: mean absolute BGR difference %u %u %u, "
            "maximum %u, changed samples %u/%u",
            label ? label : "unnamed",
            static_cast<unsigned>(absoluteDifference[0] / samples),
            static_cast<unsigned>(absoluteDifference[1] / samples),
            static_cast<unsigned>(absoluteDifference[2] / samples),
            static_cast<unsigned>(maximumDifference),
            static_cast<unsigned>(changedSamples),
            static_cast<unsigned>(samples));
    }
}

// -----------------------------------------------------------------------------
//  Per-frame entry point, called from hk_D3D8_Present
// -----------------------------------------------------------------------------

void Interop8_GrabFrame(IDirect3DDevice8* device)
{
    if (g_broken || !device)
        return;

    if (!g_initialised && !Interop8_Init(device))
        return;

    const bool useGpuMetadataOnly =
        g_d3d12BackBufferCount != 0 &&
        InterlockedCompareExchange(&g_renderMode, 0, 0) != SH3VR_RENDER_CINEMA;
    if (useGpuMetadataOnly)
    {
        SharedFrame_PublishGpuMetadata();
        const LONG frame = InterlockedIncrement(&g_frameCounter);
        if (frame == 1 || frame == 60 || frame % 600 == 0)
        {
            Log("GPU metadata frame %d: published %u, CPU readback skipped",
                frame, g_header ? g_header->publishedFrame : 0);
        }
        return;
    }

    LARGE_INTEGER start = {};
    QueryPerformanceCounter(&start);

    IDirect3DSurface8* source = AcquireFrameSource(device);
    if (!source)
    {
        g_broken = true;
        return;
    }

    // Whole-surface copy: null rectangle arrays with a count of zero.
    HRESULT hr = Slot<PFN8_CopyRects>(device, VT8_CopyRects)(
        device, source, nullptr, 0, g_staging, nullptr);
    ReleaseSurface(source);

    if (FAILED(hr))
    {
        Log("Interop8_GrabFrame: CopyRects failed, hr = 0x%08X, capture disabled",
            (unsigned)hr);
        g_broken = true;
        return;
    }

    D3D8LockedRect locked = {};
    hr = Slot<PFNS8_LockRect>(g_staging, VTS8_LockRect)(
        g_staging, &locked, nullptr, D3D8_LOCK_READONLY);
    if (FAILED(hr) || !locked.pBits)
    {
        Log("Interop8_GrabFrame: LockRect failed, hr = 0x%08X", (unsigned)hr);
        g_broken = true;
        return;
    }

    const BYTE* pixels = static_cast<const BYTE*>(locked.pBits);

    UINT left = 0, top = 0, width = g_width, height = g_height;
    DetectContentRect(pixels, locked.Pitch, &left, &top, &width, &height);

    SharedFrame_Publish(pixels, locked.Pitch, left, top, width, height);

    Slot<PFNS8_UnlockRect>(g_staging, VTS8_UnlockRect)(g_staging);

    LARGE_INTEGER end = {};
    QueryPerformanceCounter(&end);

    const LONG frame = InterlockedIncrement(&g_frameCounter);
    g_captureTicks += end.QuadPart - start.QuadPart;
    ++g_captureCalls;

    if (frame == 1 || frame == 60 || frame % 600 == 0)
    {
        const double average = (g_qpcFrequency && g_captureCalls)
            ? (double)g_captureTicks * 1000.0 / ((double)g_qpcFrequency * g_captureCalls)
            : 0.0;

        Log("capture %d: published %u, content %u,%u %ux%u, average cost %d.%02d ms",
            frame, g_header ? g_header->publishedFrame : 0,
            left, top, width, height,
            (int)average, (int)((average - (int)average) * 100.0));

        Sh3VrHeadPose headPose = {};
        if (Interop8_ReadHeadPose(&headPose))
        {
            Log("head pose: flags 0x%02X, position_mm %d %d %d, "
                "orientation_x1e4 %d %d %d %d",
                headPose.flags,
                (int)(headPose.position[0] * 1000.0f),
                (int)(headPose.position[1] * 1000.0f),
                (int)(headPose.position[2] * 1000.0f),
                (int)(headPose.orientation[0] * 10000.0f),
                (int)(headPose.orientation[1] * 10000.0f),
                (int)(headPose.orientation[2] * 10000.0f),
                (int)(headPose.orientation[3] * 10000.0f));
        }
        else
        {
            Log("head pose: not available");
        }

        g_captureTicks = 0;
        g_captureCalls = 0;
    }
}

// Called from hk_D3D8_Reset: the staging surface must be rebuilt whenever the
// back buffer size or format can change.
void Interop8_OnDeviceReset()
{
    if (g_staging)
    {
        ReleaseSurface(g_staging);
        g_staging = nullptr;
    }
    for (UINT eye = 0; eye < 2; ++eye)
    {
        ReleaseSurface(g_stereoStaging[eye]);
        g_stereoStaging[eye] = nullptr;
    }

    g_initialised = false;
    g_broken = false;
    g_stereoPairOpen = false;
    Log("Interop8_OnDeviceReset: staging surface will be rebuilt");
}

void Interop8_Shutdown()
{
    if (g_staging)
    {
        ReleaseSurface(g_staging);
        g_staging = nullptr;
    }
    for (UINT eye = 0; eye < 2; ++eye)
    {
        ReleaseSurface(g_stereoStaging[eye]);
        g_stereoStaging[eye] = nullptr;
    }

    SharedFrame_Shutdown();

    g_initialised = false;
    g_broken = false;
    g_frameCounter = 0;
    g_stereoPairOpen = false;
    g_stereoPairCounter = 0;
    Log("Interop8_Shutdown: producer stopped");
}
