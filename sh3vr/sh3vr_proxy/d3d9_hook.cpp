// =============================================================================
//  sh3vr - d3d9_hook.cpp  (version 17)
//
//  Two hook chains, both armed from DllMain while the process is still
//  single-threaded:
//
//    A. d3d8.dll!Direct3DCreate8
//         -> IDirect3D8::CreateDevice        (slot 15)
//              -> IDirect3DDevice8::Present  (slot 15)  <- per-frame VR entry
//                 IDirect3DDevice8::EndScene (slot 35)
//                 IDirect3DDevice8::Reset    (slot 14)
//                 IDirect3DDevice8::SetTransform (slot 37, diagnostics only)
//                 IDirect3DDevice8::SetVertexShader / Constant (diagnostics)
//
//    B. d3d9.dll!Direct3DCreate9 / Direct3DCreate9Ex
//         -> IDirect3D9(Ex)::CreateDevice    (slot 16)
//            IDirect3D9Ex::CreateDeviceEx    (slot 20)
//              -> IDirect3DDevice9::Present  (slot 17)
//                 IDirect3DDevice9::EndScene (slot 42)
//                 IDirect3DDevice9::Reset    (slot 16)
//                 IDirect3DDevice9Ex::PresentEx (slot 121)
//
//  Chain B exists because version 4 proved the wrapper never calls the
//  CreateDevice implementation of a plain IDirect3D9 object. An IDirect3D9Ex
//  object has its own, different CreateDevice implementation in slot 16, so
//  hooking vtables discovered through our own probe devices could never see it.
//  Hooking the factory exports removes all guesswork: we intercept the very
//  object the wrapper receives.
// =============================================================================

#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <string>
#include <vector>
#include "MinHook.h"
#include "shared_frame.h"

// Forward declarations for the Direct3D 8 interfaces used across the project.
struct IDirect3D8;
struct IDirect3DDevice8;
struct IDirect3DSurface8;
struct IDirect3DBaseTexture8;
struct IDirect3DTexture8;
struct IDirect3DVertexBuffer8;

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

extern void Log(const char* format, ...);
extern bool InputBridge_IsFlashlightEnabled();
extern void InputBridge_ObserveFlashlightEnabled();

// interop_d3d8.cpp
extern void Interop8_GrabFrame(struct IDirect3DDevice8* device);
extern void Interop8_OnDeviceReset();
extern void Interop8_Shutdown();
extern bool Interop8_ReadHeadPose(Sh3VrHeadPose* output);
extern bool Interop8_ReadControllerState(Sh3VrControllerState* state);
extern void Interop8_SetFrameRenderPose(const Sh3VrHeadPose& pose);
extern void Interop8_SetRenderMode(std::uint32_t mode);
extern void Interop8_SetRenderFlags(std::uint32_t flags);
extern void Interop8_SetRenderEye(std::uint32_t eye);
extern void Interop8_SetGamePostProcess(bool enabled, const float scale[3],
    float blend, const std::uint32_t source[4], std::uint32_t intensity);
extern void Interop8_SetD3D12BackBufferHandles(HANDLE first, HANDLE second,
    std::uint32_t currentIndex, std::uint32_t bufferCount);
extern void Interop8_SetD3D12EyeTextureHandles(HANDLE set0Left,
    HANDLE set0Right, HANDLE set1Left, HANDLE set1Right,
    std::uint32_t width, std::uint32_t height, std::uint32_t format);
extern void Interop8_SetD3D12EyeTextureFrame(std::uint32_t activeSet,
    std::uint32_t frameSequence, const Sh3VrHeadPose& renderPose);
extern void Interop8_SetWeaponDebugOrientation(bool valid,
    std::int32_t profileIndex, float pitchDegrees, float yawDegrees,
    float rollDegrees);
extern void Interop8_SetLeftHandDebugOrientation(bool valid,
    float pitchDegrees, float yawDegrees, float rollDegrees);
extern bool Interop8_ReadProjectionUvRects(float eyeRects[2][4]);
extern bool Interop8_GetRequestedEyeResolution(std::uint32_t* width,
    std::uint32_t* height, std::uint32_t* sampleCount);
extern bool Interop8_CaptureStereoEye(IDirect3DDevice8* device,
    IDirect3DSurface8* source, std::uint32_t eye);
extern void Interop8_ProbeSurface(IDirect3DDevice8* device,
    IDirect3DSurface8* source, const char* label);
extern void Interop8_CompareSurfaces(IDirect3DDevice8* device,
    IDirect3DSurface8* first, IDirect3DSurface8* second, const char* label);

// caps_probe.cpp
extern void VR_ProbeDeviceCapabilities(IDirect3DDevice8* wrapperDevice);
extern IDirect3DDevice9* g_realDevice9;
extern IDirect3DDevice9Ex* g_realDevice9Ex;

extern void Log(const char* format, ...);

// caps_probe.cpp
extern void VR_ProbeDeviceCapabilities(struct IDirect3DDevice8* wrapperDevice);
extern IDirect3DDevice9* g_realDevice9;
extern IDirect3DDevice9Ex* g_realDevice9Ex;

// -----------------------------------------------------------------------------
//  Minimal Direct3D 8 declarations (the modern SDK no longer ships d3d8.h)
// -----------------------------------------------------------------------------

struct IDirect3D8;
struct IDirect3DDevice8;

enum { VT8_CreateDevice = 15 };
enum
{
    VT8_Reset = 14,
    VT8_Present = 15,
    VT8_BeginScene = 34,
    VT8_EndScene = 35,
    VT8_Clear = 36,
    VT8_SetTransform = 37,
    VT8_SetRenderTarget = 31,
    VT8_CreateImageSurface = 27,
    VT8_CopyRects = 28,
    VT8_SetViewport = 40,
    VT8_SetRenderState = 50,
    VT8_GetRenderState = 51,
    VT8_ApplyStateBlock = 54,
    VT8_DeleteStateBlock = 56,
    VT8_CreateStateBlock = 57,
    VT8_GetTexture = 60,
    VT8_SetTexture = 61,
    VT8_GetTextureStageState = 62,
    VT8_SetTextureStageState = 63,
    VT8_GetStreamSource = 84,
    VT8_DrawPrimitive = 70,
    VT8_DrawIndexedPrimitive = 71,
    VT8_DrawPrimitiveUP = 72,
    VT8_DrawIndexedPrimitiveUP = 73,
    VT8_CreateVertexShader = 75,
    VT8_SetVertexShader = 76,
    VT8_SetVertexShaderConstant = 79,
    VT8_GetVertexShaderDeclaration = 81,
    VT8_GetVertexShaderFunction = 82
};

enum
{
    SH3VR_D3DTS_WORLD = 256,
    SH3VR_D3DTS_VIEW = 2,
    SH3VR_D3DTS_PROJECTION = 3
};

typedef IDirect3D8* (WINAPI* PFN_Direct3DCreate8)(UINT sdkVersion);
typedef HRESULT(WINAPI* PFN_D3D8_CreateDevice)(IDirect3D8*, UINT, DWORD, HWND, DWORD,
    void*, IDirect3DDevice8**);
typedef HRESULT(STDMETHODCALLTYPE* PFNS8_QueryInterface)(IDirect3DSurface8*,
    REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFNS8_GetContainer)(IDirect3DSurface8*,
    REFIID, void**);
typedef HRESULT(WINAPI* PFN_D3D8_Present)(IDirect3DDevice8*, const RECT*, const RECT*,
    HWND, const void*);
typedef HRESULT(WINAPI* PFN_D3D8_EndScene)(IDirect3DDevice8*);
typedef HRESULT(WINAPI* PFN_D3D8_Reset)(IDirect3DDevice8*, void*);
typedef HRESULT(WINAPI* PFN_D3D8_SetTransform)(IDirect3DDevice8*, DWORD,
    const D3DMATRIX*);
typedef HRESULT(WINAPI* PFN_D3D8_SetVertexShader)(IDirect3DDevice8*, DWORD);
typedef HRESULT(WINAPI* PFN_D3D8_SetTexture)(IDirect3DDevice8*, DWORD,
    IDirect3DBaseTexture8*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_GetStreamSource)(IDirect3DDevice8*,
    UINT, IDirect3DVertexBuffer8**, UINT*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_ApplyStateBlock)(IDirect3DDevice8*,
    DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_DeleteStateBlock)(IDirect3DDevice8*,
    DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_CreateStateBlock)(IDirect3DDevice8*,
    DWORD, DWORD*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_SetRenderState)(IDirect3DDevice8*,
    DWORD, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_SetMaterial)(IDirect3DDevice8*,
    const D3DMATERIAL9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_SetLight)(IDirect3DDevice8*, DWORD,
    const D3DLIGHT9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_LightEnable)(IDirect3DDevice8*, DWORD,
    BOOL);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_GetRenderState)(IDirect3DDevice8*,
    DWORD, DWORD*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_GetTexture)(IDirect3DDevice8*, DWORD,
    IDirect3DBaseTexture8**);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_GetTextureStageState)(
    IDirect3DDevice8*, DWORD, DWORD, DWORD*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_SetTextureStageState)(
    IDirect3DDevice8*, DWORD, DWORD, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_CreateTexture)(IDirect3DDevice8*,
    UINT, UINT, UINT, DWORD, DWORD, DWORD, IDirect3DTexture8**);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_CreateImageSurface)(IDirect3DDevice8*,
    UINT, UINT, DWORD, IDirect3DSurface8**);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_CopyRects)(IDirect3DDevice8*,
    IDirect3DSurface8*, const RECT*, UINT, IDirect3DSurface8*, const POINT*);
typedef HRESULT(STDMETHODCALLTYPE* PFNS8_LockRect)(IDirect3DSurface8*,
    D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFNS8_UnlockRect)(IDirect3DSurface8*);
typedef HRESULT(STDMETHODCALLTYPE* PFNT8_LockRect)(IDirect3DTexture8*, UINT,
    D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFNT8_UnlockRect)(IDirect3DTexture8*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFNV8_Lock)(IDirect3DVertexBuffer8*,
    UINT, UINT, BYTE**, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFNV8_Unlock)(IDirect3DVertexBuffer8*);
typedef ULONG(STDMETHODCALLTYPE* PFNV8_Release)(IDirect3DVertexBuffer8*);
typedef ULONG(STDMETHODCALLTYPE* PFNT8_Release)(IDirect3DBaseTexture8*);
typedef HRESULT(WINAPI* PFN_D3D8_SetVertexShaderConstant)(IDirect3DDevice8*, DWORD,
    const void*, DWORD);
typedef HRESULT(WINAPI* PFN_D3D8_GetRenderState)(IDirect3DDevice8*, DWORD,
    DWORD*);
typedef HRESULT(WINAPI* PFN_D3D8_DrawPrimitive)(IDirect3DDevice8*, DWORD, UINT, UINT);
typedef HRESULT(WINAPI* PFN_D3D8_DrawIndexedPrimitive)(IDirect3DDevice8*, DWORD,
    UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI* PFN_D3D8_DrawPrimitiveUP)(IDirect3DDevice8*, DWORD,
    UINT, const void*, UINT);
typedef HRESULT(WINAPI* PFN_D3D8_DrawIndexedPrimitiveUP)(IDirect3DDevice8*,
    DWORD, UINT, UINT, UINT, const void*, DWORD, const void*, UINT);
typedef HRESULT(WINAPI* PFN_D3D8_CreateVertexShader)(IDirect3DDevice8*,
    const DWORD*, const DWORD*, DWORD*, DWORD);
typedef HRESULT(WINAPI* PFN_D3D8_GetVertexShaderBlob)(IDirect3DDevice8*, DWORD,
    void*, DWORD*);

// -----------------------------------------------------------------------------
//  Direct3D 9 slots and signatures
// -----------------------------------------------------------------------------

enum { VT9_CreateDevice = 16, VT9Ex_CreateDeviceEx = 20 };
enum { VT9_Reset = 16, VT9_Present = 17, VT9_EndScene = 42, VT9Ex_PresentEx = 121 };

typedef IDirect3D9* (WINAPI* PFN_Direct3DCreate9)(UINT sdkVersion);
typedef HRESULT(WINAPI* PFN_Direct3DCreate9Ex)(UINT sdkVersion, IDirect3D9Ex** out);

typedef HRESULT(WINAPI* PFN_D3D9_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(WINAPI* PFN_D3D9Ex_CreateDeviceEx)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND,
    DWORD, D3DPRESENT_PARAMETERS*,
    D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
typedef HRESULT(WINAPI* PFN_D3D9_Present)(IDirect3DDevice9*, const RECT*, const RECT*,
    HWND, const RGNDATA*);
typedef HRESULT(WINAPI* PFN_D3D9_EndScene)(IDirect3DDevice9*);
typedef HRESULT(WINAPI* PFN_D3D9_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(WINAPI* PFN_D3D9_PresentEx)(IDirect3DDevice9Ex*, const RECT*, const RECT*,
    HWND, const RGNDATA*, DWORD);
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL,
    REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DXGI_CreateSwapChain)(IDXGIFactory*,
    IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DXGI_CreateSwapChainForHwnd)(IDXGIFactory2*,
    IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DXGI_CreateSwapChainForComposition)(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
    IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D12_CreateCommandQueue)(ID3D12Device*,
    const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
typedef void(STDMETHODCALLTYPE* PFN_D3D12_ExecuteCommandLists)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D12_CreateCommittedResource)(
    ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D12_CreatePlacedResource)(
    ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DXGI_Present)(IDXGISwapChain*, UINT,
    UINT);
typedef void(__cdecl* PFN_SH3_RenderFrame)();
typedef HRESULT(WINAPI* PFN8_CreateRenderTarget)(IDirect3DDevice8*, UINT, UINT,
    DWORD, DWORD, BOOL, IDirect3DSurface8**);
typedef HRESULT(WINAPI* PFN8_CreateDepthStencilSurface)(IDirect3DDevice8*, UINT,
    UINT, DWORD, DWORD, IDirect3DSurface8**);
typedef HRESULT(WINAPI* PFN8_SetRenderTarget)(IDirect3DDevice8*,
    IDirect3DSurface8*, IDirect3DSurface8*);
typedef HRESULT(WINAPI* PFN8_GetRenderTarget)(IDirect3DDevice8*,
    IDirect3DSurface8**);
typedef HRESULT(WINAPI* PFN8_GetBackBuffer)(IDirect3DDevice8*, UINT, DWORD,
    IDirect3DSurface8**);
typedef HRESULT(WINAPI* PFN8_GetDepthStencilSurface)(IDirect3DDevice8*,
    IDirect3DSurface8**);
struct D3D8ViewportLocal
{
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
};
typedef HRESULT(WINAPI* PFN8_SetViewport)(IDirect3DDevice8*,
    const D3D8ViewportLocal*);
typedef HRESULT(WINAPI* PFN8_GetViewport)(IDirect3DDevice8*,
    D3D8ViewportLocal*);
typedef HRESULT(WINAPI* PFN8_Clear)(IDirect3DDevice8*, DWORD, const void*,
    DWORD, DWORD, float, DWORD);
typedef ULONG(WINAPI* PFNS8_AddRef)(IDirect3DSurface8*);
typedef ULONG(WINAPI* PFNS8_Release)(IDirect3DSurface8*);
typedef HRESULT(WINAPI* PFNS8_GetDesc)(IDirect3DSurface8*, void*);

// -----------------------------------------------------------------------------
//  Originals
// -----------------------------------------------------------------------------

static PFN_Direct3DCreate8   o_Direct3DCreate8 = nullptr;
static PFN_D3D8_CreateDevice o_D3D8_CreateDevice = nullptr;
static PFN_D3D8_Present      o_D3D8_Present = nullptr;
static PFN_D3D8_EndScene     o_D3D8_EndScene = nullptr;
static PFN8_Clear            o_D3D8_Clear = nullptr;
static PFN_D3D8_Reset        o_D3D8_Reset = nullptr;
static PFN_D3D8_SetTransform o_D3D8_SetTransform = nullptr;
static PFN8_SetRenderTarget o_D3D8_SetRenderTarget = nullptr;
static PFN8_SetViewport o_D3D8_SetViewport = nullptr;
static PFN_D3D8_SetVertexShader o_D3D8_SetVertexShader = nullptr;
static PFN_D3D8_SetTexture o_D3D8_SetTexture = nullptr;
static PFN_D3D8_SetVertexShaderConstant o_D3D8_SetVertexShaderConstant = nullptr;
static PFN_D3D8_DrawPrimitive o_D3D8_DrawPrimitive = nullptr;
static PFN_D3D8_DrawIndexedPrimitive o_D3D8_DrawIndexedPrimitive = nullptr;
static PFN_D3D8_DrawPrimitiveUP o_D3D8_DrawPrimitiveUP = nullptr;
static PFN_D3D8_DrawIndexedPrimitiveUP o_D3D8_DrawIndexedPrimitiveUP = nullptr;
static PFN_D3D8_CreateVertexShader o_D3D8_CreateVertexShader = nullptr;

static PFN_Direct3DCreate9        o_Direct3DCreate9 = nullptr;
static PFN_Direct3DCreate9Ex      o_Direct3DCreate9Ex = nullptr;
static PFN_D3D9_CreateDevice      o_D3D9_CreateDevice_plain = nullptr;
static PFN_D3D9_CreateDevice      o_D3D9_CreateDevice_ex = nullptr;
static PFN_D3D9Ex_CreateDeviceEx  o_D3D9Ex_CreateDeviceEx = nullptr;
static PFN_D3D9_Present   o_D3D9_Present = nullptr;
static PFN_D3D9_EndScene  o_D3D9_EndScene = nullptr;
static PFN_D3D9_Reset     o_D3D9_Reset = nullptr;
static PFN_D3D9_PresentEx o_D3D9_PresentEx = nullptr;
static PFN_D3D12CreateDevice o_D3D12CreateDevice = nullptr;
static PFN_CreateDXGIFactory2 o_CreateDXGIFactory2 = nullptr;
static PFN_DXGI_CreateSwapChain o_DXGI_CreateSwapChain = nullptr;
static PFN_DXGI_CreateSwapChainForHwnd o_DXGI_CreateSwapChainForHwnd = nullptr;
static PFN_DXGI_CreateSwapChainForComposition
    o_DXGI_CreateSwapChainForComposition = nullptr;
static PFN_D3D12_CreateCommandQueue o_D3D12_CreateCommandQueue = nullptr;
static PFN_D3D12_ExecuteCommandLists o_D3D12_ExecuteCommandListsFallback = nullptr;
static PFN_D3D12_CreateCommittedResource
    o_D3D12_CreateCommittedResource = nullptr;
static PFN_D3D12_CreatePlacedResource o_D3D12_CreatePlacedResource = nullptr;
static PFN_DXGI_Present o_DXGI_Present = nullptr;
static PFN_SH3_RenderFrame o_SH3_RenderFrame = nullptr;
static PFN_SH3_RenderFrame o_SH3_PrepareFrame = nullptr;
static PFN_SH3_RenderFrame o_SH3_RenderComposite = nullptr;
static PFN_SH3_RenderFrame o_SH3_PostFrame = nullptr;

// -----------------------------------------------------------------------------
//  State
// -----------------------------------------------------------------------------

IDirect3DDevice8* g_device8 = nullptr;

static volatile LONG c_present8 = 0;
static volatile LONG c_endScene8 = 0;
static volatile LONG c_present9 = 0;
static volatile LONG c_presentEx = 0;
static volatile LONG c_sh3RenderFrame = 0;
static volatile LONG c_sh3RenderCallsSincePresent = 0;
static volatile LONG c_sh3PrepareFrame = 0;
static volatile LONG c_sh3PrepareCallsSincePresent = 0;
static bool g_loggedSh3RenderCadence = false;
static bool g_loggedSh3PrepareCadence = false;
static bool g_enableDuplicateRenderTest8 = false;
static bool g_loggedDuplicateRenderTest8 = false;
static bool g_enableOffscreenDuplicateTest8 = false;
static bool g_enableCompositeDuplicateTest8 = false;
static bool g_enableStereoSurfaceProbe8 = false;
static LONG g_compositeCallsBeforeProbe8 = 0;
// This probe only creates and identifies the native eye resources. GPU copy
// submission stays disabled until the resource states and queue boundary are
// confirmed; the game thread must never wait for this diagnostic.
static bool g_enableNativeEyeTargetProbe8 = true;
static LONG g_compositeCallsBeforeNativeEyeTargetProbe8 = 0;
static bool g_probePresentBackBufferThisFrame8 = false;
static bool g_loggedCompositeDuplicateTest8 = false;
static bool g_enablePerDrawStereoProbe8 = true;
static bool g_enableFullPassStereo8 = false;
static bool g_loggedFullPassPrepareSuppressed8 = false;
static bool g_stereoReplayWorldOnly8 = false;
static bool g_enableRuntimeDiagnostics8 = false;
static bool g_perDrawStereoProbeActive8 = false;
static bool g_perDrawStereoProbeComplete8 = false;
static LONG g_stereoShaderCensusFrames8 = 0;
static bool g_stereoShaderCensusLogged8 = false;
static DWORD g_gameClearColor8 = 0;
static bool g_haveGameClearColor8 = false;
static bool g_loggedGameClearColor8 = false;
static bool g_nativeEyeTargetProbeComplete8 = false;
static bool g_perDrawStereoTargetCleared8[2] = {};
static LONG g_perDrawStereoProbeWaitFrames8 = 120;
static LONG g_perDrawStereoProbeDraws8 = 0;
static IDirect3DSurface8* g_stereoLeftColor8 = nullptr;
static IDirect3DSurface8* g_stereoLeftDepth8 = nullptr;
static IDirect3DSurface8* g_stereoRightColor8 = nullptr;
static IDirect3DSurface8* g_stereoRightDepth8 = nullptr;
static UINT g_stereoTargetWidth8 = 0;
static UINT g_stereoTargetHeight8 = 0;
static bool g_gameRenderTargetDescriptorValid8 = false;
static UINT g_gameRenderTargetWidth8 = 0;
static UINT g_gameRenderTargetHeight8 = 0;
static DWORD g_gameRenderTargetFormat8 = 0;
static DWORD g_gameRenderTargetSamples8 = 0;
static DWORD g_gameRenderTargetType8 = 0;
static DWORD g_gameRenderTargetUsage8 = 0;
static DWORD g_gameRenderTargetPool8 = 0;
static UINT g_gameRenderTargetSize8 = 0;
static IDirect3DSurface8* g_gameBackBuffer8 = nullptr;
static IUnknown* g_gameBackBufferIdentity8 = nullptr;
static IUnknown* g_gameBackBufferContainer8 = nullptr;
static LONG g_primaryTargetRefreshFrame8 = -1;
static IDirect3DSurface8* g_stereoReplayTarget8 = nullptr;
static IUnknown* g_stereoReplayTargetContainer8 = nullptr;
static LONG g_stereoReplayTargetFrame8 = -1;
static bool g_loggedStereoSpecialTargetSkip8 = false;
static bool g_loggedStereoPrimaryTargetMatch8 = false;
static bool g_loggedStereoDescriptorTargetSkip8 = false;
static D3D8ViewportLocal g_savedStereoViewport8 = {};
static bool g_savedStereoViewportValid8 = false;
static bool g_loggedStereoViewport8 = false;
static float g_perDrawCenterViewProjection8[16] = {};
static float g_perDrawRightViewProjection8[16] = {};
static float g_perDrawOriginalViewProjection8[16] = {};
static float g_perDrawGameViewProjection8[16] = {};
static bool g_havePerDrawStereoMatrices8 = false;
static bool g_havePerDrawGameMatrix8 = false;

struct BatchedFogDraw8
{
    UINT startVertex;
};

struct BatchedFogRenderStateValue8
{
    DWORD type = 0;
    DWORD value = 0;
    bool valid = false;
};

struct BatchedFogPipeline8
{
    DWORD vertexShader = 0;
    IDirect3DBaseTexture8* texture0 = nullptr;
    BatchedFogRenderStateValue8 renderStates[21] = {};
    BatchedFogRenderStateValue8 textureStageStates[32] = {};
    bool valid = false;
};

static std::vector<BatchedFogDraw8> g_batchedFogDraws8;
static IDirect3DVertexBuffer8* g_batchedFogVertexBuffer8 = nullptr;
static UINT g_batchedFogVertexStride8 = 0;
static UINT g_batchedFogMinVertex8 = 0xFFFFFFFFu;
static UINT g_batchedFogMaxVertexEnd8 = 0;
static BatchedFogPipeline8 g_batchedFogPipeline8 = {};
static D3D8ViewportLocal g_batchedFogSourceViewport8 = {};
static float g_batchedFogGameViewProjection8[16] = {};
static float g_batchedFogLeftViewProjection8[16] = {};
static float g_batchedFogRightViewProjection8[16] = {};
static bool g_batchedFogCaptureValid8 = false;
static bool g_loggedBatchedFogStereo8 = false;
static bool g_loggedBatchedFogBufferMismatch8 = false;
static bool g_loggedBatchedFogCapture8 = false;
static bool g_loggedBatchedFogMissingMatrices8 = false;
static bool g_loggedBatchedFogFailure8 = false;

static bool BeginOffscreenDuplicatePass(std::uint32_t eye,
    IDirect3DSurface8** originalColor, IDirect3DSurface8** originalDepth);
static void EndOffscreenDuplicatePass(IDirect3DSurface8* originalColor,
    IDirect3DSurface8* originalDepth);
static bool BeginStereoPairPass(IDirect3DSurface8** originalColor,
    IDirect3DSurface8** originalDepth);
static bool BeginUiStereoPairPass(IDirect3DSurface8** originalColor,
    IDirect3DSurface8** originalDepth);
static bool SwitchStereoPairEye(std::uint32_t eye);
static void EndStereoPairPass(IDirect3DSurface8* originalColor,
    IDirect3DSurface8* originalDepth);
static bool RenderHeavyFullSceneStereo();
static bool CaptureStereoBackBufferEye(std::uint32_t eye);
static void ProbeCurrentStereoSurface();
static void ProbeStereoBackBuffer(const char* label);
static bool IsPrimaryGameRenderTarget(IDirect3DSurface8* surface);
static bool MatchesPrimaryGameRenderTargetDescription(
    IDirect3DSurface8* surface);
static void DuplicatePrimitiveForStereoProbe(DWORD primitiveType,
    UINT startVertex, UINT primitiveCount);
static void DuplicateIndexedPrimitiveForStereoProbe(DWORD primitiveType,
    UINT minIndex, UINT vertexCount, UINT startIndex, UINT primitiveCount);
static void DuplicatePrimitiveUPForStereoProbe(DWORD primitiveType,
    UINT primitiveCount, const void* vertices, UINT stride);
static void DuplicateIndexedPrimitiveUPForStereoProbe(DWORD primitiveType,
    UINT minIndex, UINT vertexCount, UINT primitiveCount, const void* indices,
    DWORD indexFormat, const void* vertices, UINT stride);
static bool IsUiPretransformedFvf(DWORD shader);
static bool IsLikelyUiAtlasDraw8();
static bool IsGamePostProcessDraw8(DWORD primitiveType,
    UINT primitiveCount, UINT stride, const void* caller);
static bool IsLowResolutionLightCompositeDraw8(DWORD primitiveType,
    UINT primitiveCount, const void* caller);
static bool IsScreenSpaceEffectCompositeDraw8(DWORD primitiveType,
    UINT primitiveCount, UINT stride, const void* caller);
static bool DescribeRenderTargetTexture8(IDirect3DBaseTexture8* texture,
    UINT* width, UINT* height, DWORD* usage);
static bool IsPrimarySceneDrawSamplingRenderTargetTexture8(DWORD* stage,
    UINT* width, UINT* height);
static void RememberOffscreenRenderTarget8(IDirect3DSurface8* color);
static bool DuplicateUiPrimitiveUPForStereo(DWORD primitiveType,
    UINT primitiveCount, const void* vertices, UINT stride);
static bool DuplicateGamePostProcessPrimitiveUPForStereo(
    DWORD primitiveType, UINT primitiveCount, const void* vertices,
    UINT stride);
static bool DuplicateUiIndexedPrimitiveUPForStereo(DWORD primitiveType,
    UINT minIndex, UINT vertexCount, UINT primitiveCount, const void* indices,
    DWORD indexFormat, const void* vertices, UINT stride);
static bool DuplicateUiPrimitiveForStereo(DWORD primitiveType,
    UINT startVertex, UINT primitiveCount);
static bool DuplicateFixedFunctionPrimitiveUPForStereo(
    DWORD primitiveType, UINT primitiveCount, const void* vertices,
    UINT stride);
static bool SetUiOverlayViewport();
static void TraceScreenSpaceDraw(const char* method, bool indexed,
    DWORD primitiveType, UINT primitiveCount, UINT stride,
    const void* caller);
static void TraceScreenSpaceState8(DWORD shader, DWORD primitiveType,
    UINT primitiveCount, UINT stride, const void* vertices,
    const void* caller);
static void LogFogPrimitiveSample8(UINT startVertex, UINT primitiveCount,
    const void* caller);
static void CaptureBatchedFogDraw8(UINT startVertex, UINT primitiveCount);
static bool ReplayBatchedFogForStereo8();
static void ResetBatchedFog8();
static void LogRenderCaller(const char* operation, const void* returnAddress);
static void ReleaseD3D8Surface(IDirect3DSurface8*& surface);
static void ReleaseStereoRenderTargets();
static bool ProbeNativeEyeRenderTargets();
static bool PrepareNativeEyeAsyncCopy();
static void TrySubmitNativeEyeAsyncCopy(ID3D12CommandQueue* queue,
    PFN_D3D12_ExecuteCommandLists original);
static void PollNativeEyeAsyncCopy();

static bool g_minHookReady = false;
static bool g_d8DeviceHooked = false;
static bool g_d9DeviceHooked = false;
static bool g_probed8 = false;
static bool g_dxgiBackBufferProbed = false;
static IDXGISwapChain* g_dxgiSharedSwapChain = nullptr;
static HANDLE g_d3d12BackBufferSharedHandles[2] = {};
static UINT g_d3d12BackBufferSharedCount = 0;
static HANDLE g_d3d12EyeTextureSharedHandles[2][2] = {};
static UINT g_d3d12EyeTextureSharedCount = 0;
static ID3D12CommandQueue* g_d3d12DirectQueue = nullptr;
static ID3D12Resource* g_d3d12NativeEyeResources[2] = {};
static ID3D12Resource* g_d3d12SharedEyeResources[2][2] = {};
static UINT g_d3d12NativeEyeResourceCount = 0;
static ID3D12CommandQueue* g_nativeEyeCopyQueue = nullptr;
static ID3D12CommandAllocator* g_nativeEyeCopyAllocator = nullptr;
static ID3D12GraphicsCommandList* g_nativeEyeCopyCommandList = nullptr;
static ID3D12Fence* g_nativeEyeGameFence = nullptr;
static ID3D12Fence* g_nativeEyeCopyFence = nullptr;
static volatile LONG g_nativeEyeCopyState = 0;
static UINT g_nativeEyeWidth = 0;
static UINT g_nativeEyeHeight = 0;
static UINT g_nativeEyeFormat = 0;
static UINT g_nativeEyeSampleCount = 1;
static bool g_nativeEyeSharedSetInitialized[2] = {};
static UINT g_nativeEyeCopyTargetSet = 0;
static UINT g_nativeEyeCompletedSet = 0;
static UINT64 g_nativeEyeGameFenceValue = 0;
static UINT64 g_nativeEyeCopyFenceValue = 0;
static UINT64 g_nativeEyePendingFenceValue = 0;
static UINT g_nativeEyeFrameSequence = 0;
static bool g_nativeEyeHandlesPublished = false;
static Sh3VrHeadPose g_nativeEyePendingRenderPose = {};
static volatile LONG g_insideD3D8Present = 0;
// The original D3D8 pass always runs. This budget only limits extra replay
// work for an abnormal number of selected draws in one frame. It never
// changes the OpenXR render mode or suppresses the game's own draw calls.
// The amusement park reaches about 273 legitimate camera-space draws. Keep
// it on the accurate per-draw path; the bakery's roughly 650-draw burst still
// crosses this limit and uses its established heavy-scene fallback.
static const LONG kPerDrawStereoReplayBudget8 = 384;
static bool g_perDrawStereoBudgetLogged8 = false;
static bool g_perDrawStereoReplayOverflow8 = false;
static LONG g_consecutiveStereoReplayOverflowFrames8 = 0;
static bool g_heavyFullSceneStereo8 = false;
static bool g_fullSceneStereoReplayActive8 = false;
static bool g_fullScenePrimaryTargetBound8 = false;
static bool g_fullSceneStereoPairReady8 = false;
static bool g_forceWaterFullSceneStereo8 = false;
static bool g_loggedWaterFullSceneStereo8 = false;
static bool g_loggedWaterRttDetection8 = false;
static bool g_loggedHeavyFullSceneStereo8 = false;
// Pre-transformed D3D8 UI is copied after the normal game draw into both
// native eye targets. The counter makes Present publish UI-only frames too.
static volatile LONG g_uiStereoOverlayDraws8 = 0;
static bool g_loggedUiStereoOverlay8 = false;
static bool g_loggedFogUiFalsePositive8 = false;
static bool g_loggedFogPrimitiveSample8 = false;
static bool g_loggedLightCompositeExcluded8 = false;
static bool g_loggedEffectCompositeExcluded8 = false;
static bool g_holdPreviousNativeEyeFrame8 = false;
static bool g_transientEffectPresentPreviousFrame8 = false;
static bool g_loggedTransientEyeFrameHeld8 = false;
static bool g_loggedUiStereoOverlayFailure8 = false;
static bool g_loggedFixedFunctionShadow8 = false;
static bool g_previousImmersiveFrame8 = false;
// The low D3D8 shader handles used by the game's screen-space lighting and
// shadow passes overlap fixed-function FVF values. Keep this experimental
// overlay disabled until a draw-site-specific UI discriminator is available.
// Narrow UI experiment: only pre-transformed FVF 0x144 draws sampling a
// 512x512 texture are replayed. Full-screen and shadow textures are excluded.
static bool g_enableUiStereoOverlay8 = true;
// Keep the screen-space probe disabled in normal builds. It performs many
// wrapper queries per draw and is intended only for an isolated diagnostic
// capture, never for interactive VR gameplay.
static bool g_enableUiTrace8 = false;
// Replays the game's identified full-screen color pass into both native eyes.
// This is deliberately opt-in because the pass is renderer-specific.
static bool g_enableGamePostProcess8 = false;
static bool g_loggedGamePostProcess8 = false;
static float g_leftHandSceneLightScale8 = 1.0f;
static float g_leftHandSceneLightColor8[3] = { 1.0f, 1.0f, 1.0f };
static bool g_leftHandSceneLightValid8 = false;
static bool g_loggedScreenSpaceState8[32] = {};
static LONG g_screenSpaceStateLogCount8 = 0;
static IDirect3DBaseTexture8* g_currentTextures8[8] = {};
static bool g_currentTextureIsRenderTarget8[8] = {};
static UINT g_currentTextureWidths8[8] = {};
static UINT g_currentTextureHeights8[8] = {};
static IDirect3DSurface8* g_recentOffscreenTargets8[32] = {};
static LONG g_recentOffscreenTargetCount8 = 0;
struct UiTraceRecord8
{
    DWORD shader;
    DWORD primitiveType;
    UINT primitiveCount;
    UINT stride;
    UINT targetWidth;
    UINT targetHeight;
    UINT viewportWidth;
    UINT viewportHeight;
    const void* caller;
    const void* texture0;
    bool indexed;
};
static UiTraceRecord8 g_uiTraceRecords8[128] = {};
static LONG g_uiTraceRecordCount8 = 0;

static bool IsPerDrawStereoReplayBudgetAvailable()
{
    const LONG replayDraws = InterlockedCompareExchange(
        &g_perDrawStereoProbeDraws8, 0, 0);
    if (replayDraws < kPerDrawStereoReplayBudget8)
        return true;

    // The budget is evaluated independently for every game frame. Keep the
    // incomplete-frame marker independent from the one-shot diagnostic; once
    // the message had been logged, the old code stopped marking later
    // overflows and published partially replayed eye targets.
    g_perDrawStereoReplayOverflow8 = true;
    if (!g_perDrawStereoBudgetLogged8)
    {
        g_perDrawStereoBudgetLogged8 = true;
        Log("Per-draw replay budget reached (%d); remaining selected draws "
            "stay in the original game pass", kPerDrawStereoReplayBudget8);
    }
    return false;
}

struct D3D12QueueDiagnostic
{
    ID3D12CommandQueue* queue;
    D3D12_COMMAND_LIST_TYPE type;
    void* executeTarget;
    PFN_D3D12_ExecuteCommandLists executeOriginal;
    LONG creationOrdinal;
    volatile LONG executeCalls;
    volatile LONG commandLists;
    volatile LONG callsInsideD3D8Present;
    volatile LONG callsOutsideD3D8Present;
    volatile LONG loggedFirstExecute;
};

static const int SH3VR_MAX_D3D12_QUEUES = 8;
static D3D12QueueDiagnostic
    g_d3d12QueueDiagnostics[SH3VR_MAX_D3D12_QUEUES] = {};
static volatile LONG g_d3d12QueueDiagnosticCount = 0;

static D3DMATRIX g_lastView8 = {};
static D3DMATRIX g_lastProjection8 = {};
static bool g_haveView8 = false;
static bool g_haveProjection8 = false;
static LONG g_viewCallsThisFrame8 = 0;
static LONG g_projectionCallsThisFrame8 = 0;
static LONG g_transformCallOrdinal8 = 0;

struct D3D8TransformSample
{
    D3DMATRIX matrix;
    LONG firstCall;
    LONG lastCall;
    LONG occurrences;
};

static const int SH3VR_MAX_TRANSFORM_SAMPLES = 64;
static D3D8TransformSample g_viewSamples8[SH3VR_MAX_TRANSFORM_SAMPLES] = {};
static D3D8TransformSample g_projectionSamples8[SH3VR_MAX_TRANSFORM_SAMPLES] = {};
static int g_viewSampleCount8 = 0;
static int g_projectionSampleCount8 = 0;
static bool g_loggedGameplayTransformSet8 = false;
static bool g_headOrientationReferenceValid8 = false;
static float g_headOrientationReference8[4] = {};
static float g_headPositionReference8[3] = {};
static bool g_enableCameraModSnapTurn8 = true;
static float g_cameraModSnapTurnActivation8 = 0.65f;
static float g_cameraModSnapTurnDegrees8 = 45.0f;
static DWORD g_cameraModCharacterAlignMilliseconds8 = 34;
static volatile LONG g_cameraModCharacterAlignEndTick8 = 0;
static bool g_autoLoadCameraModFirstPerson8 = true;
static DWORD g_cameraModAutoLoadDelaySeconds8 = 180;
static ULONGLONG g_cameraModStartupTick8 = 0;
static bool g_cameraModAutoLoadDelayLogged8 = false;
static bool g_cameraModAutoLoadDone8 = false;
// Number of consecutive frames for which the normal immersive game camera
// was submitted.  The startup movie/menu does not submit this projection;
// using a short streak avoids touching Camera Mod while that state is live.
static unsigned g_cameraModImmersiveFrameStreak8 = 0;
static bool g_enableRoomscale8 = true;
static float g_roomscalePlayerHeightMeters8 = 1.65f;
static float g_roomscaleHeightScale8 = 1.0f;
static float g_roomscaleFullKeySpeedMetersPerSecond8 = 1.50f;
static float g_roomscaleMovementPulse8 = 0.0f;
static std::int64_t g_roomscaleLastPoseTime8 = 0;
static float g_roomscaleSmoothedVelocity8[2] = {};
static volatile LONG g_roomscaleMovementMask8 = SH3VR_ROOMSCALE_NONE;
static LONG g_roomscaleMovementLogCount8 = 0;
static bool g_roomscaleHeightLogged8 = false;
static bool g_enableHeadTrackedFlashlight8 = false;
static D3DMATRIX g_flashlightBaseView8 = {};
static D3DMATRIX g_flashlightVrView8 = {};
static bool g_haveFlashlightViewPair8 = false;
static float g_flashlightProjectionSource8[12] = {};
static bool g_haveFlashlightProjectionSource8 = false;
static LONG g_flashlightProjectionSeenFrame8 = -1000;
static float g_flashlightProjectionStrength8 = 0.0f;
static bool g_loggedLeftHandFlashlight8 = false;
static LONG g_headTrackedFlashlightProjectionApplications8 = 0;
static LONG g_headTrackedFlashlightProjectionRefreshes8 = 0;
static bool g_cameraModSnapTurnLatched8 = false;
static std::int64_t g_cameraModLastControllerTime8 = 0;
static LONG g_cameraModSnapTurnCount8 = 0;
static LONG g_cameraModLayoutLogState8 = 0;
static constexpr DWORD SH3VR_CAMERA_MOD_SUPPORTED_TIMESTAMP = 0x66B9485Eu;
static constexpr DWORD SH3VR_CAMERA_MOD_STATE_POINTER_RVA = 0x1173Cu;
static constexpr DWORD SH3VR_CAMERA_MOD_GAME_MODULE_POINTER_RVA = 0x11744u;
static constexpr DWORD SH3VR_CAMERA_MOD_LOAD_FPS_PRESET_RVA = 0x6D50u;
static constexpr DWORD SH3VR_CAMERA_MOD_TOGGLE_RVA = 0x9060u;
static constexpr DWORD SH3VR_CAMERA_MOD_ENABLED_OFFSET = 0x40Cu;
static constexpr DWORD SH3VR_CAMERA_MOD_YAW_OFFSET = 0x41Cu;
static constexpr DWORD SH3VR_CAMERA_MOD_HIDE_PLAYER_OFFSET = 0x442u;
static constexpr float SH3VR_ROOMSCALE_FOLLOW_RADIUS_METERS = 0.08f;
static constexpr float SH3VR_ROOMSCALE_START_SPEED_METERS_PER_SECOND = 0.08f;
static constexpr float SH3VR_ROOMSCALE_TRACKING_JUMP_METERS = 0.35f;
static LONG g_headPoseCalibrationStartFrame8 = -1;
static constexpr LONG SH3VR_HEAD_POSE_CALIBRATION_FRAMES = 30;
static Sh3VrHeadPose g_latchedFrameHeadPose8 = {};
static bool g_haveLatchedFrameHeadPose8 = false;
static LONG g_headRotationApplications8 = 0;
static bool g_enableExperimentalHeadRotation8 = true;
static bool g_enableExperimentalShaderHeadRotation8 = false;
static bool g_enableExperimentalColumnViewHeadRotation8 = false;
static bool g_enableViewProjectionHeadRotation8 = true;
static bool g_enableImmersiveWideFov8 = true;
static bool g_enableAlternatingStereo8 = false;
static bool g_enableSynchronizedStereo8 = false;
static bool g_applyStereoEyeOffset8 = false;
static bool g_stereoPairPublishedThisFrame8 = false;
static bool g_enableShaderDumps8 = false;
static LONG g_shaderHeadRotationApplications8 = 0;
static LONG g_columnViewHeadRotationApplications8 = 0;
static LONG g_viewProjectionHeadRotationApplications8 = 0;
static bool g_viewProjectionAppliedThisFrame8 = false;
static std::uint32_t g_renderEye8 = 0;
static DWORD g_loggedVrCameraShaders8[32] = {};
static std::uint32_t g_loggedVrCameraShaderCount8 = 0;
static DWORD g_loggedStereoDrawShaders8[64] = {};
static std::uint32_t g_loggedStereoDrawShaderCount8 = 0;

// cot(60 degrees): render a 120-degree source image. This keeps a useful
// timewarp guard band without spending most eye-target pixels outside the
// headset's visible FOV.
static constexpr float SH3VR_IMMERSIVE_VERTICAL_SCALE = 0.5773502692f;
static constexpr float SH3VR_DEFAULT_WORLD_SCALE = 360.0f;
static constexpr float SH3VR_IPD_METERS = 0.064f;
static float g_worldScale8 = SH3VR_DEFAULT_WORLD_SCALE;
static constexpr LONGLONG SH3VR_TARGET_GAME_FPS = 90;
static LONGLONG g_framePacingFrequency8 = 0;
static LONGLONG g_framePacingDeadline8 = 0;
static bool g_enableFixedStep90Test8 = false;
static bool g_enableThirteenAGFrameRateFix8 = true;
static bool g_enableProxyPresentationUnlock90Hz8 = false;
static bool g_enableProxyNativeTimingUnlock90Hz8 = false;
static bool g_proxyNativeTimingUnlockApplied8 = false;
static bool g_proxyNativeTimingUnlockLogged8 = false;
static bool g_enableProxyFrameTimeOverride90Hz8 = false;
static bool g_proxyFrameTimeOverrideApplied8 = false;
static LONG g_proxyFrameTimeOverrideCalls8 = 0;
static bool g_enableProxyVirtualMode490Hz8 = false;
static bool g_proxyVirtualMode4Applied8 = false;
static LONG g_proxyVirtualFrameTimeTupleCalls8 = 0;
static bool g_proxyPresentationUnlockFailureLogged8 = false;
static bool g_proxyPresentationUnlockLogged8 = false;
static LONG g_proxyExtraPresentCount8 = 0;
static bool g_thirteenAGFrameRateFixApplied8 = false;
static bool g_thirteenAGFrameRateFixLogged8 = false;
static bool g_fixedStep90ModeLogged8 = false;
static bool g_fixedStep90SkipLogged8 = false;
static LONG g_fixedStep90Phase8 = 0;
static LONG g_fixedStep90UpdateCalls8 = 0;
static LONG g_fixedStep90SkippedCalls8 = 0;
static bool g_fixedStep90SkipPostFrame8 = false;
static LONG g_fixedStep90PostUpdateCalls8 = 0;
static LONG g_fixedStep90PostSkippedCalls8 = 0;

struct D3D8ShaderConstantGroup
{
    DWORD shader;
    DWORD startRegister;
    DWORD registerCount;
    LONG occurrences;
    bool changed;
    float firstValues[16];
    float lastValues[16];
};

static const int SH3VR_MAX_SHADER_CONSTANT_GROUPS = 128;
static D3D8ShaderConstantGroup
    g_shaderConstantGroups8[SH3VR_MAX_SHADER_CONSTANT_GROUPS] = {};
static int g_shaderConstantGroupCount8 = 0;
static DWORD g_currentVertexShader8 = 0;
static bool g_loggedWorldDrawStack8 = false;
static bool g_loggedUpDrawStack8 = false;
static LONG g_vertexShaderChangesThisFrame8 = 0;
static LONG g_vertexShaderConstantCallsThisFrame8 = 0;
static bool g_loggedGameplayShaderSet8 = false;
// A bounded, read-only capture around the right-grip press. It is used to
// identify the actual D3D8 draw signature of the equipped weapon before
// attempting any controller-driven visual transform.
static bool g_enableWeaponRenderProbe8 = false;
// Visual controller attachment for the normal Heather weapon set. It never
// generates an attack; combat remains driven by the original game input while
// the visual poses are calibrated independently.
static bool g_enableWeaponPosePrototype8 = false;
static bool g_enableWeaponPoseRotation8 = false;
static bool g_enableControllerOrientationOverlay8 = false;
static bool g_weaponPoseRotateSecondaryBone8 = false;
static float g_weaponPoseOffset8[3] = {};
static float g_weaponPoseBoneA8[12] = {};
static float g_weaponPoseBoneB8[12] = {};
static BYTE g_weaponPoseBoneAMask8 = 0;
static BYTE g_weaponPoseBoneBMask8 = 0;
static DWORD g_weaponPoseConstantsShader8 = 0;
static constexpr DWORD SH3VR_WEAPON_PALETTE_START8 = 48;
static constexpr DWORD SH3VR_WEAPON_PALETTE_BONE_COUNT8 = 16;
static float g_weaponPosePalette8[SH3VR_WEAPON_PALETTE_BONE_COUNT8][12] = {};
static BYTE g_weaponPosePaletteMasks8[SH3VR_WEAPON_PALETTE_BONE_COUNT8] = {};
static float g_weaponPoseBaselinePalette8[SH3VR_WEAPON_PALETTE_BONE_COUNT8][12] = {};
static BYTE g_weaponPoseBaselinePaletteMasks8[SH3VR_WEAPON_PALETTE_BONE_COUNT8] = {};
static bool g_weaponPoseBaselinePaletteValid8 = false;
static DWORD g_weaponPoseDebugReferenceBone8 = 0;
static float g_weaponPoseBaselinePivot8[3] = {};
static bool g_weaponPoseBaselineGripPointValid8 = false;
static float g_weaponPoseBaselineGripPoint8[3] = {};
static bool g_weaponPoseAbsolutePosition8 = true;
static bool g_weaponPoseWorldReferenceValid8 = false;
static float g_weaponPoseBaselineHandWorldRotation8[3][3] = {};
static float g_weaponPoseGripPitchRadians8 = 0.0f;
static float g_weaponPoseGripYawRadians8 = 1.57079632679f;
static float g_weaponPoseGripRollRadians8 = 1.30899693899f;
static float g_weaponPoseScale8 = 0.90f;
static float g_weaponPoseMinimumForward8 = 0.0f;
static bool g_weaponPoseDisableClipping8 = true;
static bool g_weaponPoseBaselineValid8 = false;
static float g_weaponPoseBaselineOrientation8[4] = {};
static float g_weaponPoseBaselinePosition8[3] = {};
static LONG g_weaponPoseApplications8 = 0;
struct WeaponPoseProfile8
{
    const char* section;
    const char* displayName;
    UINT vertexCount[2];
    UINT primitiveCount[2];
    UINT signatureCount;
    float position[3];
    float pitchRadians;
    float yawRadians;
    float rollRadians;
    float scale;
    float aimPitchRadians;
    float aimYawRadians;
    bool showGuideDot;
};

// These mesh signatures come directly from the PC models in data/chrwp.arc.
// Katana is the only listed weapon split across two indexed mesh draws.
static WeaponPoseProfile8 g_weaponPoseProfiles8[] = {
    { "Knife", "Knife", { 166u, 0u }, { 334u, 0u }, 1u },
    { "Steel Pipe", "Steel Pipe", { 172u, 0u }, { 294u, 0u }, 1u },
    { "Maul", "Maul", { 208u, 0u }, { 426u, 0u }, 1u },
    { "Katana", "Katana", { 69u, 72u }, { 122u, 114u }, 2u },
    { "Stun Gun", "Stun Gun", { 132u, 0u }, { 226u, 0u }, 1u },
    { "Handgun", "Handgun", { 368u, 0u }, { 610u, 0u }, 1u },
    { "Shotgun", "Shotgun", { 290u, 0u }, { 490u, 0u }, 1u },
    { "Submachine Gun", "Submachine Gun", { 557u, 0u }, { 890u, 0u }, 1u }
};
static constexpr int SH3VR_WEAPON_PROFILE_COUNT8 =
    static_cast<int>(sizeof(g_weaponPoseProfiles8) /
        sizeof(g_weaponPoseProfiles8[0]));
struct WeaponPoseBaselineCache8
{
    bool valid;
    float palette[SH3VR_WEAPON_PALETTE_BONE_COUNT8][12];
    BYTE paletteMasks[SH3VR_WEAPON_PALETTE_BONE_COUNT8];
    float pivot[3];
    bool gripPointValid;
    float gripPoint[3];
    bool relativeBaselineValid;
    float relativeOrientation[4];
    float relativePosition[3];
};
static WeaponPoseBaselineCache8
    g_weaponPoseBaselineCaches8[SH3VR_WEAPON_PROFILE_COUNT8] = {};
static int g_activeWeaponPoseProfile8 = -1;
static volatile LONG g_activeWeaponPoseSeenPresent8 = -1000;
static LONG g_firearmReticleRenderedPresent8 = -1;
static bool g_firearmReticleLogged8 = false;
static ULONGLONG g_weaponIniLastPollTick8 = 0;
static ULONGLONG g_weaponIniReloadAfterTick8 = 0;
static FILETIME g_weaponIniObservedWriteTime8 = {};
static bool g_weaponIniObservedWriteTimeValid8 = false;
static bool g_weaponIniReloadPending8 = false;

struct LeftHandPoseProfile8
{
    bool enabled;
    float position[3];
    float pitchRadians;
    float yawRadians;
    float rollRadians;
    float scale;
};

struct LeftHandDiskVertex8
{
    float position[3];
    float normal[3];
    float texcoord[2];
};

struct LeftHandVertex8
{
    float position[3];
    float normal[3];
    DWORD diffuse;
    float texcoord[2];
};

// Pre-transformed hand vertices bypass D3D8's camera near-plane clip. This
// keeps a tracked hand intact when it is brought right up to the headset,
// while retaining a depth value compatible with the game's eye target.
struct LeftHandScreenVertex8
{
    float positionRhw[4];
    DWORD diffuse;
    float texcoord[2];
};

struct LeftHandClipVertex8
{
    float clip[4];
    float color[3];
    float texcoord[2];
};

struct LeftHandMeshPart8
{
    std::vector<LeftHandVertex8> vertices;
    std::vector<std::uint16_t> indices;
    UINT materialIndex;
};

static LeftHandPoseProfile8 g_leftHandPoseProfile8 = {};
static std::vector<LeftHandMeshPart8> g_leftHandMeshParts8;
static IDirect3DBaseTexture8* g_leftHandTextures8[2] = {};
static IDirect3DDevice8* g_leftHandResourceDevice8 = nullptr;
static IDirect3DSurface8* g_leftHandLightSampleSurface8 = nullptr;
static bool g_leftHandResourcesLoaded8 = false;
static bool g_leftHandResourcesAttempted8 = false;
static bool g_leftHandRenderLogged8 = false;
static bool g_leftHandResourceFailureLogged8 = false;
static LONG g_leftHandRenderedPresent8 = -1;
static bool g_leftHandPoseFailureLogged8 = false;
static bool g_leftHandStereoFailureLogged8 = false;
static bool g_leftHandStateFailureLogged8 = false;
static bool g_leftHandDrawFailureLogged8 = false;
static bool g_leftHandDesktopRenderLogged8 = false;
static bool g_leftHandDesktopFailureLogged8 = false;
static IDirect3DSurface8* g_leftHandDesktopDepth8 = nullptr;
static IDirect3DSurface8* g_leftHandDesktopWeaponDepth8 = nullptr;
static LONG g_leftHandDesktopWeaponDepthPresent8 = -1;
static UINT g_leftHandDesktopDepthWidth8 = 0;
static UINT g_leftHandDesktopDepthHeight8 = 0;
static DWORD g_leftHandDesktopDepthSamples8 = 0;
static LONG g_motionWeaponDrawCaptureFrames8 = 0;
static LONG g_motionWeaponDrawCaptureRecords8 = 0;
static LONG g_motionWeaponConstantCaptureRecords8 = 0;
static bool g_motionWeaponRightGripWasDown8 = false;
static LONG g_motionWeaponDrawCaptureSerial8 = 0;

static void LogRenderCallStack(const char* operation);
static void LogFirstStereoDrawForShader(const char* method, DWORD primitiveType,
    UINT primitiveCount, bool indexed, bool vertexBufferDraw);
static bool HookOne(const char* name, void* target, void* detour,
    void** original);
static void CaptureWeaponPoseConstants8(DWORD startRegister,
    const void* data, DWORD registerCount);
static bool BuildWeaponPoseDelta8(float rotation[3][3], float translation[3]);
static bool IsWeaponAffineBone8(const float matrix[12]);
static bool CaptureWeaponGripPointFromGeometry8(IDirect3DDevice8* device,
    UINT minIndex, UINT vertexCount);
static int FindWeaponPoseProfile8(UINT vertexCount, UINT primitiveCount);
static void ActivateWeaponPoseProfile8(int profileIndex);
static void LoadLeftHandPoseProfile8();
static bool RenderLeftHandStereo8();
static bool RenderFirearmReticleStereo8();
static void ReleaseLeftHandResources8();
static void CaptureLeftHandDesktopWeaponDepth8();
static bool CaptureWeaponGripPointFromGeometry8(IDirect3DDevice8* device,
    UINT minIndex, UINT vertexCount)
{
    if (!device || vertexCount == 0 || vertexCount > 2048u ||
        !g_weaponPoseBaselinePaletteValid8)
    {
        return false;
    }

    void** deviceVtable = *reinterpret_cast<void***>(device);
    const auto getStreamSource = reinterpret_cast<PFN8_GetStreamSource>(
        deviceVtable[VT8_GetStreamSource]);
    IDirect3DVertexBuffer8* vertexBuffer = nullptr;
    UINT stride = 0;
    if (!getStreamSource || FAILED(getStreamSource(device, 0, &vertexBuffer,
        &stride)) || !vertexBuffer || stride != 48u)
    {
        if (vertexBuffer)
        {
            void** bufferVtable = *reinterpret_cast<void***>(vertexBuffer);
            reinterpret_cast<PFNV8_Release>(bufferVtable[2])(vertexBuffer);
        }
        return false;
    }

    float points[2048][3] = {};
    UINT pointCount = 0;
    BYTE* locked = nullptr;
    void** bufferVtable = *reinterpret_cast<void***>(vertexBuffer);
    const auto lock = reinterpret_cast<PFNV8_Lock>(bufferVtable[11]);
    const auto unlock = reinterpret_cast<PFNV8_Unlock>(bufferVtable[12]);
    const auto release = reinterpret_cast<PFNV8_Release>(bufferVtable[2]);
    const UINT byteOffset = minIndex * stride;
    const UINT byteCount = vertexCount * stride;
    if (lock && unlock && SUCCEEDED(lock(vertexBuffer, byteOffset, byteCount,
        &locked, 0x00000010u)) && locked)
    {
        for (UINT vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            const BYTE* vertex = locked + vertexIndex * stride;
            float sourcePosition[3] = {};
            float weights[4] = {};
            std::memcpy(sourcePosition, vertex, sizeof(sourcePosition));
            std::memcpy(weights, vertex + 12u, 3u * sizeof(float));
            weights[3] = 1.0f - weights[0] - weights[1] - weights[2];
            const BYTE* boneIndices = vertex + 24u;
            float skinned[3] = {};
            float totalWeight = 0.0f;
            for (int influence = 0; influence < 4; ++influence)
            {
                const float weight = weights[influence];
                if (!std::isfinite(weight) || weight <= 0.0001f)
                    continue;

                DWORD bone = boneIndices[influence];
                if (bone >= SH3VR_WEAPON_PALETTE_BONE_COUNT8 ||
                    g_weaponPoseBaselinePaletteMasks8[bone] != 0x07u)
                {
                    if (bone % 3u != 0u)
                        continue;
                    bone /= 3u;
                }
                if (bone >= SH3VR_WEAPON_PALETTE_BONE_COUNT8 ||
                    g_weaponPoseBaselinePaletteMasks8[bone] != 0x07u)
                {
                    continue;
                }

                const float* matrix = g_weaponPoseBaselinePalette8[bone];
                for (int row = 0; row < 3; ++row)
                {
                    skinned[row] += weight * (
                        matrix[row * 4 + 0] * sourcePosition[0] +
                        matrix[row * 4 + 1] * sourcePosition[1] +
                        matrix[row * 4 + 2] * sourcePosition[2] +
                        matrix[row * 4 + 3]);
                }
                totalWeight += weight;
            }
            if (totalWeight <= 0.5f)
                continue;
            for (int axis = 0; axis < 3; ++axis)
                points[pointCount][axis] = skinned[axis] / totalWeight;
            ++pointCount;
        }
        unlock(vertexBuffer);
    }
    release(vertexBuffer);
    if (pointCount < 16u)
        return false;

    float mean[3] = {};
    for (UINT index = 0; index < pointCount; ++index)
        for (int axis = 0; axis < 3; ++axis)
            mean[axis] += points[index][axis];
    for (int axis = 0; axis < 3; ++axis)
        mean[axis] /= static_cast<float>(pointCount);

    float covariance[3][3] = {};
    for (UINT index = 0; index < pointCount; ++index)
    {
        float centered[3] = {
            points[index][0] - mean[0],
            points[index][1] - mean[1],
            points[index][2] - mean[2]
        };
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                covariance[row][column] +=
                    centered[row] * centered[column];
    }

    float principalAxis[3] = { 1.0f, 0.0f, 0.0f };
    for (int iteration = 0; iteration < 16; ++iteration)
    {
        float next[3] = {};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                next[row] += covariance[row][column] *
                    principalAxis[column];
        const float length = std::sqrt(next[0] * next[0] +
            next[1] * next[1] + next[2] * next[2]);
        if (!std::isfinite(length) || length < 0.0001f)
            return false;
        for (int axis = 0; axis < 3; ++axis)
            principalAxis[axis] = next[axis] / length;
    }

    float projectionMin = FLT_MAX;
    float projectionMax = -FLT_MAX;
    for (UINT index = 0; index < pointCount; ++index)
    {
        const float projection =
            (points[index][0] - mean[0]) * principalAxis[0] +
            (points[index][1] - mean[1]) * principalAxis[1] +
            (points[index][2] - mean[2]) * principalAxis[2];
        projectionMin = (std::min)(projectionMin, projection);
        projectionMax = (std::max)(projectionMax, projection);
    }
    const float projectionRange = projectionMax - projectionMin;
    if (!std::isfinite(projectionRange) || projectionRange < 0.001f)
        return false;

    float endCentroids[2][3] = {};
    float endRadialSquared[2] = {};
    UINT endCounts[2] = {};
    const float segmentFraction = 0.38f;
    const float lowThreshold = projectionMin +
        projectionRange * segmentFraction;
    const float highThreshold = projectionMax -
        projectionRange * segmentFraction;
    for (UINT index = 0; index < pointCount; ++index)
    {
        const float centered[3] = {
            points[index][0] - mean[0],
            points[index][1] - mean[1],
            points[index][2] - mean[2]
        };
        const float projection = centered[0] * principalAxis[0] +
            centered[1] * principalAxis[1] +
            centered[2] * principalAxis[2];
        int end = -1;
        if (projection <= lowThreshold)
            end = 0;
        else if (projection >= highThreshold)
            end = 1;
        if (end < 0)
            continue;
        for (int axis = 0; axis < 3; ++axis)
            endCentroids[end][axis] += points[index][axis];
        const float centeredLengthSquared = centered[0] * centered[0] +
            centered[1] * centered[1] + centered[2] * centered[2];
        endRadialSquared[end] += centeredLengthSquared -
            projection * projection;
        ++endCounts[end];
    }
    if (endCounts[0] == 0u || endCounts[1] == 0u)
        return false;
    for (int end = 0; end < 2; ++end)
    {
        for (int axis = 0; axis < 3; ++axis)
            endCentroids[end][axis] /= static_cast<float>(endCounts[end]);
        endRadialSquared[end] /= static_cast<float>(endCounts[end]);
    }
    const float lowScore = static_cast<float>(endCounts[0]) *
        (1.0f + endRadialSquared[0]);
    const float highScore = static_cast<float>(endCounts[1]) *
        (1.0f + endRadialSquared[1]);
    const int handleEnd = highScore > lowScore ? 1 : 0;
    std::memcpy(g_weaponPoseBaselineGripPoint8,
        endCentroids[handleEnd], sizeof(g_weaponPoseBaselineGripPoint8));
    g_weaponPoseBaselineGripPointValid8 = true;
    const char* weaponName = g_activeWeaponPoseProfile8 >= 0
        ? g_weaponPoseProfiles8[g_activeWeaponPoseProfile8].displayName
        : "weapon";
    Log("MotionControls: automatic %s grip point captured from %u vertices; handle end %s, endpoint counts %u/%u, radial x1000 %d/%d, grip x1000 %d/%d/%d",
        weaponName, static_cast<unsigned>(pointCount),
        handleEnd == 0 ? "low" : "high",
        static_cast<unsigned>(endCounts[0]),
        static_cast<unsigned>(endCounts[1]),
        static_cast<int>(std::lround(endRadialSquared[0] * 1000.0f)),
        static_cast<int>(std::lround(endRadialSquared[1] * 1000.0f)),
        static_cast<int>(std::lround(g_weaponPoseBaselineGripPoint8[0] *
            1000.0f)),
        static_cast<int>(std::lround(g_weaponPoseBaselineGripPoint8[1] *
            1000.0f)),
        static_cast<int>(std::lround(g_weaponPoseBaselineGripPoint8[2] *
            1000.0f)));
    return true;
}

int D3D9Hook_GetActiveMeleeWeaponProfile()
{
    const LONG present = InterlockedCompareExchange(&c_present8, 0, 0);
    const LONG seen = InterlockedCompareExchange(
        &g_activeWeaponPoseSeenPresent8, 0, 0);
    const int profile = g_activeWeaponPoseProfile8;
    return profile >= 0 && profile < 4 && present - seen >= 0 &&
        present - seen <= 2 ? profile : -1;
}

static void ApplyWeaponPoseDeltaToBone8(const float source[12],
    const float rotation[3][3], const float pivot[3],
    const float translation[3], float output[12]);
static bool InvertD3D8Matrix(const D3DMATRIX& input, D3DMATRIX& output);

static void PaceGameFrame()
{
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (g_framePacingFrequency8 == 0)
    {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        g_framePacingFrequency8 = frequency.QuadPart;
        g_framePacingDeadline8 = now.QuadPart;
    }

    const LONGLONG frameTicks = g_framePacingFrequency8 /
        SH3VR_TARGET_GAME_FPS;
    const LONGLONG target = g_framePacingDeadline8 + frameTicks;
    if (now.QuadPart >= target)
    {
        g_framePacingDeadline8 = now.QuadPart;
        return;
    }

    for (;;)
    {
        QueryPerformanceCounter(&now);
        const LONGLONG remaining = target - now.QuadPart;
        if (remaining <= 0)
            break;
        if (remaining * 1000 > g_framePacingFrequency8 * 2)
            Sleep(1);
        else
            YieldProcessor();
    }
    g_framePacingDeadline8 = target;
}

static int ReadPcFixFpsMode()
{
    HMODULE pcFix = GetModuleHandleA("Silent_Hill_3_PC_Fix.dll");
    if (!pcFix)
        return -1;

    int fpsMode = -1;
    SIZE_T bytesRead = 0;
    ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const BYTE*>(pcFix) + 0x0006CED8,
        &fpsMode, sizeof(fpsMode), &bytesRead);
    return bytesRead == sizeof(fpsMode) ? fpsMode : -1;
}

static bool ReadFixedStep90Setting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return false;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return false;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Timing", "FixedStep90Test", 0, iniPath) != 0;
}

static bool ReadThirteenAGFrameRateFixSetting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return true;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return true;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Timing", "UseThirteenAGFrameRateFix", 1,
        iniPath) != 0;
}

static bool ReadProxyPresentationUnlockSetting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return false;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return false;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Timing", "ProxyPresentationUnlock90Hz", 0,
        iniPath) != 0;
}

static bool ReadProxyNativeTimingUnlockSetting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return false;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return false;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Timing", "ProxyNativeTimingUnlock90Hz", 0,
        iniPath) != 0;
}

static bool ReadProxyFrameTimeOverrideSetting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return false;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return false;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Timing", "ProxyFrameTimeOverride90Hz", 0,
        iniPath) != 0;
}

static bool ReadProxyVirtualMode4Setting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return false;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return false;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Timing", "ProxyVirtualFPSMode4", 0,
        iniPath) != 0;
}

static bool ReadPerDrawStereoReplaySetting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return true;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return true;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Stereo", "EnablePerDrawReplay", 1,
        iniPath) != 0;
}

static bool ReadFullPassStereoSetting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return false;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return false;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Stereo", "EnableFullPassStereo", 0,
        iniPath) != 0;
}

static bool ReadGamePostProcessSetting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return false;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return false;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Image", "EnableGamePostProcess", 0,
        iniPath) != 0;
}

static bool ReadStereoReplayWorldOnlySetting()
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return true;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return true;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA("Stereo", "ReplayWorldOnly", 1,
        iniPath) != 0;
}

static int ReadIniIntSetting(const char* section, const char* key,
    int defaultValue)
{
    char iniPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, iniPath,
        static_cast<DWORD>(sizeof(iniPath))) == 0)
    {
        return defaultValue;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return defaultValue;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + sizeof(iniPath) - (finalSlash + 1)),
        "sh3vr.ini");
    return GetPrivateProfileIntA(section, key, defaultValue, iniPath);
}

static bool GetWeaponIniPath8(char iniPath[MAX_PATH])
{
    if (GetModuleFileNameA(nullptr, iniPath,
        MAX_PATH) == 0)
    {
        return false;
    }

    char* finalSlash = strrchr(iniPath, '\\');
    if (!finalSlash)
        return false;
    strcpy_s(finalSlash + 1,
        static_cast<size_t>(iniPath + MAX_PATH - (finalSlash + 1)),
        "sh3vr_weapons.ini");
    return true;
}

static int ReadWeaponIniIntSetting(const char* section, const char* key,
    int defaultValue)
{
    char iniPath[MAX_PATH] = {};
    if (!GetWeaponIniPath8(iniPath))
        return defaultValue;
    return GetPrivateProfileIntA(section, key, defaultValue, iniPath);
}

static void LoadWeaponPoseProfiles8()
{
    constexpr float degreesToRadians = 0.01745329251994329577f;
    for (int index = 0; index < SH3VR_WEAPON_PROFILE_COUNT8; ++index)
    {
        WeaponPoseProfile8& profile = g_weaponPoseProfiles8[index];
        const int defaultScale = index == 0 ? 70 : 100;
        const int pitchDegrees = std::clamp(ReadWeaponIniIntSetting(
            profile.section, "RotationPitchDegrees", 90), -180, 180);
        const int yawDegrees = std::clamp(ReadWeaponIniIntSetting(
            profile.section, "RotationYawDegrees", 90), -180, 180);
        const int rollDegrees = std::clamp(ReadWeaponIniIntSetting(
            profile.section, "RotationRollDegrees", 45), -180, 180);
        const int scalePercent = std::clamp(ReadWeaponIniIntSetting(
            profile.section, "ScalePercent", defaultScale), 25, 200);
        const bool firearmProfile = index >= 4;
        profile.showGuideDot = ReadWeaponIniIntSetting(profile.section,
            "ShowGuideDot", 1) != 0;
        const int aimPitchDegrees = std::clamp(ReadWeaponIniIntSetting(
            profile.section, "AimPitchDegrees",
            firearmProfile ? -18 : 0), -90, 90);
        const int aimYawDegrees = std::clamp(ReadWeaponIniIntSetting(
            profile.section, "AimYawDegrees", 0), -90, 90);
        profile.position[0] = static_cast<float>(std::clamp(
            ReadWeaponIniIntSetting(profile.section, "PositionX", 0),
            -300, 300));
        profile.position[1] = static_cast<float>(std::clamp(
            ReadWeaponIniIntSetting(profile.section, "PositionY", 0),
            -300, 300));
        profile.position[2] = static_cast<float>(std::clamp(
            ReadWeaponIniIntSetting(profile.section, "PositionZ", 0),
            -300, 300));
        profile.pitchRadians = static_cast<float>(pitchDegrees) *
            degreesToRadians;
        profile.yawRadians = static_cast<float>(yawDegrees) *
            degreesToRadians;
        profile.rollRadians = static_cast<float>(rollDegrees) *
            degreesToRadians;
        profile.scale = static_cast<float>(scalePercent) / 100.0f;
        profile.aimPitchRadians = static_cast<float>(aimPitchDegrees) *
            degreesToRadians;
        profile.aimYawRadians = static_cast<float>(aimYawDegrees) *
            degreesToRadians;
    }
}

static void LoadLeftHandPoseProfile8()
{
    constexpr float degreesToRadians = 0.01745329251994329577f;
    LeftHandPoseProfile8& profile = g_leftHandPoseProfile8;
    profile.enabled = ReadWeaponIniIntSetting("LeftHand", "Enabled", 1) != 0;
    profile.position[0] = static_cast<float>(std::clamp(
        ReadWeaponIniIntSetting("LeftHand", "PositionX", 0), -300, 300));
    profile.position[1] = static_cast<float>(std::clamp(
        ReadWeaponIniIntSetting("LeftHand", "PositionY", 0), -300, 300));
    profile.position[2] = static_cast<float>(std::clamp(
        ReadWeaponIniIntSetting("LeftHand", "PositionZ", 0), -300, 300));
    const int pitchDegrees = std::clamp(ReadWeaponIniIntSetting(
        "LeftHand", "RotationPitchDegrees", 0), -180, 180);
    const int yawDegrees = std::clamp(ReadWeaponIniIntSetting(
        "LeftHand", "RotationYawDegrees", 0), -180, 180);
    const int rollDegrees = std::clamp(ReadWeaponIniIntSetting(
        "LeftHand", "RotationRollDegrees", 0), -180, 180);
    const int scalePercent = std::clamp(ReadWeaponIniIntSetting(
        "LeftHand", "ScalePercent", 100), 25, 200);
    profile.pitchRadians = static_cast<float>(pitchDegrees) * degreesToRadians;
    profile.yawRadians = static_cast<float>(yawDegrees) * degreesToRadians;
    profile.rollRadians = static_cast<float>(rollDegrees) * degreesToRadians;
    profile.scale = static_cast<float>(scalePercent) / 100.0f;
}

static void ApplyActiveWeaponPoseProfileValues8()
{
    if (g_activeWeaponPoseProfile8 < 0 ||
        g_activeWeaponPoseProfile8 >= SH3VR_WEAPON_PROFILE_COUNT8)
    {
        return;
    }

    const WeaponPoseProfile8& profile =
        g_weaponPoseProfiles8[g_activeWeaponPoseProfile8];
    std::memcpy(g_weaponPoseOffset8, profile.position,
        sizeof(g_weaponPoseOffset8));
    g_weaponPoseGripPitchRadians8 = profile.pitchRadians;
    g_weaponPoseGripYawRadians8 = profile.yawRadians;
    g_weaponPoseGripRollRadians8 = profile.rollRadians;
    g_weaponPoseScale8 = profile.scale;
}

static bool ReadWeaponIniWriteTime8(FILETIME* writeTime)
{
    if (!writeTime)
        return false;

    char iniPath[MAX_PATH] = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetWeaponIniPath8(iniPath) ||
        !GetFileAttributesExA(iniPath, GetFileExInfoStandard, &attributes))
    {
        return false;
    }
    *writeTime = attributes.ftLastWriteTime;
    return true;
}

static void InitializeWeaponIniHotReload8()
{
    g_weaponIniObservedWriteTimeValid8 = ReadWeaponIniWriteTime8(
        &g_weaponIniObservedWriteTime8);
    g_weaponIniReloadPending8 = false;
    g_weaponIniLastPollTick8 = GetTickCount64();
}

static void PollWeaponIniHotReload8()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_weaponIniLastPollTick8 < 100u)
        return;
    g_weaponIniLastPollTick8 = now;

    FILETIME currentWriteTime = {};
    if (!ReadWeaponIniWriteTime8(&currentWriteTime))
        return;

    if (!g_weaponIniObservedWriteTimeValid8)
    {
        g_weaponIniObservedWriteTime8 = currentWriteTime;
        g_weaponIniObservedWriteTimeValid8 = true;
        return;
    }

    if (CompareFileTime(&currentWriteTime,
        &g_weaponIniObservedWriteTime8) != 0)
    {
        g_weaponIniObservedWriteTime8 = currentWriteTime;
        g_weaponIniReloadPending8 = true;
        g_weaponIniReloadAfterTick8 = now + 200u;
        return;
    }

    if (!g_weaponIniReloadPending8 || now < g_weaponIniReloadAfterTick8)
        return;

    g_weaponIniReloadPending8 = false;
    LoadWeaponPoseProfiles8();
    LoadLeftHandPoseProfile8();
    ApplyActiveWeaponPoseProfileValues8();

    if (g_activeWeaponPoseProfile8 >= 0 &&
        g_activeWeaponPoseProfile8 < SH3VR_WEAPON_PROFILE_COUNT8)
    {
        const WeaponPoseProfile8& profile =
            g_weaponPoseProfiles8[g_activeWeaponPoseProfile8];
        constexpr float radiansToDegrees = 57.295779513082320876f;
        Log("MotionControls: live-reloaded sh3vr_weapons.ini; active %s "
            "scale %d%% position %d/%d/%d rotation %d/%d/%d aim %d/%d",
            profile.displayName,
            static_cast<int>(std::lround(profile.scale * 100.0f)),
            static_cast<int>(profile.position[0]),
            static_cast<int>(profile.position[1]),
            static_cast<int>(profile.position[2]),
            static_cast<int>(std::lround(profile.pitchRadians *
                radiansToDegrees)),
            static_cast<int>(std::lround(profile.yawRadians *
                radiansToDegrees)),
            static_cast<int>(std::lround(profile.rollRadians *
                radiansToDegrees)),
            static_cast<int>(std::lround(profile.aimPitchRadians *
                radiansToDegrees)),
            static_cast<int>(std::lround(profile.aimYawRadians *
                radiansToDegrees)));
    }
    else
    {
        Log("MotionControls: live-reloaded sh3vr_weapons.ini; no active "
            "weapon profile");
    }
}

static void ResetWeaponPoseProfileState8()
{
    Interop8_SetWeaponDebugOrientation(false, -1, 0.0f, 0.0f, 0.0f);
    std::memset(g_weaponPoseBaselinePalette8, 0,
        sizeof(g_weaponPoseBaselinePalette8));
    std::memset(g_weaponPoseBaselinePaletteMasks8, 0,
        sizeof(g_weaponPoseBaselinePaletteMasks8));
    std::memset(g_weaponPoseBaselinePivot8, 0,
        sizeof(g_weaponPoseBaselinePivot8));
    std::memset(g_weaponPoseBaselineGripPoint8, 0,
        sizeof(g_weaponPoseBaselineGripPoint8));
    std::memset(g_weaponPoseBaselineOrientation8, 0,
        sizeof(g_weaponPoseBaselineOrientation8));
    std::memset(g_weaponPoseBaselinePosition8, 0,
        sizeof(g_weaponPoseBaselinePosition8));
    g_weaponPoseBaselinePaletteValid8 = false;
    g_weaponPoseDebugReferenceBone8 = 0;
    g_weaponPoseBaselineGripPointValid8 = false;
    g_weaponPoseBaselineValid8 = false;
    InterlockedExchange(&g_weaponPoseApplications8, 0);
}

static void SaveWeaponPoseBaselineCache8(int profileIndex)
{
    if (profileIndex < 0 || profileIndex >= SH3VR_WEAPON_PROFILE_COUNT8 ||
        !g_weaponPoseBaselinePaletteValid8)
    {
        return;
    }

    WeaponPoseBaselineCache8& cache =
        g_weaponPoseBaselineCaches8[profileIndex];
    std::memcpy(cache.palette, g_weaponPoseBaselinePalette8,
        sizeof(cache.palette));
    std::memcpy(cache.paletteMasks, g_weaponPoseBaselinePaletteMasks8,
        sizeof(cache.paletteMasks));
    std::memcpy(cache.pivot, g_weaponPoseBaselinePivot8,
        sizeof(cache.pivot));
    cache.gripPointValid = g_weaponPoseBaselineGripPointValid8;
    std::memcpy(cache.gripPoint, g_weaponPoseBaselineGripPoint8,
        sizeof(cache.gripPoint));
    cache.relativeBaselineValid = g_weaponPoseBaselineValid8;
    std::memcpy(cache.relativeOrientation, g_weaponPoseBaselineOrientation8,
        sizeof(cache.relativeOrientation));
    std::memcpy(cache.relativePosition, g_weaponPoseBaselinePosition8,
        sizeof(cache.relativePosition));
    cache.valid = true;
}

static bool RestoreWeaponPoseBaselineCache8(int profileIndex)
{
    if (profileIndex < 0 || profileIndex >= SH3VR_WEAPON_PROFILE_COUNT8)
        return false;

    const WeaponPoseBaselineCache8& cache =
        g_weaponPoseBaselineCaches8[profileIndex];
    if (!cache.valid)
        return false;

    std::memcpy(g_weaponPoseBaselinePalette8, cache.palette,
        sizeof(g_weaponPoseBaselinePalette8));
    std::memcpy(g_weaponPoseBaselinePaletteMasks8, cache.paletteMasks,
        sizeof(g_weaponPoseBaselinePaletteMasks8));
    std::memcpy(g_weaponPoseBaselinePivot8, cache.pivot,
        sizeof(g_weaponPoseBaselinePivot8));
    g_weaponPoseBaselineGripPointValid8 = cache.gripPointValid;
    std::memcpy(g_weaponPoseBaselineGripPoint8, cache.gripPoint,
        sizeof(g_weaponPoseBaselineGripPoint8));
    g_weaponPoseBaselineValid8 = cache.relativeBaselineValid;
    std::memcpy(g_weaponPoseBaselineOrientation8,
        cache.relativeOrientation, sizeof(g_weaponPoseBaselineOrientation8));
    std::memcpy(g_weaponPoseBaselinePosition8,
        cache.relativePosition, sizeof(g_weaponPoseBaselinePosition8));
    g_weaponPoseBaselinePaletteValid8 = true;
    InterlockedExchange(&g_weaponPoseApplications8, 0);
    return true;
}

static int FindWeaponPoseProfile8(UINT vertexCount, UINT primitiveCount)
{
    for (int profileIndex = 0;
        profileIndex < SH3VR_WEAPON_PROFILE_COUNT8; ++profileIndex)
    {
        const WeaponPoseProfile8& profile =
            g_weaponPoseProfiles8[profileIndex];
        for (UINT signature = 0; signature < profile.signatureCount;
            ++signature)
        {
            if (profile.vertexCount[signature] == vertexCount &&
                profile.primitiveCount[signature] == primitiveCount)
            {
                return profileIndex;
            }
        }
    }
    return -1;
}

static void ActivateWeaponPoseProfile8(int profileIndex)
{
    if (profileIndex < 0 || profileIndex >= SH3VR_WEAPON_PROFILE_COUNT8 ||
        profileIndex == g_activeWeaponPoseProfile8)
    {
        return;
    }

    SaveWeaponPoseBaselineCache8(g_activeWeaponPoseProfile8);
    g_activeWeaponPoseProfile8 = profileIndex;
    const WeaponPoseProfile8& profile =
        g_weaponPoseProfiles8[profileIndex];
    ApplyActiveWeaponPoseProfileValues8();
    const bool restoredBaseline =
        RestoreWeaponPoseBaselineCache8(profileIndex);
    if (!restoredBaseline)
        ResetWeaponPoseProfileState8();
    Log("MotionControls: activated %s pose profile (scale %d%%, local position %d/%d/%d, baseline %s)",
        profile.displayName,
        static_cast<int>(std::lround(profile.scale * 100.0f)),
        static_cast<int>(profile.position[0]),
        static_cast<int>(profile.position[1]),
        static_cast<int>(profile.position[2]),
        restoredBaseline ? "restored" : "new");
}

static int ReadInputSetting(const char* key, int defaultValue)
{
    return ReadIniIntSetting("Input", key, defaultValue);
}

std::uint32_t D3D9Hook_GetRoomscaleMovementMask()
{
    return static_cast<std::uint32_t>(InterlockedCompareExchange(
        &g_roomscaleMovementMask8, SH3VR_ROOMSCALE_NONE,
        SH3VR_ROOMSCALE_NONE));
}

bool D3D9Hook_GetCameraModCharacterAlignForward()
{
    const DWORD endTick = static_cast<DWORD>(InterlockedCompareExchange(
        &g_cameraModCharacterAlignEndTick8, 0, 0));
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(endTick - now) <= 0)
        return false;
    return true;
}

static bool ReadProcessBytes(const BYTE* address, BYTE* output, SIZE_T size)
{
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, size,
        &bytesRead) != FALSE && bytesRead == size;
}

static void UpdateGamePostProcessState8()
{
    float scale[3] = { 1.0f, 1.0f, 1.0f };
    std::uint32_t source[4] = {};
    std::uint32_t intensity = 0;
    bool active = false;
    std::uint8_t effectControls[10] = {};
    bool haveEffectControls = false;

    if (g_enableGamePostProcess8)
    {
        const UINT_PTR moduleBase = reinterpret_cast<UINT_PTR>(
            GetModuleHandleA(nullptr));
        const UINT_PTR preferredBase = 0x00400000u;
        const BYTE* statePointerAddress = reinterpret_cast<const BYTE*>(
            moduleBase + (0x0072D150u - preferredBase));
        const BYTE* effectEnabledAddress = reinterpret_cast<const BYTE*>(
            moduleBase + (0x0072C839u - preferredBase));
        const BYTE* intensityAddress = reinterpret_cast<const BYTE*>(
            moduleBase + (0x0072C835u - preferredBase));
        const BYTE* effectControlsAddress = reinterpret_cast<const BYTE*>(
            moduleBase + (0x0072C834u - preferredBase));

        std::uint32_t statePointer = 0;
        std::uint8_t effectEnabled = 0;
        std::uint8_t intensityByte = 0;
        haveEffectControls = ReadProcessBytes(effectControlsAddress,
            effectControls, sizeof(effectControls));
        if (ReadProcessBytes(statePointerAddress,
                reinterpret_cast<BYTE*>(&statePointer),
                sizeof(statePointer)) &&
            ReadProcessBytes(effectEnabledAddress, &effectEnabled,
                sizeof(effectEnabled)) &&
            ReadProcessBytes(intensityAddress, &intensityByte,
                sizeof(intensityByte)) && statePointer != 0 &&
            ReadProcessBytes(reinterpret_cast<const BYTE*>(
                static_cast<UINT_PTR>(statePointer) + 0x18u),
                reinterpret_cast<BYTE*>(source), sizeof(source)))
        {
            active = effectEnabled != 0 && intensityByte != 0;
            for (std::uint32_t channel = 0; channel < 3; ++channel)
            {
                if (source[channel] == 0)
                {
                    active = false;
                    break;
                }
                const std::uint32_t reciprocal = (std::min)(128u,
                    0x4000u / source[channel]);
                scale[channel] = 2.0f * static_cast<float>(reciprocal) /
                    255.0f;
            }
            intensity = (std::min)(255u,
                static_cast<std::uint32_t>(intensityByte) * 2u);
        }
    }

    const float blend = active
        ? static_cast<float>(intensity) / 255.0f : 0.0f;
    // Reuse the location-specific game color multiplier for the separately
    // rendered hand. This keeps it dark in rooms whose native post-process is
    // dark instead of behaving like an emissive overlay.
    const float postProcessLight = active
        ? (scale[0] + scale[1] + scale[2]) / 3.0f : 1.0f;
    // Until the first small backbuffer lighting sample is available, retain
    // the location-specific post-process multiplier as a conservative
    // fallback. Afterwards the hand follows the actual illuminated scene.
    if (!g_leftHandSceneLightValid8)
    {
        g_leftHandSceneLightScale8 = (std::min)(1.0f,
            (std::max)(0.0f, postProcessLight));
        for (float& channel : g_leftHandSceneLightColor8)
            channel = g_leftHandSceneLightScale8;
    }
    Interop8_SetGamePostProcess(active, scale, blend, source, intensity);

    struct DiagnosticSnapshot
    {
        std::uint32_t source[4];
        std::uint8_t controls[10];
    };
    static DiagnosticSnapshot lastSnapshot = {};
    static bool haveLastSnapshot = false;
    static LONG lastDiagnosticFrame = -1000;
    DiagnosticSnapshot snapshot = {};
    std::memcpy(snapshot.source, source, sizeof(snapshot.source));
    if (haveEffectControls)
        std::memcpy(snapshot.controls, effectControls, sizeof(snapshot.controls));
    const LONG presentFrame = InterlockedCompareExchange(&c_present8, 0, 0);
    const bool diagnosticChanged = !haveLastSnapshot ||
        std::memcmp(&snapshot, &lastSnapshot, sizeof(snapshot)) != 0;
    if (active && diagnosticChanged &&
        presentFrame - lastDiagnosticFrame >= 30)
    {
        lastSnapshot = snapshot;
        haveLastSnapshot = true;
        lastDiagnosticFrame = presentFrame;
        g_loggedGamePostProcess8 = true;
        Log("Game post-process parameters captured: source %u,%u,%u,%u, "
            "RGB scale x10000 %u,%u,%u, blend x10000 %u",
            source[0], source[1], source[2], source[3],
            static_cast<unsigned>(scale[0] * 10000.0f + 0.5f),
            static_cast<unsigned>(scale[1] * 10000.0f + 0.5f),
            static_cast<unsigned>(scale[2] * 10000.0f + 0.5f),
            static_cast<unsigned>(blend * 10000.0f + 0.5f));
        Log("Game post-process controls 0x72C834..0x72C83D: "
            "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
            effectControls[0], effectControls[1], effectControls[2],
            effectControls[3], effectControls[4], effectControls[5],
            effectControls[6], effectControls[7], effectControls[8],
            effectControls[9]);
    }
}

static bool WriteExecutableBytes(BYTE* address, const BYTE* bytes, SIZE_T size)
{
    DWORD oldProtection = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;

    SIZE_T bytesWritten = 0;
    const BOOL wrote = WriteProcessMemory(GetCurrentProcess(), address, bytes,
        size, &bytesWritten);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignoredProtection = 0;
    VirtualProtect(address, size, oldProtection, &ignoredProtection);
    return wrote != FALSE && bytesWritten == size;
}

static void ApplyProxyFrameTimeState90Hz()
{
    if (!g_proxyFrameTimeOverrideApplied8)
        return;

    // SH3's native limiter uses both the requested rate and a secondary upper
    // bound. PC Fix mode 1 leaves them at 60. Keep all three rate values in
    // agreement so the original limiter targets 90 instead of clamping back
    // to its 60 Hz default.
    *reinterpret_cast<volatile LONG*>(0x0072C790) = 90;
    *reinterpret_cast<volatile LONG*>(0x0072C798) = 90;
    *reinterpret_cast<volatile LONG*>(0x0072C7E8) = 90;

    // Publish one coherent real-time timing tuple. Running 90 updates with a
    // 60/90 scale keeps animation and movement speed unchanged.
    *reinterpret_cast<volatile float*>(0x070E67A8) = 0.6666666865f;
    *reinterpret_cast<volatile float*>(0x070E67AC) = 0.0111111111f;
    *reinterpret_cast<volatile LONG*>(0x070E67C0) = 1;
    *reinterpret_cast<volatile float*>(0x070E67C4) = 90.0f;

    const LONG call = InterlockedIncrement(&g_proxyFrameTimeOverrideCalls8);
    if (call == 1)
    {
        Log("Proxy frame-time override active at the SH3 frame boundary: "
            "limiter 90, frameScale 0.666667, frameSeconds 0.011111, "
            "nominalFps 90");
    }
}

static bool ApplyProxyFrameTimeOverride90Hz()
{
    if (!g_enableProxyFrameTimeOverride90Hz8)
        return false;

    // The first live-frame experiment proved that the 0x0072C7xx values are
    // internal multi-tick limiter state, not safe target-FPS controls. Writing
    // 90 to them reduced gameplay to roughly one frame per second. Keep this
    // retired setting fail-safe even if an older INI enables it.
    Log("ProxyFrameTimeOverride90Hz is retired after the live limiter-state "
        "test caused a severe gameplay stall; stable timing remains active");
    g_enableProxyFrameTimeOverride90Hz8 = false;
    return false;
}

static void ApplyProxyVirtualFrameTimeTuple90Hz()
{
    if (!g_proxyVirtualMode4Applied8)
        return;

    // FPSMode=4 removes the 60 Hz presentation cap but leaves PC Fix's
    // published simulation tuple at 1/60. At a 90 Hz render cadence that
    // makes every frame advance the world by 1/60 second and accelerates all
    // frame-dependent gameplay. Publish the equivalent real-time 90 Hz tuple
    // without touching SH3's internal multi-tick limiter state at 0x72C7xx.
    *reinterpret_cast<volatile float*>(0x070E67AC) = 0.0111111111f;
    *reinterpret_cast<volatile float*>(0x070E67A8) = 0.6666666865f;
    *reinterpret_cast<volatile LONG*>(0x070E67C0) = 1;
    *reinterpret_cast<volatile float*>(0x070E67C4) = 90.0f;

    const LONG call = InterlockedIncrement(&g_proxyVirtualFrameTimeTupleCalls8);
    if (call == 1)
    {
        Log("Proxy virtual FPSMode=4 scaled game-time tuple active: "
            "frameScale 0.666667, frameSeconds 0.011111, integerScale 1, "
            "nominalFps 90; SH3 0x72C7xx limiter state was not modified");
    }
}

static bool ApplyProxyVirtualFPSMode4()
{
    if (!g_enableProxyVirtualMode490Hz8)
        return false;

    HMODULE pcFix = GetModuleHandleA("Silent_Hill_3_PC_Fix.dll");
    if (!pcFix)
    {
        Log("Proxy virtual FPSMode=4: PC Fix module is not loaded; no "
            "timing changes were made");
        return false;
    }

    BYTE* modeAddress = reinterpret_cast<BYTE*>(pcFix) + 0x0006CED8;
    int mode = -1;
    if (!ReadProcessBytes(modeAddress, reinterpret_cast<BYTE*>(&mode),
            sizeof(mode)))
    {
        Log("Proxy virtual FPSMode=4: failed to read the PC Fix mode; no "
            "timing changes were made");
        return false;
    }
    if (mode != 1 && mode != 4)
    {
        Log("Proxy virtual FPSMode=4: expected PC Fix mode 1, got %d; no "
            "timing changes were made", mode);
        return false;
    }

    if (mode == 1)
    {
        const int virtualMode = 4;
        SIZE_T bytesWritten = 0;
        if (!WriteProcessMemory(GetCurrentProcess(), modeAddress,
                &virtualMode, sizeof(virtualMode), &bytesWritten) ||
            bytesWritten != sizeof(virtualMode))
        {
            Log("Proxy virtual FPSMode=4: failed to switch PC Fix mode at "
                "runtime; no timing changes were made");
            return false;
        }
    }

    g_proxyVirtualMode4Applied8 = true;
    Log("Proxy virtual FPSMode=4 active: user INI remains FPSMode=1; "
        "the proxy will cap the measured PC Fix path at 90 Hz");
    return true;
}

static void EnsureProxyVirtualFPSMode4()
{
    if (!g_proxyVirtualMode4Applied8)
        return;

    HMODULE pcFix = GetModuleHandleA("Silent_Hill_3_PC_Fix.dll");
    if (!pcFix)
        return;
    volatile LONG* modeAddress = reinterpret_cast<volatile LONG*>(
        reinterpret_cast<BYTE*>(pcFix) + 0x0006CED8);
    if (*modeAddress != 4)
        *modeAddress = 4;
}

static bool ApplyProxyNativeTimingUnlock90Hz()
{
    if (!g_enableProxyNativeTimingUnlock90Hz8)
        return false;

    if (ReadPcFixFpsMode() != 1)
    {
        Log("Proxy native 90 Hz timing unlock requires PC Fix FPSMode=1; "
            "no timing bytes were changed");
        return false;
    }

    HMODULE pcFix = GetModuleHandleA("Silent_Hill_3_PC_Fix.dll");
    if (!pcFix)
    {
        Log("Proxy native 90 Hz timing unlock: PC Fix module is not loaded; "
            "no timing bytes were changed");
        return false;
    }

    BYTE* modeTimingBranch = reinterpret_cast<BYTE*>(pcFix) + 0x00004A58;
    BYTE* modeScaleBranch = reinterpret_cast<BYTE*>(pcFix) + 0x000065E8;
    BYTE* scaleClamp = reinterpret_cast<BYTE*>(pcFix) + 0x00006615;
    BYTE* pacingModeImmediate = reinterpret_cast<BYTE*>(pcFix) + 0x00003596;

    BYTE modeTimingBytes[6] = {};
    BYTE modeScaleBytes[2] = {};
    BYTE scaleClampBytes[12] = {};
    BYTE pacingImmediate = 0;
    if (!ReadProcessBytes(modeTimingBranch, modeTimingBytes,
            sizeof(modeTimingBytes)) ||
        !ReadProcessBytes(modeScaleBranch, modeScaleBytes,
            sizeof(modeScaleBytes)) ||
        !ReadProcessBytes(scaleClamp, scaleClampBytes,
            sizeof(scaleClampBytes)) ||
        !ReadProcessBytes(pacingModeImmediate, &pacingImmediate,
            sizeof(pacingImmediate)))
    {
        Log("Proxy native 90 Hz timing unlock: failed to read PC Fix timing "
            "signatures; no timing bytes were changed");
        return false;
    }

    // PC Fix skips elapsed-time publication for FPSMode=1. Change only the
    // conditional branch at 0x4A58 so mode 1 follows its existing mode-4
    // elapsed-time path. The relative displacement is unchanged.
    const bool modeTimingSignature = modeTimingBytes[0] == 0x0F &&
        modeTimingBytes[1] == 0x85;
    // At 0x65E8, redirect the non-mode-4 branch to the existing elapsed-time
    // block at 0x65EA. The two-byte EB 00 instruction is intentionally local.
    const bool modeScaleSignature = modeScaleBytes[0] == 0x75 &&
        modeScaleBytes[1] == 0x24;
    // The relocatable absolute address in MOVSS is wildcarded; the trailing
    // MAXSS opcode is the stable signature for this PC Fix build.
    const bool scaleClampSignature =
        scaleClampBytes[0] == 0xF3 && scaleClampBytes[1] == 0x0F &&
        scaleClampBytes[2] == 0x10 && scaleClampBytes[3] == 0x0D &&
        scaleClampBytes[8] == 0xF3 && scaleClampBytes[9] == 0x0F &&
        scaleClampBytes[10] == 0x5F && scaleClampBytes[11] == 0xC8;
    const bool pacingSignature = pacingImmediate == 0x02;

    if (!modeTimingSignature || !modeScaleSignature ||
        !scaleClampSignature || !pacingSignature)
    {
        Log("Proxy native 90 Hz timing unlock: unexpected PC Fix timing "
            "signature (mode branch %s, scale branch %s, clamp %s, pacing %s); "
            "no timing bytes were changed", modeTimingSignature ? "ok" : "bad",
            modeScaleSignature ? "ok" : "bad",
            scaleClampSignature ? "ok" : "bad",
            pacingSignature ? "ok" : "bad");
        return false;
    }

    const BYTE patchedModeTiming[2] = { 0x0F, 0x84 };
    const BYTE patchedModeScale[2] = { 0xEB, 0x00 };
    const BYTE patchedScaleClamp[12] = {
        0x0F, 0x28, 0xC8,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    const BYTE patchedPacingImmediate = 0x00;

    if (!WriteExecutableBytes(modeTimingBranch, patchedModeTiming,
            sizeof(patchedModeTiming)) ||
        !WriteExecutableBytes(modeScaleBranch, patchedModeScale,
            sizeof(patchedModeScale)) ||
        !WriteExecutableBytes(scaleClamp, patchedScaleClamp,
            sizeof(patchedScaleClamp)) ||
        !WriteExecutableBytes(pacingModeImmediate, &patchedPacingImmediate,
            sizeof(patchedPacingImmediate)))
    {
        Log("Proxy native 90 Hz timing unlock: a PC Fix timing write failed; "
            "the process must be restarted before retrying");
        return false;
    }

    g_proxyNativeTimingUnlockApplied8 = true;
    Log("Proxy native 90 Hz timing unlock active: PC Fix FPSMode=1 now uses "
        "measured elapsed time, fractional frame scale, and the proxy 90 Hz "
        "presentation deadline");
    return true;
}

static bool ApplyThirteenAGFrameRateFix()
{
    if (g_proxyVirtualMode4Applied8)
    {
        Log("ThirteenAG byte patch skipped because proxy virtual FPSMode=4 "
            "is active");
        return false;
    }
    if (g_proxyNativeTimingUnlockApplied8)
    {
        Log("ThirteenAG byte patch skipped because the proxy native timing "
            "unlock is active");
        return false;
    }
    if (!g_enableThirteenAGFrameRateFix8 || ReadPcFixFpsMode() != 1)
        return false;

    HMODULE executable = GetModuleHandleA(nullptr);
    if (!executable)
        return false;

    // ThirteenAG's Silent Hill 3 patch changes the first byte of the
    // timestamp accumulator at image address 0x41B5D1 from DEC EDX (0x4A)
    // to INC EDX (0x42). This keeps the original simulation at a stable
    // 60 Hz and prevents the native 60/30 fluctuation.
    BYTE* patchAddress = reinterpret_cast<BYTE*>(executable) + 0x0001B5D1;
    BYTE currentByte = 0;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), patchAddress, &currentByte,
        sizeof(currentByte), &bytesRead) || bytesRead != sizeof(currentByte))
    {
        Log("ThirteenAG frame-rate fix: failed to read opcode at 0x41B5D1");
        return false;
    }

    if (currentByte == 0x42)
    {
        g_thirteenAGFrameRateFixApplied8 = true;
    }
    else if (currentByte == 0x4A)
    {
        DWORD oldProtection = 0;
        if (VirtualProtect(patchAddress, sizeof(BYTE), PAGE_EXECUTE_READWRITE,
            &oldProtection))
        {
            const BYTE replacement = 0x42;
            SIZE_T bytesWritten = 0;
            const BOOL wrote = WriteProcessMemory(GetCurrentProcess(),
                patchAddress, &replacement, sizeof(replacement), &bytesWritten);
            FlushInstructionCache(GetCurrentProcess(), patchAddress,
                sizeof(BYTE));
            DWORD ignoredProtection = 0;
            VirtualProtect(patchAddress, sizeof(BYTE), oldProtection,
                &ignoredProtection);
            g_thirteenAGFrameRateFixApplied8 = wrote &&
                bytesWritten == sizeof(replacement);
        }
    }
    else
    {
        Log("ThirteenAG frame-rate fix: unexpected opcode 0x%02X at 0x41B5D1; "
            "no write performed", currentByte);
    }

    if (!g_thirteenAGFrameRateFixLogged8)
    {
        g_thirteenAGFrameRateFixLogged8 = true;
        Log("ThirteenAG frame-rate fluctuation fix %s (opcode 0x%02X -> 0x42); "
            "simulation remains at the PC Fix 60 Hz baseline",
            g_thirteenAGFrameRateFixApplied8 ? "active" : "not applied",
            currentByte);
    }
    return g_thirteenAGFrameRateFixApplied8;
}

static bool Apply90FpsTimingPatch()
{
    // The experimental path deliberately leaves PC Fix's timing globals alone.
    // FPSMode=4 lets the outer loop render at the requested refresh rate; the
    // SH3 frame-preparation hook below keeps its simulation cadence at 60 Hz.
    if (!g_enableFixedStep90Test8 || ReadPcFixFpsMode() != 4)
        return false;

    if (!g_fixedStep90ModeLogged8)
    {
        g_fixedStep90ModeLogged8 = true;
        Log("Fixed-step 90 Hz test enabled: FPSMode=4, render pacing 90 Hz, "
            "simulation cadence 60 Hz; PC Fix timing globals are untouched");
    }
    return true;
}

static void LogGameTimingState(LONG frame)
{
    struct GameTimingState
    {
        float frameScale;
        float frameSeconds;
        std::uint32_t padding[4];
        std::int32_t integerScale;
        float nominalFps;
    };

    GameTimingState state = {};
    SIZE_T bytesRead = 0;
    const bool gameStateRead = ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(0x070E67A8), &state, sizeof(state),
        &bytesRead) != FALSE && bytesRead == sizeof(state);

    const int pcFixFpsMode = ReadPcFixFpsMode();

    if (gameStateRead)
    {
        std::uint32_t frameScaleBits = 0;
        std::uint32_t frameSecondsBits = 0;
        std::uint32_t nominalFpsBits = 0;
        std::memcpy(&frameScaleBits, &state.frameScale,
            sizeof(frameScaleBits));
        std::memcpy(&frameSecondsBits, &state.frameSeconds,
            sizeof(frameSecondsBits));
        std::memcpy(&nominalFpsBits, &state.nominalFps,
            sizeof(nominalFpsBits));
        const std::int32_t frameScaleX1e6 = static_cast<std::int32_t>(
            state.frameScale * 1000000.0f);
        const std::int32_t frameSecondsX1e9 = static_cast<std::int32_t>(
            state.frameSeconds * 1000000000.0f);
        const std::int32_t nominalFpsX1e3 = static_cast<std::int32_t>(
            state.nominalFps * 1000.0f);
        Log("Game timing state at Present %d: PC Fix FPSMode %d, "
            "frameScale bits 0x%08X x1e6 %d, frameSeconds bits 0x%08X "
            "x1e9 %d, integerScale %d, nominalFps bits 0x%08X x1e3 %d",
            frame, pcFixFpsMode, frameScaleBits, frameScaleX1e6,
            frameSecondsBits, frameSecondsX1e9, state.integerScale,
            nominalFpsBits, nominalFpsX1e3);
    }
    else
    {
        Log("Game timing state at Present %d could not be read", frame);
    }
}

static void __cdecl hk_SH3_RenderFrame()
{
    InterlockedIncrement(&c_sh3RenderFrame);
    InterlockedIncrement(&c_sh3RenderCallsSincePresent);
    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    const bool offscreenPass = g_enableDuplicateRenderTest8 &&
        g_enableOffscreenDuplicateTest8 &&
        BeginOffscreenDuplicatePass(0, &originalColor, &originalDepth);

    o_SH3_RenderFrame();
    if (offscreenPass)
    {
        EndOffscreenDuplicatePass(originalColor, originalDepth);
        o_SH3_RenderFrame();
    }
    else if (g_enableDuplicateRenderTest8)
    {
        if (!g_loggedDuplicateRenderTest8)
        {
            g_loggedDuplicateRenderTest8 = true;
            Log("Duplicate SH3 render test enabled with identical camera state");
        }
        o_SH3_RenderFrame();
    }
}

static void __cdecl hk_SH3_PrepareFrame()
{
    EnsureProxyVirtualFPSMode4();
    ApplyProxyVirtualFrameTimeTuple90Hz();
    ApplyProxyFrameTimeState90Hz();
    InterlockedIncrement(&c_sh3PrepareFrame);
    InterlockedIncrement(&c_sh3PrepareCallsSincePresent);
    g_fixedStep90SkipPostFrame8 = false;

    if (g_enableFixedStep90Test8 && g_nativeEyeHandlesPublished)
    {
        const LONG phase = InterlockedIncrement(&g_fixedStep90Phase8) % 3;
        if (phase == 2)
        {
            g_fixedStep90SkipPostFrame8 = true;
            InterlockedIncrement(&g_fixedStep90SkippedCalls8);
            if (!g_fixedStep90SkipLogged8)
            {
                g_fixedStep90SkipLogged8 = true;
                Log("Fixed-step 90 Hz test: skipping every third SH3 "
                    "frame-preparation call after native VR targets became ready");
            }
            return;
        }
        InterlockedIncrement(&g_fixedStep90UpdateCalls8);
    }
    o_SH3_PrepareFrame();
    ApplyProxyVirtualFrameTimeTuple90Hz();
    ApplyProxyFrameTimeState90Hz();
}

static void __cdecl hk_SH3_PostFrame()
{
    if (g_enableFixedStep90Test8 && g_nativeEyeHandlesPublished &&
        g_fixedStep90SkipPostFrame8)
    {
        g_fixedStep90SkipPostFrame8 = false;
        InterlockedIncrement(&g_fixedStep90PostSkippedCalls8);
        return;
    }

    if (g_enableFixedStep90Test8 && g_nativeEyeHandlesPublished)
        InterlockedIncrement(&g_fixedStep90PostUpdateCalls8);
    o_SH3_PostFrame();
}

static void __cdecl hk_SH3_RenderComposite()
{
    const bool surfaceProbe = g_enableStereoSurfaceProbe8 &&
        InterlockedIncrement(&g_compositeCallsBeforeProbe8) >= 120;
    const bool synchronizedPass = g_enableSynchronizedStereo8;
    const bool duplicatePass = g_enableCompositeDuplicateTest8 || surfaceProbe;

    g_renderEye8 = 0;
    g_applyStereoEyeOffset8 = duplicatePass && synchronizedPass;
    g_forceWaterFullSceneStereo8 = false;
    g_recentOffscreenTargetCount8 = 0;
    std::memset(g_recentOffscreenTargets8, 0,
        sizeof(g_recentOffscreenTargets8));
    o_SH3_RenderComposite();

    // At this point the full gameplay composite has configured its real D3D8
    // render/depth surfaces. Present is too late for this capability test:
    // several SH3 paths have already released or rebound those surfaces there.
    if (g_enableNativeEyeTargetProbe8 && !g_nativeEyeTargetProbeComplete8 &&
        InterlockedIncrement(&g_compositeCallsBeforeNativeEyeTargetProbe8) >= 120)
    {
        std::uint32_t requestedWidth = 0;
        std::uint32_t requestedHeight = 0;
        std::uint32_t requestedSamples = 0;
        if (Interop8_GetRequestedEyeResolution(&requestedWidth,
            &requestedHeight, &requestedSamples))
        {
            g_nativeEyeTargetProbeComplete8 = true;
            if (!ProbeNativeEyeRenderTargets())
            {
                Log("Native OpenXR eye target capability probe failed inside "
                    "RenderComposite for %ux%u, samples %u", requestedWidth,
                    requestedHeight, requestedSamples);
            }
        }
    }
    if ((g_heavyFullSceneStereo8 || g_forceWaterFullSceneStereo8) &&
        g_viewProjectionAppliedThisFrame8)
    {
        g_fullSceneStereoPairReady8 = RenderHeavyFullSceneStereo();
        if (g_forceWaterFullSceneStereo8 &&
            g_fullSceneStereoPairReady8 && !g_loggedWaterFullSceneStereo8)
        {
            g_loggedWaterFullSceneStereo8 = true;
            Log("Per-eye full-scene replay enabled for the planar water RTT");
        }
        return;
    }
    if (!duplicatePass)
    {
        g_applyStereoEyeOffset8 = false;
        return;
    }

    if (surfaceProbe)
    {
        ProbeCurrentStereoSurface();
        ProbeStereoBackBuffer("explicit backbuffer after first composite");
        g_enableStereoSurfaceProbe8 = false;
        g_probePresentBackBufferThisFrame8 = true;
    }

    if (synchronizedPass)
    {
        Interop8_SetRenderMode(SH3VR_RENDER_IMMERSIVE_STEREO);
        Interop8_SetRenderEye(0);
        CaptureStereoBackBufferEye(0);
    }

    // The second eye is a render-only replay of the already prepared game
    // state. Calling SH3's frame-preparation routine here would advance
    // animation, streaming, and room-transition state a second time and can
    // make the two eyes disagree or reintroduce the bakery GPU watchdog.
    if (g_enableFullPassStereo8)
    {
        if (!g_loggedFullPassPrepareSuppressed8)
        {
            g_loggedFullPassPrepareSuppressed8 = true;
            Log("Full-pass stereo: suppressed second SH3 frame-preparation "
                "call; second eye reuses the current simulation state");
        }
    }
    else
    {
        o_SH3_PrepareFrame();
    }
    if (!g_loggedCompositeDuplicateTest8)
    {
        g_loggedCompositeDuplicateTest8 = true;
        Log("Full composite synchronized stereo enabled with per-eye IPD");
    }
    g_renderEye8 = 1;
    Interop8_SetRenderEye(1);
    o_SH3_RenderComposite();
    if (synchronizedPass)
        g_stereoPairPublishedThisFrame8 = CaptureStereoBackBufferEye(1);
    g_applyStereoEyeOffset8 = false;
}

struct D3D8ShaderDrawGroup
{
    DWORD shader;
    LONG drawCalls;
    LONG indexedDrawCalls;
    DWORD primitives;
};

static const int SH3VR_MAX_SHADER_DRAW_GROUPS = 32;
static D3D8ShaderDrawGroup g_shaderDrawGroups8[SH3VR_MAX_SHADER_DRAW_GROUPS] = {};
static int g_shaderDrawGroupCount8 = 0;

static HANDLE        g_heartbeat = nullptr;
static volatile LONG g_stopHeartbeat = 0;

static void* g_hookedAddresses[32] = {};
static int   g_hookedCount = 0;

static bool AlreadyHooked(void* address)
{
    for (int i = 0; i < g_hookedCount; ++i)
        if (g_hookedAddresses[i] == address)
            return true;
    return false;
}

// Creates a hook and logs the target so that foreign detours stay visible.
static bool HookOne(const char* name, void* target, void* detour, void** original)
{
    if (!target)
    {
        Log("  %-38s : target is null", name);
        return false;
    }

    if (AlreadyHooked(target))
    {
        Log("  %-38s at 0x%08X already hooked, skipping",
            name, (unsigned)(UINT_PTR)target);
        return true;
    }

    const BYTE first = *static_cast<const BYTE*>(target);
    const char* note = (first == 0xE9 || first == 0xE8 || first == 0xFF)
        ? "POSSIBLE FOREIGN HOOK" : "clean";
    Log("  %-38s at 0x%08X, first byte 0x%02X (%s)",
        name, (unsigned)(UINT_PTR)target, first, note);

    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK)
    {
        Log("  ERROR: MH_CreateHook(%s) = %d", name, (int)status);
        return false;
    }

    if (g_hookedCount < 32)
        g_hookedAddresses[g_hookedCount++] = target;
    return true;
}

// =============================================================================
//  Chain A: Direct3D 8, the API Silent Hill 3 itself calls
// =============================================================================

static void ProbeD3D8Device(IDirect3DDevice8* device)
{
    if (g_probed8 || !device)
        return;

    g_probed8 = true;

    // The probe already answered its question: the process renders through the
    // stock Direct3D 8 runtime with no D3D9 anywhere. Set this to true to bring
    // the full module and capability dump back when something changes.
    const bool runFullProbe = false;
    if (runFullProbe)
        VR_ProbeDeviceCapabilities(device);
}

static int MatrixElementX10000(float value)
{
    return static_cast<int>(value * 10000.0f);
}

static void LogD3D8Matrix(const char* name, const D3DMATRIX& matrix)
{
    Log("D3D8 %s matrix x10000:", name);
    for (int row = 0; row < 4; ++row)
    {
        Log("  [%d %d %d %d]",
            MatrixElementX10000(matrix.m[row][0]),
            MatrixElementX10000(matrix.m[row][1]),
            MatrixElementX10000(matrix.m[row][2]),
            MatrixElementX10000(matrix.m[row][3]));
    }
}

static void LogD3D8TransformDiagnostics(LONG frame)
{
    const bool sample = frame == 1 ||
        (frame <= 3600 && frame % 60 == 0) ||
        frame % 600 == 0;

    if (!sample)
        return;

    Log("D3D8 transforms frame %d: view calls %d, projection calls %d",
        frame, g_viewCallsThisFrame8, g_projectionCallsThisFrame8);

    if (g_haveView8)
        LogD3D8Matrix("VIEW", g_lastView8);
    else
        Log("D3D8 VIEW matrix not observed yet");

    if (g_haveProjection8)
        LogD3D8Matrix("PROJECTION", g_lastProjection8);
    else
        Log("D3D8 PROJECTION matrix not observed yet");
}

static void CaptureD3D8TransformSample(D3D8TransformSample* samples, int& sampleCount,
    const D3DMATRIX& matrix, LONG callOrdinal)
{
    for (int i = 0; i < sampleCount; ++i)
    {
        if (std::memcmp(&samples[i].matrix, &matrix, sizeof(matrix)) == 0)
        {
            samples[i].lastCall = callOrdinal;
            ++samples[i].occurrences;
            return;
        }
    }

    if (sampleCount >= SH3VR_MAX_TRANSFORM_SAMPLES)
        return;

    D3D8TransformSample& sample = samples[sampleCount++];
    std::memcpy(&sample.matrix, &matrix, sizeof(matrix));
    sample.firstCall = callOrdinal;
    sample.lastCall = callOrdinal;
    sample.occurrences = 1;
}

static void LogD3D8TransformSamples(const char* name,
    const D3D8TransformSample* samples, int sampleCount, LONG totalCalls)
{
    Log("D3D8 %s unique matrices: %d of %d calls", name, sampleCount, totalCalls);
    for (int i = 0; i < sampleCount; ++i)
    {
        const D3D8TransformSample& sample = samples[i];
        Log("D3D8 %s sample %d: occurrences %d, calls %d..%d, x10000:",
            name, i, sample.occurrences, sample.firstCall, sample.lastCall);
        for (int row = 0; row < 4; ++row)
        {
            Log("  [%d %d %d %d]",
                MatrixElementX10000(sample.matrix.m[row][0]),
                MatrixElementX10000(sample.matrix.m[row][1]),
                MatrixElementX10000(sample.matrix.m[row][2]),
                MatrixElementX10000(sample.matrix.m[row][3]));
        }
    }
}

static void LogFirstGameplayTransformSet(LONG frame)
{
    if (g_loggedGameplayTransformSet8 || g_viewCallsThisFrame8 < 10 ||
        g_projectionCallsThisFrame8 == 0)
    {
        return;
    }

    g_loggedGameplayTransformSet8 = true;
    Log("=== D3D8 first gameplay transform set at frame %d ===", frame);
    LogD3D8TransformSamples("VIEW", g_viewSamples8, g_viewSampleCount8,
        g_viewCallsThisFrame8);
    LogD3D8TransformSamples("PROJECTION", g_projectionSamples8,
        g_projectionSampleCount8, g_projectionCallsThisFrame8);
    if (g_viewSampleCount8 == SH3VR_MAX_TRANSFORM_SAMPLES)
        Log("D3D8 VIEW sample list reached its capacity");
    if (g_projectionSampleCount8 == SH3VR_MAX_TRANSFORM_SAMPLES)
        Log("D3D8 PROJECTION sample list reached its capacity");
    Log("=== D3D8 first gameplay transform set end ===");
}

static void CaptureD3D8ShaderConstants(DWORD startRegister, const void* data,
    DWORD registerCount)
{
    if (g_loggedGameplayShaderSet8 || !data || registerCount == 0)
        return;

    ++g_vertexShaderConstantCallsThisFrame8;
    const DWORD valueCount = registerCount * 4 < 16 ? registerCount * 4 : 16;
    const float* values = static_cast<const float*>(data);

    for (int i = 0; i < g_shaderConstantGroupCount8; ++i)
    {
        D3D8ShaderConstantGroup& group = g_shaderConstantGroups8[i];
        if (group.shader != g_currentVertexShader8 ||
            group.startRegister != startRegister ||
            group.registerCount != registerCount)
        {
            continue;
        }

        ++group.occurrences;
        if (std::memcmp(group.firstValues, values, valueCount * sizeof(float)) != 0)
            group.changed = true;
        std::memcpy(group.lastValues, values, valueCount * sizeof(float));
        return;
    }

    if (g_shaderConstantGroupCount8 >= SH3VR_MAX_SHADER_CONSTANT_GROUPS)
        return;

    D3D8ShaderConstantGroup& group =
        g_shaderConstantGroups8[g_shaderConstantGroupCount8++];
    group.shader = g_currentVertexShader8;
    group.startRegister = startRegister;
    group.registerCount = registerCount;
    group.occurrences = 1;
    std::memcpy(group.firstValues, values, valueCount * sizeof(float));
    std::memcpy(group.lastValues, values, valueCount * sizeof(float));
}

static void CaptureD3D8ShaderDraw(bool indexed, UINT primitiveCount)
{
    if (g_fullSceneStereoReplayActive8)
        return;

    for (int i = 0; i < g_shaderDrawGroupCount8; ++i)
    {
        D3D8ShaderDrawGroup& group = g_shaderDrawGroups8[i];
        if (group.shader != g_currentVertexShader8)
            continue;

        ++group.drawCalls;
        if (indexed)
            ++group.indexedDrawCalls;
        group.primitives += primitiveCount;
        return;
    }

    if (g_shaderDrawGroupCount8 >= SH3VR_MAX_SHADER_DRAW_GROUPS)
        return;

    D3D8ShaderDrawGroup& group = g_shaderDrawGroups8[g_shaderDrawGroupCount8++];
    group.shader = g_currentVertexShader8;
    group.drawCalls = 1;
    group.indexedDrawCalls = indexed ? 1 : 0;
    group.primitives = primitiveCount;
}

static void LogShaderConstantValues(const char* label, const float values[16],
    DWORD registerCount)
{
    const DWORD rows = registerCount < 4 ? registerCount : 4;
    Log("  %s values x10000:", label);
    for (DWORD row = 0; row < rows; ++row)
    {
        Log("    [%d %d %d %d]",
            MatrixElementX10000(values[row * 4 + 0]),
            MatrixElementX10000(values[row * 4 + 1]),
            MatrixElementX10000(values[row * 4 + 2]),
            MatrixElementX10000(values[row * 4 + 3]));
    }
}

static void LogFirstGameplayShaderSet(LONG frame)
{
    if (g_loggedGameplayShaderSet8 || g_viewCallsThisFrame8 < 10 ||
        g_projectionCallsThisFrame8 == 0)
    {
        return;
    }


    g_loggedGameplayShaderSet8 = true;
    Log("=== D3D8 first gameplay vertex shader set at frame %d ===", frame);
    Log("D3D8 vertex shader changes %d, constant calls %d, groups %d",
        g_vertexShaderChangesThisFrame8,
        g_vertexShaderConstantCallsThisFrame8,
        g_shaderConstantGroupCount8);
    for (int i = 0; i < g_shaderConstantGroupCount8; ++i)
    {
        const D3D8ShaderConstantGroup& group = g_shaderConstantGroups8[i];
        Log("D3D8 VS constants group %d: shader 0x%08X, c%d, count %d, "
            "occurrences %d, changed %s",
            i, group.shader, group.startRegister, group.registerCount,
            group.occurrences, group.changed ? "yes" : "no");
        LogShaderConstantValues("first", group.firstValues, group.registerCount);
        if (group.changed)
            LogShaderConstantValues("last", group.lastValues, group.registerCount);
    }
    if (g_shaderConstantGroupCount8 == SH3VR_MAX_SHADER_CONSTANT_GROUPS)
        Log("D3D8 vertex shader constant group list reached its capacity");
    Log("D3D8 shader draw groups: %d", g_shaderDrawGroupCount8);
    for (int i = 0; i < g_shaderDrawGroupCount8; ++i)
    {
        const D3D8ShaderDrawGroup& group = g_shaderDrawGroups8[i];
        Log("D3D8 shader 0x%08X draws %d, indexed %d, primitives %u",
            group.shader, group.drawCalls, group.indexedDrawCalls,
            (unsigned)group.primitives);
    }
    Log("=== D3D8 first gameplay vertex shader set end ===");
}

static D3D12QueueDiagnostic* FindD3D12QueueDiagnostic(
    ID3D12CommandQueue* queue)
{
    const LONG count = InterlockedCompareExchange(
        &g_d3d12QueueDiagnosticCount, 0, 0);
    for (LONG index = 0; index < count; ++index)
    {
        if (g_d3d12QueueDiagnostics[index].queue == queue)
            return &g_d3d12QueueDiagnostics[index];
    }
    return nullptr;
}

static D3D12QueueDiagnostic* RegisterD3D12QueueDiagnostic(
    ID3D12CommandQueue* queue,
    const D3D12_COMMAND_QUEUE_DESC* description)
{
    if (!queue)
        return nullptr;
    D3D12QueueDiagnostic* existing = FindD3D12QueueDiagnostic(queue);
    if (existing)
        return existing;

    const LONG index = InterlockedCompareExchange(
        &g_d3d12QueueDiagnosticCount, 0, 0);
    if (index >= SH3VR_MAX_D3D12_QUEUES)
    {
        Log("D3D12 queue diagnostic capacity reached");
        return nullptr;
    }

    D3D12QueueDiagnostic& diagnostic = g_d3d12QueueDiagnostics[index];
    diagnostic.type = description ? description->Type
        : D3D12_COMMAND_LIST_TYPE_DIRECT;
    diagnostic.creationOrdinal = index + 1;
    diagnostic.queue = queue;
    InterlockedIncrement(&g_d3d12QueueDiagnosticCount);
    return &diagnostic;
}

static void STDMETHODCALLTYPE hk_D3D12_ExecuteCommandLists(
    ID3D12CommandQueue* self, UINT count,
    ID3D12CommandList* const* commandLists)
{
    D3D12QueueDiagnostic* diagnostic = FindD3D12QueueDiagnostic(self);
    if (diagnostic)
    {
        InterlockedIncrement(&diagnostic->executeCalls);
        InterlockedExchangeAdd(&diagnostic->commandLists,
            static_cast<LONG>(count));
        if (InterlockedCompareExchange(&g_insideD3D8Present, 0, 0) != 0)
            InterlockedIncrement(&diagnostic->callsInsideD3D8Present);
        else
            InterlockedIncrement(&diagnostic->callsOutsideD3D8Present);

        if (InterlockedCompareExchange(&diagnostic->loggedFirstExecute,
            1, 0) == 0)
        {
            Log("D3D12 queue %d first ExecuteCommandLists: object 0x%08X, "
                "type %u, lists %u, inside D3D8 Present %s",
                diagnostic->creationOrdinal,
                static_cast<unsigned>(reinterpret_cast<UINT_PTR>(self)),
                static_cast<unsigned>(diagnostic->type), count,
                InterlockedCompareExchange(&g_insideD3D8Present, 0, 0) != 0
                    ? "yes" : "no");
        }
    }
    PFN_D3D12_ExecuteCommandLists original = diagnostic
        ? diagnostic->executeOriginal : o_D3D12_ExecuteCommandListsFallback;
    if (original)
        original(self, count, commandLists);
}

static void LogD3D12QueueDiagnostics(LONG frame)
{
    if (frame != 60 && frame != 300 && frame % 600 != 0)
        return;

    const LONG count = InterlockedCompareExchange(
        &g_d3d12QueueDiagnosticCount, 0, 0);
    Log("D3D12 queue submission summary at D3D8 frame %d: %d queue(s)",
        frame, count);
    for (LONG index = 0; index < count; ++index)
    {
        D3D12QueueDiagnostic& diagnostic =
            g_d3d12QueueDiagnostics[index];
        Log("  queue %d object 0x%08X type %u: executes %d, lists %d, "
            "inside Present %d, outside Present %d",
            diagnostic.creationOrdinal,
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(diagnostic.queue)),
            static_cast<unsigned>(diagnostic.type),
            InterlockedCompareExchange(&diagnostic.executeCalls, 0, 0),
            InterlockedCompareExchange(&diagnostic.commandLists, 0, 0),
            InterlockedCompareExchange(
                &diagnostic.callsInsideD3D8Present, 0, 0),
            InterlockedCompareExchange(
                &diagnostic.callsOutsideD3D8Present, 0, 0));
    }
}

static bool BuildShaderDumpPath(DWORD shader, const char* suffix, char output[MAX_PATH])
{
    const DWORD length = GetModuleFileNameA(nullptr, output, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    char* slash = std::strrchr(output, '\\');
    if (!slash)
        return false;

    wsprintfA(slash + 1, "sh3vr_vs_%08X.%s.bin", shader, suffix);
    return true;
}

static HRESULT STDMETHODCALLTYPE hk_D3D12_CreateCommandQueue(ID3D12Device* self,
    const D3D12_COMMAND_QUEUE_DESC* description, REFIID iid, void** output)
{
    const HRESULT result = o_D3D12_CreateCommandQueue(self, description, iid,
        output);
    if (SUCCEEDED(result) && output && *output)
    {
        ID3D12CommandQueue* commandQueue =
            static_cast<ID3D12CommandQueue*>(*output);
        D3D12QueueDiagnostic* diagnostic = RegisterD3D12QueueDiagnostic(
            commandQueue, description);
        void** queueVtable = *reinterpret_cast<void***>(commandQueue);
        void* executeTarget = queueVtable[10];
        if (diagnostic)
        {
            diagnostic->executeTarget = executeTarget;
            const LONG queueCount = InterlockedCompareExchange(
                &g_d3d12QueueDiagnosticCount, 0, 0);
            for (LONG index = 0; index < queueCount; ++index)
            {
                D3D12QueueDiagnostic& other = g_d3d12QueueDiagnostics[index];
                if (&other != diagnostic &&
                    other.executeTarget == executeTarget &&
                    other.executeOriginal)
                {
                    diagnostic->executeOriginal = other.executeOriginal;
                    break;
                }
            }
            if (!diagnostic->executeOriginal &&
                HookOne("ID3D12CommandQueue::ExecuteCommandLists",
                    executeTarget, &hk_D3D12_ExecuteCommandLists,
                    reinterpret_cast<void**>(&diagnostic->executeOriginal)))
            {
                if (!o_D3D12_ExecuteCommandListsFallback)
                {
                    o_D3D12_ExecuteCommandListsFallback =
                        diagnostic->executeOriginal;
                }
                MH_EnableHook(executeTarget);
            }
        }

        Log("D3D12 command queue captured: 0x%08X, type %u, priority %d, "
            "flags 0x%X, node %u", static_cast<unsigned>(
                reinterpret_cast<UINT_PTR>(*output)),
            description ? static_cast<unsigned>(description->Type) : 0,
            description ? description->Priority : 0,
            description ? static_cast<unsigned>(description->Flags) : 0,
            description ? description->NodeMask : 0);
        if (description && description->Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
            description->Flags == D3D12_COMMAND_QUEUE_FLAG_NONE)
        {
            ID3D12CommandQueue* queue = nullptr;
            IUnknown* unknown = static_cast<IUnknown*>(*output);
            if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&queue))) && queue)
            {
                if (g_d3d12DirectQueue)
                    g_d3d12DirectQueue->Release();
                g_d3d12DirectQueue = queue;
            }
        }
    }
    return result;
}

static bool IsRequestedEyeResource(const D3D12_RESOURCE_DESC* description)
{
    if (!description || description->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return false;

    std::uint32_t requestedWidth = 0;
    std::uint32_t requestedHeight = 0;
    std::uint32_t requestedSamples = 0;
    return Interop8_GetRequestedEyeResolution(&requestedWidth,
        &requestedHeight, &requestedSamples) &&
        description->Width == requestedWidth &&
        description->Height == requestedHeight;
}

static void LogRequestedEyeD3D12Resource(const char* allocation,
    const D3D12_RESOURCE_DESC* description, D3D12_RESOURCE_STATES initialState,
    HRESULT result, void* resource)
{
    if (!IsRequestedEyeResource(description))
        return;
    Log("D3D12 %s eye resource: %ux%u format %u samples %u flags 0x%X "
        "initial state 0x%X, hr 0x%08X, object 0x%08X", allocation,
        static_cast<unsigned>(description->Width), description->Height,
        static_cast<unsigned>(description->Format), description->SampleDesc.Count,
        static_cast<unsigned>(description->Flags),
        static_cast<unsigned>(initialState), static_cast<unsigned>(result),
        static_cast<unsigned>(reinterpret_cast<UINT_PTR>(resource)));
}

static void CaptureRequestedEyeD3D12Resource(ID3D12Device* device,
    const D3D12_RESOURCE_DESC* description, HRESULT result, void* output)
{
    if (FAILED(result) || !device || !output ||
        !IsRequestedEyeResource(description) ||
        description->Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        (description->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
        g_d3d12NativeEyeResourceCount >= 2)
    {
        return;
    }

    ID3D12Resource* resource = nullptr;
    IUnknown* unknown = static_cast<IUnknown*>(output);
    const HRESULT queryResult = unknown->QueryInterface(IID_PPV_ARGS(&resource));
    if (FAILED(queryResult) || !resource)
    {
        Log("Native eye %u D3D12 resource query failed, hr 0x%08X",
            g_d3d12NativeEyeResourceCount,
            static_cast<unsigned>(queryResult));
        return;
    }

    const UINT eye = g_d3d12NativeEyeResourceCount++;
    g_d3d12NativeEyeResources[eye] = resource;

    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;
    for (UINT set = 0; set < 2; ++set)
    {
        ID3D12Resource* sharedResource = nullptr;
        D3D12_RESOURCE_DESC sharedDescription = *description;
        sharedDescription.SampleDesc.Count = 1;
        sharedDescription.SampleDesc.Quality = 0;
        const D3D12_RESOURCE_STATES destinationState =
            description->SampleDesc.Count > 1
                ? D3D12_RESOURCE_STATE_RESOLVE_DEST
                : D3D12_RESOURCE_STATE_COPY_DEST;
        const HRESULT createResult = o_D3D12_CreateCommittedResource(device,
            &heapProperties, D3D12_HEAP_FLAG_SHARED, &sharedDescription,
            destinationState, nullptr,
            IID_PPV_ARGS(&sharedResource));
        Log("Native eye set %u eye %u shared texture creation: hr 0x%08X, "
            "object 0x%08X", set, eye, static_cast<unsigned>(createResult),
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(sharedResource)));
        if (FAILED(createResult) || !sharedResource)
            continue;
        g_d3d12SharedEyeResources[set][eye] = sharedResource;

        HANDLE sharedHandle = nullptr;
        const HRESULT shareResult = device->CreateSharedHandle(sharedResource,
            nullptr, GENERIC_ALL, nullptr, &sharedHandle);
        Log("Native eye set %u eye %u shared handle: hr 0x%08X, handle "
            "0x%08X", set, eye, static_cast<unsigned>(shareResult),
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(sharedHandle)));
        if (SUCCEEDED(shareResult) && sharedHandle)
        {
            g_d3d12EyeTextureSharedHandles[set][eye] = sharedHandle;
            ++g_d3d12EyeTextureSharedCount;
        }
    }
    if (g_d3d12EyeTextureSharedCount == 4)
    {
        g_nativeEyeWidth = static_cast<UINT>(description->Width);
        g_nativeEyeHeight = description->Height;
        g_nativeEyeFormat = static_cast<UINT>(description->Format);
        g_nativeEyeSampleCount = description->SampleDesc.Count;
        Log("Two native eye buffer sets will be published after the first "
            "asynchronous copy completion");
    }
}

#if 0
static bool CopyNativeEyeResourcesToSharedTextures()
{
    if (!g_d3d12DirectQueue || g_d3d12NativeEyeResourceCount != 2 ||
        !g_d3d12SharedEyeResources[0] || !g_d3d12SharedEyeResources[1])
    {
        Log("Native eye GPU copy unavailable: queue %s, resources %u/2",
            g_d3d12DirectQueue ? "ready" : "missing",
            g_d3d12NativeEyeResourceCount);
        return false;
    }

    ID3D12Device* device = nullptr;
    HRESULT result = g_d3d12NativeEyeResources[0]->GetDevice(
        IID_PPV_ARGS(&device));
    if (FAILED(result) || !device)
        return false;

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(result))
    {
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator, nullptr, IID_PPV_ARGS(&commandList));
    }
    if (SUCCEEDED(result))
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    if (SUCCEEDED(result))
    {
        D3D12_RESOURCE_BARRIER before[2] = {};
        D3D12_RESOURCE_BARRIER after[4] = {};
        for (UINT eye = 0; eye < 2; ++eye)
        {
            before[eye].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            before[eye].Transition.pResource = g_d3d12NativeEyeResources[eye];
            before[eye].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            before[eye].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            before[eye].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

            after[eye * 2] = before[eye];
            after[eye * 2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            after[eye * 2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            after[eye * 2 + 1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            after[eye * 2 + 1].Transition.pResource =
                g_d3d12SharedEyeResources[eye];
            after[eye * 2 + 1].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            after[eye * 2 + 1].Transition.StateBefore =
                D3D12_RESOURCE_STATE_COPY_DEST;
            after[eye * 2 + 1].Transition.StateAfter =
                D3D12_RESOURCE_STATE_COMMON;
        }
        commandList->ResourceBarrier(2, before);
        for (UINT eye = 0; eye < 2; ++eye)
            commandList->CopyResource(g_d3d12SharedEyeResources[eye],
                g_d3d12NativeEyeResources[eye]);
        commandList->ResourceBarrier(4, after);
        result = commandList->Close();
    }

    if (SUCCEEDED(result))
    {
        ID3D12CommandList* lists[] = { commandList };
        g_d3d12DirectQueue->ExecuteCommandLists(1, lists);
        result = g_d3d12DirectQueue->Signal(fence, 1);
    }
    if (SUCCEEDED(result))
    {
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent)
            result = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(result) && fence->GetCompletedValue() < 1)
    {
        result = fence->SetEventOnCompletion(1, fenceEvent);
        if (SUCCEEDED(result) && WaitForSingleObject(fenceEvent, 2000) != WAIT_OBJECT_0)
            result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    Log("Native eye one-shot GPU copy: hr 0x%08X",
        static_cast<unsigned>(result));
    if (fenceEvent)
        CloseHandle(fenceEvent);
    if (fence)
        fence->Release();
    if (commandList)
        commandList->Release();
    if (allocator)
        allocator->Release();
    device->Release();
    return SUCCEEDED(result);
}
#endif

static bool PrepareNativeEyeAsyncCopy()
{
    const LONG copyState = InterlockedCompareExchange(
        &g_nativeEyeCopyState, 0, 0);
    if (g_d3d12NativeEyeResourceCount != 2 ||
        !g_d3d12NativeEyeResources[0] || !g_d3d12NativeEyeResources[1] ||
        !g_d3d12SharedEyeResources[0][0] ||
        !g_d3d12SharedEyeResources[0][1] ||
        !g_d3d12SharedEyeResources[1][0] ||
        !g_d3d12SharedEyeResources[1][1] ||
        (copyState != 0 && copyState != 3))
    {
        return false;
    }

    g_nativeEyeCopyTargetSet = g_nativeEyeHandlesPublished
        ? (g_nativeEyeCompletedSet ^ 1u) : 0u;

    ID3D12Device* device = nullptr;
    HRESULT result = g_d3d12NativeEyeResources[0]->GetDevice(
        IID_PPV_ARGS(&device));
    if (FAILED(result) || !device)
        return false;

    if (!g_nativeEyeCopyQueue)
    {
        if (!o_D3D12_CreateCommandQueue || !g_d3d12DirectQueue)
        {
            device->Release();
            return false;
        }

        const D3D12_COMMAND_QUEUE_DESC gameQueueDescription =
            g_d3d12DirectQueue->GetDesc();
        D3D12_COMMAND_QUEUE_DESC copyQueueDescription = {};
        copyQueueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        copyQueueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        copyQueueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        copyQueueDescription.NodeMask = gameQueueDescription.NodeMask;
        result = o_D3D12_CreateCommandQueue(device, &copyQueueDescription,
            IID_PPV_ARGS(&g_nativeEyeCopyQueue));
        if (SUCCEEDED(result))
        {
            Log("Dedicated native-eye D3D12 copy queue created: 0x%08X",
                static_cast<unsigned>(reinterpret_cast<UINT_PTR>(
                    g_nativeEyeCopyQueue)));
        }
    }

    if (SUCCEEDED(result) && !g_nativeEyeGameFence)
    {
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&g_nativeEyeGameFence));
    }

    if (!g_nativeEyeCopyAllocator)
    {
        if (SUCCEEDED(result))
        {
            result = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&g_nativeEyeCopyAllocator));
        }
        if (SUCCEEDED(result))
        {
            result = device->CreateCommandList(0,
                D3D12_COMMAND_LIST_TYPE_DIRECT, g_nativeEyeCopyAllocator,
                nullptr, IID_PPV_ARGS(&g_nativeEyeCopyCommandList));
        }
    }
    else if (SUCCEEDED(result))
    {
        result = g_nativeEyeCopyAllocator->Reset();
        if (SUCCEEDED(result))
            result = g_nativeEyeCopyCommandList->Reset(
                g_nativeEyeCopyAllocator, nullptr);
    }
    if (SUCCEEDED(result) && !g_nativeEyeCopyFence)
    {
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&g_nativeEyeCopyFence));
    }

    if (SUCCEEDED(result))
    {
        D3D12_RESOURCE_BARRIER before[4] = {};
        D3D12_RESOURCE_BARRIER after[4] = {};
        UINT beforeCount = 0;
        for (UINT eye = 0; eye < 2; ++eye)
        {
            D3D12_RESOURCE_BARRIER& sourceBefore = before[beforeCount++];
            sourceBefore.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            sourceBefore.Transition.pResource =
                g_d3d12NativeEyeResources[eye];
            sourceBefore.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            sourceBefore.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            sourceBefore.Transition.StateAfter = g_nativeEyeSampleCount > 1
                ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                : D3D12_RESOURCE_STATE_COPY_SOURCE;

            if (g_nativeEyeSharedSetInitialized[g_nativeEyeCopyTargetSet])
            {
                D3D12_RESOURCE_BARRIER& destinationBefore =
                    before[beforeCount++];
                destinationBefore.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                destinationBefore.Transition.pResource =
                    g_d3d12SharedEyeResources[g_nativeEyeCopyTargetSet][eye];
                destinationBefore.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                destinationBefore.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_COMMON;
                destinationBefore.Transition.StateAfter =
                    g_nativeEyeSampleCount > 1
                        ? D3D12_RESOURCE_STATE_RESOLVE_DEST
                        : D3D12_RESOURCE_STATE_COPY_DEST;
            }

            after[eye * 2] = sourceBefore;
            after[eye * 2].Transition.StateBefore =
                g_nativeEyeSampleCount > 1
                    ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                    : D3D12_RESOURCE_STATE_COPY_SOURCE;
            after[eye * 2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            after[eye * 2 + 1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            after[eye * 2 + 1].Transition.pResource =
                g_d3d12SharedEyeResources[g_nativeEyeCopyTargetSet][eye];
            after[eye * 2 + 1].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            after[eye * 2 + 1].Transition.StateBefore =
                g_nativeEyeSampleCount > 1
                    ? D3D12_RESOURCE_STATE_RESOLVE_DEST
                    : D3D12_RESOURCE_STATE_COPY_DEST;
            after[eye * 2 + 1].Transition.StateAfter =
                D3D12_RESOURCE_STATE_COMMON;
        }
        g_nativeEyeCopyCommandList->ResourceBarrier(beforeCount, before);
        for (UINT eye = 0; eye < 2; ++eye)
        {
            if (g_nativeEyeSampleCount > 1)
            {
                g_nativeEyeCopyCommandList->ResolveSubresource(
                    g_d3d12SharedEyeResources[g_nativeEyeCopyTargetSet][eye], 0,
                    g_d3d12NativeEyeResources[eye], 0,
                    static_cast<DXGI_FORMAT>(g_nativeEyeFormat));
            }
            else
            {
                g_nativeEyeCopyCommandList->CopyResource(
                    g_d3d12SharedEyeResources[g_nativeEyeCopyTargetSet][eye],
                    g_d3d12NativeEyeResources[eye]);
            }
        }
        g_nativeEyeCopyCommandList->ResourceBarrier(4, after);
        result = g_nativeEyeCopyCommandList->Close();
    }

    device->Release();
    if (FAILED(result))
    {
        Log("Native eye asynchronous copy preparation failed, hr 0x%08X",
            static_cast<unsigned>(result));
        return false;
    }

    if (g_haveLatchedFrameHeadPose8)
        g_nativeEyePendingRenderPose = g_latchedFrameHeadPose8;
    InterlockedExchange(&g_nativeEyeCopyState, 1);
    if (!g_nativeEyeHandlesPublished)
        Log("Native eye asynchronous copy prepared without submitting or waiting");
    return true;
}

static void TrySubmitNativeEyeAsyncCopy(ID3D12CommandQueue* queue,
    PFN_D3D12_ExecuteCommandLists original)
{
    if (!queue || queue != g_d3d12DirectQueue || !original ||
        !g_nativeEyeCopyQueue || !g_nativeEyeCopyCommandList ||
        !g_nativeEyeGameFence || !g_nativeEyeCopyFence ||
        InterlockedCompareExchange(&g_nativeEyeCopyState, 2, 1) != 1)
    {
        return;
    }

    const UINT64 gameFenceValue = ++g_nativeEyeGameFenceValue;
    HRESULT result = queue->Signal(g_nativeEyeGameFence, gameFenceValue);
    if (SUCCEEDED(result))
        result = g_nativeEyeCopyQueue->Wait(g_nativeEyeGameFence,
            gameFenceValue);

    ID3D12CommandList* lists[] = { g_nativeEyeCopyCommandList };
    if (SUCCEEDED(result))
        original(g_nativeEyeCopyQueue, 1, lists);

    if (SUCCEEDED(result))
    {
        g_nativeEyePendingFenceValue = ++g_nativeEyeCopyFenceValue;
        result = g_nativeEyeCopyQueue->Signal(g_nativeEyeCopyFence,
            g_nativeEyePendingFenceValue);
    }
    if (SUCCEEDED(result))
    {
        // Prevent the game's next queue submission from writing the native
        // eye render targets until the dedicated queue has finished reading
        // them. Both waits are GPU-side and do not block the game thread.
        result = queue->Wait(g_nativeEyeCopyFence,
            g_nativeEyePendingFenceValue);
    }
    if (FAILED(result))
    {
        Log("Dedicated native-eye copy submission failed, hr 0x%08X",
            static_cast<unsigned>(result));
        InterlockedExchange(&g_nativeEyeCopyState, 4);
        return;
    }
    if (!g_nativeEyeHandlesPublished)
    {
        Log("Native eye asynchronous copy submitted on the dedicated queue "
            "after synchronized game work; no CPU wait");
    }
}

static void PollNativeEyeAsyncCopy()
{
    if (InterlockedCompareExchange(&g_nativeEyeCopyState, 0, 0) != 2 ||
        !g_nativeEyeCopyFence || g_nativeEyeCopyFence->GetCompletedValue() <
            g_nativeEyePendingFenceValue)
    {
        return;
    }

    g_nativeEyeCompletedSet = g_nativeEyeCopyTargetSet;
    g_nativeEyeSharedSetInitialized[g_nativeEyeCompletedSet] = true;
    if (!g_nativeEyeHandlesPublished)
    {
        Interop8_SetD3D12EyeTextureHandles(
            g_d3d12EyeTextureSharedHandles[0][0],
            g_d3d12EyeTextureSharedHandles[0][1],
            g_d3d12EyeTextureSharedHandles[1][0],
            g_d3d12EyeTextureSharedHandles[1][1],
            g_nativeEyeWidth, g_nativeEyeHeight, g_nativeEyeFormat);
        g_nativeEyeHandlesPublished = true;
        Log("Native eye asynchronous copy completed; double-buffered "
            "handles published to Host");
    }
    ++g_nativeEyeFrameSequence;
    Interop8_SetRenderFlags(SH3VR_RENDER_FLAG_NONE);
    Interop8_SetD3D12EyeTextureFrame(g_nativeEyeCompletedSet,
        g_nativeEyeFrameSequence, g_nativeEyePendingRenderPose);
    if (g_nativeEyeFrameSequence == 1 ||
        g_nativeEyeFrameSequence % 600 == 0)
    {
        Log("Native stereo frame %u completed in shared eye set %u",
            g_nativeEyeFrameSequence, g_nativeEyeCompletedSet);
    }
    InterlockedExchange(&g_nativeEyeCopyState, 3);
}

static HRESULT STDMETHODCALLTYPE hk_D3D12_CreateCommittedResource(
    ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heapProperties,
    D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* description,
    D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
    REFIID iid, void** output)
{
    const HRESULT result = o_D3D12_CreateCommittedResource(self, heapProperties,
        heapFlags, description, initialState, clearValue, iid, output);
    LogRequestedEyeD3D12Resource("committed", description, initialState,
        result, output ? *output : nullptr);
    CaptureRequestedEyeD3D12Resource(self, description, result,
        output ? *output : nullptr);
    return result;
}

static HRESULT STDMETHODCALLTYPE hk_D3D12_CreatePlacedResource(
    ID3D12Device* self, ID3D12Heap* heap, UINT64 heapOffset,
    const D3D12_RESOURCE_DESC* description, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID iid, void** output)
{
    const HRESULT result = o_D3D12_CreatePlacedResource(self, heap, heapOffset,
        description, initialState, clearValue, iid, output);
    LogRequestedEyeD3D12Resource("placed", description, initialState,
        result, output ? *output : nullptr);
    return result;
}

static void HookD3D12Device(ID3D12Device* device)
{
    if (!device)
        return;
    void** vtable = *reinterpret_cast<void***>(device);
    if (HookOne("ID3D12Device::CreateCommandQueue", vtable[8],
        &hk_D3D12_CreateCommandQueue,
        reinterpret_cast<void**>(&o_D3D12_CreateCommandQueue)))
    {
        MH_EnableHook(vtable[8]);
    }
    if (HookOne("ID3D12Device::CreateCommittedResource", vtable[27],
        &hk_D3D12_CreateCommittedResource,
        reinterpret_cast<void**>(&o_D3D12_CreateCommittedResource)))
    {
        MH_EnableHook(vtable[27]);
    }
    if (HookOne("ID3D12Device::CreatePlacedResource", vtable[29],
        &hk_D3D12_CreatePlacedResource,
        reinterpret_cast<void**>(&o_D3D12_CreatePlacedResource)))
    {
        MH_EnableHook(vtable[29]);
    }
}

static HRESULT WINAPI hk_D3D12CreateDevice(IUnknown* adapter,
    D3D_FEATURE_LEVEL minimumLevel, REFIID iid, void** output)
{
    const HRESULT result = o_D3D12CreateDevice(adapter, minimumLevel, iid,
        output);
    Log("D3D12CreateDevice: level 0x%X, hr 0x%08X, object 0x%08X",
        static_cast<unsigned>(minimumLevel), static_cast<unsigned>(result),
        output && *output ? static_cast<unsigned>(
            reinterpret_cast<UINT_PTR>(*output)) : 0);
    if (SUCCEEDED(result) && output && *output)
    {
        ID3D12Device* device = nullptr;
        IUnknown* unknown = static_cast<IUnknown*>(*output);
        if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&device))) && device)
        {
            HookD3D12Device(device);
            device->Release();
        }
    }
    return result;
}

static void ReleaseDxgiBackBufferSharedHandles()
{
    Interop8_SetD3D12BackBufferHandles(nullptr, nullptr, 0, 0);
    for (UINT index = 0; index < 2; ++index)
    {
        if (g_d3d12BackBufferSharedHandles[index])
        {
            CloseHandle(g_d3d12BackBufferSharedHandles[index]);
            g_d3d12BackBufferSharedHandles[index] = nullptr;
        }
    }
    g_d3d12BackBufferSharedCount = 0;
    g_dxgiSharedSwapChain = nullptr;
}

static void ReleaseD3D12EyeTextureSharedHandles()
{
    Interop8_SetD3D12EyeTextureHandles(nullptr, nullptr, nullptr, nullptr,
        0, 0, 0);
    const Sh3VrHeadPose emptyPose = {};
    Interop8_SetD3D12EyeTextureFrame(0, 0, emptyPose);
    Interop8_SetRenderFlags(SH3VR_RENDER_FLAG_NONE);
    if (g_nativeEyeCopyCommandList)
    {
        g_nativeEyeCopyCommandList->Release();
        g_nativeEyeCopyCommandList = nullptr;
    }
    if (g_nativeEyeCopyAllocator)
    {
        g_nativeEyeCopyAllocator->Release();
        g_nativeEyeCopyAllocator = nullptr;
    }
    if (g_nativeEyeGameFence)
    {
        g_nativeEyeGameFence->Release();
        g_nativeEyeGameFence = nullptr;
    }
    if (g_nativeEyeCopyFence)
    {
        g_nativeEyeCopyFence->Release();
        g_nativeEyeCopyFence = nullptr;
    }
    if (g_nativeEyeCopyQueue)
    {
        g_nativeEyeCopyQueue->Release();
        g_nativeEyeCopyQueue = nullptr;
    }
    for (UINT set = 0; set < 2; ++set)
    {
        for (UINT eye = 0; eye < 2; ++eye)
        {
            if (g_d3d12EyeTextureSharedHandles[set][eye])
            {
                CloseHandle(g_d3d12EyeTextureSharedHandles[set][eye]);
                g_d3d12EyeTextureSharedHandles[set][eye] = nullptr;
            }
            if (g_d3d12SharedEyeResources[set][eye])
            {
                g_d3d12SharedEyeResources[set][eye]->Release();
                g_d3d12SharedEyeResources[set][eye] = nullptr;
            }
        }
        g_nativeEyeSharedSetInitialized[set] = false;
    }
    for (UINT eye = 0; eye < 2; ++eye)
    {
        if (g_d3d12NativeEyeResources[eye])
        {
            g_d3d12NativeEyeResources[eye]->Release();
            g_d3d12NativeEyeResources[eye] = nullptr;
        }
    }
    if (g_d3d12DirectQueue)
    {
        g_d3d12DirectQueue->Release();
        g_d3d12DirectQueue = nullptr;
    }
    g_d3d12EyeTextureSharedCount = 0;
    g_d3d12NativeEyeResourceCount = 0;
    g_nativeEyeWidth = 0;
    g_nativeEyeHeight = 0;
    g_nativeEyeFormat = 0;
    g_nativeEyeSampleCount = 1;
    g_nativeEyeCopyTargetSet = 0;
    g_nativeEyeCompletedSet = 0;
    g_nativeEyeGameFenceValue = 0;
    g_nativeEyeCopyFenceValue = 0;
    g_nativeEyePendingFenceValue = 0;
    g_nativeEyeFrameSequence = 0;
    g_nativeEyeHandlesPublished = false;
    g_nativeEyePendingRenderPose = {};
    g_consecutiveStereoReplayOverflowFrames8 = 0;
    InterlockedExchange(&g_nativeEyeCopyState, 0);
}

static UINT GetDxgiCurrentBackBufferIndex(IDXGISwapChain* swapChain,
    UINT fallbackIndex)
{
    IDXGISwapChain3* swapChain3 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3),
        reinterpret_cast<void**>(&swapChain3))) && swapChain3)
    {
        const UINT index = swapChain3->GetCurrentBackBufferIndex();
        swapChain3->Release();
        return index;
    }
    return fallbackIndex;
}

static void ProbeDxgiBackBuffer(IDXGISwapChain* swapChain)
{
    if (!swapChain)
        return;

    if (g_dxgiSharedSwapChain != swapChain)
    {
        ReleaseDxgiBackBufferSharedHandles();
        g_dxgiSharedSwapChain = swapChain;
        g_dxgiBackBufferProbed = false;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDescription = {};
    const HRESULT descriptionResult = swapChain->GetDesc(&swapChainDescription);
    if (FAILED(descriptionResult))
        return;

    if (!g_dxgiBackBufferProbed)
    {
        g_dxgiBackBufferProbed = true;
        Log("DXGI backbuffer probe: GetDesc hr 0x%08X, size %ux%u, format %u, "
            "buffers %u, swap effect %u, flags 0x%X",
            static_cast<unsigned>(descriptionResult),
            swapChainDescription.BufferDesc.Width,
            swapChainDescription.BufferDesc.Height,
            static_cast<unsigned>(swapChainDescription.BufferDesc.Format),
            swapChainDescription.BufferCount,
            static_cast<unsigned>(swapChainDescription.SwapEffect),
            swapChainDescription.Flags);
    }

    const UINT sharedCount = swapChainDescription.BufferCount > 2
        ? 2 : swapChainDescription.BufferCount;
    for (UINT index = 0; index < sharedCount; ++index)
    {
        if (g_d3d12BackBufferSharedHandles[index])
            continue;

        ID3D12Resource* backBuffer = nullptr;
        const HRESULT bufferResult = swapChain->GetBuffer(index,
            IID_PPV_ARGS(&backBuffer));
        Log("DXGI backbuffer probe: GetBuffer(%u) hr 0x%08X, resource 0x%08X",
            index, static_cast<unsigned>(bufferResult), backBuffer
                ? static_cast<unsigned>(reinterpret_cast<UINT_PTR>(backBuffer)) : 0);
        if (FAILED(bufferResult) || !backBuffer)
            continue;

        if (index == 0)
        {
            const D3D12_RESOURCE_DESC resourceDescription = backBuffer->GetDesc();
            Log("D3D12 backbuffer: dimension %u, width %u, height %u, array %u, mip levels %u, "
                "format %u, samples %u/%u, layout %u, flags 0x%X",
                static_cast<unsigned>(resourceDescription.Dimension),
                static_cast<unsigned>(resourceDescription.Width),
                resourceDescription.Height, resourceDescription.DepthOrArraySize,
                resourceDescription.MipLevels,
                static_cast<unsigned>(resourceDescription.Format),
                resourceDescription.SampleDesc.Count,
                resourceDescription.SampleDesc.Quality,
                static_cast<unsigned>(resourceDescription.Layout),
                static_cast<unsigned>(resourceDescription.Flags));
        }

        ID3D12Device* device = nullptr;
        const HRESULT deviceResult = backBuffer->GetDevice(IID_PPV_ARGS(&device));
        Log("D3D12 backbuffer %u device query: hr 0x%08X, device 0x%08X",
            index, static_cast<unsigned>(deviceResult), device
                ? static_cast<unsigned>(reinterpret_cast<UINT_PTR>(device)) : 0);
        if (SUCCEEDED(deviceResult) && device)
        {
            HANDLE sharedHandle = nullptr;
            const HRESULT shareResult = device->CreateSharedHandle(backBuffer,
                nullptr, GENERIC_ALL, nullptr, &sharedHandle);
            Log("D3D12 swapchain backbuffer %u shared handle: hr 0x%08X, handle 0x%08X",
                index, static_cast<unsigned>(shareResult),
                static_cast<unsigned>(reinterpret_cast<UINT_PTR>(sharedHandle)));
            if (SUCCEEDED(shareResult) && sharedHandle)
                g_d3d12BackBufferSharedHandles[index] = sharedHandle;
            device->Release();
        }
        backBuffer->Release();
    }

    g_d3d12BackBufferSharedCount = sharedCount;
    UINT currentIndex = GetDxgiCurrentBackBufferIndex(swapChain, 0);
    if (currentIndex >= sharedCount)
        currentIndex = 0;
    Interop8_SetD3D12BackBufferHandles(
        g_d3d12BackBufferSharedHandles[0],
        g_d3d12BackBufferSharedHandles[1],
        currentIndex,
        g_d3d12BackBufferSharedCount);
}

static HRESULT STDMETHODCALLTYPE hk_DXGI_Present(IDXGISwapChain* self,
    UINT syncInterval, UINT flags)
{
    ProbeDxgiBackBuffer(self);
    return o_DXGI_Present(self, syncInterval, flags);
}

static void HookDxgiSwapChain(IDXGISwapChain* swapChain)
{
    if (!swapChain)
        return;
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (HookOne("IDXGISwapChain::Present", vtable[8], &hk_DXGI_Present,
        reinterpret_cast<void**>(&o_DXGI_Present)))
    {
        MH_EnableHook(vtable[8]);
    }
}

static HRESULT STDMETHODCALLTYPE hk_DXGI_CreateSwapChain(IDXGIFactory* self,
    IUnknown* device, DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** output)
{
    const HRESULT result = o_DXGI_CreateSwapChain(self, device, description,
        output);
    Log("DXGI CreateSwapChain: %ux%u, buffers %u, hr 0x%08X, object 0x%08X",
        description ? description->BufferDesc.Width : 0,
        description ? description->BufferDesc.Height : 0,
        description ? description->BufferCount : 0,
        static_cast<unsigned>(result), output && *output
            ? static_cast<unsigned>(reinterpret_cast<UINT_PTR>(*output)) : 0);
    if (SUCCEEDED(result) && output && *output)
        HookDxgiSwapChain(*output);
    return result;
}

static HRESULT STDMETHODCALLTYPE hk_DXGI_CreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* description,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* restrictToOutput, IDXGISwapChain1** output)
{
    const HRESULT result = o_DXGI_CreateSwapChainForHwnd(self, device, window,
        description, fullscreen, restrictToOutput, output);
    Log("DXGI CreateSwapChainForHwnd: %ux%u, buffers %u, hr 0x%08X, "
        "object 0x%08X", description ? description->Width : 0,
        description ? description->Height : 0,
        description ? description->BufferCount : 0,
        static_cast<unsigned>(result), output && *output
            ? static_cast<unsigned>(reinterpret_cast<UINT_PTR>(*output)) : 0);
    if (SUCCEEDED(result) && output && *output)
        HookDxgiSwapChain(*output);
    return result;
}

static HRESULT STDMETHODCALLTYPE hk_DXGI_CreateSwapChainForComposition(
    IDXGIFactory2* self, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* description, IDXGIOutput* restrictToOutput,
    IDXGISwapChain1** output)
{
    const HRESULT result = o_DXGI_CreateSwapChainForComposition(self, device,
        description, restrictToOutput, output);
    Log("DXGI CreateSwapChainForComposition: %ux%u, buffers %u, hr 0x%08X, "
        "object 0x%08X", description ? description->Width : 0,
        description ? description->Height : 0,
        description ? description->BufferCount : 0,
        static_cast<unsigned>(result), output && *output
            ? static_cast<unsigned>(reinterpret_cast<UINT_PTR>(*output)) : 0);
    if (SUCCEEDED(result) && output && *output)
        HookDxgiSwapChain(*output);
    return result;
}

static void HookDxgiFactory(IUnknown* factoryUnknown)
{
    if (!factoryUnknown)
        return;
    IDXGIFactory2* factory = nullptr;
    if (FAILED(factoryUnknown->QueryInterface(IID_PPV_ARGS(&factory))) ||
        !factory)
    {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(factory);
    HookOne("IDXGIFactory::CreateSwapChain", vtable[10],
        &hk_DXGI_CreateSwapChain,
        reinterpret_cast<void**>(&o_DXGI_CreateSwapChain));
    HookOne("IDXGIFactory2::CreateSwapChainForHwnd", vtable[15],
        &hk_DXGI_CreateSwapChainForHwnd,
        reinterpret_cast<void**>(&o_DXGI_CreateSwapChainForHwnd));
    HookOne("IDXGIFactory2::CreateSwapChainForComposition", vtable[22],
        &hk_DXGI_CreateSwapChainForComposition,
        reinterpret_cast<void**>(&o_DXGI_CreateSwapChainForComposition));
    MH_EnableHook(MH_ALL_HOOKS);
    factory->Release();
}

static HRESULT WINAPI hk_CreateDXGIFactory2(UINT flags, REFIID iid,
    void** output)
{
    const HRESULT result = o_CreateDXGIFactory2(flags, iid, output);
    Log("CreateDXGIFactory2: flags 0x%X, hr 0x%08X, object 0x%08X",
        flags, static_cast<unsigned>(result), output && *output
            ? static_cast<unsigned>(reinterpret_cast<UINT_PTR>(*output)) : 0);
    if (SUCCEEDED(result) && output && *output)
        HookDxgiFactory(static_cast<IUnknown*>(*output));
    return result;
}

static void DumpD3D8VertexShaderBlob(IDirect3DDevice8* device, DWORD shader,
    int methodSlot, const char* suffix)
{
    void** vtable = *reinterpret_cast<void***>(device);
    PFN_D3D8_GetVertexShaderBlob getBlob =
        reinterpret_cast<PFN_D3D8_GetVertexShaderBlob>(vtable[methodSlot]);
    if (!getBlob)
        return;

    DWORD byteCount = 0;
    if (FAILED(getBlob(device, shader, nullptr, &byteCount)) || byteCount == 0 ||
        byteCount > 1024 * 1024)
    {
        return;
    }

    void* data = HeapAlloc(GetProcessHeap(), 0, byteCount);
    if (!data)
        return;

    if (SUCCEEDED(getBlob(device, shader, data, &byteCount)))
    {
        char path[MAX_PATH] = {};
        if (BuildShaderDumpPath(shader, suffix, path))
        {
            HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(file, data, byteCount, &written, nullptr);
                CloseHandle(file);
                Log("D3D8 vertex shader 0x%08X %s dump: %u bytes",
                    shader, suffix, (unsigned)written);
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, data);
}

static HRESULT WINAPI hk_D3D8_CreateVertexShader(IDirect3DDevice8* device,
    const DWORD* declaration, const DWORD* function, DWORD* shader, DWORD usage)
{
    const HRESULT result = o_D3D8_CreateVertexShader(device, declaration,
        function, shader, usage);
    if (g_enableShaderDumps8 && SUCCEEDED(result) && shader)
    {
        DumpD3D8VertexShaderBlob(device, *shader,
            VT8_GetVertexShaderDeclaration, "decl");
        DumpD3D8VertexShaderBlob(device, *shader,
            VT8_GetVertexShaderFunction, "code");
    }
    return result;
}

static bool ApplyHeadRotationToShaderCamera(const float gameView[12],
    float output[12]);
static bool ApplyHeadRotationToViewProjection(const float gameViewProjection[16],
    float output[16]);

static void LogVrCameraShaderApplication(DWORD shader)
{
    for (std::uint32_t index = 0; index < g_loggedVrCameraShaderCount8; ++index)
    {
        if (g_loggedVrCameraShaders8[index] == shader)
            return;
    }
    if (g_loggedVrCameraShaderCount8 >=
        sizeof(g_loggedVrCameraShaders8) / sizeof(g_loggedVrCameraShaders8[0]))
    {
        return;
    }

    g_loggedVrCameraShaders8[g_loggedVrCameraShaderCount8++] = shader;
    Log("6-DOF view-projection applied to vertex shader 0x%08X", shader);
}

static bool IsStereoReplayShader(DWORD shader)
{
    // Keep this as an explicit allowlist. Screen-space and render-to-texture
    // effects can also upload values that resemble the camera c2-c5 matrix,
    // but replaying those draws directly into an eye target produces stretched
    // shadows, blur, and decals because their intermediate textures belong to
    // the original flat render pass.
    //
    // Shader 0x2D draws the world. Character and enemy materials use several
    // equivalent shader families in different rooms. Each family has a base
    // opaque pass followed by material, hair, or effect passes that reuse the
    // same camera constants without uploading c2-c5 again.
    if (g_stereoReplayWorldOnly8)
        return shader == 0x2D;

    switch (shader)
    {
    case 0x2D:
    // Small transparent/emissive 3D passes used by lamps and EXIT signage.
    // This shader uses the normal c2-c5 view-projection and is not a
    // pre-transformed screen-space effect.
    case 0x2F:
    // Some locations use this equivalent camera-space shader for emissive
    // lamps and signage instead of the 0x2F material family.
    case 0xCF:
    case 0x87:
    case 0x89:
    case 0x8B:
    // Indexed alpha/material passes that complete Heather's volumetric hair
    // in the amusement-park shader family.
    case 0x8F:
    case 0xCB:
    case 0x5D:
    // Save-game room variants use these additional model/material shaders.
    // They are 3D passes, not screen-space effects, and must be replayed or
    // the native eye targets contain hair/effects without the character body.
    case 0x8D:
    case 0x61:
    case 0xC9:
    case 0xC7:
    case 0x5F:
    case 0xD1:
    case 0xD3:
    // Amusement-park actor material variants selected as Heather and enemies
    // move between lighting sectors. These are indexed camera-space draws;
    // omitting them makes complete actor meshes disappear from native eyes.
    case 0x4D:
    case 0x65:
    case 0x63:
    case 0xD5:
    case 0x5B:
    case 0x45:
    case 0x47:
    case 0x49:
    case 0x3F:
    case 0x41:
    case 0x43:
    case 0x4B:
        return true;
    default:
        return false;
    }
}

static bool IsCanonicalActorDrawForStereo(bool indexed, DWORD primitiveType,
    UINT primitiveCount)
{
    if (!indexed || (primitiveType != D3DPT_TRIANGLESTRIP &&
        primitiveType != D3DPT_TRIANGLELIST))
    {
        return false;
    }

    // D3D8 shader handles are allocation-order dependent, so their numeric
    // values change between saves and locations. Heather, enemy, and held
    // weapon passes consistently use these indexed mesh sizes: base geometry,
    // material shells, and the 240-triangle alpha/hair pass. Identifying this
    // topology avoids replaying large render-to-texture composites such as
    // sewer water while covering the same actor family across every location.
    return primitiveCount == 610u || primitiveCount == 614u ||
        primitiveCount == 890u || primitiveCount == 174u ||
        primitiveCount == 240u;
}

static bool ShouldReplayStereoDraw(DWORD shader, bool indexed,
    DWORD primitiveType, UINT primitiveCount, bool vertexBufferDraw)
{
    if (IsStereoReplayShader(shader) || IsCanonicalActorDrawForStereo(
        indexed, primitiveType, primitiveCount))
    {
        return true;
    }

    if (!vertexBufferDraw || (primitiveType != D3DPT_TRIANGLESTRIP &&
        primitiveType != D3DPT_TRIANGLELIST))
    {
        return false;
    }

    // SH3 submits the real room, actor, weapon, and water-surface geometry
    // from vertex buffers. Shader handles and individual mesh sizes vary with
    // load order, so both are unsuitable identifiers. Small city-sector and
    // actor material batches can contain fewer than the old 16/32 primitive
    // limits and vanished after loading a save with a different shader order.
    // Screen-space effects, shadows, blur, decals, and UI use the separate UP
    // paths; buffered full-screen quads contain only two primitives. Accept
    // every non-quad 3D strip/list so distant room pieces survive in VR.
    return primitiveCount >= 3u;
}

static bool IsHeavySceneDynamicReplayDraw(DWORD shader, bool indexed,
    DWORD primitiveType, UINT primitiveCount, bool vertexBufferDraw)
{
    // The heavy full-scene pass reproduces the non-indexed room/material
    // batches itself. Shader handles are allocation-order dependent: in the
    // bakery the same high-volume family appeared as 0xD1 rather than the
    // previously observed 0x2F and exhausted the replay budget before the
    // held weapon was submitted. Classify by draw topology instead. SH3's
    // skinned actors and held weapons use indexed draws, while the bakery's
    // 676-draw room burst is non-indexed. Preserve every selected indexed draw
    // from the original composite and let the per-eye full-scene pass restore
    // all non-indexed geometry. This keeps weapon/actor state without relying
    // on unstable shader IDs and avoids replaying the room three times.
    if (!indexed)
        return false;
    return ShouldReplayStereoDraw(shader, indexed,
        primitiveType, primitiveCount, vertexBufferDraw);
}

static void LogFirstStereoDrawForShader(const char* method, DWORD primitiveType,
    UINT primitiveCount, bool indexed, bool vertexBufferDraw)
{
    if (!g_perDrawStereoProbeActive8)
        return;
    for (std::uint32_t index = 0; index < g_loggedStereoDrawShaderCount8;
        ++index)
    {
        if (g_loggedStereoDrawShaders8[index] == g_currentVertexShader8)
            return;
    }
    if (g_loggedStereoDrawShaderCount8 >=
        sizeof(g_loggedStereoDrawShaders8) /
            sizeof(g_loggedStereoDrawShaders8[0]))
    {
        return;
    }

    g_loggedStereoDrawShaders8[g_loggedStereoDrawShaderCount8++] =
        g_currentVertexShader8;

    const DWORD positionMask = g_currentVertexShader8 & 0x0000000Eu;
    Log("First stereo-era draw for shader/FVF 0x%08X: method %s, "
        "primitive type %u, primitives %u, position mask 0x%X, replayed %s",
        g_currentVertexShader8, method, static_cast<unsigned>(primitiveType),
        static_cast<unsigned>(primitiveCount),
        static_cast<unsigned>(positionMask),
        ShouldReplayStereoDraw(g_currentVertexShader8, indexed, primitiveType,
            primitiveCount, vertexBufferDraw) ? "yes" : "no");
}

static HRESULT WINAPI hk_D3D8_SetVertexShader(IDirect3DDevice8* device, DWORD shader)
{
    if (g_enableWeaponPosePrototype8 && shader != g_weaponPoseConstantsShader8)
    {
        // Bone constants are shader-local state. Never carry a palette from a
        // previous shader into a verified weapon draw after a scene change.
        g_weaponPoseConstantsShader8 = shader;
        g_weaponPoseBoneAMask8 = 0;
        g_weaponPoseBoneBMask8 = 0;
        std::memset(g_weaponPosePaletteMasks8, 0,
            sizeof(g_weaponPosePaletteMasks8));
    }
    g_currentVertexShader8 = shader;
    ++g_vertexShaderChangesThisFrame8;
    return o_D3D8_SetVertexShader(device, shader);
}

static HRESULT WINAPI hk_D3D8_SetTexture(IDirect3DDevice8* device,
    DWORD stage, IDirect3DBaseTexture8* texture)
{
    const HRESULT result = o_D3D8_SetTexture(device, stage, texture);
    if (SUCCEEDED(result) && stage < _countof(g_currentTextures8) &&
        g_currentTextures8[stage] != texture)
    {
        g_currentTextures8[stage] = texture;
        UINT width = 0;
        UINT height = 0;
        DWORD usage = 0;
        g_currentTextureIsRenderTarget8[stage] =
            DescribeRenderTargetTexture8(texture, &width, &height, &usage);
        g_currentTextureWidths8[stage] = width;
        g_currentTextureHeights8[stage] = height;
    }
    return result;
}

static IDirect3DBaseTexture8* FirstBoundTexture8(DWORD* stage)
{
    for (DWORD index = 0; index < _countof(g_currentTextures8); ++index)
    {
        if (g_currentTextures8[index])
        {
            if (stage)
                *stage = index;
            return g_currentTextures8[index];
        }
    }
    if (stage)
        *stage = 0;
    return nullptr;
}

static void UpdateMotionWeaponDrawCapture8()
{
    if (!g_enableWeaponRenderProbe8)
    {
        g_motionWeaponRightGripWasDown8 = false;
        g_motionWeaponDrawCaptureFrames8 = 0;
        return;
    }

    Sh3VrControllerState controller = {};
    if (!Interop8_ReadControllerState(&controller))
        return;

    const bool rightGripDown =
        (controller.buttons & SH3VR_BUTTON_RIGHT_GRIP) != 0;
    if (rightGripDown && !g_motionWeaponRightGripWasDown8)
    {
        // Streaming and menu fades can occupy the next few game frames. Keep
        // the read-only window long enough to observe the weapon geometry
        // after the grip press without altering any render state.
        g_motionWeaponDrawCaptureFrames8 = 180;
        g_motionWeaponDrawCaptureRecords8 = 0;
        g_motionWeaponConstantCaptureRecords8 = 0;
        const LONG serial = InterlockedIncrement(&g_motionWeaponDrawCaptureSerial8);
        Log("MotionControls: weapon render probe %d armed by the right grip "
            "for the next 180 game frames", serial);
    }
    else if (g_motionWeaponDrawCaptureFrames8 > 0)
    {
        --g_motionWeaponDrawCaptureFrames8;
        if (g_motionWeaponDrawCaptureFrames8 == 0)
        {
            Log("MotionControls: weapon draw capture complete: %d render "
                "records", g_motionWeaponDrawCaptureRecords8);
        }
    }
    g_motionWeaponRightGripWasDown8 = rightGripDown;
}

static void LogMotionWeaponVertexConstants8(DWORD startRegister,
    const void* data, DWORD registerCount)
{
    // The probe records source constants around a candidate weapon draw
    // without changing any game state.
    if (g_motionWeaponDrawCaptureFrames8 <= 0 ||
        g_currentVertexShader8 != 0x00000089u || !data ||
        registerCount == 0 || g_motionWeaponConstantCaptureRecords8 >= 64)
    {
        return;
    }

    const float* values = static_cast<const float*>(data);
    const DWORD rows = (std::min)(registerCount, static_cast<DWORD>(4));
    const LONG record = ++g_motionWeaponConstantCaptureRecords8;
    Log("MotionControls: candidate weapon constants %d: shader 0x%08X, c%u, count %u",
        record, g_currentVertexShader8, static_cast<unsigned>(startRegister),
        static_cast<unsigned>(registerCount));
    for (DWORD row = 0; row < rows; ++row)
    {
        const float* vector = values + static_cast<std::size_t>(row) * 4u;
        Log("  c%u x1000 [%d %d %d %d]",
            static_cast<unsigned>(startRegister + row),
            static_cast<int>(std::lround(vector[0] * 1000.0f)),
            static_cast<int>(std::lround(vector[1] * 1000.0f)),
            static_cast<int>(std::lround(vector[2] * 1000.0f)),
            static_cast<int>(std::lround(vector[3] * 1000.0f)));
    }
}

static void CaptureWeaponPoseConstantRange8(DWORD startRegister,
    const float* values, DWORD registerCount, DWORD targetRegister,
    float target[12], BYTE* mask)
{
    if (!values || !target || !mask)
        return;

    const DWORD endRegister = startRegister + registerCount;
    const DWORD targetEnd = targetRegister + 3;
    const DWORD copyStart = (std::max)(startRegister, targetRegister);
    const DWORD copyEnd = (std::min)(endRegister, targetEnd);
    if (copyStart >= copyEnd)
        return;

    for (DWORD registerIndex = copyStart; registerIndex < copyEnd;
        ++registerIndex)
    {
        const DWORD sourceIndex = registerIndex - startRegister;
        const DWORD targetIndex = registerIndex - targetRegister;
        std::memcpy(target + targetIndex * 4u, values + sourceIndex * 4u,
            4u * sizeof(float));
        *mask |= static_cast<BYTE>(1u << targetIndex);
    }
}

static void CaptureWeaponPoseConstants8(DWORD startRegister,
    const void* data, DWORD registerCount)
{
    if (!g_enableWeaponPosePrototype8 || !data || registerCount == 0)
    {
        return;
    }

    const float* values = static_cast<const float*>(data);
    CaptureWeaponPoseConstantRange8(startRegister, values, registerCount, 48,
        g_weaponPoseBoneA8, &g_weaponPoseBoneAMask8);
    CaptureWeaponPoseConstantRange8(startRegister, values, registerCount, 51,
        g_weaponPoseBoneB8, &g_weaponPoseBoneBMask8);
    for (DWORD bone = 0; bone < SH3VR_WEAPON_PALETTE_BONE_COUNT8; ++bone)
    {
        CaptureWeaponPoseConstantRange8(startRegister, values, registerCount,
            SH3VR_WEAPON_PALETTE_START8 + bone * 3u,
            g_weaponPosePalette8[bone], &g_weaponPosePaletteMasks8[bone]);
    }
}

static void LogMotionWeaponDrawCapture8(IDirect3DDevice8* device,
    const char* method, DWORD primitiveType, UINT primitiveCount,
    bool indexed, UINT firstArgument, UINT secondArgument, UINT thirdArgument,
    UINT sourceStride, const void* caller)
{
    if (g_motionWeaponDrawCaptureFrames8 <= 0 ||
        g_motionWeaponDrawCaptureRecords8 >= 512 || !device)
    {
        return;
    }

    const LONG record = ++g_motionWeaponDrawCaptureRecords8;
    DWORD textureStage = 0;
    const IDirect3DBaseTexture8* texture = FirstBoundTexture8(&textureStage);
    IDirect3DVertexBuffer8* vertexBuffer = nullptr;
    UINT vertexStride = sourceStride;
    if (sourceStride == 0)
    {
        void** deviceVtable = *reinterpret_cast<void***>(device);
        const auto getStreamSource = reinterpret_cast<PFN8_GetStreamSource>(
            deviceVtable[VT8_GetStreamSource]);
        const HRESULT streamResult = getStreamSource(device, 0, &vertexBuffer,
            &vertexStride);
        if (FAILED(streamResult))
        {
            vertexBuffer = nullptr;
            vertexStride = 0;
        }
    }

    // The equipment meshes observed so far are indexed vertex-buffer draws.
    // Excluding screen-space UP passes keeps the extended diagnostic capture
    // concise and avoids mistaking post-process quads for weapon geometry.
    if (!indexed || !vertexBuffer || vertexStride < 12 || vertexStride > 256)
    {
        if (vertexBuffer)
        {
            void** bufferVtable = *reinterpret_cast<void***>(vertexBuffer);
            const auto release = reinterpret_cast<PFNV8_Release>(bufferVtable[2]);
            release(vertexBuffer);
        }
        return;
    }

    HMODULE gameModule = GetModuleHandleA(nullptr);
    const UINT_PTR callerAddress = reinterpret_cast<UINT_PTR>(caller);
    const UINT_PTR gameBase = reinterpret_cast<UINT_PTR>(gameModule);
    const UINT_PTR callerRva = gameModule && callerAddress >= gameBase
        ? callerAddress - gameBase : 0;
    float boundsMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float boundsMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool haveBounds = false;
    // The indexed actor candidate exposes a small, readable vertex range.
    // Inspect only that bounded range; never lock arbitrary scene buffers.
    if (secondArgument > 0 && secondArgument <= 2048u)
    {
        const std::size_t byteCount = static_cast<std::size_t>(secondArgument) *
            static_cast<std::size_t>(vertexStride);
        BYTE* locked = nullptr;
        void** bufferVtable = *reinterpret_cast<void***>(vertexBuffer);
        const auto lock = reinterpret_cast<PFNV8_Lock>(bufferVtable[11]);
        const auto unlock = reinterpret_cast<PFNV8_Unlock>(bufferVtable[12]);
        if (SUCCEEDED(lock(vertexBuffer, 0, static_cast<UINT>(byteCount),
            &locked, 0x00000010u)) && locked)
        {
            for (UINT index = 0; index < secondArgument; ++index)
            {
                const BYTE* vertex = locked +
                    static_cast<std::size_t>(index) * vertexStride;
                float position[3] = {};
                std::memcpy(position, vertex, sizeof(position));
                if (!std::isfinite(position[0]) ||
                    !std::isfinite(position[1]) ||
                    !std::isfinite(position[2]) ||
                    std::fabs(position[0]) > 100000.0f ||
                    std::fabs(position[1]) > 100000.0f ||
                    std::fabs(position[2]) > 100000.0f)
                {
                    continue;
                }
                haveBounds = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    boundsMin[axis] = (std::min)(boundsMin[axis],
                        position[axis]);
                    boundsMax[axis] = (std::max)(boundsMax[axis],
                        position[axis]);
                }
            }
            unlock(vertexBuffer);
        }
    }
    Log("MotionControls: weapon draw %d: %s, shader/FVF 0x%08X, indexed %u, "
        "primitive %u x %u, args %u/%u/%u, stream 0x%08X stride %u, "
        "texture stage %u 0x%08X %ux%u, bounds x1000 "
        "%d/%d y1000 %d/%d z1000 %d/%d, caller RVA 0x%08X",
        record, method, g_currentVertexShader8, indexed ? 1u : 0u,
        static_cast<unsigned>(primitiveType), static_cast<unsigned>(primitiveCount),
        static_cast<unsigned>(firstArgument), static_cast<unsigned>(secondArgument),
        static_cast<unsigned>(thirdArgument),
        static_cast<unsigned>(reinterpret_cast<UINT_PTR>(vertexBuffer)),
        static_cast<unsigned>(vertexStride), static_cast<unsigned>(textureStage),
        static_cast<unsigned>(reinterpret_cast<UINT_PTR>(texture)),
        textureStage < _countof(g_currentTextureWidths8)
            ? g_currentTextureWidths8[textureStage] : 0u,
        textureStage < _countof(g_currentTextureHeights8)
            ? g_currentTextureHeights8[textureStage] : 0u,
        haveBounds ? static_cast<int>(std::lround(boundsMin[0] * 1000.0f)) : 0,
        haveBounds ? static_cast<int>(std::lround(boundsMax[0] * 1000.0f)) : 0,
        haveBounds ? static_cast<int>(std::lround(boundsMin[1] * 1000.0f)) : 0,
        haveBounds ? static_cast<int>(std::lround(boundsMax[1] * 1000.0f)) : 0,
        haveBounds ? static_cast<int>(std::lround(boundsMin[2] * 1000.0f)) : 0,
        haveBounds ? static_cast<int>(std::lround(boundsMax[2] * 1000.0f)) : 0,
        static_cast<unsigned>(callerRva));

    if (vertexBuffer)
    {
        void** bufferVtable = *reinterpret_cast<void***>(vertexBuffer);
        const auto release = reinterpret_cast<PFNV8_Release>(bufferVtable[2]);
        release(vertexBuffer);
    }
}

static float DotFlashlightVector8(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool NormalizeFlashlightVector8(float vector[3])
{
    const float lengthSquared = DotFlashlightVector8(vector, vector);
    if (!std::isfinite(lengthSquared) || lengthSquared < 0.000001f)
        return false;
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    vector[0] *= inverseLength;
    vector[1] *= inverseLength;
    vector[2] *= inverseLength;
    return true;
}

static bool ReadFlashlightViewBasis8(const D3DMATRIX& view, bool useColumns,
    float basis[3][3])
{
    for (int axis = 0; axis < 3; ++axis)
    {
        for (int component = 0; component < 3; ++component)
        {
            basis[axis][component] = useColumns
                ? view.m[component][axis]
                : view.m[axis][component];
        }
        if (!NormalizeFlashlightVector8(basis[axis]))
            return false;
    }
    return true;
}

static bool IsFlashlightProjectionShader8(DWORD shader)
{
    switch (shader)
    {
    case 0x0000000Fu:
    case 0x00000015u:
    case 0x00000019u:
    case 0x0000002Du:
        return true;
    default:
        return false;
    }
}

static bool ApplyHeadTrackedFlashlightProjection8(const float source[12],
    float output[12])
{
    if (!g_enableHeadTrackedFlashlight8 || !g_haveFlashlightViewPair8 ||
        !source || !output)
    {
        return false;
    }

    float baseBasis[3][3] = {};
    float vrBasis[3][3] = {};
    if (!ReadFlashlightViewBasis8(g_flashlightBaseView8, false, baseBasis) ||
        !ReadFlashlightViewBasis8(g_flashlightVrView8, false, vrBasis))
    {
        return false;
    }

    for (int projectionAxis = 0; projectionAxis < 3; ++projectionAxis)
    {
        float gameAxis[3] = {
            source[projectionAxis * 4 + 0],
            source[projectionAxis * 4 + 1],
            source[projectionAxis * 4 + 2]
        };
        const float axisLengthSquared = DotFlashlightVector8(gameAxis,
            gameAxis);
        if (!std::isfinite(axisLengthSquared) ||
            axisLengthSquared < 0.000001f)
        {
            return false;
        }
        const float axisLength = std::sqrt(axisLengthSquared);
        if (!NormalizeFlashlightVector8(gameAxis))
            return false;

        const float localAxis[3] = {
            DotFlashlightVector8(gameAxis, baseBasis[0]),
            DotFlashlightVector8(gameAxis, baseBasis[1]),
            DotFlashlightVector8(gameAxis, baseBasis[2])
        };
        float headAxis[3] = {};
        for (int component = 0; component < 3; ++component)
        {
            headAxis[component] =
                localAxis[0] * vrBasis[0][component] +
                localAxis[1] * vrBasis[1][component] +
                localAxis[2] * vrBasis[2][component];
        }
        if (!NormalizeFlashlightVector8(headAxis))
            return false;

        output[projectionAxis * 4 + 0] = headAxis[0] * axisLength;
        output[projectionAxis * 4 + 1] = headAxis[1] * axisLength;
        output[projectionAxis * 4 + 2] = headAxis[2] * axisLength;
        output[projectionAxis * 4 + 3] = source[projectionAxis * 4 + 3];
    }

    if (InterlockedIncrement(
            &g_headTrackedFlashlightProjectionApplications8) == 1)
    {
        Log("Head tracking applied to the SH3 flashlight projection matrix "
            "in vertex constants c80-c82");
    }
    return true;
}

static void RefreshHeadTrackedFlashlightProjection8(IDirect3DDevice8* device)
{
    if (!device || !g_haveFlashlightProjectionSource8)
        return;

    float headProjection[12] = {};
    if (!ApplyHeadTrackedFlashlightProjection8(
            g_flashlightProjectionSource8, headProjection))
    {
        return;
    }

    if (FAILED(o_D3D8_SetVertexShaderConstant(device, 80,
            headProjection, 3)))
    {
        return;
    }

    const LONG refreshCount = InterlockedIncrement(
        &g_headTrackedFlashlightProjectionRefreshes8);
    if (refreshCount == 1)
    {
        Log("Head-tracked flashlight projection c80-c82 is refreshed with "
            "every VR view update");
    }
}

static HRESULT WINAPI hk_D3D8_SetVertexShaderConstant(IDirect3DDevice8* device,
    DWORD startRegister, const void* data, DWORD registerCount)
{
    LogMotionWeaponVertexConstants8(startRegister, data, registerCount);
    CaptureWeaponPoseConstants8(startRegister, data, registerCount);
    if (g_enableRuntimeDiagnostics8)
        CaptureD3D8ShaderConstants(startRegister, data, registerCount);

    constexpr DWORD flashlightProjectionRegister = 80;
    constexpr DWORD flashlightProjectionRegisterCount = 3;
    if (data && IsFlashlightProjectionShader8(g_currentVertexShader8) &&
        startRegister <= flashlightProjectionRegister &&
        startRegister + registerCount >= flashlightProjectionRegister +
            flashlightProjectionRegisterCount &&
        registerCount <= 96u)
    {
        float modifiedConstants[96u * 4u] = {};
        std::memcpy(modifiedConstants, data,
            static_cast<std::size_t>(registerCount) * 4u * sizeof(float));
        float* projection = modifiedConstants +
            static_cast<std::size_t>(flashlightProjectionRegister -
                startRegister) * 4u;
        std::memcpy(g_flashlightProjectionSource8, projection,
            sizeof(g_flashlightProjectionSource8));
        g_haveFlashlightProjectionSource8 = true;
        float projectionStrengthSquared = 0.0f;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                projectionStrengthSquared +=
                    projection[row * 4 + column] *
                    projection[row * 4 + column];
        g_flashlightProjectionStrength8 =
            std::isfinite(projectionStrengthSquared)
            ? std::sqrt(projectionStrengthSquared) : 0.0f;
        if (g_flashlightProjectionStrength8 > 0.001f)
        {
            g_flashlightProjectionSeenFrame8 =
                InterlockedCompareExchange(&c_present8, 0, 0);
        }
        float headProjection[12] = {};
        if (ApplyHeadTrackedFlashlightProjection8(projection,
                headProjection))
        {
            std::memcpy(projection, headProjection, sizeof(headProjection));
            return o_D3D8_SetVertexShaderConstant(device, startRegister,
                modifiedConstants, registerCount);
        }
    }

    if (g_enableViewProjectionHeadRotation8 && data &&
        startRegister == 2 && registerCount == 4 && g_haveProjection8)
    {
        float vrViewProjection[16] = {};
        if (ApplyHeadRotationToViewProjection(static_cast<const float*>(data),
            vrViewProjection))
        {
            LogVrCameraShaderApplication(g_currentVertexShader8);
            if (g_perDrawStereoProbeActive8 &&
                !g_fullSceneStereoReplayActive8)
            {
                std::memcpy(g_perDrawGameViewProjection8, data,
                    sizeof(g_perDrawGameViewProjection8));
                g_havePerDrawGameMatrix8 = true;
                std::memcpy(g_perDrawOriginalViewProjection8,
                    vrViewProjection,
                    sizeof(g_perDrawOriginalViewProjection8));
                const bool savedEyeOffset = g_applyStereoEyeOffset8;
                const std::uint32_t savedEye = g_renderEye8;
                g_applyStereoEyeOffset8 = true;
                g_renderEye8 = 0;
                const bool haveLeft = ApplyHeadRotationToViewProjection(
                    static_cast<const float*>(data),
                    g_perDrawCenterViewProjection8);
                g_renderEye8 = 1;
                const bool haveRight = ApplyHeadRotationToViewProjection(
                    static_cast<const float*>(data),
                    g_perDrawRightViewProjection8);
                g_havePerDrawStereoMatrices8 = haveLeft && haveRight;
                g_applyStereoEyeOffset8 = savedEyeOffset;
                g_renderEye8 = savedEye;
            }
            return o_D3D8_SetVertexShaderConstant(device, startRegister,
                vrViewProjection, registerCount);
        }
    }

    if (g_enableExperimentalShaderHeadRotation8 && data &&
        startRegister == 8 && registerCount == 3 && g_haveView8 &&
        std::memcmp(data, &g_lastView8.m[0][0], 12 * sizeof(float)) == 0)
    {
        float vrView[12] = {};
        if (ApplyHeadRotationToShaderCamera(static_cast<const float*>(data), vrView))
        {
            return o_D3D8_SetVertexShaderConstant(device, startRegister,
                vrView, registerCount);
        }
    }

    return o_D3D8_SetVertexShaderConstant(device, startRegister, data, registerCount);
}

static HRESULT WINAPI hk_D3D8_DrawPrimitive(IDirect3DDevice8* device,
    DWORD primitiveType, UINT startVertex, UINT primitiveCount)
{
    if (!g_loggedWorldDrawStack8 && g_currentVertexShader8 == 0x2D)
    {
        g_loggedWorldDrawStack8 = true;
        LogRenderCallStack("First shader 0x2D world draw");
    }
    CaptureD3D8ShaderDraw(false, primitiveCount);
    LogFirstStereoDrawForShader("DrawPrimitive", primitiveType, primitiveCount,
        false, true);
    const void* caller = _ReturnAddress();
    LogMotionWeaponDrawCapture8(device, "DrawPrimitive", primitiveType,
        primitiveCount, false, startVertex, 0, 0, 0, caller);
    const bool lightCompositeCandidate =
        g_enableUiStereoOverlay8 && g_perDrawStereoProbeActive8 &&
        !g_fullSceneStereoReplayActive8 &&
        IsLowResolutionLightCompositeDraw8(primitiveType, primitiveCount,
            caller);
    const HRESULT result = o_D3D8_DrawPrimitive(device, primitiveType,
        startVertex, primitiveCount);
    TraceScreenSpaceDraw("DrawPrimitive", false, primitiveType,
        primitiveCount, 0, caller);
    if (lightCompositeCandidate && !g_loggedLightCompositeExcluded8)
    {
        g_loggedLightCompositeExcluded8 = true;
        Log("Low-resolution light composite excluded from native eye "
            "overlay replay: shader/FVF 0x%08X, texture 256x256, caller "
            "0x%08X", g_currentVertexShader8,
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(caller)));
    }
    if (lightCompositeCandidate)
        g_holdPreviousNativeEyeFrame8 = true;
    // The amusement-park fog uses the same FVF and 512x512 texture dimensions
    // as the UI, but is submitted as a four-primitive triangle fan from a
    // vertex buffer. Replaying every fog billboard as UI makes it head-locked
    // and forces an eye-target transition for each draw. Keep UP and indexed
    // UI paths intact while excluding this exact false-positive signature.
    const bool likelyUiAtlasDraw = IsLikelyUiAtlasDraw8();
    const bool fogUiFalsePositive =
        g_currentVertexShader8 == 0x00000144u &&
        primitiveType == D3DPT_TRIANGLEFAN && primitiveCount == 4u &&
        reinterpret_cast<UINT_PTR>(caller) == 0x005F572Eu;
    if (fogUiFalsePositive && !g_loggedFogUiFalsePositive8)
    {
        g_loggedFogUiFalsePositive8 = true;
        Log("High-volume fog billboard excluded from stereo UI replay: "
            "FVF 0x00000144, triangle fan, 4 primitives, atlas 512x512, "
            "caller 0x%08X", static_cast<unsigned>(
                reinterpret_cast<UINT_PTR>(caller)));
    }
    if (fogUiFalsePositive && !g_loggedFogPrimitiveSample8)
        LogFogPrimitiveSample8(startVertex, primitiveCount, caller);
    if (SUCCEEDED(result) && fogUiFalsePositive)
        CaptureBatchedFogDraw8(startVertex, primitiveCount);
    if (SUCCEEDED(result) && g_enableUiStereoOverlay8 &&
        g_perDrawStereoProbeActive8 && !g_fullSceneStereoReplayActive8 &&
        !fogUiFalsePositive &&
        likelyUiAtlasDraw)
    {
        DuplicateUiPrimitiveForStereo(primitiveType, startVertex,
            primitiveCount);
    }
    DWORD renderTargetTextureStage = 0;
    UINT renderTargetTextureWidth = 0;
    UINT renderTargetTextureHeight = 0;
    if (SUCCEEDED(result) && g_perDrawStereoProbeActive8 &&
        !g_fullSceneStereoReplayActive8 &&
        ShouldReplayStereoDraw(g_currentVertexShader8, false, primitiveType,
            primitiveCount, true) &&
        IsPrimarySceneDrawSamplingRenderTargetTexture8(
            &renderTargetTextureStage, &renderTargetTextureWidth,
            &renderTargetTextureHeight))
    {
        g_forceWaterFullSceneStereo8 = true;
        if (!g_loggedWaterRttDetection8)
        {
            g_loggedWaterRttDetection8 = true;
            Log("Scene geometry sampled a render-target texture: stage %u, "
                "texture %ux%u, method DrawPrimitive, primitives %u; "
                "requesting per-eye full-scene replay",
                static_cast<unsigned>(renderTargetTextureStage),
                renderTargetTextureWidth, renderTargetTextureHeight,
                primitiveCount);
        }
    }
    if (SUCCEEDED(result) && g_perDrawStereoProbeActive8 &&
        (!g_heavyFullSceneStereo8 ||
            IsHeavySceneDynamicReplayDraw(g_currentVertexShader8, false,
                primitiveType, primitiveCount, true)) &&
        !g_fullSceneStereoReplayActive8 &&
        IsPerDrawStereoReplayBudgetAvailable() &&
        ShouldReplayStereoDraw(g_currentVertexShader8, false, primitiveType,
            primitiveCount, true))
    {
        DuplicatePrimitiveForStereoProbe(primitiveType, startVertex,
            primitiveCount);
    }
    return result;
}

static HRESULT WINAPI hk_D3D8_DrawIndexedPrimitive(IDirect3DDevice8* device,
    DWORD primitiveType, UINT minIndex, UINT vertexCount, UINT startIndex,
    UINT primitiveCount)
{
    if (!g_loggedWorldDrawStack8 && g_currentVertexShader8 == 0x2D)
    {
        g_loggedWorldDrawStack8 = true;
        LogRenderCallStack("First shader 0x2D indexed world draw");
    }
    CaptureD3D8ShaderDraw(true, primitiveCount);
    LogFirstStereoDrawForShader("DrawIndexedPrimitive", primitiveType,
        primitiveCount, true, true);
    const void* caller = _ReturnAddress();
    LogMotionWeaponDrawCapture8(device, "DrawIndexedPrimitive", primitiveType,
        primitiveCount, true, minIndex, vertexCount, startIndex, 0, caller);

    HMODULE gameModule = GetModuleHandleA(nullptr);
    const UINT_PTR callerAddress = reinterpret_cast<UINT_PTR>(caller);
    const UINT_PTR gameBase = reinterpret_cast<UINT_PTR>(gameModule);
    const UINT_PTR callerRva = gameModule && callerAddress >= gameBase
        ? callerAddress - gameBase : 0;
    // Shader IDs are allocated dynamically and change between levels. The
    // indexed mesh dimensions come from the PC weapon models themselves and,
    // together with the common model-render caller, distinguish every normal
    // Heather weapon without touching actor or room geometry.
    const bool weaponDrawShape = g_enableWeaponPosePrototype8 &&
        primitiveType == 5u &&
        callerRva == 0x0005F518u &&
        g_weaponPoseConstantsShader8 == g_currentVertexShader8;
    const int weaponProfileIndex = weaponDrawShape
        ? FindWeaponPoseProfile8(vertexCount, primitiveCount) : -1;
    if (weaponProfileIndex >= 0)
        ActivateWeaponPoseProfile8(weaponProfileIndex);
    const bool weaponCandidate = weaponProfileIndex >= 0;
    if (weaponCandidate)
    {
        InterlockedExchange(&g_activeWeaponPoseSeenPresent8,
            InterlockedCompareExchange(&c_present8, 0, 0));
    }
    struct WeaponPaletteUpdate
    {
        DWORD startRegister;
        float original[12];
    };
    WeaponPaletteUpdate weaponPaletteUpdates[SH3VR_WEAPON_PALETTE_BONE_COUNT8] = {};
    DWORD weaponPaletteUpdateCount = 0;
    bool weaponPoseApplied = false;
    if (weaponCandidate)
    {
        // Katana mesh 0 exposes only bone 0. Mesh 1 exposes the complete
        // two-bone palette shared by the blade and handle, so defer the frozen
        // baseline until that second draw. On following frames the completed
        // palette is applied to both draws.
        const bool incompleteKatanaPalette = weaponProfileIndex == 3 &&
            vertexCount == 69u && primitiveCount == 122u;
        if (!g_weaponPoseBaselinePaletteValid8 &&
            !incompleteKatanaPalette)
        {
            DWORD capturedBones = 0;
            for (DWORD bone = 0;
                bone < SH3VR_WEAPON_PALETTE_BONE_COUNT8; ++bone)
            {
                if (g_weaponPosePaletteMasks8[bone] != 0x07u ||
                    !IsWeaponAffineBone8(g_weaponPosePalette8[bone]))
                {
                    continue;
                }
                std::memcpy(g_weaponPoseBaselinePalette8[bone],
                    g_weaponPosePalette8[bone],
                    sizeof(g_weaponPoseBaselinePalette8[bone]));
                g_weaponPoseBaselinePaletteMasks8[bone] = 0x07u;
                if (capturedBones == 0)
                {
                    g_weaponPoseBaselinePivot8[0] =
                        g_weaponPosePalette8[bone][3];
                    g_weaponPoseBaselinePivot8[1] =
                        g_weaponPosePalette8[bone][7];
                    g_weaponPoseBaselinePivot8[2] =
                        g_weaponPosePalette8[bone][11];
                }
                ++capturedBones;
            }
            // Bone 1 is the stable equipment-side pivot in the SH3 weapon
            // models. Using it also prevents long weapons from orbiting around
            // the blade or barrel end.
            if (g_weaponPoseBaselinePaletteMasks8[1] == 0x07u)
            {
                g_weaponPoseBaselinePivot8[0] =
                    g_weaponPoseBaselinePalette8[1][3];
                g_weaponPoseBaselinePivot8[1] =
                    g_weaponPoseBaselinePalette8[1][7];
                g_weaponPoseBaselinePivot8[2] =
                    g_weaponPoseBaselinePalette8[1][11];
            }
            g_weaponPoseBaselinePaletteValid8 = capturedBones > 0;
            if (g_weaponPoseBaselinePaletteValid8)
            {
                const char* weaponName = g_activeWeaponPoseProfile8 >= 0
                    ? g_weaponPoseProfiles8[g_activeWeaponPoseProfile8].displayName
                    : "weapon";
                Log("MotionControls: frozen %s palette captured with %u affine bones",
                    weaponName, static_cast<unsigned>(capturedBones));
            }
        }
        if (g_weaponPoseBaselinePaletteValid8 &&
            !g_weaponPoseBaselineGripPointValid8)
        {
            CaptureWeaponGripPointFromGeometry8(device, minIndex, vertexCount);
        }

        float rotation[3][3] = {};
        float translation[3] = {};
        if (g_weaponPoseBaselinePaletteValid8 &&
            BuildWeaponPoseDelta8(rotation, translation))
        {
            if (g_weaponPoseAbsolutePosition8)
            {
                // BuildWeaponPoseDelta8 returns the desired controller-space
                // root position in absolute mode. Convert it to the uniform
                // delta consumed by the common-pivot transform.
                for (int axis = 0; axis < 3; ++axis)
                    translation[axis] -= g_weaponPoseBaselinePivot8[axis];
                if (g_weaponPoseBaselineGripPointValid8)
                {
                    const float gripFromPivot[3] = {
                        g_weaponPoseBaselineGripPoint8[0] -
                            g_weaponPoseBaselinePivot8[0],
                        g_weaponPoseBaselineGripPoint8[1] -
                            g_weaponPoseBaselinePivot8[1],
                        g_weaponPoseBaselineGripPoint8[2] -
                            g_weaponPoseBaselinePivot8[2]
                    };
                    for (int row = 0; row < 3; ++row)
                    {
                        translation[row] -=
                            rotation[row][0] * gripFromPivot[0] +
                            rotation[row][1] * gripFromPivot[1] +
                            rotation[row][2] * gripFromPivot[2];
                    }
                }
            }
            for (DWORD bone = 0; bone < SH3VR_WEAPON_PALETTE_BONE_COUNT8;
                ++bone)
            {
                if (g_weaponPosePaletteMasks8[bone] != 0x07u ||
                    g_weaponPoseBaselinePaletteMasks8[bone] != 0x07u)
                {
                    continue;
                }

                WeaponPaletteUpdate& update = weaponPaletteUpdates[
                    weaponPaletteUpdateCount];
                update.startRegister = SH3VR_WEAPON_PALETTE_START8 +
                    bone * 3u;
                std::memcpy(update.original, g_weaponPosePalette8[bone],
                    sizeof(update.original));
                float modified[12] = {};
                ApplyWeaponPoseDeltaToBone8(
                    g_weaponPoseBaselinePalette8[bone], rotation,
                    g_weaponPoseBaselinePivot8, translation, modified);
                if (bone == g_weaponPoseDebugReferenceBone8)
                {
                    // Read orientation from the exact affine matrix that is
                    // about to be submitted for the visible model. Remove
                    // uniform scale first, then decompose its actual basis.
                    float modelBasis[3][3] = {
                        { modified[0], modified[1], modified[2] },
                        { modified[4], modified[5], modified[6] },
                        { modified[8], modified[9], modified[10] }
                    };
                    bool validBasis = true;
                    for (int row = 0; row < 3; ++row)
                    {
                        const float length = std::sqrt(
                            modelBasis[row][0] * modelBasis[row][0] +
                            modelBasis[row][1] * modelBasis[row][1] +
                            modelBasis[row][2] * modelBasis[row][2]);
                        if (!std::isfinite(length) || length < 0.0001f)
                        {
                            validBasis = false;
                            break;
                        }
                        for (int column = 0; column < 3; ++column)
                            modelBasis[row][column] /= length;
                    }
                    if (validBasis)
                    {
                        constexpr float radiansToDegrees =
                            57.29577951308232f;
                        const float modelPitch = std::atan2(
                            modelBasis[2][1], modelBasis[2][2]) *
                            radiansToDegrees;
                        const float modelYaw = std::asin((std::max)(-1.0f,
                            (std::min)(1.0f, -modelBasis[2][0]))) *
                            radiansToDegrees;
                        const float modelRoll = std::atan2(
                            modelBasis[1][0], modelBasis[0][0]) *
                            radiansToDegrees;
                        Interop8_SetWeaponDebugOrientation(true,
                            g_activeWeaponPoseProfile8, modelPitch,
                            modelYaw, modelRoll);
                    }
                }
                if (FAILED(o_D3D8_SetVertexShaderConstant(device,
                        update.startRegister, modified, 3)))
                {
                    break;
                }
                ++weaponPaletteUpdateCount;
            }
            weaponPoseApplied = weaponPaletteUpdateCount > 0;
            if (!weaponPoseApplied)
            {
                const char* weaponName = g_activeWeaponPoseProfile8 >= 0
                    ? g_weaponPoseProfiles8[g_activeWeaponPoseProfile8].displayName
                    : "weapon";
                Log("MotionControls: %s draw matched, but no affine bone palette was available for a safe pose update",
                    weaponName);
            }
        }
    }
    PFN8_SetRenderState weaponSetRenderState = nullptr;
    DWORD originalWeaponClipping = TRUE;
    bool weaponClippingChanged = false;
    if (weaponPoseApplied && g_weaponPoseDisableClipping8)
    {
        void** deviceVtable = *reinterpret_cast<void***>(device);
        const auto weaponGetRenderState = reinterpret_cast<PFN8_GetRenderState>(
            deviceVtable[VT8_GetRenderState]);
        weaponSetRenderState = reinterpret_cast<PFN8_SetRenderState>(
            deviceVtable[VT8_SetRenderState]);
        constexpr DWORD kD3D8RenderStateClipping = 136u;
        if (weaponGetRenderState && weaponSetRenderState &&
            SUCCEEDED(weaponGetRenderState(device,
                kD3D8RenderStateClipping, &originalWeaponClipping)) &&
            SUCCEEDED(weaponSetRenderState(device,
                kD3D8RenderStateClipping, FALSE)))
        {
            weaponClippingChanged = true;
        }
    }
    const HRESULT result = o_D3D8_DrawIndexedPrimitive(device, primitiveType,
        minIndex, vertexCount, startIndex, primitiveCount);
    if (SUCCEEDED(result) && weaponPoseApplied &&
        !g_fullSceneStereoReplayActive8)
        CaptureLeftHandDesktopWeaponDepth8();
    TraceScreenSpaceDraw("DrawIndexedPrimitive", true, primitiveType,
        primitiveCount, 0, caller);
    DWORD renderTargetTextureStage = 0;
    UINT renderTargetTextureWidth = 0;
    UINT renderTargetTextureHeight = 0;
    if (SUCCEEDED(result) && g_perDrawStereoProbeActive8 &&
        !g_fullSceneStereoReplayActive8 &&
        ShouldReplayStereoDraw(g_currentVertexShader8, true, primitiveType,
            primitiveCount, true) &&
        IsPrimarySceneDrawSamplingRenderTargetTexture8(
            &renderTargetTextureStage, &renderTargetTextureWidth,
            &renderTargetTextureHeight))
    {
        g_forceWaterFullSceneStereo8 = true;
        if (!g_loggedWaterRttDetection8)
        {
            g_loggedWaterRttDetection8 = true;
            Log("Scene geometry sampled a render-target texture: stage %u, "
                "texture %ux%u, method DrawIndexedPrimitive, primitives "
                "%u; requesting per-eye full-scene replay",
                static_cast<unsigned>(renderTargetTextureStage),
                renderTargetTextureWidth, renderTargetTextureHeight,
                primitiveCount);
        }
    }
    if (SUCCEEDED(result) && g_perDrawStereoProbeActive8 &&
        (!g_heavyFullSceneStereo8 ||
            IsHeavySceneDynamicReplayDraw(g_currentVertexShader8, true,
                primitiveType, primitiveCount, true)) &&
        !g_fullSceneStereoReplayActive8 &&
        IsPerDrawStereoReplayBudgetAvailable() &&
        ShouldReplayStereoDraw(g_currentVertexShader8, true, primitiveType,
            primitiveCount, true))
    {
        DuplicateIndexedPrimitiveForStereoProbe(primitiveType, minIndex,
            vertexCount, startIndex, primitiveCount);
    }
    if (weaponPoseApplied)
    {
        for (DWORD index = 0; index < weaponPaletteUpdateCount; ++index)
        {
            o_D3D8_SetVertexShaderConstant(device,
                weaponPaletteUpdates[index].startRegister,
                weaponPaletteUpdates[index].original, 3);
        }
    }
    if (weaponClippingChanged && weaponSetRenderState)
    {
        constexpr DWORD kD3D8RenderStateClipping = 136u;
        weaponSetRenderState(device, kD3D8RenderStateClipping,
            originalWeaponClipping);
    }
    return result;
}

static HRESULT WINAPI hk_D3D8_DrawPrimitiveUP(IDirect3DDevice8* device,
    DWORD primitiveType, UINT primitiveCount, const void* vertices,
    UINT stride)
{
    CaptureD3D8ShaderDraw(false, primitiveCount);
    LogFirstStereoDrawForShader("DrawPrimitiveUP", primitiveType,
        primitiveCount, false, false);
    const void* caller = _ReturnAddress();
    LogMotionWeaponDrawCapture8(device, "DrawPrimitiveUP", primitiveType,
        primitiveCount, false, 0, 0, 0, stride, caller);
    const HRESULT result = o_D3D8_DrawPrimitiveUP(device, primitiveType,
        primitiveCount, vertices, stride);
    TraceScreenSpaceDraw("DrawPrimitiveUP", false, primitiveType,
        primitiveCount, stride, caller);
    TraceScreenSpaceState8(g_currentVertexShader8, primitiveType,
        primitiveCount, stride, vertices, caller);
    const bool gamePostProcessCandidate = IsGamePostProcessDraw8(
        primitiveType, primitiveCount, stride, caller);
    if (SUCCEEDED(result) && gamePostProcessCandidate &&
        g_perDrawStereoProbeActive8 && !g_fullSceneStereoReplayActive8)
    {
        DuplicateGamePostProcessPrimitiveUPForStereo(primitiveType,
            primitiveCount, vertices, stride);
    }
    const bool effectCompositeCandidate =
        IsScreenSpaceEffectCompositeDraw8(primitiveType, primitiveCount,
            stride, caller);
    if (effectCompositeCandidate && !g_loggedEffectCompositeExcluded8)
    {
        g_loggedEffectCompositeExcluded8 = true;
        Log("Screen-space effect composite excluded from native eye UI "
            "replay: FVF 0x%08X, caller 0x%08X", g_currentVertexShader8,
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(caller)));
    }
    if (effectCompositeCandidate)
        g_holdPreviousNativeEyeFrame8 = true;
    if (SUCCEEDED(result) && g_enableUiStereoOverlay8 &&
        g_perDrawStereoProbeActive8 &&
        !g_fullSceneStereoReplayActive8 &&
        !effectCompositeCandidate &&
        IsUiPretransformedFvf(g_currentVertexShader8) &&
        IsLikelyUiAtlasDraw8())
    {
        DuplicateUiPrimitiveUPForStereo(primitiveType, primitiveCount,
            vertices, stride);
    }
    if (SUCCEEDED(result) && g_perDrawStereoProbeActive8 &&
        !g_fullSceneStereoReplayActive8 &&
        g_currentVertexShader8 == 0x00000002u)
    {
        DuplicateFixedFunctionPrimitiveUPForStereo(primitiveType,
            primitiveCount, vertices, stride);
    }
    if (SUCCEEDED(result) && g_perDrawStereoProbeActive8 &&
        (!g_heavyFullSceneStereo8 ||
            IsHeavySceneDynamicReplayDraw(g_currentVertexShader8, false,
                primitiveType, primitiveCount, false)) &&
        !g_fullSceneStereoReplayActive8 &&
        IsPerDrawStereoReplayBudgetAvailable() &&
        ShouldReplayStereoDraw(g_currentVertexShader8, false, primitiveType,
            primitiveCount, false))
    {
        if (!g_loggedUpDrawStack8)
        {
            g_loggedUpDrawStack8 = true;
            Log("First selected DrawPrimitiveUP replay uses shader 0x%08X",
                g_currentVertexShader8);
            LogRenderCallStack("First selected DrawPrimitiveUP");
        }
        DuplicatePrimitiveUPForStereoProbe(primitiveType, primitiveCount,
            vertices, stride);
    }
    return result;
}

static HRESULT WINAPI hk_D3D8_DrawIndexedPrimitiveUP(IDirect3DDevice8* device,
    DWORD primitiveType, UINT minIndex, UINT vertexCount, UINT primitiveCount,
    const void* indices, DWORD indexFormat, const void* vertices, UINT stride)
{
    CaptureD3D8ShaderDraw(true, primitiveCount);
    LogFirstStereoDrawForShader("DrawIndexedPrimitiveUP", primitiveType,
        primitiveCount, true, false);
    const void* caller = _ReturnAddress();
    LogMotionWeaponDrawCapture8(device, "DrawIndexedPrimitiveUP", primitiveType,
        primitiveCount, true, minIndex, vertexCount, 0, stride, caller);
    const HRESULT result = o_D3D8_DrawIndexedPrimitiveUP(device, primitiveType,
        minIndex, vertexCount, primitiveCount, indices, indexFormat, vertices,
        stride);
    TraceScreenSpaceDraw("DrawIndexedPrimitiveUP", true, primitiveType,
        primitiveCount, stride, caller);
    if (SUCCEEDED(result) && g_enableUiStereoOverlay8 &&
        g_perDrawStereoProbeActive8 &&
        !g_fullSceneStereoReplayActive8 &&
        IsUiPretransformedFvf(g_currentVertexShader8) &&
        IsLikelyUiAtlasDraw8())
    {
        DuplicateUiIndexedPrimitiveUPForStereo(primitiveType, minIndex,
            vertexCount, primitiveCount, indices, indexFormat, vertices,
            stride);
    }
    if (SUCCEEDED(result) && g_perDrawStereoProbeActive8 &&
        (!g_heavyFullSceneStereo8 ||
            IsHeavySceneDynamicReplayDraw(g_currentVertexShader8, true,
                primitiveType, primitiveCount, false)) &&
        !g_fullSceneStereoReplayActive8 &&
        IsPerDrawStereoReplayBudgetAvailable() &&
        ShouldReplayStereoDraw(g_currentVertexShader8, true, primitiveType,
            primitiveCount, false))
    {
        if (!g_loggedUpDrawStack8)
        {
            g_loggedUpDrawStack8 = true;
            Log("First selected DrawIndexedPrimitiveUP replay uses shader "
                "0x%08X", g_currentVertexShader8);
            LogRenderCallStack("First selected DrawIndexedPrimitiveUP");
        }
        DuplicateIndexedPrimitiveUPForStereoProbe(primitiveType, minIndex,
            vertexCount, primitiveCount, indices, indexFormat, vertices,
            stride);
    }
    return result;
}

static float Dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool IsMainCameraView(const D3DMATRIX& matrix)
{
    const float affineTolerance = 0.001f;
    if (std::fabs(matrix.m[0][3]) > affineTolerance ||
        std::fabs(matrix.m[1][3]) > affineTolerance ||
        std::fabs(matrix.m[2][3]) > affineTolerance ||
        std::fabs(matrix.m[3][3] - 1.0f) > affineTolerance)
    {
        return false;
    }

    const float translationMagnitudeSquared =
        matrix.m[3][0] * matrix.m[3][0] +
        matrix.m[3][1] * matrix.m[3][1] +
        matrix.m[3][2] * matrix.m[3][2];
    if (translationMagnitudeSquared < 1.0f)
        return false;

    float rows[3][3] = {};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            rows[row][column] = matrix.m[row][column];

    const float orthonormalTolerance = 0.02f;
    for (int row = 0; row < 3; ++row)
    {
        if (std::fabs(Dot3(rows[row], rows[row]) - 1.0f) > orthonormalTolerance)
            return false;
    }

    if (std::fabs(Dot3(rows[0], rows[1])) > orthonormalTolerance ||
        std::fabs(Dot3(rows[0], rows[2])) > orthonormalTolerance ||
        std::fabs(Dot3(rows[1], rows[2])) > orthonormalTolerance)
    {
        return false;
    }

    return true;
}

static bool NormalizeQuaternion(float quaternion[4])
{
    const float lengthSquared =
        quaternion[0] * quaternion[0] +
        quaternion[1] * quaternion[1] +
        quaternion[2] * quaternion[2] +
        quaternion[3] * quaternion[3];
    if (lengthSquared < 0.000001f)
        return false;

    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    for (int i = 0; i < 4; ++i)
        quaternion[i] *= inverseLength;
    return true;
}

static void MultiplyQuaternions(const float a[4], const float b[4], float output[4])
{
    output[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    output[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    output[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    output[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static bool ExtractYawQuaternion(const float orientation[4], float output[4])
{
    const float x = orientation[0];
    const float y = orientation[1];
    const float z = orientation[2];
    const float w = orientation[3];
    const float yaw = std::atan2(
        2.0f * (w * y + x * z),
        1.0f - 2.0f * (y * y + z * z));
    const float halfYaw = yaw * 0.5f;
    output[0] = 0.0f;
    output[1] = std::sin(halfYaw);
    output[2] = 0.0f;
    output[3] = std::cos(halfYaw);
    return NormalizeQuaternion(output);
}

static void RotateVectorByQuaternion(const float quaternion[4],
    const float input[3], float output[3])
{
    const float q[3] = {
        quaternion[0], quaternion[1], quaternion[2]
    };
    const float firstCross[3] = {
        q[1] * input[2] - q[2] * input[1],
        q[2] * input[0] - q[0] * input[2],
        q[0] * input[1] - q[1] * input[0]
    };
    const float secondCross[3] = {
        q[1] * firstCross[2] - q[2] * firstCross[1],
        q[2] * firstCross[0] - q[0] * firstCross[2],
        q[0] * firstCross[1] - q[1] * firstCross[0]
    };

    for (int axis = 0; axis < 3; ++axis)
    {
        output[axis] = input[axis] +
            2.0f * quaternion[3] * firstCross[axis] +
            2.0f * secondCross[axis];
    }
}

static void BuildD3D8HeadViewRotation(const float quaternion[4], D3DMATRIX& output)
{
    const float x = quaternion[0];
    const float y = quaternion[1];
    const float z = quaternion[2];
    const float w = quaternion[3];

    std::memset(&output, 0, sizeof(output));
    output.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    output.m[0][1] = 2.0f * (x * y - z * w);
    output.m[0][2] = -2.0f * (x * z + y * w);
    output.m[1][0] = 2.0f * (x * y + z * w);
    output.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    output.m[1][2] = -2.0f * (y * z - x * w);
    output.m[2][0] = -2.0f * (x * z - y * w);
    output.m[2][1] = -2.0f * (y * z + x * w);
    output.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    output.m[3][3] = 1.0f;
}

static bool ReadControllerRelativePoseForWeapon8(int handIndex,
    float orientation[4], float position[3], bool useAimPose = false)
{
    if (handIndex < 0 || handIndex > 1 || !orientation || !position)
        return false;

    Sh3VrControllerState controller = {};
    Sh3VrHeadPose head = {};
    if (!Interop8_ReadControllerState(&controller) ||
        !Interop8_ReadHeadPose(&head) ||
        ((useAimPose ? controller.aimPose[handIndex].flags :
            controller.gripPose[handIndex].flags) &
            (SH3VR_POSE_POSITION_VALID |
            SH3VR_POSE_ORIENTATION_VALID)) !=
            (SH3VR_POSE_POSITION_VALID | SH3VR_POSE_ORIENTATION_VALID) ||
        (head.flags & (SH3VR_POSE_POSITION_VALID |
            SH3VR_POSE_ORIENTATION_VALID)) !=
            (SH3VR_POSE_POSITION_VALID | SH3VR_POSE_ORIENTATION_VALID))
    {
        return false;
    }

    float headOrientation[4] = {
        head.orientation[0], head.orientation[1], head.orientation[2],
        head.orientation[3]
    };
    const Sh3VrControllerPose& trackedPose = useAimPose
        ? controller.aimPose[handIndex] : controller.gripPose[handIndex];
    float handOrientation[4] = {
        trackedPose.orientation[0],
        trackedPose.orientation[1],
        trackedPose.orientation[2],
        trackedPose.orientation[3]
    };
    if (!NormalizeQuaternion(headOrientation) ||
        !NormalizeQuaternion(handOrientation))
    {
        return false;
    }

    const float inverseHead[4] = {
        -headOrientation[0], -headOrientation[1], -headOrientation[2],
        headOrientation[3]
    };
    float relativeOrientation[4] = {};
    MultiplyQuaternions(inverseHead, handOrientation, relativeOrientation);
    if (!NormalizeQuaternion(relativeOrientation))
        return false;

    const float handOffset[3] = {
        trackedPose.position[0] - head.position[0],
        trackedPose.position[1] - head.position[1],
        trackedPose.position[2] - head.position[2]
    };
    float relativePosition[3] = {};
    RotateVectorByQuaternion(inverseHead, handOffset, relativePosition);

    // OpenXR local space is +X right, +Y up, -Z forward. The D3D8 game
    // convention used by the VR camera is +X right, +Y down, +Z forward.
    orientation[0] = relativeOrientation[0];
    orientation[1] = -relativeOrientation[1];
    orientation[2] = -relativeOrientation[2];
    orientation[3] = relativeOrientation[3];
    position[0] = relativePosition[0] * g_worldScale8;
    position[1] = -relativePosition[1] * g_worldScale8;
    position[2] = -relativePosition[2] * g_worldScale8;
    return NormalizeQuaternion(orientation);
}

static bool ReadRightHandRelativePoseForWeapon8(float orientation[4],
    float position[3])
{
    return ReadControllerRelativePoseForWeapon8(1, orientation, position);
}

static bool ReadRightHandRelativeAimPose8(float orientation[4],
    float position[3])
{
    return ReadControllerRelativePoseForWeapon8(1, orientation, position,
        true);
}

static void BuildQuaternionRotation3x3(const float quaternion[4],
    float matrix[3][3])
{
    const float x = quaternion[0];
    const float y = quaternion[1];
    const float z = quaternion[2];
    const float w = quaternion[3];
    matrix[0][0] = 1.0f - 2.0f * (y * y + z * z);
    matrix[0][1] = 2.0f * (x * y - z * w);
    matrix[0][2] = 2.0f * (x * z + y * w);
    matrix[1][0] = 2.0f * (x * y + z * w);
    matrix[1][1] = 1.0f - 2.0f * (x * x + z * z);
    matrix[1][2] = 2.0f * (y * z - x * w);
    matrix[2][0] = 2.0f * (x * z - y * w);
    matrix[2][1] = 2.0f * (y * z + x * w);
    matrix[2][2] = 1.0f - 2.0f * (x * x + y * y);
}

static void ApplyWeaponPoseDeltaToBone8(const float source[12],
    const float rotation[3][3], const float pivot[3],
    const float translation[3], float output[12])
{
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            output[row * 4 + column] =
                rotation[row][0] * source[column] +
                rotation[row][1] * source[4 + column] +
                rotation[row][2] * source[8 + column];
        }
        const float relativeOrigin[3] = {
            source[3] - pivot[0],
            source[7] - pivot[1],
            source[11] - pivot[2]
        };
        output[row * 4 + 3] = pivot[row] +
            rotation[row][0] * relativeOrigin[0] +
            rotation[row][1] * relativeOrigin[1] +
            rotation[row][2] * relativeOrigin[2] + translation[row];
    }
}

static bool IsWeaponAffineBone8(const float matrix[12])
{
    if (!matrix)
        return false;

    for (int row = 0; row < 3; ++row)
    {
        const float x = matrix[row * 4 + 0];
        const float y = matrix[row * 4 + 1];
        const float z = matrix[row * 4 + 2];
        const float lengthSquared = x * x + y * y + z * z;
        if (!std::isfinite(lengthSquared) ||
            std::fabs(lengthSquared - 1.0f) > 0.08f ||
            !std::isfinite(matrix[row * 4 + 3]))
        {
            return false;
        }
    }
    return true;
}

static bool BuildWeaponPoseDelta8(float rotation[3][3], float translation[3])
{
    PollWeaponIniHotReload8();
    if (!g_enableWeaponPosePrototype8 || !rotation || !translation)
        return false;

    float orientation[4] = {};
    float position[3] = {};
    if (!ReadRightHandRelativePoseForWeapon8(orientation, position))
        return false;

    if (g_weaponPoseAbsolutePosition8)
    {
        if (!g_haveFlashlightViewPair8)
            return false;

        D3DMATRIX inverseVrView = {};
        if (!InvertD3D8Matrix(g_flashlightVrView8, inverseVrView))
            return false;

        const bool rowVectorView = IsMainCameraView(g_flashlightVrView8);
        float localPosition[3] = {
            position[0], position[1], position[2]
        };
        // Weapon INI position is controller-local. Rotating the offset by the
        // tracked grip orientation keeps manual calibration attached to the
        // physical handle instead of the camera axes.
        const float weaponWorldCompensation = g_worldScale8 /
            SH3VR_DEFAULT_WORLD_SCALE;
        const float compensatedWeaponOffset[3] = {
            g_weaponPoseOffset8[0] * weaponWorldCompensation,
            g_weaponPoseOffset8[1] * weaponWorldCompensation,
            g_weaponPoseOffset8[2] * weaponWorldCompensation
        };
        float cameraGripOffset[3] = {};
        RotateVectorByQuaternion(orientation, compensatedWeaponOffset,
            cameraGripOffset);
        for (int axis = 0; axis < 3; ++axis)
            localPosition[axis] += cameraGripOffset[axis];
        // Keep the grip just beyond the VR near plane. The blade is still
        // free to approach the headset, but the entire weapon no longer gets
        // clipped away when the controller touches the face.
        localPosition[2] = (std::max)(localPosition[2],
            g_weaponPoseMinimumForward8);
        for (int component = 0; component < 3; ++component)
        {
            translation[component] = rowVectorView
                ? localPosition[0] * inverseVrView.m[0][component] +
                    localPosition[1] * inverseVrView.m[1][component] +
                    localPosition[2] * inverseVrView.m[2][component] +
                    inverseVrView.m[3][component]
                : inverseVrView.m[component][0] * localPosition[0] +
                    inverseVrView.m[component][1] * localPosition[1] +
                    inverseVrView.m[component][2] * localPosition[2] +
                    inverseVrView.m[component][3];
        }

        float localHandRotation[3][3] = {};
        BuildQuaternionRotation3x3(orientation, localHandRotation);
        float handWorldRotation[3][3] = {};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                for (int k = 0; k < 3; ++k)
                {
                    handWorldRotation[row][column] += rowVectorView
                        ? inverseVrView.m[k][row] *
                            localHandRotation[k][column]
                        : inverseVrView.m[row][k] *
                            localHandRotation[k][column];
                }
            }
        }

        std::memset(rotation, 0, 9u * sizeof(float));
        if (g_enableWeaponPoseRotation8)
        {
            float calibratedHandWorld[3][3] = {};
            // Rotate the captured long knife axis onto the controller's
            // forward axis. This is a per-weapon grip calibration, not part of
            // the tracked controller orientation.
            const float gripYawCos = std::cos(g_weaponPoseGripYawRadians8);
            const float gripYawSin = std::sin(g_weaponPoseGripYawRadians8);
            const float gripPitchCos = std::cos(
                g_weaponPoseGripPitchRadians8);
            const float gripPitchSin = std::sin(
                g_weaponPoseGripPitchRadians8);
            const float gripRollCos = std::cos(
                g_weaponPoseGripRollRadians8);
            const float gripRollSin = std::sin(
                g_weaponPoseGripRollRadians8);
            // Apply roll in controller-local space after mapping the model's
            // long axis onto the controller. This aligns the visual handle
            // with the physical Quest controller handle.
            const float rollYaw[3][3] = {
                { gripRollCos * gripYawCos, -gripRollSin,
                    gripRollCos * gripYawSin },
                { gripRollSin * gripYawCos, gripRollCos,
                    gripRollSin * gripYawSin },
                { -gripYawSin, 0.0f, gripYawCos }
            };
            const float gripCalibration[3][3] = {
                { rollYaw[0][0],
                    rollYaw[0][1] * gripPitchCos +
                        rollYaw[0][2] * gripPitchSin,
                    -rollYaw[0][1] * gripPitchSin +
                        rollYaw[0][2] * gripPitchCos },
                { rollYaw[1][0],
                    rollYaw[1][1] * gripPitchCos +
                        rollYaw[1][2] * gripPitchSin,
                    -rollYaw[1][1] * gripPitchSin +
                        rollYaw[1][2] * gripPitchCos },
                { rollYaw[2][0],
                    rollYaw[2][1] * gripPitchCos +
                        rollYaw[2][2] * gripPitchSin,
                    -rollYaw[2][1] * gripPitchSin +
                        rollYaw[2][2] * gripPitchCos }
            };
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        calibratedHandWorld[row][column] +=
                            handWorldRotation[row][k] *
                            gripCalibration[k][column];
                    }
                }
            }
            // Convert the desired absolute controller orientation into the
            // delta consumed by ApplyWeaponPoseDeltaToBone8. Using the
            // controller orientation from the first rendered frame as the
            // reference made identical INI values produce a different pose
            // on every launch. The frozen weapon pivot bone is deterministic
            // and also preserves relationships between multi-part weapons.
            DWORD referenceBone =
                g_weaponPoseBaselinePaletteMasks8[1] == 0x07u ? 1u : 0u;
            if (g_weaponPoseBaselinePaletteMasks8[referenceBone] != 0x07u)
            {
                for (DWORD bone = 0;
                    bone < SH3VR_WEAPON_PALETTE_BONE_COUNT8; ++bone)
                {
                    if (g_weaponPoseBaselinePaletteMasks8[bone] == 0x07u)
                    {
                        referenceBone = bone;
                        break;
                    }
                }
            }
            g_weaponPoseDebugReferenceBone8 = referenceBone;
            const float* baselineRotation =
                g_weaponPoseBaselinePalette8[referenceBone];
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        rotation[row][column] +=
                            calibratedHandWorld[row][k] *
                            baselineRotation[column * 4 + k];
                    }
                }
            }
        }
        else
        {
            rotation[0][0] = 1.0f;
            rotation[1][1] = 1.0f;
            rotation[2][2] = 1.0f;
        }
        // WorldScale changes the stereo scale of the scene. Counter-scale the
        // game-unit weapon mesh so its physical size remains identical to the
        // calibrated size in sh3vr_weapons.ini while only the world changes.
        const float compensatedWeaponScale = g_weaponPoseScale8 *
            weaponWorldCompensation;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                rotation[row][column] *= compensatedWeaponScale;

        if (InterlockedIncrement(&g_weaponPoseApplications8) == 1)
        {
            Log("MotionControls: controller pose is being applied in game-world space to the frozen melee palette");
        }
        return true;
    }

    if (!g_weaponPoseBaselineValid8)
    {
        std::memcpy(g_weaponPoseBaselineOrientation8, orientation,
            sizeof(g_weaponPoseBaselineOrientation8));
        std::memcpy(g_weaponPoseBaselinePosition8, position,
            sizeof(g_weaponPoseBaselinePosition8));
        g_weaponPoseBaselineValid8 = true;
        Log("MotionControls: melee weapon pose baseline captured; move and rotate the right controller to test the visual attachment");
        return false;
    }

    const float inverseBaseline[4] = {
        -g_weaponPoseBaselineOrientation8[0],
        -g_weaponPoseBaselineOrientation8[1],
        -g_weaponPoseBaselineOrientation8[2],
        g_weaponPoseBaselineOrientation8[3]
    };
    float rotationQuaternion[4] = {};
    MultiplyQuaternions(orientation, inverseBaseline, rotationQuaternion);
    if (!NormalizeQuaternion(rotationQuaternion))
        return false;

    std::memset(rotation, 0, 9u * sizeof(float));
    if (g_enableWeaponPoseRotation8)
    {
        BuildQuaternionRotation3x3(rotationQuaternion, rotation);
    }
    else
    {
        rotation[0][0] = 1.0f;
        rotation[1][1] = 1.0f;
        rotation[2][2] = 1.0f;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        translation[axis] = position[axis] -
            g_weaponPoseBaselinePosition8[axis] + g_weaponPoseOffset8[axis];
    }

    if (InterlockedIncrement(&g_weaponPoseApplications8) == 1)
    {
        Log("MotionControls: controller pose is being applied to every affine bone in the active weapon palette");
    }
    return true;
}

bool D3D9Hook_GetMeleeWeaponHitbox(std::uint8_t gameWeapon,
    float gripWorld[4], float tipWorld[4], float* radiusGameUnits)
{
    if (!gripWorld || !tipWorld || !radiusGameUnits)
        return false;

    int profileIndex = -1;
    float reachMeters = 0.0f;
    float radiusMeters = 0.0f;
    switch (gameWeapon)
    {
    case 4u: // Knife
        profileIndex = 0;
        reachMeters = 0.38f;
        radiusMeters = 0.10f;
        break;
    case 5u: // Steel Pipe
        profileIndex = 1;
        reachMeters = 0.82f;
        radiusMeters = 0.14f;
        break;
    case 6u: // Katana
        profileIndex = 3;
        reachMeters = 0.95f;
        radiusMeters = 0.12f;
        break;
    case 7u: // Maul
        profileIndex = 2;
        reachMeters = 0.76f;
        radiusMeters = 0.22f;
        break;
    default:
        return false;
    }

    PollWeaponIniHotReload8();
    float orientation[4] = {};
    float localPosition[3] = {};
    if (!ReadRightHandRelativePoseForWeapon8(orientation, localPosition))
    {
        return false;
    }

    // Input is polled before the next frame establishes its view matrix.
    // g_flashlightVrView8 intentionally retains the previous completed
    // frame, which is the stable game-world transform needed by combat.
    D3DMATRIX inverseVrView = {};
    if (!InvertD3D8Matrix(g_flashlightVrView8, inverseVrView))
        return false;
    const bool rowVectorView = IsMainCameraView(g_flashlightVrView8);

    // Use the same controller-local calibration as the visible model, but
    // build the combat capsule independently of captured draw calls or mesh
    // visibility. Weapon switching and render culling therefore cannot turn
    // melee collision off.
    const WeaponPoseProfile8& profile = g_weaponPoseProfiles8[profileIndex];
    const float weaponWorldCompensation = g_worldScale8 /
        SH3VR_DEFAULT_WORLD_SCALE;
    const float calibratedOffset[3] = {
        profile.position[0] * weaponWorldCompensation,
        profile.position[1] * weaponWorldCompensation,
        profile.position[2] * weaponWorldCompensation
    };
    float rotatedOffset[3] = {};
    RotateVectorByQuaternion(orientation, calibratedOffset, rotatedOffset);
    for (int axis = 0; axis < 3; ++axis)
        localPosition[axis] += rotatedOffset[axis];

    const float controllerForward[3] = { 0.0f, 0.0f, 1.0f };
    float localForward[3] = {};
    RotateVectorByQuaternion(orientation, controllerForward, localForward);
    float worldForward[3] = {};
    for (int component = 0; component < 3; ++component)
    {
        gripWorld[component] = rowVectorView
            ? localPosition[0] * inverseVrView.m[0][component] +
                localPosition[1] * inverseVrView.m[1][component] +
                localPosition[2] * inverseVrView.m[2][component] +
                inverseVrView.m[3][component]
            : inverseVrView.m[component][0] * localPosition[0] +
                inverseVrView.m[component][1] * localPosition[1] +
                inverseVrView.m[component][2] * localPosition[2] +
                inverseVrView.m[component][3];
        worldForward[component] = rowVectorView
            ? localForward[0] * inverseVrView.m[0][component] +
                localForward[1] * inverseVrView.m[1][component] +
                localForward[2] * inverseVrView.m[2][component]
            : inverseVrView.m[component][0] * localForward[0] +
                inverseVrView.m[component][1] * localForward[1] +
                inverseVrView.m[component][2] * localForward[2];
    }
    const float forwardLength = std::sqrt(
        worldForward[0] * worldForward[0] +
        worldForward[1] * worldForward[1] +
        worldForward[2] * worldForward[2]);
    if (!std::isfinite(forwardLength) || forwardLength < 0.001f)
        return false;
    for (int axis = 0; axis < 3; ++axis)
        worldForward[axis] /= forwardLength;

    gripWorld[3] = 1.0f;
    const float reachGameUnits = reachMeters * g_worldScale8;
    for (int axis = 0; axis < 3; ++axis)
        tipWorld[axis] = gripWorld[axis] +
            worldForward[axis] * reachGameUnits;
    tipWorld[3] = 1.0f;
    *radiusGameUnits = radiusMeters * g_worldScale8;
    return true;
}

static int FirearmProfileIndex8(std::uint8_t gameWeapon)
{
    switch (gameWeapon)
    {
    case 0u: return 5; // Handgun
    case 1u: return 6; // Shotgun
    case 2u: return 7; // Submachine Gun
    case 3u: return 4; // Stun Gun
    default: return -1;
    }
}

bool D3D9Hook_GetFirearmAimRay(std::uint8_t gameWeapon,
    float muzzleWorld[4], float rayEndWorld[4])
{
    if (!muzzleWorld || !rayEndWorld)
        return false;

    const int profileIndex = FirearmProfileIndex8(gameWeapon);
    if (profileIndex < 0)
        return false;

    PollWeaponIniHotReload8();
    float orientation[4] = {};
    float localPosition[3] = {};
    if (!ReadRightHandRelativePoseForWeapon8(orientation, localPosition) ||
        !g_haveFlashlightViewPair8)
    {
        return false;
    }

    D3DMATRIX inverseVrView = {};
    if (!InvertD3D8Matrix(g_flashlightVrView8, inverseVrView))
        return false;
    const bool rowVectorView = IsMainCameraView(g_flashlightVrView8);

    const WeaponPoseProfile8& profile = g_weaponPoseProfiles8[profileIndex];
    const float weaponWorldCompensation = g_worldScale8 /
        SH3VR_DEFAULT_WORLD_SCALE;
    const float calibratedOffset[3] = {
        profile.position[0] * weaponWorldCompensation,
        profile.position[1] * weaponWorldCompensation,
        profile.position[2] * weaponWorldCompensation
    };
    float rotatedOffset[3] = {};
    RotateVectorByQuaternion(orientation, calibratedOffset, rotatedOffset);
    for (int axis = 0; axis < 3; ++axis)
        localPosition[axis] += rotatedOffset[axis];

    // Grip pose owns the rendered handle and its calibrated position. OpenXR
    // aim pose owns the pointing ray; on Quest controllers these axes are not
    // interchangeable.
    float aimOrientation[4] = {};
    float unusedAimPosition[3] = {};
    if (!ReadRightHandRelativeAimPose8(aimOrientation,
        unusedAimPosition))
    {
        return false;
    }
    // Quest's OpenXR aim pose is intentionally independent from the rendered
    // grip pose, but its factory pointing axis sits above SH3's visible gun
    // barrels. Apply a controller-local calibration before rotating the ray.
    // In the game's camera convention +Y points down, so a negative pitch
    // lowers the ray and reticle. The same corrected ray feeds native damage
    // and the stereo dot, keeping the two results exactly aligned.
    const float pitch = profile.aimPitchRadians;
    const float yaw = profile.aimYawRadians;
    const float controllerForward[3] = {
        std::sin(yaw) * std::cos(pitch),
        -std::sin(pitch),
        std::cos(yaw) * std::cos(pitch)
    };
    float localForward[3] = {};
    RotateVectorByQuaternion(aimOrientation, controllerForward,
        localForward);
    const float localForwardLength = std::sqrt(
        localForward[0] * localForward[0] +
        localForward[1] * localForward[1] +
        localForward[2] * localForward[2]);
    if (!std::isfinite(localForwardLength) || localForwardLength < 0.001f)
        return false;
    for (int axis = 0; axis < 3; ++axis)
        localForward[axis] /= localForwardLength;

    // Start the native ray at the visible barrel rather than at Heather's
    // old animation socket. Distances are intentionally independent of mesh
    // scale and only follow the game's metres-to-world-units conversion.
    const float muzzleForwardMeters = gameWeapon == 3u ? 0.18f :
        (gameWeapon == 0u ? 0.26f : (gameWeapon == 1u ? 0.78f : 0.48f));
    for (int axis = 0; axis < 3; ++axis)
    {
        localPosition[axis] += localForward[axis] *
            muzzleForwardMeters * g_worldScale8;
    }

    float worldForward[3] = {};
    for (int component = 0; component < 3; ++component)
    {
        muzzleWorld[component] = rowVectorView
            ? localPosition[0] * inverseVrView.m[0][component] +
                localPosition[1] * inverseVrView.m[1][component] +
                localPosition[2] * inverseVrView.m[2][component] +
                inverseVrView.m[3][component]
            : inverseVrView.m[component][0] * localPosition[0] +
                inverseVrView.m[component][1] * localPosition[1] +
                inverseVrView.m[component][2] * localPosition[2] +
                inverseVrView.m[component][3];
        worldForward[component] = rowVectorView
            ? localForward[0] * inverseVrView.m[0][component] +
                localForward[1] * inverseVrView.m[1][component] +
                localForward[2] * inverseVrView.m[2][component]
            : inverseVrView.m[component][0] * localForward[0] +
                inverseVrView.m[component][1] * localForward[1] +
                inverseVrView.m[component][2] * localForward[2];
    }
    const float worldForwardLength = std::sqrt(
        worldForward[0] * worldForward[0] +
        worldForward[1] * worldForward[1] +
        worldForward[2] * worldForward[2]);
    if (!std::isfinite(worldForwardLength) || worldForwardLength < 0.001f)
        return false;
    for (int axis = 0; axis < 3; ++axis)
        worldForward[axis] /= worldForwardLength;

    const float rangeMeters = gameWeapon == 3u ? 2.5f : 60.0f;
    muzzleWorld[3] = 1.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        rayEndWorld[axis] = muzzleWorld[axis] + worldForward[axis] *
            rangeMeters * g_worldScale8;
    }
    rayEndWorld[3] = 1.0f;
    return true;
}

bool D3D9Hook_GetActiveFirearmAimRay(float muzzleWorld[4],
    float rayEndWorld[4])
{
    // The visual signature is already the authoritative weapon identity for
    // motion attachment. Unlike SH3's transient gameplay byte, it remains
    // stable across animation phases and weapon changes.
    const int profileIndex = g_activeWeaponPoseProfile8;
    std::uint8_t canonicalWeapon = 0xFFu;
    switch (profileIndex)
    {
    case 4: canonicalWeapon = 3u; break; // Stun Gun
    case 5: canonicalWeapon = 0u; break; // Handgun
    case 6: canonicalWeapon = 1u; break; // Shotgun
    case 7: canonicalWeapon = 2u; break; // Submachine Gun
    default: return false;
    }
    return D3D9Hook_GetFirearmAimRay(canonicalWeapon, muzzleWorld,
        rayEndWorld);
}

static bool IsWritableMemoryRange(void* address, SIZE_T size)
{
    if (!address || size == 0)
        return false;

    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(address, &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    {
        return false;
    }

    const DWORD protection = memory.Protect & 0xFFu;
    const bool writable =
        protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
    if (!writable)
        return false;

    const auto rangeStart = reinterpret_cast<std::uintptr_t>(address);
    const auto rangeEnd = rangeStart + size;
    const auto regionStart =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto regionEnd = regionStart + memory.RegionSize;
    return rangeEnd >= rangeStart && rangeEnd <= regionEnd;
}

static bool GetSupportedCameraModState(BYTE** moduleBaseOutput,
    BYTE** pluginStateOutput)
{
    HMODULE cameraMod = GetModuleHandleA("OTSMod.dll");
    if (!cameraMod)
        return false;

    auto* moduleBase = reinterpret_cast<BYTE*>(cameraMod);
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        moduleBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->FileHeader.TimeDateStamp !=
            SH3VR_CAMERA_MOD_SUPPORTED_TIMESTAMP ||
        ntHeaders->OptionalHeader.SizeOfImage <
            SH3VR_CAMERA_MOD_GAME_MODULE_POINTER_RVA + sizeof(void*))
    {
        if (InterlockedCompareExchange(&g_cameraModLayoutLogState8, -1, 0) == 0)
        {
            Log("Camera Mod integration disabled: unsupported OTSMod.dll "
                "layout");
        }
        return false;
    }

    auto** pluginStatePointer = reinterpret_cast<BYTE**>(
        moduleBase + SH3VR_CAMERA_MOD_STATE_POINTER_RVA);
    BYTE* pluginState = *pluginStatePointer;
    if (!pluginState)
        return false;

    if (moduleBaseOutput)
        *moduleBaseOutput = moduleBase;
    if (pluginStateOutput)
        *pluginStateOutput = pluginState;

    if (InterlockedCompareExchange(&g_cameraModLayoutLogState8, 1, 0) == 0)
    {
        Log("Camera Mod memory integration active for the supported "
            "OTSMod.dll build");
    }
    return true;
}

static bool ApplyCameraModYawDelta(float degrees, float* updatedYaw)
{
    BYTE* pluginState = nullptr;
    if (!GetSupportedCameraModState(nullptr, &pluginState))
        return false;

    float* cameraYaw = reinterpret_cast<float*>(
        pluginState + SH3VR_CAMERA_MOD_YAW_OFFSET);
    if (!IsWritableMemoryRange(cameraYaw, sizeof(*cameraYaw)) ||
        !std::isfinite(*cameraYaw) || std::fabs(*cameraYaw) > 100000.0f)
    {
        return false;
    }

    float yaw = *cameraYaw + degrees;
    while (yaw > 180.0f)
        yaw -= 360.0f;
    while (yaw < -180.0f)
        yaw += 360.0f;
    *cameraYaw = yaw;
    if (updatedYaw)
        *updatedYaw = yaw;
    return true;
}

static bool ReadCameraModYaw(float* yawOutput)
{
    if (!yawOutput)
        return false;

    BYTE* pluginState = nullptr;
    if (!GetSupportedCameraModState(nullptr, &pluginState) ||
        pluginState[SH3VR_CAMERA_MOD_ENABLED_OFFSET] == 0)
    {
        return false;
    }

    float* cameraYaw = reinterpret_cast<float*>(
        pluginState + SH3VR_CAMERA_MOD_YAW_OFFSET);
    if (!IsWritableMemoryRange(cameraYaw, sizeof(*cameraYaw)) ||
        !std::isfinite(*cameraYaw) || std::fabs(*cameraYaw) > 100000.0f)
    {
        return false;
    }

    *yawOutput = *cameraYaw;
    return true;
}

static void TryAutoLoadCameraModFirstPerson()
{
    if (!g_autoLoadCameraModFirstPerson8 || g_cameraModAutoLoadDone8)
        return;

    const ULONGLONG now = GetTickCount64();
    if (g_cameraModStartupTick8 == 0)
        g_cameraModStartupTick8 = now;

    // Present runs before the per-frame flag is cleared below, so these
    // values describe the frame that has just completed.  Wait for a few
    // consecutive gameplay frames; this is the transition out of the intro
    // cutscene and remains safe even when the user skips it immediately.
    if (g_viewProjectionAppliedThisFrame8 || g_previousImmersiveFrame8)
        g_cameraModImmersiveFrameStreak8 =
            (std::min)(g_cameraModImmersiveFrameStreak8 + 1u, 8u);
    else
        g_cameraModImmersiveFrameStreak8 = 0;
    const bool gameplayConfirmed = g_cameraModImmersiveFrameStreak8 >= 3u;

    const ULONGLONG delayMilliseconds =
        static_cast<ULONGLONG>(g_cameraModAutoLoadDelaySeconds8) * 1000u;
    // The configured delay is only a fallback for builds/scenes where the
    // projection hook cannot observe gameplay.  Once gameplay is confirmed,
    // never wait out the old three-minute startup delay.
    if (!gameplayConfirmed && delayMilliseconds != 0 &&
        now - g_cameraModStartupTick8 < delayMilliseconds)
    {
        if (!g_cameraModAutoLoadDelayLogged8)
        {
            g_cameraModAutoLoadDelayLogged8 = true;
            Log("Camera Mod First Person auto-load delayed for %u seconds "
                "to keep the startup/new-game cutscene in third person",
                static_cast<unsigned>(g_cameraModAutoLoadDelaySeconds8));
        }
        return;
    }

    Sh3VrHeadPose pose = {};
    if (!Interop8_ReadHeadPose(&pose) ||
        (pose.flags & SH3VR_POSE_ORIENTATION_VALID) == 0)
    {
        return;
    }

    BYTE* moduleBase = nullptr;
    BYTE* pluginState = nullptr;
    if (!GetSupportedCameraModState(&moduleBase, &pluginState))
        return;

    BYTE* settingsStart = pluginState + SH3VR_CAMERA_MOD_ENABLED_OFFSET;
    const SIZE_T settingsSize =
        SH3VR_CAMERA_MOD_HIDE_PLAYER_OFFSET -
        SH3VR_CAMERA_MOD_ENABLED_OFFSET + sizeof(BYTE);
    if (!IsWritableMemoryRange(settingsStart, settingsSize))
        return;

    auto** gameModulePointer = reinterpret_cast<BYTE**>(
        moduleBase + SH3VR_CAMERA_MOD_GAME_MODULE_POINTER_RVA);
    if (*gameModulePointer == nullptr)
        return;

    using CameraModAction = void(__cdecl*)();
    const auto loadFirstPersonPreset = reinterpret_cast<CameraModAction>(
        moduleBase + SH3VR_CAMERA_MOD_LOAD_FPS_PRESET_RVA);
    const auto toggleCameraMod = reinterpret_cast<CameraModAction>(
        moduleBase + SH3VR_CAMERA_MOD_TOGGLE_RVA);

    loadFirstPersonPreset();
    if (pluginState[SH3VR_CAMERA_MOD_ENABLED_OFFSET] == 0)
        toggleCameraMod();

    if (pluginState[SH3VR_CAMERA_MOD_ENABLED_OFFSET] != 0 &&
        pluginState[SH3VR_CAMERA_MOD_HIDE_PLAYER_OFFSET] != 0)
    {
        g_cameraModAutoLoadDone8 = true;
        Log("Camera Mod First Person preset auto-loaded after %u confirmed "
            "immersive gameplay frames and Camera Mod enabled",
            g_cameraModImmersiveFrameStreak8);
    }
}

static void ResetRoomscaleMovement(const Sh3VrHeadPose* pose)
{
    InterlockedExchange(&g_roomscaleMovementMask8, SH3VR_ROOMSCALE_NONE);
    g_roomscaleLastPoseTime8 = 0;
    g_roomscaleMovementPulse8 = 0.0f;
    g_roomscaleSmoothedVelocity8[0] = 0.0f;
    g_roomscaleSmoothedVelocity8[1] = 0.0f;
    if (pose && g_headOrientationReferenceValid8)
    {
        g_headPositionReference8[0] = pose->position[0];
        g_headPositionReference8[2] = pose->position[2];
    }
}

static void UpdateRoomscaleMovement(bool immersive)
{
    Sh3VrHeadPose pose = {};
    const bool havePose = Interop8_ReadHeadPose(&pose) &&
        (pose.flags & SH3VR_POSE_POSITION_VALID) != 0 &&
        (pose.flags & SH3VR_POSE_ORIENTATION_VALID) != 0;
    if (!g_enableRoomscale8 || !immersive ||
        !g_headOrientationReferenceValid8 || !havePose)
    {
        ResetRoomscaleMovement(havePose ? &pose : nullptr);
        return;
    }

    float cameraModYawDegrees = 0.0f;
    if (!ReadCameraModYaw(&cameraModYawDegrees))
    {
        ResetRoomscaleMovement(&pose);
        return;
    }

    if (pose.predictedDisplayTime == g_roomscaleLastPoseTime8)
        return;

    if (g_roomscaleLastPoseTime8 == 0)
    {
        g_roomscaleLastPoseTime8 = pose.predictedDisplayTime;
        InterlockedExchange(&g_roomscaleMovementMask8,
            SH3VR_ROOMSCALE_NONE);
        return;
    }

    const double elapsedSeconds = static_cast<double>(
        pose.predictedDisplayTime - g_roomscaleLastPoseTime8) * 1.0e-9;
    g_roomscaleLastPoseTime8 = pose.predictedDisplayTime;
    if (!(elapsedSeconds > 0.0 && elapsedSeconds < 0.25))
    {
        ResetRoomscaleMovement(&pose);
        return;
    }

    const float stageOffsetX =
        pose.position[0] - g_headPositionReference8[0];
    const float stageOffsetZ =
        pose.position[2] - g_headPositionReference8[2];
    const float stageDistance = std::sqrt(
        stageOffsetX * stageOffsetX + stageOffsetZ * stageOffsetZ);
    if (!std::isfinite(stageDistance) ||
        stageDistance > SH3VR_ROOMSCALE_TRACKING_JUMP_METERS)
    {
        Log("Roomscale tracking discontinuity ignored; horizontal origin "
            "was recentered");
        ResetRoomscaleMovement(&pose);
        return;
    }

    float consumedStageX = 0.0f;
    float consumedStageZ = 0.0f;
    if (stageDistance > SH3VR_ROOMSCALE_FOLLOW_RADIUS_METERS)
    {
        const float consumedFraction =
            (stageDistance - SH3VR_ROOMSCALE_FOLLOW_RADIUS_METERS) /
            stageDistance;
        consumedStageX = stageOffsetX * consumedFraction;
        consumedStageZ = stageOffsetZ * consumedFraction;
        g_headPositionReference8[0] += consumedStageX;
        g_headPositionReference8[2] += consumedStageZ;
    }

    const float inverseReference[4] = {
        -g_headOrientationReference8[0],
        -g_headOrientationReference8[1],
        -g_headOrientationReference8[2],
         g_headOrientationReference8[3]
    };
    const float consumedStage[3] = {
        consumedStageX, 0.0f, consumedStageZ
    };
    float consumedReferenceLocal[3] = {};
    RotateVectorByQuaternion(inverseReference, consumedStage,
        consumedReferenceLocal);

    const float rawRightVelocity = consumedReferenceLocal[0] /
        static_cast<float>(elapsedSeconds);
    const float rawForwardVelocity = -consumedReferenceLocal[2] /
        static_cast<float>(elapsedSeconds);
    const float smoothing = std::clamp(
        static_cast<float>(elapsedSeconds) * 14.0f, 0.0f, 1.0f);
    g_roomscaleSmoothedVelocity8[0] += smoothing *
        (rawRightVelocity - g_roomscaleSmoothedVelocity8[0]);
    g_roomscaleSmoothedVelocity8[1] += smoothing *
        (rawForwardVelocity - g_roomscaleSmoothedVelocity8[1]);

    float current[4] = {
        pose.orientation[0], pose.orientation[1],
        pose.orientation[2], pose.orientation[3]
    };
    if (!NormalizeQuaternion(current))
    {
        InterlockedExchange(&g_roomscaleMovementMask8,
            SH3VR_ROOMSCALE_NONE);
        return;
    }
    float relativeOrientation[4] = {};
    MultiplyQuaternions(inverseReference, current, relativeOrientation);
    if (!NormalizeQuaternion(relativeOrientation))
    {
        InterlockedExchange(&g_roomscaleMovementMask8,
            SH3VR_ROOMSCALE_NONE);
        return;
    }

    const float x = relativeOrientation[0];
    const float y = relativeOrientation[1];
    const float z = relativeOrientation[2];
    const float w = relativeOrientation[3];
    const float openXrYaw = std::atan2(
        2.0f * (w * y + x * z),
        1.0f - 2.0f * (y * y + z * z));
    constexpr float degreesToRadians = 0.01745329251994329577f;
    const float virtualRightYaw =
        cameraModYawDegrees * degreesToRadians - openXrYaw;
    const float yawCos = std::cos(virtualRightYaw);
    const float yawSin = std::sin(virtualRightYaw);
    // OpenXR stage translation and Camera Mod movement use opposite signs on
    // both horizontal axes. Negate the final camera-relative vector so a real
    // step and Heather's movement always point in the same direction.
    const float cameraForwardVelocity = -(
        g_roomscaleSmoothedVelocity8[1] * yawCos +
        g_roomscaleSmoothedVelocity8[0] * yawSin);
    const float cameraRightVelocity = -(
        g_roomscaleSmoothedVelocity8[0] * yawCos -
        g_roomscaleSmoothedVelocity8[1] * yawSin);
    const float speed = std::sqrt(
        cameraForwardVelocity * cameraForwardVelocity +
        cameraRightVelocity * cameraRightVelocity);

    std::uint32_t movementMask = SH3VR_ROOMSCALE_NONE;
    if (std::isfinite(speed) &&
        speed >= SH3VR_ROOMSCALE_START_SPEED_METERS_PER_SECOND)
    {
        constexpr float directionThreshold = 0.32f;
        const float componentThreshold = speed * directionThreshold;
        if (cameraForwardVelocity > componentThreshold)
            movementMask |= SH3VR_ROOMSCALE_FORWARD;
        else if (cameraForwardVelocity < -componentThreshold)
            movementMask |= SH3VR_ROOMSCALE_BACKWARD;
        if (cameraRightVelocity > componentThreshold)
            movementMask |= SH3VR_ROOMSCALE_RIGHT;
        else if (cameraRightVelocity < -componentThreshold)
            movementMask |= SH3VR_ROOMSCALE_LEFT;
    }
    // Camera Mod exposes digital movement keys, while OpenXR reports analog
    // physical velocity. Pulse the digital keys proportionally instead of
    // holding them continuously for every movement above the dead zone. This
    // keeps slow physical steps from producing a full-speed in-game stride.
    float movementDuty = 0.0f;
    if (movementMask != SH3VR_ROOMSCALE_NONE)
    {
        movementDuty = std::clamp(
            speed * g_roomscaleHeightScale8 /
                g_roomscaleFullKeySpeedMetersPerSecond8,
            0.0f, 1.0f);
        g_roomscaleMovementPulse8 += movementDuty;
        if (g_roomscaleMovementPulse8 >= 1.0f)
            g_roomscaleMovementPulse8 -= 1.0f;
        else
            movementMask = SH3VR_ROOMSCALE_NONE;
    }
    else
    {
        g_roomscaleMovementPulse8 = 0.0f;
    }

    InterlockedExchange(&g_roomscaleMovementMask8,
        static_cast<LONG>(movementMask));

    if (movementMask != SH3VR_ROOMSCALE_NONE)
    {
        const LONG count = InterlockedIncrement(&g_roomscaleMovementLogCount8);
        if (count <= 8 || count % 900 == 0)
        {
            Log("Roomscale movement %d: mask 0x%X, speed cm/s %d, duty "
                "x1000 %d, height scale x1000 %d", count, movementMask,
                static_cast<int>(std::lround(speed * 100.0f)),
                static_cast<int>(std::lround(movementDuty * 1000.0f)),
                static_cast<int>(std::lround(
                    g_roomscaleHeightScale8 * 1000.0f)));
        }
    }
}

static void UpdateCameraModSnapTurn()
{
    if (!g_enableCameraModSnapTurn8 ||
        GetModuleHandleA("OTSMod.dll") == nullptr)
    {
        InterlockedExchange(&g_cameraModCharacterAlignEndTick8, 0);
        return;
    }

    Sh3VrControllerState controller = {};
    if (!Interop8_ReadControllerState(&controller) || controller.active == 0 ||
        controller.predictedDisplayTime == g_cameraModLastControllerTime8)
    {
        return;
    }
    g_cameraModLastControllerTime8 = controller.predictedDisplayTime;

    const float axis = controller.thumbstick[1][0];
    const float magnitude = axis < 0.0f ? -axis : axis;
    constexpr float releaseThreshold = 0.35f;
    if (magnitude <= releaseThreshold)
    {
        g_cameraModSnapTurnLatched8 = false;
        return;
    }
    if (g_cameraModSnapTurnLatched8 ||
        magnitude < g_cameraModSnapTurnActivation8)
    {
        return;
    }

    g_cameraModSnapTurnLatched8 = true;
    // Camera Mod stores horizontal yaw in degrees. Updating its own state keeps
    // character-relative movement, the game camera and both stereo eyes in the
    // same coordinate system. Its positive yaw follows positive stick X.
    const float yawDelta = axis < 0.0f
        ? -g_cameraModSnapTurnDegrees8
        : g_cameraModSnapTurnDegrees8;
    float cameraModYaw = 0.0f;
    if (!ApplyCameraModYawDelta(yawDelta, &cameraModYaw))
        return;

    // Camera Mod rotates only its detached camera. A short forward pulse uses
    // the mod's proven 2D movement path so the game can align Heather without
    // writing to an unverified player-orientation address.
    InterlockedExchange(&g_cameraModCharacterAlignEndTick8,
        static_cast<LONG>(GetTickCount() +
            g_cameraModCharacterAlignMilliseconds8));

    const LONG count = InterlockedIncrement(&g_cameraModSnapTurnCount8);
    if (count <= 16 || count % 100 == 0)
    {
        Log("Camera Mod snap turn %d: %s, Camera Mod yaw x10 %d, "
            "Heather alignment pulse %u ms",
            count, axis < 0.0f ? "left" : "right",
            static_cast<int>(std::lround(cameraModYaw * 10.0f)),
            g_cameraModCharacterAlignMilliseconds8);
    }
}

static bool ReadRelativeHeadPose(float orientation[4], float position[3])
{
    Sh3VrHeadPose pose = {};
    if (g_headOrientationReferenceValid8 && g_haveLatchedFrameHeadPose8)
    {
        pose = g_latchedFrameHeadPose8;
    }
    else if (!Interop8_ReadHeadPose(&pose))
    {
        return false;
    }
    if (
        (pose.flags & SH3VR_POSE_ORIENTATION_VALID) == 0 ||
        (pose.flags & SH3VR_POSE_POSITION_VALID) == 0)
    {
        return false;
    }

    float current[4] = {
        pose.orientation[0], pose.orientation[1],
        pose.orientation[2], pose.orientation[3]
    };
    if (!NormalizeQuaternion(current))
        return false;

    if (!g_headOrientationReferenceValid8)
    {
        const LONG presentFrame = InterlockedCompareExchange(&c_present8, 0, 0);
        if (g_headPoseCalibrationStartFrame8 < 0)
        {
            g_headPoseCalibrationStartFrame8 = presentFrame;
            Log("6-DOF yaw calibration started; headset pitch and roll will "
                "be leveled automatically");
            return false;
        }
        if (presentFrame - g_headPoseCalibrationStartFrame8 <
            SH3VR_HEAD_POSE_CALIBRATION_FRAMES)
        {
            return false;
        }

        // Recenter only yaw. OpenXR provides gravity-aligned pitch and roll,
        // so retaining them makes the initial horizon level without requiring
        // the player to hold the headset straight during startup.
        if (!ExtractYawQuaternion(current, g_headOrientationReference8))
            return false;
        std::memcpy(g_headPositionReference8, pose.position,
            sizeof(g_headPositionReference8));
        g_headOrientationReferenceValid8 = true;
        if (g_enableRoomscale8 && pose.position[1] >= 1.0f &&
            pose.position[1] <= 2.3f)
        {
            constexpr float eyeToTopMeters = 0.10f;
            const float targetEyeHeight = (std::max)(1.0f,
                g_roomscalePlayerHeightMeters8 - eyeToTopMeters);
            g_roomscaleHeightScale8 = std::clamp(
                targetEyeHeight / pose.position[1], 0.70f, 1.40f);
            g_roomscaleHeightLogged8 = true;
            Log("Roomscale height calibrated: measured HMD height %d cm, "
                "target player height %d cm, tracking scale x1000 %d",
                static_cast<int>(std::lround(pose.position[1] * 100.0f)),
                static_cast<int>(std::lround(
                    g_roomscalePlayerHeightMeters8 * 100.0f)),
                static_cast<int>(std::lround(
                    g_roomscaleHeightScale8 * 1000.0f)));
        }
        else if (g_enableRoomscale8 && !g_roomscaleHeightLogged8)
        {
            g_roomscaleHeightLogged8 = true;
            g_roomscaleHeightScale8 = 1.0f;
            Log("Roomscale height normalization unavailable because the "
                "OpenXR reference space did not provide a plausible floor "
                "height; tracking scale remains 1.0");
        }
        Log("6-DOF yaw reference captured after the calibration delay; "
            "pitch and roll use the OpenXR gravity-aligned horizon");
    }

    if (!g_haveLatchedFrameHeadPose8)
    {
        g_latchedFrameHeadPose8 = pose;
        g_haveLatchedFrameHeadPose8 = true;
    }

    const float inverseReference[4] = {
        -g_headOrientationReference8[0],
        -g_headOrientationReference8[1],
        -g_headOrientationReference8[2],
         g_headOrientationReference8[3]
    };
    MultiplyQuaternions(inverseReference, current, orientation);
    if (!NormalizeQuaternion(orientation))
        return false;

    // Silent Hill 3's camera space uses the opposite pitch and roll directions
    // from OpenXR. Keep yaw unchanged.
    orientation[0] = -orientation[0];
    orientation[2] = -orientation[2];

    UpdateCameraModSnapTurn();

    const float openXrDelta[3] = {
        pose.position[0] - g_headPositionReference8[0],
        pose.position[1] - g_headPositionReference8[1],
        pose.position[2] - g_headPositionReference8[2]
    };
    float headLocalDelta[3] = {};
    RotateVectorByQuaternion(inverseReference, openXrDelta, headLocalDelta);

    // D3D8 camera space is +X right, +Y down and +Z forward. OpenXR is
    // +X right, +Y up and -Z forward. Position is reported in app-space axes,
    // so first rotate it into the reference headset's local axes. Then convert
    // meters to SH3's centimeter-like world units.
    const float trackingScale = g_enableRoomscale8
        ? g_roomscaleHeightScale8 : 1.0f;
    position[0] = headLocalDelta[0] * g_worldScale8 *
        trackingScale;
    position[1] = -headLocalDelta[1] * g_worldScale8 *
        trackingScale;
    position[2] = -headLocalDelta[2] * g_worldScale8 *
        trackingScale;

    // Render parallel stereo cameras on alternating game frames. Left is
    // negative camera X and right is positive camera X.
    if (g_enableAlternatingStereo8 || g_applyStereoEyeOffset8)
    {
        const float eyeSign = g_renderEye8 == 0 ? -1.0f : 1.0f;
        position[0] += eyeSign * 0.5f * SH3VR_IPD_METERS *
            g_worldScale8;
    }
    Interop8_SetFrameRenderPose(pose);
    return true;
}

static void AddHeadViewTranslation(const float position[3],
    D3DMATRIX& columnView, D3DMATRIX* rowView)
{
    for (int row = 0; row < 3; ++row)
    {
        columnView.m[row][3] = -(
            columnView.m[row][0] * position[0] +
            columnView.m[row][1] * position[1] +
            columnView.m[row][2] * position[2]);
    }

    if (rowView)
    {
        rowView->m[3][0] = columnView.m[0][3];
        rowView->m[3][1] = columnView.m[1][3];
        rowView->m[3][2] = columnView.m[2][3];
    }
}

static bool ApplyHeadRotationToShaderCamera(const float gameView[12],
    float output[12])
{
    float relativeOrientation[4] = {};
    float relativePosition[3] = {};
    if (!ReadRelativeHeadPose(relativeOrientation, relativePosition))
        return false;

    D3DMATRIX headPoseRotation = {};
    BuildD3D8HeadViewRotation(relativeOrientation, headPoseRotation);

    D3DMATRIX headViewColumn = {};
    headViewColumn.m[3][3] = 1.0f;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            headViewColumn.m[row][column] = headPoseRotation.m[column][row];
    AddHeadViewTranslation(relativePosition, headViewColumn, nullptr);

    // The shader constants are a column-vector 3x4 view matrix. The matrix
    // built above is the equivalent row-vector pose rotation, so transpose its
    // 3x3 part to obtain the column-vector inverse view rotation.
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            output[row * 4 + column] = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                output[row * 4 + column] +=
                    headPoseRotation.m[k][row] * gameView[k * 4 + column];
            }
            if (column == 3)
                output[row * 4 + column] += headViewColumn.m[row][3];
        }
    }

    if (InterlockedIncrement(&g_shaderHeadRotationApplications8) == 1)
        Log("Head rotation applied to the main D3D8 shader camera constants");
    return true;
}

static bool IsColumnVectorMainView(const D3DMATRIX& matrix)
{
    const float affineTolerance = 0.001f;
    if (std::fabs(matrix.m[3][0]) > affineTolerance ||
        std::fabs(matrix.m[3][1]) > affineTolerance ||
        std::fabs(matrix.m[3][2]) > affineTolerance ||
        std::fabs(matrix.m[3][3] - 1.0f) > affineTolerance)
    {
        return false;
    }

    const float translationMagnitudeSquared =
        matrix.m[0][3] * matrix.m[0][3] +
        matrix.m[1][3] * matrix.m[1][3] +
        matrix.m[2][3] * matrix.m[2][3];
    if (translationMagnitudeSquared < 1.0f)
        return false;

    float rows[3][3] = {};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            rows[row][column] = matrix.m[row][column];

    const float orthonormalTolerance = 0.02f;
    for (int row = 0; row < 3; ++row)
    {
        if (std::fabs(Dot3(rows[row], rows[row]) - 1.0f) > orthonormalTolerance)
            return false;
    }

    if (std::fabs(Dot3(rows[0], rows[1])) > orthonormalTolerance ||
        std::fabs(Dot3(rows[0], rows[2])) > orthonormalTolerance ||
        std::fabs(Dot3(rows[1], rows[2])) > orthonormalTolerance)
    {
        return false;
    }

    return true;
}

static bool ApplyHeadRotationToColumnView(const D3DMATRIX& gameView,
    D3DMATRIX& output)
{
    if (!IsColumnVectorMainView(gameView))
        return false;

    float relativeOrientation[4] = {};
    float relativePosition[3] = {};
    if (!ReadRelativeHeadPose(relativeOrientation, relativePosition))
        return false;

    D3DMATRIX headPoseRotation = {};
    BuildD3D8HeadViewRotation(relativeOrientation, headPoseRotation);
    D3DMATRIX headViewColumn = {};
    headViewColumn.m[3][3] = 1.0f;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            headViewColumn.m[row][column] = headPoseRotation.m[column][row];
    AddHeadViewTranslation(relativePosition, headViewColumn, nullptr);
    output = gameView;

    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            output.m[row][column] = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                output.m[row][column] +=
                    headPoseRotation.m[k][row] * gameView.m[k][column];
            }
            if (column == 3)
                output.m[row][column] += headViewColumn.m[row][3];
        }
    }

    if (InterlockedIncrement(&g_columnViewHeadRotationApplications8) == 1)
        Log("Head rotation applied to the main D3D8 column-vector view");
    return true;
}

static void MultiplyD3D8Matrices(const D3DMATRIX& a, const D3DMATRIX& b,
    D3DMATRIX& output)
{
    D3DMATRIX result = {};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            for (int k = 0; k < 4; ++k)
                result.m[row][column] += a.m[row][k] * b.m[k][column];
        }
    }
    output = result;
}

static bool InvertD3D8Matrix(const D3DMATRIX& input, D3DMATRIX& output)
{
    float augmented[4][8] = {};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
            augmented[row][column] = input.m[row][column];
        augmented[row][row + 4] = 1.0f;
    }

    for (int column = 0; column < 4; ++column)
    {
        int pivotRow = column;
        for (int row = column + 1; row < 4; ++row)
        {
            if (std::fabs(augmented[row][column]) >
                std::fabs(augmented[pivotRow][column]))
            {
                pivotRow = row;
            }
        }

        if (std::fabs(augmented[pivotRow][column]) < 0.000001f)
            return false;

        if (pivotRow != column)
        {
            for (int element = 0; element < 8; ++element)
            {
                const float temporary = augmented[column][element];
                augmented[column][element] = augmented[pivotRow][element];
                augmented[pivotRow][element] = temporary;
            }
        }

        const float inversePivot = 1.0f / augmented[column][column];
        for (int element = 0; element < 8; ++element)
            augmented[column][element] *= inversePivot;

        for (int row = 0; row < 4; ++row)
        {
            if (row == column)
                continue;
            const float factor = augmented[row][column];
            for (int element = 0; element < 8; ++element)
                augmented[row][element] -= factor * augmented[column][element];
        }
    }

    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            output.m[row][column] = augmented[row][column + 4];
    return true;
}

static bool IsColumnVectorCameraView(const D3DMATRIX& matrix)
{
    const float affineTolerance = 0.05f;
    if (std::fabs(matrix.m[3][0]) > affineTolerance ||
        std::fabs(matrix.m[3][1]) > affineTolerance ||
        std::fabs(matrix.m[3][2]) > affineTolerance ||
        std::fabs(matrix.m[3][3] - 1.0f) > affineTolerance)
    {
        return false;
    }

    const float translationMagnitudeSquared =
        matrix.m[0][3] * matrix.m[0][3] +
        matrix.m[1][3] * matrix.m[1][3] +
        matrix.m[2][3] * matrix.m[2][3];
    if (translationMagnitudeSquared < 1.0f)
        return false;

    float rows[3][3] = {};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            rows[row][column] = matrix.m[row][column];

    const float orthonormalTolerance = 0.05f;
    for (int row = 0; row < 3; ++row)
    {
        if (std::fabs(Dot3(rows[row], rows[row]) - 1.0f) > orthonormalTolerance)
            return false;
    }
    if (std::fabs(Dot3(rows[0], rows[1])) > orthonormalTolerance ||
        std::fabs(Dot3(rows[0], rows[2])) > orthonormalTolerance ||
        std::fabs(Dot3(rows[1], rows[2])) > orthonormalTolerance)
    {
        return false;
    }
    return true;
}

static bool ApplyHeadRotationToViewProjection(const float gameViewProjection[16],
    float output[16])
{
    D3DMATRIX projectionColumn = {};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            projectionColumn.m[row][column] = g_lastProjection8.m[column][row];

    D3DMATRIX inverseProjection = {};
    if (!InvertD3D8Matrix(projectionColumn, inverseProjection))
        return false;

    D3DMATRIX viewProjection = {};
    std::memcpy(&viewProjection.m[0][0], gameViewProjection,
        16 * sizeof(float));

    D3DMATRIX cameraView = {};
    MultiplyD3D8Matrices(inverseProjection, viewProjection, cameraView);
    if (!IsColumnVectorCameraView(cameraView))
        return false;

    float relativeOrientation[4] = {};
    float relativePosition[3] = {};
    if (!ReadRelativeHeadPose(relativeOrientation, relativePosition))
        return false;

    D3DMATRIX headPoseRotation = {};
    BuildD3D8HeadViewRotation(relativeOrientation, headPoseRotation);

    D3DMATRIX headViewColumn = {};
    headViewColumn.m[3][3] = 1.0f;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            headViewColumn.m[row][column] = headPoseRotation.m[column][row];
    AddHeadViewTranslation(relativePosition, headViewColumn, nullptr);

    D3DMATRIX rotatedView = {};
    D3DMATRIX rotatedViewProjection = {};
    MultiplyD3D8Matrices(headViewColumn, cameraView, rotatedView);

    D3DMATRIX presentationProjection = projectionColumn;
    if (g_enableImmersiveWideFov8 &&
        std::fabs(projectionColumn.m[0][0]) > 0.0001f &&
        std::fabs(projectionColumn.m[1][1]) > 0.0001f)
    {
        float aspect = std::fabs(projectionColumn.m[1][1] /
            projectionColumn.m[0][0]);
        if (g_applyStereoEyeOffset8 && g_stereoTargetWidth8 != 0 &&
            g_stereoTargetHeight8 != 0)
        {
            aspect = static_cast<float>(g_stereoTargetWidth8) /
                static_cast<float>(g_stereoTargetHeight8);
        }
        presentationProjection.m[0][0] = std::copysign(
            SH3VR_IMMERSIVE_VERTICAL_SCALE / aspect,
            projectionColumn.m[0][0]);
        presentationProjection.m[1][1] = std::copysign(
            SH3VR_IMMERSIVE_VERTICAL_SCALE,
            projectionColumn.m[1][1]);
    }

    MultiplyD3D8Matrices(presentationProjection, rotatedView,
        rotatedViewProjection);
    std::memcpy(output, &rotatedViewProjection.m[0][0], 16 * sizeof(float));

    if (InterlockedIncrement(&g_viewProjectionHeadRotationApplications8) == 1)
        Log("6-DOF head pose applied to the main D3D8 view-projection constants "
            "at %.0f game units per meter", g_worldScale8);
    g_viewProjectionAppliedThisFrame8 = true;
    return true;
}

static bool ApplyHeadRotationToMainCamera(const D3DMATRIX& gameView,
    D3DMATRIX& output)
{
    if (!IsMainCameraView(gameView))
        return false;

    float relativeOrientation[4] = {};
    float relativePosition[3] = {};
    if (!ReadRelativeHeadPose(relativeOrientation, relativePosition))
        return false;

    D3DMATRIX headViewRotation = {};
    BuildD3D8HeadViewRotation(relativeOrientation, headViewRotation);
    D3DMATRIX headViewColumn = {};
    headViewColumn.m[3][3] = 1.0f;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            headViewColumn.m[row][column] = headViewRotation.m[column][row];
    AddHeadViewTranslation(relativePosition, headViewColumn, &headViewRotation);
    MultiplyD3D8Matrices(gameView, headViewRotation, output);

    if (InterlockedIncrement(&g_headRotationApplications8) == 1)
        Log("6-DOF head pose applied to the main D3D8 camera view");
    return true;
}

static bool BuildImmersiveProjection(const D3DMATRIX& gameProjection,
    D3DMATRIX& output)
{
    if (!g_enableImmersiveWideFov8 || !g_headOrientationReferenceValid8 ||
        std::fabs(gameProjection.m[0][0]) < 0.0001f ||
        std::fabs(gameProjection.m[1][1]) < 0.0001f ||
        std::fabs(gameProjection.m[2][3] - 1.0f) > 0.01f ||
        std::fabs(gameProjection.m[3][3]) > 0.01f)
    {
        return false;
    }

    output = gameProjection;
    const float aspect = std::fabs(gameProjection.m[1][1] /
        gameProjection.m[0][0]);
    output.m[0][0] = std::copysign(SH3VR_IMMERSIVE_VERTICAL_SCALE / aspect,
        gameProjection.m[0][0]);
    output.m[1][1] = std::copysign(SH3VR_IMMERSIVE_VERTICAL_SCALE,
        gameProjection.m[1][1]);
    return true;
}

struct D3D8SurfaceDescLocal
{
    DWORD Format;
    DWORD Type;
    DWORD Usage;
    DWORD Pool;
    UINT Size;
    DWORD MultiSampleType;
    UINT Width;
    UINT Height;
};

template <typename T>
static T D3D8Slot(void* object, int index)
{
    void** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<T>(vtable[index]);
}

static void ReleaseBatchedFogPipeline8(BatchedFogPipeline8& pipeline)
{
    if (pipeline.texture0)
    {
        D3D8Slot<PFNT8_Release>(pipeline.texture0, 2)(pipeline.texture0);
        pipeline.texture0 = nullptr;
    }
    pipeline = {};
}

static bool CaptureBatchedFogPipeline8(BatchedFogPipeline8& pipeline,
    DWORD vertexShader, bool requireTexture)
{
    static const DWORD renderStateTypes[] = {
        7u, 14u, 15u, 16u, 19u, 20u, 22u, 23u, 24u, 25u, 26u,
        27u, 28u, 29u, 34u, 35u, 36u, 37u, 38u, 48u, 168u
    };

    ReleaseBatchedFogPipeline8(pipeline);
    if (!g_device8)
        return false;

    pipeline.vertexShader = vertexShader;
    const HRESULT textureResult = D3D8Slot<PFN8_GetTexture>(g_device8,
        VT8_GetTexture)(g_device8, 0, &pipeline.texture0);
    if (FAILED(textureResult) || (requireTexture && !pipeline.texture0))
    {
        ReleaseBatchedFogPipeline8(pipeline);
        return false;
    }

    for (UINT index = 0; index < _countof(renderStateTypes); ++index)
    {
        BatchedFogRenderStateValue8& state = pipeline.renderStates[index];
        state.type = renderStateTypes[index];
        state.valid = SUCCEEDED(D3D8Slot<PFN8_GetRenderState>(g_device8,
            VT8_GetRenderState)(g_device8, state.type, &state.value));
    }
    for (UINT type = 1; type <= _countof(pipeline.textureStageStates);
        ++type)
    {
        BatchedFogRenderStateValue8& state =
            pipeline.textureStageStates[type - 1];
        state.type = type;
        state.valid = SUCCEEDED(D3D8Slot<PFN8_GetTextureStageState>(
            g_device8, VT8_GetTextureStageState)(g_device8, 0, state.type,
                &state.value));
    }
    pipeline.valid = true;
    return true;
}

static HRESULT ApplyBatchedFogPipeline8(const BatchedFogPipeline8& pipeline)
{
    if (!g_device8 || !pipeline.valid)
        return E_FAIL;

    HRESULT result = o_D3D8_SetVertexShader(g_device8,
        pipeline.vertexShader);
    if (FAILED(result))
        return result;
    result = o_D3D8_SetTexture(g_device8, 0, pipeline.texture0);
    if (FAILED(result))
        return result;
    for (const BatchedFogRenderStateValue8& state : pipeline.renderStates)
    {
        if (!state.valid)
            continue;
        result = D3D8Slot<PFN8_SetRenderState>(g_device8,
            VT8_SetRenderState)(g_device8, state.type, state.value);
        if (FAILED(result))
            return result;
    }
    for (const BatchedFogRenderStateValue8& state :
        pipeline.textureStageStates)
    {
        if (!state.valid)
            continue;
        result = D3D8Slot<PFN8_SetTextureStageState>(g_device8,
            VT8_SetTextureStageState)(g_device8, 0, state.type, state.value);
        if (FAILED(result))
            return result;
    }
    return S_OK;
}

static void ResetBatchedFog8()
{
    ReleaseBatchedFogPipeline8(g_batchedFogPipeline8);
    if (g_batchedFogVertexBuffer8)
    {
        D3D8Slot<PFNV8_Release>(g_batchedFogVertexBuffer8, 2)(
            g_batchedFogVertexBuffer8);
        g_batchedFogVertexBuffer8 = nullptr;
    }
    g_batchedFogDraws8.clear();
    g_batchedFogVertexStride8 = 0;
    g_batchedFogMinVertex8 = 0xFFFFFFFFu;
    g_batchedFogMaxVertexEnd8 = 0;
    g_batchedFogSourceViewport8 = {};
    g_batchedFogCaptureValid8 = false;
}

static void LogBatchedFogFailure8(const char* stage, HRESULT result)
{
    if (g_loggedBatchedFogFailure8)
        return;
    g_loggedBatchedFogFailure8 = true;
    Log("Batched fog stereo failed at %s: hr 0x%08X, draws %u, vertex "
        "range %u..%u, stride %u", stage, static_cast<unsigned>(result),
        static_cast<unsigned>(g_batchedFogDraws8.size()),
        g_batchedFogMinVertex8, g_batchedFogMaxVertexEnd8,
        g_batchedFogVertexStride8);
}

static void CaptureBatchedFogDraw8(UINT startVertex, UINT primitiveCount)
{
    const UINT verticesPerFogDraw = 6u;
    const UINT expectedStride = 28u;
    const std::size_t maximumFogDraws = 2048u;
    if (!g_device8 || primitiveCount != 4u ||
        g_batchedFogDraws8.size() >= maximumFogDraws ||
        startVertex > 0xFFFFFFFFu - verticesPerFogDraw)
    {
        return;
    }

    IDirect3DVertexBuffer8* vertexBuffer = nullptr;
    UINT stride = 0;
    if (FAILED(D3D8Slot<PFN8_GetStreamSource>(g_device8,
        VT8_GetStreamSource)(g_device8, 0, &vertexBuffer, &stride)) ||
        !vertexBuffer || stride != expectedStride)
    {
        if (vertexBuffer)
            D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
        return;
    }

    if (!g_batchedFogVertexBuffer8)
    {
        D3D8ViewportLocal viewport = {};
        const HRESULT viewportResult = D3D8Slot<PFN8_GetViewport>(g_device8,
            41)(g_device8, &viewport);
        if (FAILED(viewportResult) || viewport.Width == 0 ||
            viewport.Height == 0 || !CaptureBatchedFogPipeline8(
                g_batchedFogPipeline8, g_currentVertexShader8, true))
        {
            D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
            return;
        }

        g_batchedFogVertexBuffer8 = vertexBuffer;
        g_batchedFogVertexStride8 = stride;
        g_batchedFogSourceViewport8 = viewport;
        g_batchedFogCaptureValid8 = true;
        if (!g_loggedBatchedFogCapture8)
        {
            g_loggedBatchedFogCapture8 = true;
            Log("Batched fog capture started from exact draw caller "
                "0x005F572E");
        }
    }
    else if (vertexBuffer == g_batchedFogVertexBuffer8 &&
        stride == g_batchedFogVertexStride8)
    {
        D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
    }
    else
    {
        if (!g_loggedBatchedFogBufferMismatch8)
        {
            g_loggedBatchedFogBufferMismatch8 = true;
            Log("Batched fog skipped an additional vertex buffer; the "
                "primary fog stream remains active");
        }
        D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
        return;
    }

    g_batchedFogDraws8.push_back({ startVertex });
    g_batchedFogMinVertex8 = (std::min)(g_batchedFogMinVertex8,
        startVertex);
    g_batchedFogMaxVertexEnd8 = (std::max)(g_batchedFogMaxVertexEnd8,
        startVertex + verticesPerFogDraw);
}

static bool TransformBatchedFogVertex8(const BYTE* source, UINT stride,
    const D3D8ViewportLocal& sourceViewport,
    const D3DMATRIX& inverseGameViewProjection,
    const D3DMATRIX& eyeViewProjection, BYTE* destination)
{
    if (!source || !destination || stride < 16u ||
        sourceViewport.Width == 0 || sourceViewport.Height == 0 ||
        g_stereoTargetWidth8 == 0 || g_stereoTargetHeight8 == 0)
    {
        return false;
    }

    std::memcpy(destination, source, stride);
    float screenX = 0.0f;
    float screenY = 0.0f;
    std::memcpy(&screenX, source, sizeof(screenX));
    std::memcpy(&screenY, source + 4, sizeof(screenY));

    const float clip[4] = {
        ((screenX - static_cast<float>(sourceViewport.X)) /
            static_cast<float>(sourceViewport.Width)) * 2.0f - 1.0f,
        1.0f - ((screenY - static_cast<float>(sourceViewport.Y)) /
            static_cast<float>(sourceViewport.Height)) * 2.0f,
        1.0f,
        1.0f
    };
    float farPoint[4] = {};
    float eyeClip[4] = {};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
            farPoint[row] += inverseGameViewProjection.m[row][column] *
                clip[column];
    }
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
            eyeClip[row] += eyeViewProjection.m[row][column] *
                farPoint[column];
    }
    if (std::fabs(eyeClip[3]) < 0.000001f)
        return false;

    const float inverseW = 1.0f / eyeClip[3];
    const float targetX = (eyeClip[0] * inverseW * 0.5f + 0.5f) *
        static_cast<float>(g_stereoTargetWidth8);
    const float targetY = (1.0f -
        (eyeClip[1] * inverseW * 0.5f + 0.5f)) *
        static_cast<float>(g_stereoTargetHeight8);
    if (!std::isfinite(targetX) || !std::isfinite(targetY))
        return false;

    std::memcpy(destination, &targetX, sizeof(targetX));
    std::memcpy(destination + 4, &targetY, sizeof(targetY));
    // The original transformed fog vertices use z slightly above 1.0.
    // D3D8-on-12 can clip those vertices even when depth testing is disabled.
    // Keep the reconstructed far layer inside the legal viewport depth range.
    const float targetZ = 0.999f;
    const float targetRhw = 1.0f;
    std::memcpy(destination + 8, &targetZ, sizeof(targetZ));
    std::memcpy(destination + 12, &targetRhw, sizeof(targetRhw));
    return true;
}

static bool ReplayBatchedFogForStereo8()
{
    const UINT sourceVerticesPerDraw = 6u;
    const UINT outputVerticesPerDraw = 12u;
    static const UINT fanTriangleIndices[outputVerticesPerDraw] = {
        0u, 1u, 2u, 0u, 2u, 3u, 0u, 3u, 4u, 0u, 4u, 5u
    };

    if (!g_batchedFogCaptureValid8 || g_batchedFogDraws8.empty() ||
        !g_batchedFogVertexBuffer8 || g_batchedFogVertexStride8 != 28u ||
        g_batchedFogMinVertex8 == 0xFFFFFFFFu ||
        g_batchedFogMaxVertexEnd8 <= g_batchedFogMinVertex8 ||
        !g_batchedFogPipeline8.valid || g_heavyFullSceneStereo8)
    {
        if (g_batchedFogCaptureValid8 || !g_batchedFogDraws8.empty())
            LogBatchedFogFailure8("capture validation", E_FAIL);
        ResetBatchedFog8();
        return false;
    }

    if (!g_havePerDrawStereoMatrices8 || !g_havePerDrawGameMatrix8)
    {
        if (!g_loggedBatchedFogMissingMatrices8)
        {
            g_loggedBatchedFogMissingMatrices8 = true;
            Log("Batched fog capture skipped at Present because the frame "
                "did not provide complete game and stereo matrices");
        }
        ResetBatchedFog8();
        return false;
    }

    std::memcpy(g_batchedFogGameViewProjection8,
        g_perDrawGameViewProjection8,
        sizeof(g_batchedFogGameViewProjection8));
    std::memcpy(g_batchedFogLeftViewProjection8,
        g_perDrawCenterViewProjection8,
        sizeof(g_batchedFogLeftViewProjection8));
    std::memcpy(g_batchedFogRightViewProjection8,
        g_perDrawRightViewProjection8,
        sizeof(g_batchedFogRightViewProjection8));

    const std::size_t sourceVertexCount = static_cast<std::size_t>(
        g_batchedFogMaxVertexEnd8 - g_batchedFogMinVertex8);
    const std::size_t sourceByteCount = sourceVertexCount *
        g_batchedFogVertexStride8;
    if (sourceByteCount == 0 || sourceByteCount > 16u * 1024u * 1024u ||
        static_cast<std::size_t>(g_batchedFogMinVertex8) *
            g_batchedFogVertexStride8 > 0xFFFFFFFFu - sourceByteCount)
    {
        LogBatchedFogFailure8("source vertex range validation", E_FAIL);
        ResetBatchedFog8();
        return false;
    }

    BYTE* locked = nullptr;
    const HRESULT lockResult = D3D8Slot<PFNV8_Lock>(
        g_batchedFogVertexBuffer8, 11)(g_batchedFogVertexBuffer8,
            g_batchedFogMinVertex8 * g_batchedFogVertexStride8,
            static_cast<UINT>(sourceByteCount), &locked, 0x00000010u);
    if (FAILED(lockResult) || !locked)
    {
        LogBatchedFogFailure8("source vertex-buffer lock", lockResult);
        ResetBatchedFog8();
        return false;
    }
    std::vector<BYTE> source(sourceByteCount);
    std::memcpy(source.data(), locked, sourceByteCount);
    D3D8Slot<PFNV8_Unlock>(g_batchedFogVertexBuffer8, 12)(
        g_batchedFogVertexBuffer8);

    D3DMATRIX gameViewProjection = {};
    D3DMATRIX leftViewProjection = {};
    D3DMATRIX rightViewProjection = {};
    std::memcpy(&gameViewProjection.m[0][0],
        g_batchedFogGameViewProjection8, sizeof(gameViewProjection));
    std::memcpy(&leftViewProjection.m[0][0],
        g_batchedFogLeftViewProjection8, sizeof(leftViewProjection));
    std::memcpy(&rightViewProjection.m[0][0],
        g_batchedFogRightViewProjection8, sizeof(rightViewProjection));
    D3DMATRIX inverseGameViewProjection = {};
    if (!InvertD3D8Matrix(gameViewProjection, inverseGameViewProjection))
    {
        LogBatchedFogFailure8("game view-projection inversion", E_FAIL);
        ResetBatchedFog8();
        return false;
    }

    const std::size_t outputVertexCount = g_batchedFogDraws8.size() *
        outputVerticesPerDraw;
    const std::size_t outputByteCount = outputVertexCount *
        g_batchedFogVertexStride8;
    std::vector<BYTE> leftVertices(outputByteCount);
    std::vector<BYTE> rightVertices(outputByteCount);

    // SH3 builds fog from many overlapping screen-space cards. Once the
    // cards are reconstructed at a VR far plane, alpha blending turns those
    // layers into an expensive bright stack near the edge of a head turn.
    // Keep the card nearest to the center of each source-space cell. This
    // preserves the broad fog distribution while bounding overdraw.
    static constexpr UINT fogGridColumns = 6u;
    static constexpr UINT fogGridRows = 4u;
    static constexpr UINT fogGridCellCount = fogGridColumns * fogGridRows;
    UINT selectedFogStarts[fogGridCellCount] = {};
    float selectedFogScores[fogGridCellCount] = {};
    for (UINT index = 0; index < fogGridCellCount; ++index)
    {
        selectedFogStarts[index] = 0xFFFFFFFFu;
        selectedFogScores[index] = FLT_MAX;
    }

    const float viewportLeft = static_cast<float>(
        g_batchedFogSourceViewport8.X);
    const float viewportTop = static_cast<float>(
        g_batchedFogSourceViewport8.Y);
    const float viewportRight = viewportLeft + static_cast<float>(
        g_batchedFogSourceViewport8.Width);
    const float viewportBottom = viewportTop + static_cast<float>(
        g_batchedFogSourceViewport8.Height);
    bool sourceSelectionComplete = true;
    for (const BatchedFogDraw8& draw : g_batchedFogDraws8)
    {
        if (draw.startVertex < g_batchedFogMinVertex8 ||
            static_cast<std::size_t>(draw.startVertex -
                g_batchedFogMinVertex8) + sourceVerticesPerDraw >
                sourceVertexCount)
        {
            sourceSelectionComplete = false;
            break;
        }
        const BYTE* drawVertices = source.data() +
            static_cast<std::size_t>(draw.startVertex -
                g_batchedFogMinVertex8) * g_batchedFogVertexStride8;

        float minimumX = FLT_MAX;
        float maximumX = -FLT_MAX;
        float minimumY = FLT_MAX;
        float maximumY = -FLT_MAX;
        for (UINT sourceIndex = 0; sourceIndex < sourceVerticesPerDraw;
            ++sourceIndex)
        {
            const BYTE* sourceVertex = drawVertices +
                static_cast<std::size_t>(sourceIndex) *
                    g_batchedFogVertexStride8;
            float sourceX = 0.0f;
            float sourceY = 0.0f;
            std::memcpy(&sourceX, sourceVertex, sizeof(sourceX));
            std::memcpy(&sourceY, sourceVertex + 4, sizeof(sourceY));
            minimumX = (std::min)(minimumX, sourceX);
            maximumX = (std::max)(maximumX, sourceX);
            minimumY = (std::min)(minimumY, sourceY);
            maximumY = (std::max)(maximumY, sourceY);
        }
        if (maximumX < viewportLeft || minimumX > viewportRight ||
            maximumY < viewportTop || minimumY > viewportBottom)
        {
            continue;
        }

        const float centerX = (std::max)(viewportLeft, (std::min)(
            viewportRight, (minimumX + maximumX) * 0.5f));
        const float centerY = (std::max)(viewportTop, (std::min)(
            viewportBottom, (minimumY + maximumY) * 0.5f));
        const UINT column = (std::min)(fogGridColumns - 1u,
            static_cast<UINT>((centerX - viewportLeft) * fogGridColumns /
                (viewportRight - viewportLeft)));
        const UINT row = (std::min)(fogGridRows - 1u,
            static_cast<UINT>((centerY - viewportTop) * fogGridRows /
                (viewportBottom - viewportTop)));
        const float cellCenterX = viewportLeft +
            (static_cast<float>(column) + 0.5f) *
                (viewportRight - viewportLeft) / fogGridColumns;
        const float cellCenterY = viewportTop +
            (static_cast<float>(row) + 0.5f) *
                (viewportBottom - viewportTop) / fogGridRows;
        const float score = (centerX - cellCenterX) *
            (centerX - cellCenterX) + (centerY - cellCenterY) *
            (centerY - cellCenterY);
        const UINT cellIndex = row * fogGridColumns + column;
        if (score < selectedFogScores[cellIndex])
        {
            selectedFogScores[cellIndex] = score;
            selectedFogStarts[cellIndex] = draw.startVertex;
        }
    }

    std::size_t outputVertex = 0;
    UINT replayedFogDrawCount = 0;
    bool transformComplete = sourceSelectionComplete;
    for (UINT cellIndex = 0; cellIndex < fogGridCellCount &&
        transformComplete; ++cellIndex)
    {
        const UINT startVertex = selectedFogStarts[cellIndex];
        if (startVertex == 0xFFFFFFFFu)
            continue;
        if (startVertex < g_batchedFogMinVertex8 ||
            static_cast<std::size_t>(startVertex -
                g_batchedFogMinVertex8) + sourceVerticesPerDraw >
                sourceVertexCount)
        {
            transformComplete = false;
            break;
        }
        const BYTE* drawVertices = source.data() +
            static_cast<std::size_t>(startVertex -
                g_batchedFogMinVertex8) * g_batchedFogVertexStride8;

        for (UINT triangleVertex = 0;
            triangleVertex < outputVerticesPerDraw; ++triangleVertex)
        {
            const BYTE* sourceVertex = drawVertices +
                static_cast<std::size_t>(fanTriangleIndices[triangleVertex]) *
                g_batchedFogVertexStride8;
            BYTE* leftVertex = leftVertices.data() + outputVertex *
                g_batchedFogVertexStride8;
            BYTE* rightVertex = rightVertices.data() + outputVertex *
                g_batchedFogVertexStride8;
            if (!TransformBatchedFogVertex8(sourceVertex,
                g_batchedFogVertexStride8, g_batchedFogSourceViewport8,
                inverseGameViewProjection, leftViewProjection, leftVertex) ||
                !TransformBatchedFogVertex8(sourceVertex,
                    g_batchedFogVertexStride8, g_batchedFogSourceViewport8,
                    inverseGameViewProjection, rightViewProjection,
                    rightVertex))
            {
                transformComplete = false;
                break;
            }
            ++outputVertex;
        }
        if (!transformComplete)
            break;
        ++replayedFogDrawCount;
    }
    if (!transformComplete || outputVertex == 0)
    {
        LogBatchedFogFailure8(outputVertex == 0
            ? "source viewport fog cull" : "far-layer vertex reprojection",
            E_FAIL);
        ResetBatchedFog8();
        return false;
    }

    BatchedFogPipeline8 restorePipeline = {};
    if (!CaptureBatchedFogPipeline8(restorePipeline,
        g_currentVertexShader8, false))
    {
        LogBatchedFogFailure8("manual device-state capture", E_FAIL);
        ResetBatchedFog8();
        return false;
    }

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    bool leftComplete = false;
    bool rightComplete = false;
    HRESULT leftStateResult = E_FAIL;
    HRESULT leftDrawResult = E_FAIL;
    HRESULT rightStateResult = E_FAIL;
    HRESULT rightDrawResult = E_FAIL;
    bool leftTargetBound = false;
    bool leftViewportSet = false;
    bool rightTargetBound = false;
    bool rightViewportSet = false;
    if (BeginUiStereoPairPass(&originalColor, &originalDepth))
    {
        const UINT primitiveCount = static_cast<UINT>(
            outputVertex / 3u);
        leftStateResult = ApplyBatchedFogPipeline8(g_batchedFogPipeline8);
        leftTargetBound = SUCCEEDED(leftStateResult) &&
            SwitchStereoPairEye(0);
        leftViewportSet = leftTargetBound && SetUiOverlayViewport();
        if (leftViewportSet)
        {
            leftDrawResult = o_D3D8_DrawPrimitiveUP(g_device8,
                D3DPT_TRIANGLELIST, primitiveCount, leftVertices.data(),
                g_batchedFogVertexStride8);
        }
        leftComplete = SUCCEEDED(leftDrawResult);

        rightStateResult = ApplyBatchedFogPipeline8(g_batchedFogPipeline8);
        rightTargetBound = SUCCEEDED(rightStateResult) &&
            SwitchStereoPairEye(1);
        rightViewportSet = rightTargetBound && SetUiOverlayViewport();
        if (rightViewportSet)
        {
            rightDrawResult = o_D3D8_DrawPrimitiveUP(g_device8,
                D3DPT_TRIANGLELIST, primitiveCount, rightVertices.data(),
                g_batchedFogVertexStride8);
        }
        rightComplete = SUCCEEDED(rightDrawResult);
        EndStereoPairPass(originalColor, originalDepth);
    }
    else
    {
        LogBatchedFogFailure8("native eye target binding", E_FAIL);
    }

    const HRESULT restoreStateResult = ApplyBatchedFogPipeline8(restorePipeline);
    ReleaseBatchedFogPipeline8(restorePipeline);
    const UINT drawCount = static_cast<UINT>(g_batchedFogDraws8.size());

    if (leftComplete && rightComplete)
    {
        ResetBatchedFog8();
        InterlockedIncrement(&g_uiStereoOverlayDraws8);
        if (!g_loggedBatchedFogStereo8)
        {
            g_loggedBatchedFogStereo8 = true;
            Log("Batched distant fog stereo enabled: %u of %u source draws "
                "were reprojected as one triangle list per eye",
                replayedFogDrawCount, drawCount);
        }
        return true;
    }
    if (!g_loggedBatchedFogFailure8)
    {
        g_loggedBatchedFogFailure8 = true;
        Log("Batched fog stereo failed: draws %u, vertex range %u..%u, "
            "stride %u; left state 0x%08X target %u viewport %u draw "
            "0x%08X; right state 0x%08X target %u viewport %u draw "
            "0x%08X; restore state 0x%08X", drawCount,
            g_batchedFogMinVertex8,
            g_batchedFogMaxVertexEnd8, g_batchedFogVertexStride8,
            static_cast<unsigned>(leftStateResult),
            leftTargetBound ? 1u : 0u, leftViewportSet ? 1u : 0u,
            static_cast<unsigned>(leftDrawResult),
            static_cast<unsigned>(rightStateResult),
            rightTargetBound ? 1u : 0u, rightViewportSet ? 1u : 0u,
            static_cast<unsigned>(rightDrawResult),
            static_cast<unsigned>(restoreStateResult));
    }
    ResetBatchedFog8();
    return false;
}

static void LogFogPrimitiveSample8(UINT startVertex, UINT primitiveCount,
    const void* caller)
{
    if (g_loggedFogPrimitiveSample8 || !g_device8)
        return;
    g_loggedFogPrimitiveSample8 = true;

    IDirect3DVertexBuffer8* vertexBuffer = nullptr;
    UINT stride = 0;
    const HRESULT streamResult = D3D8Slot<PFN8_GetStreamSource>(g_device8,
        VT8_GetStreamSource)(g_device8, 0, &vertexBuffer, &stride);
    if (SUCCEEDED(streamResult) && vertexBuffer && stride >= 16u &&
        stride <= 1024u)
    {
        const UINT vertexCount = primitiveCount + 2u;
        const UINT lockOffset = startVertex * stride;
        const UINT lockBytes = vertexCount * stride;
        BYTE* locked = nullptr;
        const HRESULT lockResult = D3D8Slot<PFNV8_Lock>(vertexBuffer, 11)(
            vertexBuffer, lockOffset, lockBytes, &locked, 0x00000010u);
        Log("Fog vertex sample: caller 0x%08X, stream hr 0x%08X, lock hr "
            "0x%08X, stride %u, start %u, vertices %u",
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(caller)),
            static_cast<unsigned>(streamResult),
            static_cast<unsigned>(lockResult), stride, startVertex,
            vertexCount);
        if (SUCCEEDED(lockResult) && locked)
        {
            for (UINT vertexIndex = 0; vertexIndex < vertexCount;
                ++vertexIndex)
            {
                const BYTE* vertex = locked + vertexIndex * stride;
                float position[4] = {};
                std::memcpy(position, vertex, sizeof(position));
                const int xTimes1000 = static_cast<int>(
                    position[0] * 1000.0f);
                const int yTimes1000 = static_cast<int>(
                    position[1] * 1000.0f);
                const int zTimes1000000 = static_cast<int>(
                    position[2] * 1000000.0f);
                const int rhwTimes100000000 = static_cast<int>(
                    position[3] * 100000000.0f);
                Log("  fog vertex %u: x*1e3 %d, y*1e3 %d, z*1e6 %d, "
                    "rhw*1e8 %d", vertexIndex, xTimes1000, yTimes1000,
                    zTimes1000000, rhwTimes100000000);
            }
            D3D8Slot<PFNV8_Unlock>(vertexBuffer, 12)(vertexBuffer);
        }
        D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
    }
    else if (vertexBuffer)
    {
        D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
    }

    typedef HRESULT(WINAPI* PFN8_GetRenderStateLocal)(IDirect3DDevice8*,
        DWORD, DWORD*);
    const DWORD fogStates[] = { 7u, 14u, 19u, 20u, 27u };
    const char* fogStateNames[] = {
        "ZENABLE", "ZWRITEENABLE", "SRCBLEND", "DESTBLEND",
        "ALPHABLENDENABLE"
    };
    for (UINT stateIndex = 0; stateIndex < _countof(fogStates); ++stateIndex)
    {
        DWORD value = 0;
        const HRESULT stateResult = D3D8Slot<PFN8_GetRenderStateLocal>(
            g_device8, 51)(g_device8, fogStates[stateIndex], &value);
        Log("  fog state %s: value %u, hr 0x%08X",
            fogStateNames[stateIndex], static_cast<unsigned>(value),
            static_cast<unsigned>(stateResult));
    }
}

typedef DWORD(STDMETHODCALLTYPE* PFN8_BaseTextureGetType)(
    IDirect3DBaseTexture8*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_TextureGetLevelDesc)(
    IDirect3DBaseTexture8*, UINT, D3D8SurfaceDescLocal*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_TextureGetSurfaceLevel)(
    IDirect3DBaseTexture8*, UINT, IDirect3DSurface8**);

static void RememberOffscreenRenderTarget8(IDirect3DSurface8* color)
{
    if (!color || g_recentOffscreenTargetCount8 >=
        static_cast<LONG>(_countof(g_recentOffscreenTargets8)))
    {
        return;
    }
    for (LONG index = 0; index < g_recentOffscreenTargetCount8; ++index)
    {
        if (g_recentOffscreenTargets8[index] == color)
            return;
    }
    g_recentOffscreenTargets8[g_recentOffscreenTargetCount8++] = color;
}

static bool TextureUsesRecentOffscreenTarget8(
    IDirect3DBaseTexture8* texture)
{
    if (!texture || g_recentOffscreenTargetCount8 <= 0)
        return false;

    IDirect3DSurface8* levelSurface = nullptr;
    bool matches = false;
    __try
    {
        if (FAILED(D3D8Slot<PFN8_TextureGetSurfaceLevel>(texture, 15)(
            texture, 0, &levelSurface)) || !levelSurface)
        {
            return false;
        }
        for (LONG index = 0; index < g_recentOffscreenTargetCount8; ++index)
        {
            if (g_recentOffscreenTargets8[index] == levelSurface)
            {
                matches = true;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        matches = false;
    }
    if (levelSurface)
        ReleaseD3D8Surface(levelSurface);
    return matches;
}

static bool DescribeRenderTargetTexture8(IDirect3DBaseTexture8* texture,
    UINT* width, UINT* height, DWORD* usage)
{
    if (width)
        *width = 0;
    if (height)
        *height = 0;
    if (usage)
        *usage = 0;
    if (!texture)
        return false;

    D3D8SurfaceDescLocal desc = {};
    __try
    {
        const DWORD type = D3D8Slot<PFN8_BaseTextureGetType>(texture, 10)(
            texture);
        if (type != 3u || FAILED(D3D8Slot<PFN8_TextureGetLevelDesc>(
            texture, 14)(texture, 0, &desc)))
        {
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (width)
        *width = desc.Width;
    if (height)
        *height = desc.Height;
    if (usage)
        *usage = desc.Usage;
    return (desc.Usage & 0x00000001u) != 0;
}

static bool IsPrimarySceneDrawSamplingRenderTargetTexture8(DWORD* stage,
    UINT* width, UINT* height)
{
    DWORD matchingStage = 0;
    bool foundRenderTargetTexture = false;
    for (DWORD index = 0; index < _countof(g_currentTextures8); ++index)
    {
        if (g_currentTextures8[index] &&
            (g_currentTextureIsRenderTarget8[index] ||
                TextureUsesRecentOffscreenTarget8(g_currentTextures8[index])))
        {
            matchingStage = index;
            foundRenderTargetTexture = true;
            break;
        }
    }
    if (!foundRenderTargetTexture || !g_device8)
        return false;

    IDirect3DSurface8* target = nullptr;
    if (FAILED(D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(g_device8,
        &target)) || !target)
    {
        return false;
    }
    const bool primaryTarget = MatchesPrimaryGameRenderTargetDescription(
        target);
    ReleaseD3D8Surface(target);
    if (!primaryTarget)
        return false;

    if (stage)
        *stage = matchingStage;
    if (width)
        *width = g_currentTextureWidths8[matchingStage];
    if (height)
        *height = g_currentTextureHeights8[matchingStage];
    return true;
}

static bool IsLikelyUiAtlasDraw8()
{
    if (g_currentVertexShader8 != 0x00000144u || !g_device8)
        return false;

    IDirect3DBaseTexture8* texture = FirstBoundTexture8(nullptr);
    if (!texture)
        return false;

    DWORD type = 0;
    UINT width = 0;
    UINT height = 0;
    __try
    {
        type = D3D8Slot<PFN8_BaseTextureGetType>(texture, 10)(texture);
        if (type != 3u)
            return false;
        D3D8SurfaceDescLocal desc = {};
        if (FAILED(D3D8Slot<PFN8_TextureGetLevelDesc>(texture, 14)(
            texture, 0, &desc)))
        {
            return false;
        }
        width = desc.Width;
        height = desc.Height;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return width == 512u && height == 512u;
}

static bool IsGamePostProcessDraw8(DWORD primitiveType,
    UINT primitiveCount, UINT stride, const void* caller)
{
    if (!g_enableGamePostProcess8 || !g_device8 ||
        g_currentVertexShader8 != 0x00000044u ||
        primitiveType != D3DPT_TRIANGLESTRIP || primitiveCount != 2u ||
        stride != 20u || !caller)
    {
        return false;
    }

    const UINT_PTR address = reinterpret_cast<UINT_PTR>(caller);
    if (address != 0x005E8407u && address != 0x005E85EAu)
        return false;

    // The game applies its location-dependent grade as a full-size textured
    // quad on the primary desktop target. Do not replay intermediate light,
    // shadow, or effect surfaces that happen to use the same FVF.
    IDirect3DSurface8* target = nullptr;
    if (FAILED(D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(g_device8,
        &target)) || !target)
    {
        return false;
    }

    bool primaryTarget = MatchesPrimaryGameRenderTargetDescription(target);
    D3D8SurfaceDescLocal targetDesc = {};
    const bool haveTargetDesc = SUCCEEDED(D3D8Slot<PFNS8_GetDesc>(target, 8)(
        target, &targetDesc));
    ReleaseD3D8Surface(target);
    if (!primaryTarget || !haveTargetDesc || targetDesc.Width < 1280u ||
        targetDesc.Height < 720u)
    {
        return false;
    }

    IDirect3DBaseTexture8* texture = FirstBoundTexture8(nullptr);
    if (!texture)
        return false;

    DWORD type = 0;
    D3D8SurfaceDescLocal textureDesc = {};
    __try
    {
        type = D3D8Slot<PFN8_BaseTextureGetType>(texture, 10)(texture);
        if (type != 3u || FAILED(D3D8Slot<PFN8_TextureGetLevelDesc>(
            texture, 14)(texture, 0, &textureDesc)))
        {
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return textureDesc.Width == targetDesc.Width &&
        textureDesc.Height == targetDesc.Height;
}

static bool IsLowResolutionLightCompositeDraw8(DWORD primitiveType,
    UINT primitiveCount, const void* caller)
{
    // This draw becomes visible during a gunshot, but the 256x256 texture also
    // contains lamps and EXIT signs over a black background. It is SH3's
    // low-resolution light composite, not an independent muzzle-flash sprite.
    if (g_currentVertexShader8 != 0x00000005u ||
        primitiveType != D3DPT_TRIANGLESTRIP || primitiveCount != 1u ||
        reinterpret_cast<UINT_PTR>(caller) != 0x005F572Eu || !g_device8)
    {
        return false;
    }

    IDirect3DBaseTexture8* texture = FirstBoundTexture8(nullptr);
    if (!texture)
        return false;

    DWORD type = 0;
    UINT width = 0;
    UINT height = 0;
    __try
    {
        type = D3D8Slot<PFN8_BaseTextureGetType>(texture, 10)(texture);
        if (type != 3u)
            return false;
        D3D8SurfaceDescLocal desc = {};
        if (FAILED(D3D8Slot<PFN8_TextureGetLevelDesc>(texture, 14)(
            texture, 0, &desc)))
        {
            return false;
        }
        width = desc.Width;
        height = desc.Height;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return width == 256u && height == 256u;
}

static bool IsScreenSpaceEffectCompositeDraw8(DWORD primitiveType,
    UINT primitiveCount, UINT stride, const void* caller)
{
    if (g_currentVertexShader8 != 0x00000144u ||
        primitiveType != D3DPT_TRIANGLESTRIP || primitiveCount != 2u ||
        stride != 28u || !caller)
    {
        return false;
    }

    const UINT_PTR address = reinterpret_cast<UINT_PTR>(caller);
    if (address != 0x005F6937u && address != 0x005F68CCu)
        return false;

    // These two adjacent SH3 draw sites composite the transient blood and
    // muzzle-flash effect buffer. Their 512x512 texture overlaps the UI atlas
    // dimensions, but replaying the quad as UI produces a black mono layer.
    return IsLikelyUiAtlasDraw8();
}

static void TraceScreenSpaceDraw(const char* method, bool indexed,
    DWORD primitiveType, UINT primitiveCount, UINT stride,
    const void* caller)
{
    if (!g_enableUiTrace8 || !g_perDrawStereoProbeActive8 ||
        g_fullSceneStereoReplayActive8 ||
        (g_currentVertexShader8 & 0x0000000Eu) != 0x00000004u ||
        !g_device8)
    {
        return;
    }

    UINT targetWidth = 0;
    UINT targetHeight = 0;
    DWORD targetFormat = 0;
    IDirect3DSurface8* target = nullptr;
    if (SUCCEEDED(D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(g_device8,
        &target)) && target)
    {
        D3D8SurfaceDescLocal desc = {};
        if (SUCCEEDED(D3D8Slot<PFNS8_GetDesc>(target, 8)(target, &desc)))
        {
            targetWidth = desc.Width;
            targetHeight = desc.Height;
            targetFormat = desc.Format;
        }
        ReleaseD3D8Surface(target);
    }

    D3D8ViewportLocal viewport = {};
    const bool haveViewport = SUCCEEDED(D3D8Slot<PFN8_GetViewport>(g_device8,
        41)(g_device8, &viewport));
    const UINT viewportWidth = haveViewport ? viewport.Width : 0;
    const UINT viewportHeight = haveViewport ? viewport.Height : 0;
    DWORD boundTextureStage = 0;
    IDirect3DBaseTexture8* boundTexture = FirstBoundTexture8(
        &boundTextureStage);
    DWORD boundTextureType = 0;
    UINT boundTextureWidth = 0;
    UINT boundTextureHeight = 0;
    DWORD boundTextureFormat = 0;
    if (boundTexture)
    {
        __try
        {
            boundTextureType = D3D8Slot<PFN8_BaseTextureGetType>(
                boundTexture, 10)(boundTexture);
            if (boundTextureType == 3u)
            {
                D3D8SurfaceDescLocal textureDesc = {};
                if (SUCCEEDED(D3D8Slot<PFN8_TextureGetLevelDesc>(
                    boundTexture, 14)(boundTexture, 0, &textureDesc)))
                {
                    boundTextureWidth = textureDesc.Width;
                    boundTextureHeight = textureDesc.Height;
                    boundTextureFormat = textureDesc.Format;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            boundTextureType = 0;
            boundTextureWidth = 0;
            boundTextureHeight = 0;
            boundTextureFormat = 0;
        }
    }

    const LONG count = g_uiTraceRecordCount8;
    for (LONG index = 0; index < count; ++index)
    {
        const UiTraceRecord8& record = g_uiTraceRecords8[index];
        if (record.shader == g_currentVertexShader8 &&
            record.primitiveType == primitiveType &&
            record.primitiveCount == primitiveCount &&
            record.stride == stride &&
            record.targetWidth == targetWidth &&
            record.targetHeight == targetHeight &&
            record.viewportWidth == viewportWidth &&
            record.viewportHeight == viewportHeight &&
            record.caller == caller &&
            record.texture0 == boundTexture &&
            record.indexed == indexed)
        {
            return;
        }
    }

    if (count >= static_cast<LONG>(_countof(g_uiTraceRecords8)))
        return;

    UiTraceRecord8& record = g_uiTraceRecords8[count];
    record.shader = g_currentVertexShader8;
    record.primitiveType = primitiveType;
    record.primitiveCount = primitiveCount;
    record.stride = stride;
    record.targetWidth = targetWidth;
    record.targetHeight = targetHeight;
    record.viewportWidth = viewportWidth;
    record.viewportHeight = viewportHeight;
    record.caller = caller;
    record.texture0 = boundTexture;
    record.indexed = indexed;
    g_uiTraceRecordCount8 = count + 1;

    Log("Screen-space draw candidate: method %s, shader/FVF 0x%08X, "
        "primitive type %u, primitives %u, stride %u, target %ux%u "
        "format %u, viewport %ux%u, caller 0x%08X", method,
        g_currentVertexShader8, static_cast<unsigned>(primitiveType),
        static_cast<unsigned>(primitiveCount), static_cast<unsigned>(stride),
        targetWidth, targetHeight, static_cast<unsigned>(targetFormat),
        viewportWidth, viewportHeight,
        static_cast<unsigned>(reinterpret_cast<UINT_PTR>(caller)));
    Log("  screen-space first bound texture stage %u: 0x%08X",
        static_cast<unsigned>(boundTextureStage), static_cast<unsigned>(
            reinterpret_cast<UINT_PTR>(boundTexture)));
    Log("  screen-space texture description: type %u, %ux%u, format %u",
        static_cast<unsigned>(boundTextureType), boundTextureWidth,
        boundTextureHeight, static_cast<unsigned>(boundTextureFormat));
    LogRenderCaller("Screen-space draw candidate", caller);
}

static void TraceScreenSpaceState8(DWORD shader, DWORD primitiveType,
    UINT primitiveCount, UINT stride, const void* vertices,
    const void* caller)
{
    if (!g_enableUiTrace8 || !g_perDrawStereoProbeActive8 || !g_device8 ||
        (shader != 0x00000044u && shader != 0x00000104u) ||
        primitiveType != D3DPT_TRIANGLESTRIP || primitiveCount != 2u ||
        !caller || !vertices || stride < 20u)
    {
        return;
    }

    const UINT_PTR address = reinterpret_cast<UINT_PTR>(caller);
    const std::size_t hash = ((static_cast<std::size_t>(shader) * 131u) ^
        (static_cast<std::size_t>(address) >> 4)) %
        _countof(g_loggedScreenSpaceState8);
    if (g_loggedScreenSpaceState8[hash] ||
        InterlockedCompareExchange(&g_screenSpaceStateLogCount8, 0, 0) >=
            static_cast<LONG>(_countof(g_loggedScreenSpaceState8)))
    {
        return;
    }
    g_loggedScreenSpaceState8[hash] = true;
    InterlockedIncrement(&g_screenSpaceStateLogCount8);

    typedef HRESULT(WINAPI* PFN8_GetRenderStateLocal)(IDirect3DDevice8*,
        DWORD, DWORD*);
    struct RenderStateName
    {
        DWORD state;
        const char* name;
    };
    const RenderStateName states[] = {
        { 7u, "ZENABLE" },
        { 14u, "ZWRITEENABLE" },
        { 15u, "ALPHATESTENABLE" },
        { 19u, "SRCBLEND" },
        { 20u, "DESTBLEND" },
        { 27u, "ALPHABLENDENABLE" },
        { 174u, "SCISSORTESTENABLE" },
    };

    Log("Screen-space state probe: shader/FVF 0x%08X, caller 0x%08X, "
        "stride %u", shader,
        static_cast<unsigned>(address), static_cast<unsigned>(stride));
    for (const RenderStateName& item : states)
    {
        DWORD value = 0;
        HRESULT result = E_FAIL;
        __try
        {
            result = D3D8Slot<PFN8_GetRenderStateLocal>(g_device8, 51)(
                g_device8, item.state, &value);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            result = E_FAIL;
        }
        if (SUCCEEDED(result))
        {
            Log("  state %s (%u) = %u (0x%08X)", item.name, item.state,
                value, value);
        }
    }

    const UINT vertexCount = primitiveCount + 2u;
    for (UINT index = 0; index < vertexCount && index < 4u; ++index)
    {
        const std::uint8_t* vertex = static_cast<const std::uint8_t*>(
            vertices) + static_cast<std::size_t>(index) * stride;
        DWORD xBits = 0;
        DWORD yBits = 0;
        std::memcpy(&xBits, vertex + 0, sizeof(xBits));
        std::memcpy(&yBits, vertex + 4, sizeof(yBits));
        if (shader == 0x00000044u && stride >= 20u)
        {
            DWORD color = 0;
            std::memcpy(&color, vertex + 16, sizeof(color));
            Log("  vertex %u: xyBits=(0x%08X, 0x%08X), diffuse=0x%08X",
                index, xBits, yBits, color);
        }
        else if (shader == 0x00000104u && stride >= 24u)
        {
            DWORD uBits = 0;
            DWORD vBits = 0;
            std::memcpy(&uBits, vertex + 16, sizeof(uBits));
            std::memcpy(&vBits, vertex + 20, sizeof(vBits));
            Log("  vertex %u: xyBits=(0x%08X, 0x%08X), "
                "uvBits=(0x%08X, 0x%08X)", index, xBits, yBits,
                uBits, vBits);
        }
    }
}

static void ReleaseD3D8Surface(IDirect3DSurface8*& surface)
{
    if (surface)
    {
        D3D8Slot<PFNS8_Release>(surface, 2)(surface);
        surface = nullptr;
    }
}

static void ReleaseStereoRenderTargets()
{
    ResetBatchedFog8();
    ReleaseD3D8Surface(g_stereoRightDepth8);
    ReleaseD3D8Surface(g_stereoRightColor8);
    ReleaseD3D8Surface(g_stereoLeftDepth8);
    ReleaseD3D8Surface(g_stereoLeftColor8);
    g_stereoTargetWidth8 = 0;
    g_stereoTargetHeight8 = 0;
    g_gameRenderTargetDescriptorValid8 = false;
    g_gameRenderTargetWidth8 = 0;
    g_gameRenderTargetHeight8 = 0;
    g_gameRenderTargetFormat8 = 0;
    g_gameRenderTargetSamples8 = 0;
    g_gameRenderTargetType8 = 0;
    g_gameRenderTargetUsage8 = 0;
    g_gameRenderTargetPool8 = 0;
    g_gameRenderTargetSize8 = 0;
    ReleaseD3D8Surface(g_gameBackBuffer8);
    if (g_gameBackBufferIdentity8)
    {
        g_gameBackBufferIdentity8->Release();
        g_gameBackBufferIdentity8 = nullptr;
    }
    if (g_gameBackBufferContainer8)
    {
        g_gameBackBufferContainer8->Release();
        g_gameBackBufferContainer8 = nullptr;
    }
    ReleaseD3D8Surface(g_stereoReplayTarget8);
    if (g_stereoReplayTargetContainer8)
    {
        g_stereoReplayTargetContainer8->Release();
        g_stereoReplayTargetContainer8 = nullptr;
    }
    g_stereoReplayTargetFrame8 = -1;
    g_primaryTargetRefreshFrame8 = -1;
    g_loggedStereoSpecialTargetSkip8 = false;
    g_loggedStereoPrimaryTargetMatch8 = false;
    g_loggedStereoDescriptorTargetSkip8 = false;
    g_heavyFullSceneStereo8 = false;
    g_fullSceneStereoReplayActive8 = false;
    g_fullScenePrimaryTargetBound8 = false;
    g_fullSceneStereoPairReady8 = false;
    g_forceWaterFullSceneStereo8 = false;
    g_loggedWaterFullSceneStereo8 = false;
    g_loggedWaterRttDetection8 = false;
    g_loggedHeavyFullSceneStereo8 = false;
    g_uiStereoOverlayDraws8 = 0;
    g_loggedUiStereoOverlay8 = false;
    g_loggedFogUiFalsePositive8 = false;
    g_loggedFogPrimitiveSample8 = false;
    g_loggedBatchedFogStereo8 = false;
    g_loggedBatchedFogBufferMismatch8 = false;
    g_loggedBatchedFogCapture8 = false;
    g_loggedBatchedFogMissingMatrices8 = false;
    g_loggedBatchedFogFailure8 = false;
    g_loggedLightCompositeExcluded8 = false;
    g_loggedEffectCompositeExcluded8 = false;
    g_holdPreviousNativeEyeFrame8 = false;
    g_transientEffectPresentPreviousFrame8 = false;
    g_loggedTransientEyeFrameHeld8 = false;
    g_loggedFixedFunctionShadow8 = false;
}

static bool EnsureStereoRenderTargets(IDirect3DSurface8* originalColor,
    IDirect3DSurface8* originalDepth)
{
    if (g_stereoLeftColor8 && g_stereoLeftDepth8 &&
        g_stereoRightColor8 && g_stereoRightDepth8)
        return true;

    if (!g_device8 || !originalColor || !originalDepth)
        return false;

    D3D8SurfaceDescLocal colorDesc = {};
    D3D8SurfaceDescLocal depthDesc = {};
    if (FAILED(D3D8Slot<PFNS8_GetDesc>(originalColor, 8)(originalColor,
        &colorDesc)) ||
        FAILED(D3D8Slot<PFNS8_GetDesc>(originalDepth, 8)(originalDepth,
            &depthDesc)))
    {
        Log("Offscreen stereo test: failed to query render target descriptions");
        return false;
    }

    std::uint32_t requestedWidth = 0;
    std::uint32_t requestedHeight = 0;
    std::uint32_t requestedSamples = 0;
    if (!Interop8_GetRequestedEyeResolution(&requestedWidth, &requestedHeight,
        &requestedSamples))
    {
        return false;
    }

    // OpenXR sample count 1 maps to D3D8 multisample type NONE (zero).
    const DWORD targetMultisample = requestedSamples > 1
        ? requestedSamples : 0;

    HRESULT result = D3D8Slot<PFN8_CreateRenderTarget>(g_device8, 25)(g_device8,
        requestedWidth, requestedHeight, colorDesc.Format,
        targetMultisample, FALSE, &g_stereoLeftColor8);
    if (FAILED(result) || !g_stereoLeftColor8)
    {
        Log("Offscreen stereo test: CreateRenderTarget failed, hr=0x%08X",
            static_cast<unsigned>(result));
        ReleaseStereoRenderTargets();
        return false;
    }

    result = D3D8Slot<PFN8_CreateDepthStencilSurface>(g_device8, 26)(g_device8,
        requestedWidth, requestedHeight, depthDesc.Format,
        targetMultisample, &g_stereoLeftDepth8);
    if (FAILED(result) || !g_stereoLeftDepth8)
    {
        Log("Offscreen stereo test: CreateDepthStencilSurface failed, hr=0x%08X",
            static_cast<unsigned>(result));
        ReleaseStereoRenderTargets();
        return false;
    }

    result = D3D8Slot<PFN8_CreateRenderTarget>(g_device8, 25)(g_device8,
        requestedWidth, requestedHeight, colorDesc.Format,
        targetMultisample, FALSE, &g_stereoRightColor8);
    if (FAILED(result) || !g_stereoRightColor8)
    {
        Log("Offscreen stereo test: right CreateRenderTarget failed, hr=0x%08X",
            static_cast<unsigned>(result));
        ReleaseStereoRenderTargets();
        return false;
    }

    result = D3D8Slot<PFN8_CreateDepthStencilSurface>(g_device8, 26)(g_device8,
        requestedWidth, requestedHeight, depthDesc.Format,
        targetMultisample, &g_stereoRightDepth8);
    if (FAILED(result) || !g_stereoRightDepth8)
    {
        Log("Offscreen stereo test: right CreateDepthStencilSurface failed, "
            "hr=0x%08X", static_cast<unsigned>(result));
        ReleaseStereoRenderTargets();
        return false;
    }

    Log("Native OpenXR eye targets created: %ux%u color format %u, "
        "depth format %u, samples %u", requestedWidth, requestedHeight,
        static_cast<unsigned>(colorDesc.Format),
        static_cast<unsigned>(depthDesc.Format), requestedSamples);
    g_stereoTargetWidth8 = requestedWidth;
    g_stereoTargetHeight8 = requestedHeight;
    return true;
}

static bool ProbeNativeEyeRenderTargets()
{
    if (!g_device8)
        return false;

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    HRESULT result = D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(g_device8,
        &originalColor);
    if (FAILED(result) || !originalColor)
        return false;
    result = D3D8Slot<PFN8_GetDepthStencilSurface>(g_device8, 33)(g_device8,
        &originalDepth);
    const bool created = SUCCEEDED(result) && originalDepth &&
        EnsureStereoRenderTargets(originalColor, originalDepth);
    ReleaseD3D8Surface(originalDepth);
    ReleaseD3D8Surface(originalColor);
    if (created)
    {
        Log("Native OpenXR eye target capability probe succeeded");
        Log("Native eye targets retained for one-frame stereo draw replay");
    }
    return created;
}

static bool BeginOffscreenDuplicatePass(std::uint32_t eye,
    IDirect3DSurface8** originalColor, IDirect3DSurface8** originalDepth)
{
    if (!g_device8 || !originalColor || !originalDepth)
        return false;

    *originalColor = nullptr;
    *originalDepth = nullptr;
    g_savedStereoViewportValid8 = SUCCEEDED(
        D3D8Slot<PFN8_GetViewport>(g_device8, 41)(g_device8,
            &g_savedStereoViewport8));
    HRESULT result = D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(g_device8,
        originalColor);
    if (FAILED(result) || !*originalColor)
        return false;

    result = D3D8Slot<PFN8_GetDepthStencilSurface>(g_device8, 33)(g_device8,
        originalDepth);
    if (FAILED(result) || !*originalDepth ||
        !EnsureStereoRenderTargets(*originalColor, *originalDepth))
    {
        ReleaseD3D8Surface(*originalDepth);
        ReleaseD3D8Surface(*originalColor);
        return false;
    }

    IDirect3DSurface8* color = eye == 0
        ? g_stereoLeftColor8 : g_stereoRightColor8;
    IDirect3DSurface8* depth = eye == 0
        ? g_stereoLeftDepth8 : g_stereoRightDepth8;
    result = D3D8Slot<PFN8_SetRenderTarget>(g_device8, 31)(g_device8,
        color, depth);
    if (FAILED(result))
    {
        Log("Offscreen stereo test: SetRenderTarget(eye %u) failed, hr=0x%08X",
            eye, static_cast<unsigned>(result));
        ReleaseD3D8Surface(*originalDepth);
        ReleaseD3D8Surface(*originalColor);
        return false;
    }

    D3D8ViewportLocal automaticViewport = {};
    const bool haveAutomaticViewport = SUCCEEDED(
        D3D8Slot<PFN8_GetViewport>(g_device8, 41)(g_device8,
            &automaticViewport));
    D3D8ViewportLocal eyeViewport = {};
    eyeViewport.Width = g_stereoTargetWidth8;
    eyeViewport.Height = g_stereoTargetHeight8;
    eyeViewport.MinZ = 0.0f;
    eyeViewport.MaxZ = 1.0f;
    result = D3D8Slot<PFN8_SetViewport>(g_device8, 40)(g_device8,
        &eyeViewport);
    if (FAILED(result))
    {
        Log("Stereo eye viewport %ux%u failed, hr=0x%08X",
            eyeViewport.Width, eyeViewport.Height,
            static_cast<unsigned>(result));
    }
    if (!g_loggedStereoViewport8)
    {
        g_loggedStereoViewport8 = true;
        Log("Stereo viewport transition: original %ux%u at %u,%u; "
            "automatic after SetRenderTarget %ux%u at %u,%u; forced %ux%u",
            g_savedStereoViewport8.Width, g_savedStereoViewport8.Height,
            g_savedStereoViewport8.X, g_savedStereoViewport8.Y,
            haveAutomaticViewport ? automaticViewport.Width : 0,
            haveAutomaticViewport ? automaticViewport.Height : 0,
            haveAutomaticViewport ? automaticViewport.X : 0,
            haveAutomaticViewport ? automaticViewport.Y : 0,
            eyeViewport.Width, eyeViewport.Height);
    }
    return true;
}

static void EndOffscreenDuplicatePass(IDirect3DSurface8* originalColor,
    IDirect3DSurface8* originalDepth)
{
    const HRESULT result = D3D8Slot<PFN8_SetRenderTarget>(g_device8, 31)(g_device8,
        originalColor, originalDepth);
    if (FAILED(result))
    {
        Log("Offscreen stereo test: restoring the game render target failed, "
            "hr=0x%08X", static_cast<unsigned>(result));
    }
    if (g_savedStereoViewportValid8)
    {
        const HRESULT viewportResult =
            D3D8Slot<PFN8_SetViewport>(g_device8, 40)(g_device8,
                &g_savedStereoViewport8);
        if (FAILED(viewportResult))
        {
            Log("Restoring the game viewport failed, hr=0x%08X",
                static_cast<unsigned>(viewportResult));
        }
    }
    g_savedStereoViewportValid8 = false;
    ReleaseD3D8Surface(originalDepth);
    ReleaseD3D8Surface(originalColor);
}

static void RefreshPrimaryGameRenderTarget()
{
    if (!g_device8)
        return;

    const LONG presentFrame = InterlockedCompareExchange(&c_present8, 0, 0);
    if (g_primaryTargetRefreshFrame8 == presentFrame)
        return;
    g_primaryTargetRefreshFrame8 = presentFrame;

    IDirect3DSurface8* backBuffer = nullptr;
    const HRESULT result = D3D8Slot<PFN8_GetBackBuffer>(g_device8, 16)(
        g_device8, 0, 0, &backBuffer);
    if (FAILED(result) || !backBuffer)
        return;

    if (g_gameBackBuffer8 != backBuffer)
    {
        ReleaseD3D8Surface(g_gameBackBuffer8);
        g_gameBackBuffer8 = backBuffer;
        backBuffer = nullptr;

        if (g_gameBackBufferIdentity8)
        {
            g_gameBackBufferIdentity8->Release();
            g_gameBackBufferIdentity8 = nullptr;
        }
        D3D8Slot<PFNS8_QueryInterface>(g_gameBackBuffer8, 0)(
            g_gameBackBuffer8, IID_IUnknown,
            reinterpret_cast<void**>(&g_gameBackBufferIdentity8));
        D3D8Slot<PFNS8_GetContainer>(g_gameBackBuffer8, 7)(
            g_gameBackBuffer8, IID_IUnknown,
            reinterpret_cast<void**>(&g_gameBackBufferContainer8));

        D3D8SurfaceDescLocal description = {};
        if (SUCCEEDED(D3D8Slot<PFNS8_GetDesc>(g_gameBackBuffer8, 8)(
            g_gameBackBuffer8, &description)))
        {
            g_gameRenderTargetWidth8 = description.Width;
            g_gameRenderTargetHeight8 = description.Height;
            g_gameRenderTargetFormat8 = description.Format;
            g_gameRenderTargetSamples8 = description.MultiSampleType;
            g_gameRenderTargetType8 = description.Type;
            g_gameRenderTargetUsage8 = description.Usage;
            g_gameRenderTargetPool8 = description.Pool;
            g_gameRenderTargetSize8 = description.Size;
            g_gameRenderTargetDescriptorValid8 = true;
        }
    }
    ReleaseD3D8Surface(backBuffer);
}

static bool IsPrimaryGameRenderTarget(IDirect3DSurface8* surface)
{
    if (!g_device8 || !surface)
        return false;

    const LONG presentFrame = InterlockedCompareExchange(&c_present8, 0, 0);
    if (g_stereoReplayTargetFrame8 != presentFrame)
    {
        ReleaseD3D8Surface(g_stereoReplayTarget8);
        if (g_stereoReplayTargetContainer8)
        {
            g_stereoReplayTargetContainer8->Release();
            g_stereoReplayTargetContainer8 = nullptr;
        }
        g_stereoReplayTargetFrame8 = presentFrame;

        // The first selected draw in a gameplay frame is the world pass. Lock
        // that surface for the rest of the frame; later selected draws must
        // use the same scene target. This preserves stereo for SH3's actual
        // scene render target while excluding unrelated post-process targets.
        if (g_currentVertexShader8 != 0x2D)
            return false;

        g_stereoReplayTarget8 = surface;
        D3D8Slot<PFNS8_AddRef>(surface, 1)(surface);
        D3D8Slot<PFNS8_GetContainer>(surface, 7)(
            surface, IID_IUnknown,
            reinterpret_cast<void**>(&g_stereoReplayTargetContainer8));
        Log("Stereo replay target locked to world surface for game frame %d",
            presentFrame + 1);
        return true;
    }

    if (surface == g_stereoReplayTarget8)
        return true;
    if (g_stereoReplayTargetContainer8)
    {
        IUnknown* container = nullptr;
        if (SUCCEEDED(D3D8Slot<PFNS8_GetContainer>(surface, 7)(
            surface, IID_IUnknown, reinterpret_cast<void**>(&container))) &&
            container)
        {
            const bool sameContainer =
                container == g_stereoReplayTargetContainer8;
            container->Release();
            if (sameContainer)
                return true;
        }
    }

    if (!g_loggedStereoSpecialTargetSkip8)
    {
        g_loggedStereoSpecialTargetSkip8 = true;
        Log("Stereo replay skipped a target change after the world surface "
            "was locked for this game frame");
    }
    return false;
}

static bool MatchesPrimaryGameRenderTargetDescription(
    IDirect3DSurface8* surface)
{
    if (!g_device8 || !surface)
        return false;

    RefreshPrimaryGameRenderTarget();
    if (!g_gameRenderTargetDescriptorValid8)
    {
        // Preserve the working replay path if the wrapper cannot expose a
        // backbuffer description. Descriptor filtering is a safety guard, not
        // a reason to disable native stereo.
        return true;
    }

    D3D8SurfaceDescLocal description = {};
    if (FAILED(D3D8Slot<PFNS8_GetDesc>(surface, 8)(surface, &description)))
        return true;

    const bool matches =
        description.Width == g_gameRenderTargetWidth8 &&
        description.Height == g_gameRenderTargetHeight8 &&
        description.Format == g_gameRenderTargetFormat8;
    if (matches)
    {
        if (!g_loggedStereoPrimaryTargetMatch8)
        {
            g_loggedStereoPrimaryTargetMatch8 = true;
            Log("Stereo replay accepted primary target description %ux%u, "
                "format %u", description.Width, description.Height,
                static_cast<unsigned>(description.Format));
        }
        return true;
    }

    if (!g_loggedStereoDescriptorTargetSkip8)
    {
        g_loggedStereoDescriptorTargetSkip8 = true;
        Log("Stereo replay skipped an intermediate target %ux%u, format %u; "
            "primary target is %ux%u, format %u",
            description.Width, description.Height,
            static_cast<unsigned>(description.Format),
            g_gameRenderTargetWidth8, g_gameRenderTargetHeight8,
            static_cast<unsigned>(g_gameRenderTargetFormat8));
    }
    return false;
}

static HRESULT WINAPI hk_D3D8_SetRenderTarget(IDirect3DDevice8* device,
    IDirect3DSurface8* color, IDirect3DSurface8* depth)
{
    if (!g_fullSceneStereoReplayActive8 && color &&
        color != g_stereoLeftColor8 && color != g_stereoRightColor8 &&
        (!g_gameRenderTargetDescriptorValid8 ||
            !MatchesPrimaryGameRenderTargetDescription(color)))
    {
        RememberOffscreenRenderTarget8(color);
    }
    if (g_fullSceneStereoReplayActive8 && color &&
        MatchesPrimaryGameRenderTargetDescription(color))
    {
        IDirect3DSurface8* eyeColor = g_renderEye8 == 0
            ? g_stereoLeftColor8 : g_stereoRightColor8;
        IDirect3DSurface8* eyeDepth = g_renderEye8 == 0
            ? g_stereoLeftDepth8 : g_stereoRightDepth8;
        if (eyeColor && eyeDepth)
        {
            g_fullScenePrimaryTargetBound8 = true;
            return o_D3D8_SetRenderTarget(device, eyeColor, eyeDepth);
        }
    }

    if (g_fullSceneStereoReplayActive8)
        g_fullScenePrimaryTargetBound8 = false;
    return o_D3D8_SetRenderTarget(device, color, depth);
}

static HRESULT WINAPI hk_D3D8_SetViewport(IDirect3DDevice8* device,
    const D3D8ViewportLocal* viewport)
{
    if (g_fullSceneStereoReplayActive8 &&
        g_fullScenePrimaryTargetBound8 && viewport)
    {
        D3D8ViewportLocal eyeViewport = *viewport;
        eyeViewport.X = 0;
        eyeViewport.Y = 0;
        eyeViewport.Width = g_stereoTargetWidth8;
        eyeViewport.Height = g_stereoTargetHeight8;
        return o_D3D8_SetViewport(device, &eyeViewport);
    }
    return o_D3D8_SetViewport(device, viewport);
}

static bool BindStereoEyeTarget(std::uint32_t eye)
{
    if (!g_device8)
        return false;

    IDirect3DSurface8* color = eye == 0
        ? g_stereoLeftColor8 : g_stereoRightColor8;
    IDirect3DSurface8* depth = eye == 0
        ? g_stereoLeftDepth8 : g_stereoRightDepth8;
    if (!color || !depth)
        return false;

    const HRESULT result = D3D8Slot<PFN8_SetRenderTarget>(g_device8, 31)(
        g_device8, color, depth);
    if (FAILED(result))
    {
        Log("Stereo pair: SetRenderTarget(eye %u) failed, hr=0x%08X",
            eye, static_cast<unsigned>(result));
        return false;
    }

    // D3D8 resets the viewport to the bound render target dimensions. The
    // old per-draw path redundantly called SetViewport here for every eye,
    // adding hundreds of state transitions per frame and contributing to the
    // driver watchdog. EndStereoPairPass restores the original game viewport.
    return true;
}

static bool BeginStereoPairPass(IDirect3DSurface8** originalColor,
    IDirect3DSurface8** originalDepth)
{
    if (!g_device8 || !originalColor || !originalDepth)
        return false;

    *originalColor = nullptr;
    *originalDepth = nullptr;
    g_savedStereoViewportValid8 = SUCCEEDED(
        D3D8Slot<PFN8_GetViewport>(g_device8, 41)(g_device8,
            &g_savedStereoViewport8));

    HRESULT result = D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(g_device8,
        originalColor);
    if (FAILED(result) || !*originalColor)
        return false;

    if (!MatchesPrimaryGameRenderTargetDescription(*originalColor))
    {
        ReleaseD3D8Surface(*originalColor);
        g_savedStereoViewportValid8 = false;
        return false;
    }

    result = D3D8Slot<PFN8_GetDepthStencilSurface>(g_device8, 33)(g_device8,
        originalDepth);
    if (FAILED(result) || !*originalDepth ||
        !EnsureStereoRenderTargets(*originalColor, *originalDepth))
    {
        ReleaseD3D8Surface(*originalDepth);
        ReleaseD3D8Surface(*originalColor);
        g_savedStereoViewportValid8 = false;
        return false;
    }

    if (!BindStereoEyeTarget(0))
    {
        ReleaseD3D8Surface(*originalDepth);
        ReleaseD3D8Surface(*originalColor);
        g_savedStereoViewportValid8 = false;
        return false;
    }
    return true;
}

static bool BeginUiStereoPairPass(IDirect3DSurface8** originalColor,
    IDirect3DSurface8** originalDepth)
{
    if (!g_device8 || !originalColor || !originalDepth)
        return false;

    *originalColor = nullptr;
    *originalDepth = nullptr;
    g_savedStereoViewportValid8 = SUCCEEDED(
        D3D8Slot<PFN8_GetViewport>(g_device8, 41)(g_device8,
            &g_savedStereoViewport8));

    HRESULT result = D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(
        g_device8, originalColor);
    if (FAILED(result) || !*originalColor)
    {
        g_savedStereoViewportValid8 = false;
        return false;
    }

    if (!MatchesPrimaryGameRenderTargetDescription(*originalColor))
    {
        ReleaseD3D8Surface(*originalColor);
        g_savedStereoViewportValid8 = false;
        return false;
    }

    // SH3 draws subtitles and pickup notifications with no depth-stencil
    // surface bound. UI replay only needs the eye color/depth targets; the
    // original NULL depth surface is restored by EndStereoPairPass.
    result = D3D8Slot<PFN8_GetDepthStencilSurface>(g_device8, 33)(
        g_device8, originalDepth);
    if (FAILED(result))
        *originalDepth = nullptr;

    const bool haveEyeTargets = g_stereoLeftColor8 && g_stereoLeftDepth8 &&
        g_stereoRightColor8 && g_stereoRightDepth8;
    if (!haveEyeTargets)
    {
        if (!*originalDepth || !EnsureStereoRenderTargets(*originalColor,
            *originalDepth))
        {
            ReleaseD3D8Surface(*originalDepth);
            ReleaseD3D8Surface(*originalColor);
            g_savedStereoViewportValid8 = false;
            return false;
        }
    }

    if (!BindStereoEyeTarget(0))
    {
        ReleaseD3D8Surface(*originalDepth);
        ReleaseD3D8Surface(*originalColor);
        g_savedStereoViewportValid8 = false;
        return false;
    }
    return true;
}

// EndScene can be reached with one of SH3's intermediate post-process targets
// still bound.  The tracked hand is an eye-space overlay and only needs the
// already-created native eye color/depth surfaces, so do not require the
// current game target to match the primary desktop backbuffer here.
static bool BeginExistingStereoPairPass(IDirect3DSurface8** originalColor,
    IDirect3DSurface8** originalDepth)
{
    if (!g_device8 || !originalColor || !originalDepth ||
        !g_stereoLeftColor8 || !g_stereoLeftDepth8 ||
        !g_stereoRightColor8 || !g_stereoRightDepth8)
    {
        return false;
    }

    *originalColor = nullptr;
    *originalDepth = nullptr;
    g_savedStereoViewportValid8 = SUCCEEDED(
        D3D8Slot<PFN8_GetViewport>(g_device8, 41)(g_device8,
            &g_savedStereoViewport8));

    HRESULT result = D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(
        g_device8, originalColor);
    if (FAILED(result) || !*originalColor)
    {
        g_savedStereoViewportValid8 = false;
        return false;
    }

    result = D3D8Slot<PFN8_GetDepthStencilSurface>(g_device8, 33)(
        g_device8, originalDepth);
    if (FAILED(result))
        *originalDepth = nullptr;

    if (!BindStereoEyeTarget(0))
    {
        ReleaseD3D8Surface(*originalDepth);
        ReleaseD3D8Surface(*originalColor);
        g_savedStereoViewportValid8 = false;
        return false;
    }
    return true;
}

static bool SwitchStereoPairEye(std::uint32_t eye)
{
    return BindStereoEyeTarget(eye);
}

static void ClearStereoPairEyeIfNeeded(std::uint32_t eye)
{
    if (eye > 1 || g_perDrawStereoTargetCleared8[eye] || !g_device8)
        return;

    const DWORD clearTarget = 0x00000001u;
    const DWORD clearDepth = 0x00000002u;
    const DWORD clearStencil = 0x00000004u;
    const DWORD clearColor = g_haveGameClearColor8
        ? g_gameClearColor8 : 0x00000000u;
    const HRESULT result = o_D3D8_Clear(g_device8, 0, nullptr,
        clearTarget | clearDepth | clearStencil, clearColor, 1.0f, 0);
    if (FAILED(result))
    {
        Log("Stereo pair: eye %u clear failed, hr=0x%08X", eye,
            static_cast<unsigned>(result));
    }
    g_perDrawStereoTargetCleared8[eye] = true;
}

static void EndStereoPairPass(IDirect3DSurface8* originalColor,
    IDirect3DSurface8* originalDepth)
{
    if (g_device8)
    {
        const HRESULT result = D3D8Slot<PFN8_SetRenderTarget>(g_device8, 31)(
            g_device8, originalColor, originalDepth);
        if (FAILED(result))
        {
            Log("Stereo pair: restoring the game render target failed, "
                "hr=0x%08X", static_cast<unsigned>(result));
        }
        if (g_savedStereoViewportValid8)
        {
            const HRESULT viewportResult =
                D3D8Slot<PFN8_SetViewport>(g_device8, 40)(g_device8,
                    &g_savedStereoViewport8);
            if (FAILED(viewportResult))
            {
                Log("Stereo pair: restoring the game viewport failed, "
                    "hr=0x%08X", static_cast<unsigned>(viewportResult));
            }
        }
    }
    g_savedStereoViewportValid8 = false;
    ReleaseD3D8Surface(originalDepth);
    ReleaseD3D8Surface(originalColor);
}

static std::wstring GetModuleSiblingPath8(const wchar_t* fileName)
{
    wchar_t modulePath[MAX_PATH] = {};
    if (!fileName || GetModuleFileNameW(nullptr, modulePath,
        static_cast<DWORD>(_countof(modulePath))) == 0)
    {
        return {};
    }
    wchar_t* finalSlash = wcsrchr(modulePath, L'\\');
    if (!finalSlash)
        return {};
    *(finalSlash + 1) = L'\0';
    std::wstring result(modulePath);
    result += fileName;
    return result;
}

static bool ReadWholeFile8(const std::wstring& path,
    std::vector<std::uint8_t>* output)
{
    if (path.empty() || !output)
        return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
        size.QuadPart <= 64ll * 1024ll * 1024ll;
    if (!validSize)
    {
        CloseHandle(file);
        return false;
    }
    output->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const bool read = ReadFile(file, output->data(),
        static_cast<DWORD>(output->size()), &bytesRead, nullptr) &&
        bytesRead == output->size();
    CloseHandle(file);
    if (!read)
        output->clear();
    return read;
}

static void ReleaseLeftHandResources8()
{
    ReleaseD3D8Surface(g_leftHandDesktopDepth8);
    ReleaseD3D8Surface(g_leftHandDesktopWeaponDepth8);
    g_leftHandDesktopWeaponDepthPresent8 = -1;
    ReleaseD3D8Surface(g_leftHandLightSampleSurface8);
    g_leftHandDesktopDepthWidth8 = 0;
    g_leftHandDesktopDepthHeight8 = 0;
    g_leftHandDesktopDepthSamples8 = 0;
    for (IDirect3DBaseTexture8*& texture : g_leftHandTextures8)
    {
        if (texture)
        {
            D3D8Slot<PFNT8_Release>(texture, 2)(texture);
            texture = nullptr;
        }
    }
    g_leftHandMeshParts8.clear();
    g_leftHandResourceDevice8 = nullptr;
    g_leftHandResourcesLoaded8 = false;
    g_leftHandResourcesAttempted8 = false;
    g_leftHandSceneLightValid8 = false;
}

static void RebuildLeftHandGeometryNormals8(
    std::vector<LeftHandMeshPart8>& parts)
{
    for (LeftHandMeshPart8& part : parts)
    {
        std::vector<float> accumulated(part.vertices.size() * 3u, 0.0f);
        for (std::size_t triangle = 0; triangle + 2u < part.indices.size();
            triangle += 3u)
        {
            const std::uint16_t indices[3] = {
                part.indices[triangle], part.indices[triangle + 1u],
                part.indices[triangle + 2u]
            };
            if (indices[0] >= part.vertices.size() ||
                indices[1] >= part.vertices.size() ||
                indices[2] >= part.vertices.size())
            {
                continue;
            }
            const float* p0 = part.vertices[indices[0]].position;
            const float* p1 = part.vertices[indices[1]].position;
            const float* p2 = part.vertices[indices[2]].position;
            const float edge1[3] = {
                p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
            };
            const float edge2[3] = {
                p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
            };
            float faceNormal[3] = {
                edge1[1] * edge2[2] - edge1[2] * edge2[1],
                edge1[2] * edge2[0] - edge1[0] * edge2[2],
                edge1[0] * edge2[1] - edge1[1] * edge2[0]
            };
            const float authoredNormal[3] = {
                part.vertices[indices[0]].normal[0] +
                    part.vertices[indices[1]].normal[0] +
                    part.vertices[indices[2]].normal[0],
                part.vertices[indices[0]].normal[1] +
                    part.vertices[indices[1]].normal[1] +
                    part.vertices[indices[2]].normal[1],
                part.vertices[indices[0]].normal[2] +
                    part.vertices[indices[1]].normal[2] +
                    part.vertices[indices[2]].normal[2]
            };
            const float agreement = faceNormal[0] * authoredNormal[0] +
                faceNormal[1] * authoredNormal[1] +
                faceNormal[2] * authoredNormal[2];
            if (agreement < 0.0f)
                for (float& component : faceNormal)
                    component = -component;
            for (std::uint16_t index : indices)
                for (int axis = 0; axis < 3; ++axis)
                    accumulated[static_cast<std::size_t>(index) * 3u + axis]
                        += faceNormal[axis];
        }

        // Average coincident vertices as well. GLB material and UV seams often
        // duplicate a position; leaving separate normal islands is what made
        // the top of the hand, fingers and watch split into visible patches.
        constexpr float kWeldDistanceSquared = 0.00000001f;
        for (std::size_t vertexIndex = 0;
            vertexIndex < part.vertices.size(); ++vertexIndex)
        {
            float normal[3] = {};
            const float* position = part.vertices[vertexIndex].position;
            for (std::size_t candidate = 0;
                candidate < part.vertices.size(); ++candidate)
            {
                const float* other = part.vertices[candidate].position;
                const float dx = position[0] - other[0];
                const float dy = position[1] - other[1];
                const float dz = position[2] - other[2];
                if (dx * dx + dy * dy + dz * dz > kWeldDistanceSquared)
                    continue;
                for (int axis = 0; axis < 3; ++axis)
                    normal[axis] += accumulated[candidate * 3u + axis];
            }
            const float length = std::sqrt(normal[0] * normal[0] +
                normal[1] * normal[1] + normal[2] * normal[2]);
            if (length > 0.000001f)
                for (int axis = 0; axis < 3; ++axis)
                    part.vertices[vertexIndex].normal[axis] =
                        normal[axis] / length;
        }
    }
}

static bool EnsureLeftHandDesktopDepth8(IDirect3DSurface8* backBuffer)
{
    if (!g_device8 || !backBuffer || !g_stereoLeftDepth8)
        return false;

    D3D8SurfaceDescLocal colorDesc = {};
    D3D8SurfaceDescLocal depthDesc = {};
    if (FAILED(D3D8Slot<PFNS8_GetDesc>(backBuffer, 8)(backBuffer,
            &colorDesc)) ||
        FAILED(D3D8Slot<PFNS8_GetDesc>(g_stereoLeftDepth8, 8)(
            g_stereoLeftDepth8, &depthDesc)))
    {
        return false;
    }

    if (g_leftHandDesktopDepth8 &&
        g_leftHandDesktopDepthWidth8 == colorDesc.Width &&
        g_leftHandDesktopDepthHeight8 == colorDesc.Height &&
        g_leftHandDesktopDepthSamples8 == colorDesc.MultiSampleType)
    {
        return true;
    }

    ReleaseD3D8Surface(g_leftHandDesktopDepth8);
    const HRESULT result = D3D8Slot<PFN8_CreateDepthStencilSurface>(
        g_device8, 26)(g_device8, colorDesc.Width, colorDesc.Height,
        depthDesc.Format, colorDesc.MultiSampleType,
        &g_leftHandDesktopDepth8);
    if (FAILED(result) || !g_leftHandDesktopDepth8)
        return false;

    g_leftHandDesktopDepthWidth8 = colorDesc.Width;
    g_leftHandDesktopDepthHeight8 = colorDesc.Height;
    g_leftHandDesktopDepthSamples8 = colorDesc.MultiSampleType;
    Log("LeftHand: desktop self-depth surface created at %ux%u",
        colorDesc.Width, colorDesc.Height);
    return true;
}

static bool IsLeftHandDesktopDepthCompatible8(IDirect3DSurface8* color,
    IDirect3DSurface8* depth)
{
    if (!color || !depth)
        return false;
    D3D8SurfaceDescLocal colorDesc = {};
    D3D8SurfaceDescLocal depthDesc = {};
    return SUCCEEDED(D3D8Slot<PFNS8_GetDesc>(color, 8)(color,
            &colorDesc)) &&
        SUCCEEDED(D3D8Slot<PFNS8_GetDesc>(depth, 8)(depth,
            &depthDesc)) &&
        colorDesc.Width == depthDesc.Width &&
        colorDesc.Height == depthDesc.Height &&
        colorDesc.MultiSampleType == depthDesc.MultiSampleType;
}

static void CaptureLeftHandDesktopWeaponDepth8()
{
    if (!g_device8)
        return;
    IDirect3DSurface8* depth = nullptr;
    if (FAILED(D3D8Slot<PFN8_GetDepthStencilSurface>(g_device8, 33)(
            g_device8, &depth)) || !depth)
    {
        return;
    }
    ReleaseD3D8Surface(g_leftHandDesktopWeaponDepth8);
    g_leftHandDesktopWeaponDepth8 = depth;
    g_leftHandDesktopWeaponDepthPresent8 =
        InterlockedCompareExchange(&c_present8, 0, 0);
}

static void UpdateLeftHandSceneLighting8()
{
    if (!g_device8)
        return;

    // Reading a D3D8 render target synchronizes the GPU. Keep the surface
    // deliberately tiny, but update often enough that entering a differently
    // lit room or switching the flashlight does not lag behind the player.
    static LONG lastSampleFrame = -1000;
    const LONG presentFrame = InterlockedCompareExchange(&c_present8, 0, 0);
    if (presentFrame - lastSampleFrame < 5)
        return;
    lastSampleFrame = presentFrame;

    IDirect3DSurface8* backBuffer = nullptr;
    if (FAILED(D3D8Slot<PFN8_GetBackBuffer>(g_device8, 16)(g_device8, 0,
            0, &backBuffer)) || !backBuffer)
    {
        return;
    }

    D3D8SurfaceDescLocal description = {};
    if (FAILED(D3D8Slot<PFNS8_GetDesc>(backBuffer, 8)(backBuffer,
            &description)) || description.Width < 64u ||
        description.Height < 64u)
    {
        ReleaseD3D8Surface(backBuffer);
        return;
    }

    if (!g_leftHandLightSampleSurface8)
    {
        const HRESULT createResult = D3D8Slot<PFN8_CreateImageSurface>(
            g_device8, VT8_CreateImageSurface)(g_device8, 64u, 64u,
            description.Format, &g_leftHandLightSampleSurface8);
        if (FAILED(createResult) || !g_leftHandLightSampleSurface8)
        {
            ReleaseD3D8Surface(backBuffer);
            return;
        }
    }

    RECT sourceRectangles[4] = {};
    POINT destinationPoints[4] = {
        { 0, 0 }, { 32, 0 }, { 0, 32 }, { 32, 32 }
    };
    const LONG centersX[2] = {
        static_cast<LONG>(description.Width / 4u),
        static_cast<LONG>(description.Width * 3u / 4u)
    };
    const LONG centersY[2] = {
        static_cast<LONG>(description.Height / 4u),
        static_cast<LONG>(description.Height * 3u / 4u)
    };
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            RECT& rectangle = sourceRectangles[y * 2 + x];
            rectangle.left = centersX[x] - 16;
            rectangle.top = centersY[y] - 16;
            rectangle.right = rectangle.left + 32;
            rectangle.bottom = rectangle.top + 32;
        }
    }
    // The fourth tile follows the middle of the image instead of another
    // distant quadrant. In first person this is where SH3's flashlight cone
    // is concentrated, so its contribution reaches the tracked hand.
    sourceRectangles[3].left = static_cast<LONG>(description.Width / 2u) - 16;
    sourceRectangles[3].top = static_cast<LONG>(description.Height / 2u) - 16;
    sourceRectangles[3].right = sourceRectangles[3].left + 32;
    sourceRectangles[3].bottom = sourceRectangles[3].top + 32;

    const HRESULT copyResult = D3D8Slot<PFN8_CopyRects>(g_device8,
        VT8_CopyRects)(g_device8, backBuffer, sourceRectangles, 4u,
        g_leftHandLightSampleSurface8, destinationPoints);
    ReleaseD3D8Surface(backBuffer);
    if (FAILED(copyResult))
        return;

    D3DLOCKED_RECT locked = {};
    constexpr DWORD kD3dLockReadOnly = 0x10u;
    if (FAILED(D3D8Slot<PFNS8_LockRect>(g_leftHandLightSampleSurface8, 9)(
            g_leftHandLightSampleSurface8, &locked, nullptr,
            kD3dLockReadOnly)) || !locked.pBits)
    {
        return;
    }

    double regionTotals[4][3] = {};
    for (UINT y = 0; y < 64u; ++y)
    {
        const std::uint32_t* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(locked.pBits) +
            static_cast<std::size_t>(y) * locked.Pitch);
        for (UINT x = 0; x < 64u; ++x)
        {
            const std::uint32_t pixel = row[x];
            const UINT region = (y >= 32u ? 2u : 0u) +
                (x >= 32u ? 1u : 0u);
            regionTotals[region][0] += static_cast<double>(
                (pixel >> 16u) & 0xFFu);
            regionTotals[region][1] += static_cast<double>(
                (pixel >> 8u) & 0xFFu);
            regionTotals[region][2] += static_cast<double>(pixel & 0xFFu);
        }
    }
    D3D8Slot<PFNS8_UnlockRect>(g_leftHandLightSampleSurface8, 10)(
        g_leftHandLightSampleSurface8);

    constexpr float inverseRegionTotal = 1.0f / (32.0f * 32.0f * 255.0f);
    float globalAverage[3] = {};
    float centerAverage[3] = {};
    float average[3] = {};
    for (int channel = 0; channel < 3; ++channel)
    {
        globalAverage[channel] = static_cast<float>(
            regionTotals[0][channel] + regionTotals[1][channel] +
            regionTotals[2][channel]) * (inverseRegionTotal / 3.0f);
        centerAverage[channel] = static_cast<float>(
            regionTotals[3][channel]) * inverseRegionTotal;
        // Global illumination supplies the baseline. A brighter central
        // sample (normally the flashlight spot) raises it immediately.
        average[channel] = (std::max)(globalAverage[channel],
            centerAverage[channel] * 0.90f);
    }
    const float luminance = average[0] * 0.2126f +
        average[1] * 0.7152f + average[2] * 0.0722f;
    const float globalLuminance = globalAverage[0] * 0.2126f +
        globalAverage[1] * 0.7152f + globalAverage[2] * 0.0722f;
    const float centerLuminance = centerAverage[0] * 0.2126f +
        centerAverage[1] * 0.7152f + centerAverage[2] * 0.0722f;
    // The hand is drawn after the game scene, so a ray aimed directly at it
    // cannot be sampled from the backbuffer.  Use a clear central hotspot only
    // to discover that a save loaded with SH3's flashlight already on; the
    // input bridge then keeps that state while the ray moves off the wall and
    // onto the tracked hand.
    if (centerLuminance > 0.16f &&
        centerLuminance > globalLuminance * 1.32f + 0.025f)
    {
        InputBridge_ObserveFlashlightEnabled();
    }
    const float brightness = (std::min)(1.25f,
        (std::max)(0.0f, luminance * 2.4f));
    float targetColor[3] = {};
    for (int channel = 0; channel < 3; ++channel)
    {
        const float colorRatio = average[channel] /
            (std::max)(0.02f, luminance);
        const float tint = (std::min)(1.5f,
            (std::max)(0.5f, 0.75f + colorRatio * 0.25f));
        targetColor[channel] = (std::min)(1.25f, brightness * tint);
    }

    if (!g_leftHandSceneLightValid8)
    {
        for (int channel = 0; channel < 3; ++channel)
            g_leftHandSceneLightColor8[channel] = targetColor[channel];
    }
    else
    {
        for (int channel = 0; channel < 3; ++channel)
            g_leftHandSceneLightColor8[channel] =
                g_leftHandSceneLightColor8[channel] * 0.20f +
                targetColor[channel] * 0.80f;
    }
    g_leftHandSceneLightScale8 = brightness;
    g_leftHandSceneLightValid8 = true;
}

static bool LoadLeftHandTexture8(IDirect3DDevice8* device,
    const std::wstring& path, IDirect3DBaseTexture8** output)
{
    if (!device || path.empty() || !output)
        return false;
    *output = nullptr;

    const HRESULT initializeResult = CoInitializeEx(nullptr,
        COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IDirect3DTexture8* texture = nullptr;

    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result))
        result = factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result))
        result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result))
        result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result))
        result = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result))
        result = converter->GetSize(&width, &height);
    if (SUCCEEDED(result) && (width == 0 || height == 0 || width > 4096 ||
        height > 4096))
    {
        result = E_INVALIDARG;
    }

    constexpr DWORD kD3dFormatA8R8G8B8 = 21u;
    constexpr DWORD kD3dPoolManaged = 1u;
    if (SUCCEEDED(result))
    {
        result = D3D8Slot<PFN8_CreateTexture>(device, 20)(device, width,
            height, 1, 0, kD3dFormatA8R8G8B8, kD3dPoolManaged, &texture);
    }

    D3DLOCKED_RECT locked = {};
    if (SUCCEEDED(result))
        result = D3D8Slot<PFNT8_LockRect>(texture, 16)(texture, 0, &locked,
            nullptr, 0);
    if (SUCCEEDED(result))
    {
        const UINT sourceStride = width * 4u;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(sourceStride) * height);
        result = converter->CopyPixels(nullptr, sourceStride,
            static_cast<UINT>(pixels.size()), pixels.data());
        if (SUCCEEDED(result))
        {
            for (UINT row = 0; row < height; ++row)
            {
                std::memcpy(static_cast<std::uint8_t*>(locked.pBits) +
                    static_cast<std::size_t>(row) * locked.Pitch,
                    pixels.data() + static_cast<std::size_t>(row) *
                    sourceStride, sourceStride);
            }
        }
        D3D8Slot<PFNT8_UnlockRect>(texture, 17)(texture, 0);
    }

    if (converter)
        converter->Release();
    if (frame)
        frame->Release();
    if (decoder)
        decoder->Release();
    if (factory)
        factory->Release();
    if (SUCCEEDED(initializeResult))
        CoUninitialize();

    if (FAILED(result) || !texture)
    {
        if (texture)
            D3D8Slot<PFNT8_Release>(texture, 2)(
                reinterpret_cast<IDirect3DBaseTexture8*>(texture));
        return false;
    }
    *output = reinterpret_cast<IDirect3DBaseTexture8*>(texture);
    return true;
}

static bool EnsureLeftHandResources8(IDirect3DDevice8* device)
{
    if (!g_leftHandPoseProfile8.enabled || !device)
        return false;
    if (g_leftHandResourcesLoaded8 && g_leftHandResourceDevice8 == device)
        return true;
    if (g_leftHandResourceDevice8 && g_leftHandResourceDevice8 != device)
        ReleaseLeftHandResources8();
    if (g_leftHandResourcesAttempted8)
        return false;
    g_leftHandResourcesAttempted8 = true;

    std::vector<std::uint8_t> bytes;
    const std::wstring meshPath = GetModuleSiblingPath8(
        L"sh3vr_assets\\sh3vr_lefthand.mesh");
    if (!ReadWholeFile8(meshPath, &bytes) || bytes.size() < 16 ||
        std::memcmp(bytes.data(), "SH3LH01\0", 8) != 0)
    {
        if (!g_leftHandResourceFailureLogged8)
        {
            g_leftHandResourceFailureLogged8 = true;
            Log("LeftHand: sh3vr_assets\\sh3vr_lefthand.mesh is missing or invalid");
        }
        return false;
    }

    std::size_t cursor = 8;
    auto readU32 = [&bytes, &cursor](std::uint32_t* value) -> bool
    {
        if (!value || cursor + sizeof(*value) > bytes.size())
            return false;
        std::memcpy(value, bytes.data() + cursor, sizeof(*value));
        cursor += sizeof(*value);
        return true;
    };
    std::uint32_t version = 0;
    std::uint32_t partCount = 0;
    if (!readU32(&version) || !readU32(&partCount) || version != 1 ||
        partCount == 0 || partCount > 8)
    {
        return false;
    }

    std::vector<LeftHandMeshPart8> parts;
    parts.reserve(partCount);
    for (std::uint32_t partIndex = 0; partIndex < partCount; ++partIndex)
    {
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t materialIndex = 0;
        if (!readU32(&vertexCount) || !readU32(&indexCount) ||
            !readU32(&materialIndex) || vertexCount == 0 ||
            vertexCount > 65535 || indexCount == 0 || indexCount > 196605 ||
            indexCount % 3 != 0 || materialIndex >= 2)
        {
            return false;
        }
        const std::size_t vertexBytes = static_cast<std::size_t>(vertexCount) *
            sizeof(LeftHandDiskVertex8);
        const std::size_t indexBytes = static_cast<std::size_t>(indexCount) *
            sizeof(std::uint16_t);
        if (cursor + vertexBytes + indexBytes > bytes.size())
            return false;

        LeftHandMeshPart8 part = {};
        part.vertices.resize(vertexCount);
        part.indices.resize(indexCount);
        part.materialIndex = materialIndex;
        const LeftHandDiskVertex8* diskVertices =
            reinterpret_cast<const LeftHandDiskVertex8*>(bytes.data() + cursor);
        for (std::uint32_t vertexIndex = 0; vertexIndex < vertexCount;
            ++vertexIndex)
        {
            std::memcpy(part.vertices[vertexIndex].position,
                diskVertices[vertexIndex].position,
                sizeof(part.vertices[vertexIndex].position));
            std::memcpy(part.vertices[vertexIndex].normal,
                diskVertices[vertexIndex].normal,
                sizeof(part.vertices[vertexIndex].normal));
            part.vertices[vertexIndex].diffuse = 0xFFFFFFFFu;
            std::memcpy(part.vertices[vertexIndex].texcoord,
                diskVertices[vertexIndex].texcoord,
                sizeof(part.vertices[vertexIndex].texcoord));
        }
        cursor += vertexBytes;
        std::memcpy(part.indices.data(), bytes.data() + cursor, indexBytes);
        cursor += indexBytes;
        parts.push_back(std::move(part));
    }

    // Do not trust exported vertex normals here. The source GLB contains
    // split normal islands around the fingers, back of the hand and watch;
    // rebuild a continuous geometric field after loading the complete mesh.
    RebuildLeftHandGeometryNormals8(parts);

    IDirect3DBaseTexture8* textures[2] = {};
    const bool firstTexture = LoadLeftHandTexture8(device,
        GetModuleSiblingPath8(L"sh3vr_assets\\sh3vr_lefthand_0.png"),
        &textures[0]);
    const bool secondTexture = firstTexture && LoadLeftHandTexture8(device,
        GetModuleSiblingPath8(L"sh3vr_assets\\sh3vr_lefthand_1.png"),
        &textures[1]);
    if (!firstTexture || !secondTexture)
    {
        for (IDirect3DBaseTexture8* texture : textures)
            if (texture)
                D3D8Slot<PFNT8_Release>(texture, 2)(texture);
        if (!g_leftHandResourceFailureLogged8)
        {
            g_leftHandResourceFailureLogged8 = true;
            Log("LeftHand: one or more PNG textures could not be loaded");
        }
        return false;
    }

    g_leftHandMeshParts8 = std::move(parts);
    g_leftHandTextures8[0] = textures[0];
    g_leftHandTextures8[1] = textures[1];
    g_leftHandResourceDevice8 = device;
    g_leftHandResourcesLoaded8 = true;
    Log("LeftHand: loaded %u mesh parts and 2 textures",
        static_cast<unsigned>(g_leftHandMeshParts8.size()));
    return true;
}

static void BuildLeftHandWorldMatrix8(const float orientation[4],
    const float position[3], D3DMATRIX* world)
{
    if (!orientation || !position || !world)
        return;
    float handRotation[3][3] = {};
    BuildQuaternionRotation3x3(orientation, handRotation);

    const float yawCos = std::cos(g_leftHandPoseProfile8.yawRadians);
    const float yawSin = std::sin(g_leftHandPoseProfile8.yawRadians);
    const float pitchCos = std::cos(g_leftHandPoseProfile8.pitchRadians);
    const float pitchSin = std::sin(g_leftHandPoseProfile8.pitchRadians);
    const float rollCos = std::cos(g_leftHandPoseProfile8.rollRadians);
    const float rollSin = std::sin(g_leftHandPoseProfile8.rollRadians);
    const float rollYaw[3][3] = {
        { rollCos * yawCos, -rollSin, rollCos * yawSin },
        { rollSin * yawCos, rollCos, rollSin * yawSin },
        { -yawSin, 0.0f, yawCos }
    };
    const float calibration[3][3] = {
        { rollYaw[0][0],
            rollYaw[0][1] * pitchCos + rollYaw[0][2] * pitchSin,
            -rollYaw[0][1] * pitchSin + rollYaw[0][2] * pitchCos },
        { rollYaw[1][0],
            rollYaw[1][1] * pitchCos + rollYaw[1][2] * pitchSin,
            -rollYaw[1][1] * pitchSin + rollYaw[1][2] * pitchCos },
        { rollYaw[2][0],
            rollYaw[2][1] * pitchCos + rollYaw[2][2] * pitchSin,
            -rollYaw[2][1] * pitchSin + rollYaw[2][2] * pitchCos }
    };
    float calibrated[3][3] = {};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            for (int k = 0; k < 3; ++k)
                calibrated[row][column] +=
                    handRotation[row][k] * calibration[k][column];

    *world = {};
    const float scale = g_worldScale8 * g_leftHandPoseProfile8.scale;
    // D3D8 fixed-function transforms use row vectors. The controller and
    // calibration matrices above use column vectors, hence the transpose.
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            world->m[row][column] = calibrated[column][row] * scale;
    world->m[3][0] = position[0];
    world->m[3][1] = position[1];
    world->m[3][2] = position[2];
    world->m[3][3] = 1.0f;

    float modelBasis[3][3] = {};
    bool validBasis = true;
    for (int row = 0; row < 3; ++row)
    {
        const float length = std::sqrt(
            world->m[row][0] * world->m[row][0] +
            world->m[row][1] * world->m[row][1] +
            world->m[row][2] * world->m[row][2]);
        if (!std::isfinite(length) || length < 0.0001f)
        {
            validBasis = false;
            break;
        }
        for (int column = 0; column < 3; ++column)
            modelBasis[row][column] = world->m[row][column] / length;
    }
    if (validBasis)
    {
        constexpr float radiansToDegrees = 57.29577951308232f;
        const float modelPitch = std::atan2(modelBasis[2][1],
            modelBasis[2][2]) * radiansToDegrees;
        const float modelYaw = std::asin((std::max)(-1.0f,
            (std::min)(1.0f, -modelBasis[2][0]))) * radiansToDegrees;
        const float modelRoll = std::atan2(modelBasis[1][0],
            modelBasis[0][0]) * radiansToDegrees;
        Interop8_SetLeftHandDebugOrientation(true, modelPitch, modelYaw,
            modelRoll);
    }
}

struct ControllerOrientationOverlayVertex8
{
    float x;
    float y;
    float z;
    float rhw;
    DWORD diffuse;
};

static bool DrawControllerOrientationOverlayRect8(float left, float top,
    float right, float bottom, DWORD color)
{
    const ControllerOrientationOverlayVertex8 vertices[4] = {
        { left, top, 0.0f, 1.0f, color },
        { right, top, 0.0f, 1.0f, color },
        { left, bottom, 0.0f, 1.0f, color },
        { right, bottom, 0.0f, 1.0f, color }
    };
    return SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8, 5u, 2u, vertices,
        sizeof(ControllerOrientationOverlayVertex8)));
}

static bool ReadRightControllerEulerDegrees8(float* pitch, float* yaw,
    float* roll)
{
    if (!pitch || !yaw || !roll)
        return false;

    float orientation[4] = {};
    float position[3] = {};
    if (!ReadRightHandRelativePoseForWeapon8(orientation, position))
        return false;

    const float x = orientation[0];
    const float y = orientation[1];
    const float z = orientation[2];
    const float w = orientation[3];
    // These are the same head-relative controller axes used by the weapon
    // attachment: pitch about X, yaw about Y, roll about Z.
    const float pitchRadians = std::atan2(2.0f * (w * x + y * z),
        1.0f - 2.0f * (x * x + y * y));
    const float yawArgument = (std::max)(-1.0f, (std::min)(1.0f,
        2.0f * (w * y - z * x)));
    const float yawRadians = std::asin(yawArgument);
    const float rollRadians = std::atan2(2.0f * (w * z + x * y),
        1.0f - 2.0f * (y * y + z * z));
    constexpr float radiansToDegrees = 57.29577951308232f;
    *pitch = pitchRadians * radiansToDegrees;
    *yaw = yawRadians * radiansToDegrees;
    *roll = rollRadians * radiansToDegrees;
    return std::isfinite(*pitch) && std::isfinite(*yaw) &&
        std::isfinite(*roll);
}

static void DrawControllerOrientationOverlay8()
{
    if (!g_enableControllerOrientationOverlay8 || !g_device8)
        return;

    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    if (!ReadRightControllerEulerDegrees8(&pitch, &yaw, &roll))
        return;

    D3D8ViewportLocal viewport = {};
    if (FAILED(D3D8Slot<PFN8_GetViewport>(g_device8, 41)(g_device8,
        &viewport)) || viewport.Width < 160u || viewport.Height < 160u)
    {
        return;
    }

    const auto setRenderState = D3D8Slot<PFN8_SetRenderState>(g_device8,
        VT8_SetRenderState);
    const auto setTextureStage = D3D8Slot<PFN8_SetTextureStageState>(
        g_device8, VT8_SetTextureStageState);
    const auto setTexture = o_D3D8_SetTexture ? o_D3D8_SetTexture
        : D3D8Slot<PFN_D3D8_SetTexture>(g_device8, VT8_SetTexture);
    const auto setVertexShader = o_D3D8_SetVertexShader
        ? o_D3D8_SetVertexShader
        : D3D8Slot<PFN_D3D8_SetVertexShader>(g_device8,
            VT8_SetVertexShader);
    if (!setRenderState || !setTextureStage || !setTexture ||
        !setVertexShader || FAILED(setVertexShader(g_device8, 0x44u)))
    {
        return;
    }

    // A small fixed-function, pre-transformed overlay is copied to both eye
    // targets at identical screen coordinates. It intentionally has no depth
    // parallax: diagnostic text remains razor-sharp in the headset.
    setTexture(g_device8, 0, nullptr);
    setRenderState(g_device8, 7u, FALSE);   // ZENABLE
    setRenderState(g_device8, 14u, FALSE);  // ZWRITEENABLE
    setRenderState(g_device8, 27u, FALSE);  // ALPHABLENDENABLE
    setRenderState(g_device8, 22u, 1u);     // CULLMODE = NONE
    setTextureStage(g_device8, 0, 1u, 2u);  // COLOROP = SELECTARG1
    setTextureStage(g_device8, 0, 2u, 0u);  // COLORARG1 = DIFFUSE
    setTextureStage(g_device8, 0, 4u, 2u);  // ALPHAOP = SELECTARG1
    setTextureStage(g_device8, 0, 5u, 0u);  // ALPHAARG1 = DIFFUSE
    setTextureStage(g_device8, 1, 1u, 1u);  // COLOROP = DISABLE
    setTextureStage(g_device8, 1, 4u, 1u);  // ALPHAOP = DISABLE

    constexpr float x = 22.0f;
    constexpr float y = 22.0f;
    constexpr float segmentLength = 15.0f;
    constexpr float segmentThickness = 3.0f;
    constexpr float glyphWidth = 18.0f;
    constexpr float glyphHeight = 30.0f;
    constexpr float glyphSpacing = 5.0f;
    constexpr float rowSpacing = 11.0f;
    constexpr DWORD background = 0xFF101010u;
    constexpr DWORD pitchColor = 0xFFFFD060u;
    constexpr DWORD yawColor = 0xFF68D8FFu;
    constexpr DWORD rollColor = 0xFF8DFF88u;
    DrawControllerOrientationOverlayRect8(x - 10.0f, y - 10.0f,
        x + 138.0f, y + 3.0f * (glyphHeight + rowSpacing) + 4.0f,
        background);

    auto drawSegments = [=](float glyphX, float glyphY, unsigned mask,
        DWORD color)
    {
        const float h = segmentLength;
        const float t = segmentThickness;
        // a, b, c, d, e, f, g in the normal seven-segment layout.
        if (mask & 0x01u) DrawControllerOrientationOverlayRect8(glyphX + t,
            glyphY, glyphX + h, glyphY + t, color);
        if (mask & 0x02u) DrawControllerOrientationOverlayRect8(glyphX + h,
            glyphY + t, glyphX + h + t, glyphY + h, color);
        if (mask & 0x04u) DrawControllerOrientationOverlayRect8(glyphX + h,
            glyphY + h + t, glyphX + h + t, glyphY + 2.0f * h + t, color);
        if (mask & 0x08u) DrawControllerOrientationOverlayRect8(glyphX + t,
            glyphY + 2.0f * h + t, glyphX + h, glyphY + 2.0f * h + 2.0f * t,
            color);
        if (mask & 0x10u) DrawControllerOrientationOverlayRect8(glyphX,
            glyphY + h + t, glyphX + t, glyphY + 2.0f * h + t, color);
        if (mask & 0x20u) DrawControllerOrientationOverlayRect8(glyphX,
            glyphY + t, glyphX + t, glyphY + h, color);
        if (mask & 0x40u) DrawControllerOrientationOverlayRect8(glyphX + t,
            glyphY + h, glyphX + h, glyphY + h + t, color);
    };
    static constexpr unsigned digitMasks[10] = {
        0x3Fu, 0x06u, 0x5Bu, 0x4Fu, 0x66u,
        0x6Du, 0x7Du, 0x07u, 0x7Fu, 0x6Fu
    };
    auto drawValue = [&](float glyphX, float glyphY,
        float value, unsigned labelMask, DWORD color)
    {
        drawSegments(glyphX, glyphY, labelMask, color);
        int whole = std::clamp(static_cast<int>(std::lround(value)), -999, 999);
        float cursor = glyphX + glyphWidth + glyphSpacing;
        if (whole < 0)
        {
            drawSegments(cursor, glyphY, 0x40u, color);
            cursor += glyphWidth + glyphSpacing;
            whole = -whole;
        }
        int divisor = whole >= 100 ? 100 : (whole >= 10 ? 10 : 1);
        do
        {
            const int digit = whole / divisor;
            drawSegments(cursor, glyphY, digitMasks[digit], color);
            cursor += glyphWidth + glyphSpacing;
            whole %= divisor;
            divisor /= 10;
        } while (divisor > 0);
    };

    // P = a,b,e,f,g.  Y and R use the closest readable seven-segment forms.
    drawValue(x, y, pitch, 0x73u, pitchColor);
    drawValue(x, y + glyphHeight + rowSpacing, yaw, 0x6Eu, yawColor);
    drawValue(x, y + 2.0f * (glyphHeight + rowSpacing), roll, 0x77u,
        rollColor);
}

static bool DrawLeftHandEye8(std::uint32_t eye, const D3DMATRIX& world)
{
    if (!g_device8 || eye > 2 || g_leftHandMeshParts8.empty())
        return false;

    D3DMATRIX view = {};
    view.m[0][0] = 1.0f;
    view.m[1][1] = 1.0f;
    view.m[2][2] = 1.0f;
    view.m[3][3] = 1.0f;
    if (eye < 2)
    {
        const float eyeSign = eye == 0 ? -1.0f : 1.0f;
        view.m[3][0] = -eyeSign * 0.5f * SH3VR_IPD_METERS * g_worldScale8;
    }

    D3DMATRIX projection = {};
    if (!BuildImmersiveProjection(g_lastProjection8, projection))
        return false;
    const UINT targetWidth = eye < 2
        ? g_stereoTargetWidth8 : g_gameRenderTargetWidth8;
    const UINT targetHeight = eye < 2
        ? g_stereoTargetHeight8 : g_gameRenderTargetHeight8;
    if (targetWidth != 0 && targetHeight != 0)
    {
        const float eyeAspect = static_cast<float>(targetWidth) /
            static_cast<float>(targetHeight);
        projection.m[0][0] = std::copysign(
            SH3VR_IMMERSIVE_VERTICAL_SCALE / eyeAspect,
            projection.m[0][0]);
    }

    if (FAILED(o_D3D8_SetTransform(g_device8, SH3VR_D3DTS_WORLD, &world)) ||
        FAILED(o_D3D8_SetTransform(g_device8, SH3VR_D3DTS_VIEW, &view)) ||
        FAILED(o_D3D8_SetTransform(g_device8, SH3VR_D3DTS_PROJECTION,
            &projection)))
    {
        return false;
    }

    const auto setVertexShader = o_D3D8_SetVertexShader
        ? o_D3D8_SetVertexShader
        : D3D8Slot<PFN_D3D8_SetVertexShader>(g_device8,
            VT8_SetVertexShader);
    const auto setTexture = o_D3D8_SetTexture ? o_D3D8_SetTexture
        : D3D8Slot<PFN_D3D8_SetTexture>(g_device8, VT8_SetTexture);
    const auto draw = o_D3D8_DrawIndexedPrimitiveUP
        ? o_D3D8_DrawIndexedPrimitiveUP
        : D3D8Slot<PFN_D3D8_DrawIndexedPrimitiveUP>(g_device8,
            VT8_DrawIndexedPrimitiveUP);
    constexpr DWORD kLeftHandFvf = 0x00000144u; // XYZRHW | DIFFUSE | TEX1
    constexpr DWORD kTriangleList = 4u;
    constexpr DWORD kIndex16 = 101u;
    if (FAILED(setVertexShader(g_device8, kLeftHandFvf)))
        return false;

    bool complete = true;
    // The hand is composited after SH3 has finished its scene, so it cannot
    // receive the game's fixed-function lights directly. Modulate geometric
    // shading with the color and brightness sampled from that finished scene.
    constexpr float lightDirection[3] = { 0.25f, -0.35f, 0.90f };
    const bool flashlightActive = InputBridge_IsFlashlightEnabled();
    if (flashlightActive && !g_loggedLeftHandFlashlight8)
    {
        g_loggedLeftHandFlashlight8 = true;
        Log("LeftHand: direct controller-tracked SH3 flashlight lighting is active");
    }
    for (LeftHandMeshPart8& part : g_leftHandMeshParts8)
    {
        for (LeftHandVertex8& vertex : part.vertices)
        {
            float transformedNormal[3] = {
                vertex.normal[0] * world.m[0][0] +
                    vertex.normal[1] * world.m[1][0] +
                    vertex.normal[2] * world.m[2][0],
                vertex.normal[0] * world.m[0][1] +
                    vertex.normal[1] * world.m[1][1] +
                    vertex.normal[2] * world.m[2][1],
                vertex.normal[0] * world.m[0][2] +
                    vertex.normal[1] * world.m[1][2] +
                    vertex.normal[2] * world.m[2][2]
            };
            const float normalLength = std::sqrt(
                transformedNormal[0] * transformedNormal[0] +
                transformedNormal[1] * transformedNormal[1] +
                transformedNormal[2] * transformedNormal[2]);
            if (normalLength > 0.00001f)
            {
                transformedNormal[0] /= normalLength;
                transformedNormal[1] /= normalLength;
                transformedNormal[2] /= normalLength;
            }
            const float lightDot =
                transformedNormal[0] * lightDirection[0] +
                transformedNormal[1] * lightDirection[1] +
                transformedNormal[2] * lightDirection[2];
            const float facing = std::fabs(lightDot);
            const float shapeLight = 0.35f + 0.65f * facing;
            const float vertexPosition[3] = {
                vertex.position[0] * world.m[0][0] +
                    vertex.position[1] * world.m[1][0] +
                    vertex.position[2] * world.m[2][0] + world.m[3][0],
                vertex.position[0] * world.m[0][1] +
                    vertex.position[1] * world.m[1][1] +
                    vertex.position[2] * world.m[2][1] + world.m[3][1],
                vertex.position[0] * world.m[0][2] +
                    vertex.position[1] * world.m[1][2] +
                    vertex.position[2] * world.m[2][2] + world.m[3][2]
            };
            const float distance = std::sqrt(
                vertexPosition[0] * vertexPosition[0] +
                vertexPosition[1] * vertexPosition[1] +
                vertexPosition[2] * vertexPosition[2]);
            float flashlightContribution = 0.0f;
            if (flashlightActive && distance > 0.0001f)
            {
                const float inverseDistance = 1.0f / distance;
                const float toLight[3] = {
                    -vertexPosition[0] * inverseDistance,
                    -vertexPosition[1] * inverseDistance,
                    -vertexPosition[2] * inverseDistance
                };
                const float normalFacingLight = (std::max)(0.0f,
                    transformedNormal[0] * toLight[0] +
                    transformedNormal[1] * toLight[1] +
                    transformedNormal[2] * toLight[2]);
                const float forwardCosine = vertexPosition[2] *
                    inverseDistance;
                const float cone = (std::min)(1.0f, (std::max)(0.0f,
                    (forwardCosine - 0.35f) / 0.50f));
                const float distanceMeters = distance /
                    (std::max)(1.0f, g_worldScale8);
                const float attenuation = (std::min)(1.0f,
                    (std::max)(0.25f, 1.15f - distanceMeters * 0.65f));
                flashlightContribution = cone * attenuation *
                    (0.20f + 0.80f * normalFacingLight) * 0.95f;
            }
            constexpr float flashlightColor[3] = { 1.0f, 0.96f, 0.82f };
            constexpr float kMinimumHandVisibility = 0.15f;
            DWORD channels[3] = {};
            for (int channel = 0; channel < 3; ++channel)
            {
                const float value = (std::min)(1.0f, (std::max)(0.0f,
                    g_leftHandSceneLightColor8[channel] * shapeLight +
                    flashlightContribution * flashlightColor[channel] +
                    kMinimumHandVisibility));
                channels[channel] = static_cast<DWORD>(value * 255.0f +
                    0.5f);
            }
            vertex.diffuse = 0xFF000000u | (channels[0] << 16u) |
                (channels[1] << 8u) | channels[2];
        }

        std::vector<LeftHandClipVertex8> clipVertices(part.vertices.size());
        for (std::size_t vertexIndex = 0;
            vertexIndex < part.vertices.size(); ++vertexIndex)
        {
            const LeftHandVertex8& source = part.vertices[vertexIndex];
            const float x = source.position[0];
            const float y = source.position[1];
            const float z = source.position[2];
            const float worldX = x * world.m[0][0] + y * world.m[1][0] +
                z * world.m[2][0] + world.m[3][0];
            const float worldY = x * world.m[0][1] + y * world.m[1][1] +
                z * world.m[2][1] + world.m[3][1];
            const float worldZ = x * world.m[0][2] + y * world.m[1][2] +
                z * world.m[2][2] + world.m[3][2];
            const float viewX = worldX * view.m[0][0] +
                worldY * view.m[1][0] + worldZ * view.m[2][0] + view.m[3][0];
            const float viewY = worldX * view.m[0][1] +
                worldY * view.m[1][1] + worldZ * view.m[2][1] + view.m[3][1];
            const float viewZ = worldX * view.m[0][2] +
                worldY * view.m[1][2] + worldZ * view.m[2][2] + view.m[3][2];

            LeftHandClipVertex8& output = clipVertices[vertexIndex];
            output.clip[0] = viewX * projection.m[0][0] +
                viewY * projection.m[1][0] + viewZ * projection.m[2][0] +
                projection.m[3][0];
            output.clip[1] = viewX * projection.m[0][1] +
                viewY * projection.m[1][1] + viewZ * projection.m[2][1] +
                projection.m[3][1];
            output.clip[2] = viewX * projection.m[0][2] +
                viewY * projection.m[1][2] + viewZ * projection.m[2][2] +
                projection.m[3][2];
            output.clip[3] = viewX * projection.m[0][3] +
                viewY * projection.m[1][3] + viewZ * projection.m[2][3] +
                projection.m[3][3];
            output.color[0] = static_cast<float>((source.diffuse >> 16u) &
                0xFFu) / 255.0f;
            output.color[1] = static_cast<float>((source.diffuse >> 8u) &
                0xFFu) / 255.0f;
            output.color[2] = static_cast<float>(source.diffuse & 0xFFu) /
                255.0f;
            output.texcoord[0] = source.texcoord[0];
            output.texcoord[1] = source.texcoord[1];
        }

        // Clip every source triangle just in front of the camera plane using
        // W, while retaining the game's original depth projection. Rebuilding
        // the projection with a millimetre near plane pushes hand depth close
        // to 1.0 and lets the entire world occlude it in native eye targets.
        // This creates intersection vertices instead of moving the original
        // ones, so a hand crossing the camera plane remains watertight.
        std::vector<LeftHandScreenVertex8> screenVertices;
        std::vector<std::uint16_t> screenIndices;
        screenVertices.reserve(part.indices.size() * 2u);
        screenIndices.reserve(part.indices.size() * 2u);
        const auto interpolateClipVertex = [](const LeftHandClipVertex8& a,
            const LeftHandClipVertex8& b, float amount)
        {
            LeftHandClipVertex8 result = {};
            for (int component = 0; component < 4; ++component)
                result.clip[component] = a.clip[component] +
                    (b.clip[component] - a.clip[component]) * amount;
            for (int component = 0; component < 3; ++component)
                result.color[component] = a.color[component] +
                    (b.color[component] - a.color[component]) * amount;
            for (int component = 0; component < 2; ++component)
                result.texcoord[component] = a.texcoord[component] +
                    (b.texcoord[component] - a.texcoord[component]) * amount;
            return result;
        };
        const auto emitScreenVertex = [&](const LeftHandClipVertex8& input)
        {
            const float safeW = (std::max)(0.000001f, input.clip[3]);
            const float invW = 1.0f / safeW;
            LeftHandScreenVertex8 output = {};
            output.positionRhw[0] = (input.clip[0] * invW + 1.0f) *
                0.5f * static_cast<float>(targetWidth);
            output.positionRhw[1] = (1.0f - input.clip[1] * invW) *
                0.5f * static_cast<float>(targetHeight);
            const float projectedDepth = input.clip[2] * invW;
            // The game's projection produces negative Z between the eye and
            // its regular near plane. Clamping that whole interval to zero
            // made the palm, back of the hand and weapon coplanar in depth:
            // whichever triangle happened to draw last became visible. Keep
            // a narrow viewmodel depth band for this interval instead, while
            // preserving the normal scene projection above the near plane.
            const float projectionM22 = projection.m[2][2];
            const float projectionM23 = projection.m[2][3];
            const float gameNearW = std::fabs(projectionM22) > 0.000001f
                ? std::fabs((-projection.m[3][2] / projectionM22) *
                    projectionM23)
                : 0.01f;
            constexpr float kViewmodelDepthBand = 0.002f;
            // Weapon vertices inside SH3's normal near plane settle at the
            // front edge of D3D's depth range. Never let the later hand pass
            // also reach exactly zero: equality there made the hand overwrite
            // the weapon solely because it was submitted last.
            constexpr float kNearHandDepthFloor = 0.00075f;
            if (projectedDepth < 0.0f && gameNearW > 0.000051f)
            {
                const float closeDepth = (safeW - 0.00005f) /
                    (gameNearW - 0.00005f);
                output.positionRhw[2] = kNearHandDepthFloor +
                    (kViewmodelDepthBand - kNearHandDepthFloor) *
                    (std::min)(1.0f, (std::max)(0.0f, closeDepth));
            }
            else
            {
                const float sceneDepth = (std::min)(1.0f,
                    (std::max)(0.0f, projectedDepth));
                output.positionRhw[2] = kViewmodelDepthBand +
                    (1.0f - kViewmodelDepthBand) * sceneDepth;
            }
            output.positionRhw[3] = invW;
            DWORD channels[3] = {};
            for (int channel = 0; channel < 3; ++channel)
                channels[channel] = static_cast<DWORD>((std::min)(1.0f,
                    (std::max)(0.0f, input.color[channel])) * 255.0f +
                    0.5f);
            output.diffuse = 0xFF000000u | (channels[0] << 16u) |
                (channels[1] << 8u) | channels[2];
            output.texcoord[0] = input.texcoord[0];
            output.texcoord[1] = input.texcoord[1];
            screenVertices.push_back(output);
            screenIndices.push_back(static_cast<std::uint16_t>(
                screenVertices.size() - 1u));
        };
        for (std::size_t triangle = 0; triangle + 2u < part.indices.size();
            triangle += 3u)
        {
            LeftHandClipVertex8 inputPolygon[4] = {
                clipVertices[part.indices[triangle]],
                clipVertices[part.indices[triangle + 1u]],
                clipVertices[part.indices[triangle + 2u]],
                {}
            };
            LeftHandClipVertex8 clippedPolygon[4] = {};
            int clippedCount = 0;
            LeftHandClipVertex8 previous = inputPolygon[2];
            // Dedicated viewmodel camera plane. The scene's regular near
            // plane is far too coarse for an object intentionally brought
            // within centimetres of the eyes. Keep true tracked placement and
            // clip only geometry that has effectively crossed the eye itself.
            constexpr float kCameraPlaneW = 0.00005f;
            float previousDistance = previous.clip[3] - kCameraPlaneW;
            bool previousInside = previousDistance >= 0.0f;
            for (int vertex = 0; vertex < 3; ++vertex)
            {
                const LeftHandClipVertex8 current = inputPolygon[vertex];
                const float currentDistance = current.clip[3] -
                    kCameraPlaneW;
                const bool currentInside = currentDistance >= 0.0f;
                if (currentInside != previousInside)
                {
                    const float denominator = previousDistance -
                        currentDistance;
                    const float amount = std::fabs(denominator) > 0.0000001f
                        ? previousDistance / denominator : 0.0f;
                    clippedPolygon[clippedCount++] = interpolateClipVertex(
                        previous, current, amount);
                }
                if (currentInside)
                    clippedPolygon[clippedCount++] = current;
                previous = current;
                previousDistance = currentDistance;
                previousInside = currentInside;
            }
            for (int vertex = 1; vertex + 1 < clippedCount; ++vertex)
            {
                if (screenVertices.size() + 3u > 65535u)
                    break;
                emitScreenVertex(clippedPolygon[0]);
                emitScreenVertex(clippedPolygon[vertex]);
                emitScreenVertex(clippedPolygon[vertex + 1]);
            }
        }
        if (part.materialIndex >= _countof(g_leftHandTextures8) ||
            FAILED(setTexture(g_device8, 0,
                g_leftHandTextures8[part.materialIndex])) ||
            screenVertices.empty() ||
            FAILED(draw(g_device8, kTriangleList, 0,
                static_cast<UINT>(screenVertices.size()),
                static_cast<UINT>(screenIndices.size() / 3u),
                screenIndices.data(), kIndex16, screenVertices.data(),
                sizeof(LeftHandScreenVertex8))))
        {
            complete = false;
            break;
        }
    }
    return complete;
}

static bool RenderLeftHandStereo8()
{
    if (!g_leftHandPoseProfile8.enabled || !g_device8 || !g_haveProjection8 ||
        !EnsureLeftHandResources8(g_device8))
    {
        return false;
    }

    float orientation[4] = {};
    float position[3] = {};
    if (!ReadControllerRelativePoseForWeapon8(0, orientation, position))
    {
        if (!g_leftHandPoseFailureLogged8)
        {
            g_leftHandPoseFailureLogged8 = true;
            Log("LeftHand: left grip pose is not available yet");
        }
        return false;
    }
    float rotatedOffset[3] = {};
    RotateVectorByQuaternion(orientation, g_leftHandPoseProfile8.position,
        rotatedOffset);
    for (int axis = 0; axis < 3; ++axis)
        position[axis] += rotatedOffset[axis];

    D3DMATRIX world = {};
    BuildLeftHandWorldMatrix8(orientation, position, &world);

    // Sample the untouched desktop scene before switching to the native eye
    // targets and drawing the hand into them.
    UpdateLeftHandSceneLighting8();

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginExistingStereoPairPass(&originalColor, &originalDepth))
    {
        if (!g_leftHandStereoFailureLogged8)
        {
            g_leftHandStereoFailureLogged8 = true;
            Log("LeftHand: existing native stereo targets are not available at EndScene");
        }
        return false;
    }

    DWORD stateBlock = 0;
    const bool haveStateBlock = SUCCEEDED(
        D3D8Slot<PFN8_CreateStateBlock>(g_device8, VT8_CreateStateBlock)(
            g_device8, 1u, &stateBlock));
    bool leftComplete = false;
    bool rightComplete = false;
    bool desktopComplete = false;
    if (haveStateBlock)
    {
        const auto setRenderState = D3D8Slot<PFN8_SetRenderState>(g_device8,
            VT8_SetRenderState);
        const auto setTextureStage = D3D8Slot<PFN8_SetTextureStageState>(
            g_device8, VT8_SetTextureStageState);
        setRenderState(g_device8, 7u, TRUE);   // ZENABLE: hand self-depth
        setRenderState(g_device8, 14u, TRUE);  // ZWRITEENABLE
        setRenderState(g_device8, 15u, FALSE); // GLB alphaMode = OPAQUE
        setRenderState(g_device8, 19u, 5u);    // SRCBLEND = SRCALPHA
        setRenderState(g_device8, 20u, 6u);    // DESTBLEND = INVSRCALPHA
        // The authored fingers/watch mix thin and double-sided surfaces. Draw
        // both sides into a fresh view-model depth buffer so the nearest outer
        // surface hides the inside regardless of local winding.
        setRenderState(g_device8, 22u, 1u);    // CULLMODE = NONE
        // Strict LESS is important for the pre-transformed viewmodel. Equal
        // depth must not let a later back-facing triangle overwrite the
        // already visible outer surface.
        setRenderState(g_device8, 23u, 2u);    // ZFUNC = LESS
        setRenderState(g_device8, 24u, 8u);    // ALPHAREF
        setRenderState(g_device8, 25u, 5u);    // ALPHAFUNC = GREATER
        setRenderState(g_device8, 27u, FALSE); // GLB alphaMode = OPAQUE
        setRenderState(g_device8, 29u, FALSE); // SPECULARENABLE
        setRenderState(g_device8, 80u, FALSE); // FOGENABLE
        setRenderState(g_device8, 137u, FALSE); // CPU two-sided lighting

        setTextureStage(g_device8, 0, 1u, 4u); // COLOROP = MODULATE
        setTextureStage(g_device8, 0, 2u, 2u); // COLORARG1 = TEXTURE
        setTextureStage(g_device8, 0, 3u, 0u); // COLORARG2 = DIFFUSE
        setTextureStage(g_device8, 0, 4u, 2u); // ALPHAOP = SELECTARG1
        setTextureStage(g_device8, 0, 5u, 2u); // ALPHAARG1 = TEXTURE
        setTextureStage(g_device8, 0, 11u, 0u); // TEXCOORDINDEX = TEXCOORD0
        setTextureStage(g_device8, 0, 13u, 1u); // ADDRESSU = WRAP (glTF default)
        setTextureStage(g_device8, 0, 14u, 1u); // ADDRESSV = WRAP (glTF default)
        setTextureStage(g_device8, 0, 16u, 2u); // MAGFILTER = LINEAR
        setTextureStage(g_device8, 0, 17u, 2u); // MINFILTER = LINEAR
        setTextureStage(g_device8, 0, 18u, 0u); // MIPFILTER = NONE
        setTextureStage(g_device8, 0, 24u, 0u); // TEXTURETRANSFORMFLAGS = DISABLE
        setTextureStage(g_device8, 1, 1u, 1u); // COLOROP = DISABLE
        setTextureStage(g_device8, 1, 4u, 1u); // ALPHAOP = DISABLE

        constexpr DWORD kClearDepth = 0x00000002u;
        // Preserve native eye depth. The tracked hand now uses the same
        // projection as replayed weapon geometry, so depth testing decides
        // which model is actually closer to the player.
        leftComplete = DrawLeftHandEye8(0, world);
        if (SwitchStereoPairEye(1))
        {
            rightComplete = DrawLeftHandEye8(1, world);
        }

        // The native eye surfaces are not the desktop mirror. Draw a central
        // (zero-IPD) copy into the actual D3D8 backbuffer as the last scene
        // element so the monitor shows the same tracked hand.
        IDirect3DSurface8* desktopBackBuffer = nullptr;
        const HRESULT backBufferResult = D3D8Slot<PFN8_GetBackBuffer>(
            g_device8, 16)(g_device8, 0, 0, &desktopBackBuffer);
        if (SUCCEEDED(backBufferResult) && desktopBackBuffer)
        {
            const LONG presentFrame = InterlockedCompareExchange(
                &c_present8, 0, 0);
            IDirect3DSurface8* desktopDepth = nullptr;
            bool preserveWeaponDepth =
                g_leftHandDesktopWeaponDepthPresent8 == presentFrame &&
                IsLeftHandDesktopDepthCompatible8(desktopBackBuffer,
                    g_leftHandDesktopWeaponDepth8);
            if (preserveWeaponDepth)
            {
                desktopDepth = g_leftHandDesktopWeaponDepth8;
            }
            else if (EnsureLeftHandDesktopDepth8(desktopBackBuffer))
            {
                desktopDepth = g_leftHandDesktopDepth8;
            }
            HRESULT bindResult = desktopDepth
                ? D3D8Slot<PFN8_SetRenderTarget>(g_device8, 31)(g_device8,
                    desktopBackBuffer, desktopDepth)
                : E_FAIL;
            if (FAILED(bindResult) && preserveWeaponDepth &&
                EnsureLeftHandDesktopDepth8(desktopBackBuffer))
            {
                preserveWeaponDepth = false;
                desktopDepth = g_leftHandDesktopDepth8;
                bindResult = D3D8Slot<PFN8_SetRenderTarget>(g_device8, 31)(
                    g_device8, desktopBackBuffer, desktopDepth);
            }
            if (SUCCEEDED(bindResult))
            {
                setRenderState(g_device8, 7u, TRUE);
                setRenderState(g_device8, 14u, TRUE);
                setRenderState(g_device8, 22u, 1u);
                if (!preserveWeaponDepth)
                {
                    o_D3D8_Clear(g_device8, 0, nullptr, kClearDepth,
                        0, 1.0f, 0);
                }
                desktopComplete = DrawLeftHandEye8(2, world);
            }
            ReleaseD3D8Surface(desktopBackBuffer);
        }
    }
    else if (!g_leftHandStateFailureLogged8)
    {
        g_leftHandStateFailureLogged8 = true;
        Log("LeftHand: CreateStateBlock failed");
    }
    if (haveStateBlock)
    {
        D3D8Slot<PFN8_ApplyStateBlock>(g_device8, VT8_ApplyStateBlock)(
            g_device8, stateBlock);
        D3D8Slot<PFN8_DeleteStateBlock>(g_device8, VT8_DeleteStateBlock)(
            g_device8, stateBlock);
    }
    EndStereoPairPass(originalColor, originalDepth);

    if (desktopComplete && !g_leftHandDesktopRenderLogged8)
    {
        g_leftHandDesktopRenderLogged8 = true;
        Log("LeftHand: central tracked model is rendering in the desktop backbuffer");
    }
    else if (!desktopComplete && !g_leftHandDesktopFailureLogged8)
    {
        g_leftHandDesktopFailureLogged8 = true;
        Log("LeftHand: desktop backbuffer draw failed");
    }

    if (leftComplete && rightComplete)
    {
        InterlockedIncrement(&g_uiStereoOverlayDraws8);
        if (!g_leftHandRenderLogged8)
        {
            g_leftHandRenderLogged8 = true;
            Log("LeftHand: tracked model is rendering in both native eyes");
        }
        return true;
    }
    if (!g_leftHandDrawFailureLogged8)
    {
        g_leftHandDrawFailureLogged8 = true;
        Log("LeftHand: eye draw incomplete (left=%d right=%d)",
            leftComplete ? 1 : 0, rightComplete ? 1 : 0);
    }
    return false;
}

static bool RenderHeavyFullSceneStereo()
{
    if (!o_SH3_RenderComposite || !g_device8)
        return false;

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginStereoPairPass(&originalColor, &originalDepth))
        return false;

    const bool savedEyeOffset = g_applyStereoEyeOffset8;
    const std::uint32_t savedEye = g_renderEye8;
    bool leftComplete = false;
    bool rightComplete = false;

    const bool haveDynamicActorReplay =
        InterlockedCompareExchange(&g_perDrawStereoProbeDraws8, 0, 0) > 0;
    if (!haveDynamicActorReplay)
    {
        g_perDrawStereoTargetCleared8[0] = false;
        g_perDrawStereoTargetCleared8[1] = false;
        ClearStereoPairEyeIfNeeded(0);
    }
    g_renderEye8 = 0;
    g_applyStereoEyeOffset8 = true;
    g_fullScenePrimaryTargetBound8 = true;
    g_fullSceneStereoReplayActive8 = true;
    o_SH3_RenderComposite();
    g_fullSceneStereoReplayActive8 = false;
    leftComplete = true;

    if (SwitchStereoPairEye(1))
    {
        if (!haveDynamicActorReplay)
            ClearStereoPairEyeIfNeeded(1);
        g_renderEye8 = 1;
        g_applyStereoEyeOffset8 = true;
        g_fullScenePrimaryTargetBound8 = true;
        g_fullSceneStereoReplayActive8 = true;
        o_SH3_RenderComposite();
        g_fullSceneStereoReplayActive8 = false;
        rightComplete = true;
    }

    g_fullSceneStereoReplayActive8 = false;
    g_fullScenePrimaryTargetBound8 = false;
    g_applyStereoEyeOffset8 = savedEyeOffset;
    g_renderEye8 = savedEye;
    EndStereoPairPass(originalColor, originalDepth);

    if (leftComplete && rightComplete && !g_loggedHeavyFullSceneStereo8)
    {
        g_loggedHeavyFullSceneStereo8 = true;
        Log("Heavy-scene full stereo replay active: the prepared scene is "
            "rendered once per eye after preserving %d dynamic model draws",
            static_cast<int>(InterlockedCompareExchange(
                &g_perDrawStereoProbeDraws8, 0, 0)));
    }
    return leftComplete && rightComplete;
}

static bool CaptureStereoBackBufferEye(std::uint32_t eye)
{
    if (!g_device8)
        return false;

    IDirect3DSurface8* backBuffer = nullptr;
    const HRESULT result = D3D8Slot<PFN8_GetBackBuffer>(g_device8, 16)(
        g_device8, 0, 0, &backBuffer);
    if (FAILED(result) || !backBuffer)
        return false;

    const bool published = Interop8_CaptureStereoEye(
        g_device8, backBuffer, eye);
    ReleaseD3D8Surface(backBuffer);
    return published;
}

static void ProbeCurrentStereoSurface()
{
    if (!g_device8)
        return;

    IDirect3DSurface8* currentColor = nullptr;
    const HRESULT result = D3D8Slot<PFN8_GetRenderTarget>(g_device8, 32)(
        g_device8, &currentColor);
    if (SUCCEEDED(result) && currentColor)
    {
        Interop8_ProbeSurface(g_device8, currentColor,
            "current target after first composite");
    }
    ReleaseD3D8Surface(currentColor);
}

static void ProbeStereoBackBuffer(const char* label)
{
    if (!g_device8)
        return;

    IDirect3DSurface8* backBuffer = nullptr;
    const HRESULT result = D3D8Slot<PFN8_GetBackBuffer>(g_device8, 16)(
        g_device8, 0, 0, &backBuffer);
    if (SUCCEEDED(result) && backBuffer)
    {
        Interop8_ProbeSurface(g_device8, backBuffer, label);
    }
    ReleaseD3D8Surface(backBuffer);
}

static HRESULT WINAPI hk_D3D8_Clear(IDirect3DDevice8* device, DWORD count,
    const void* rectangles, DWORD flags, DWORD color, float depth,
    DWORD stencil)
{
    if (g_fullSceneStereoReplayActive8 &&
        g_fullScenePrimaryTargetBound8)
    {
        // Dynamic actors were already copied from the only composite call in
        // which SH3 exposes them. Keep their color/depth while the batched
        // replay fills static world geometry around them.
        const DWORD sceneClearMask = 0x00000001u | 0x00000002u | 0x00000004u;
        const DWORD remainingFlags = flags & ~sceneClearMask;
        if (remainingFlags == 0)
            return D3D_OK;
        return o_D3D8_Clear(device, count, rectangles, remainingFlags, color,
            depth, stencil);
    }

    if ((flags & 0x00000001u) != 0)
    {
        g_gameClearColor8 = color;
        g_haveGameClearColor8 = true;
        if (g_headOrientationReferenceValid8 && !g_loggedGameClearColor8)
        {
            g_loggedGameClearColor8 = true;
            Log("Gameplay render-target clear color captured: 0x%08X", color);
        }
    }
    return o_D3D8_Clear(device, count, rectangles, flags, color, depth,
        stencil);
}

static bool BeginPerDrawStereoProbe(std::uint32_t eye,
    IDirect3DSurface8** originalColor, IDirect3DSurface8** originalDepth)
{
    if (!BeginOffscreenDuplicatePass(eye, originalColor, originalDepth))
        return false;

    if (!g_perDrawStereoTargetCleared8[eye])
    {
        const DWORD clearTarget = 0x00000001u;
        const DWORD clearDepth = 0x00000002u;
        const DWORD clearStencil = 0x00000004u;
        const DWORD clearColor = g_haveGameClearColor8
            ? g_gameClearColor8 : 0x00000000u;
        const HRESULT result = o_D3D8_Clear(g_device8, 0, nullptr,
            clearTarget | clearDepth | clearStencil, clearColor, 1.0f, 0);
        if (FAILED(result))
        {
            Log("Per-draw stereo probe clear failed, hr=0x%08X",
                static_cast<unsigned>(result));
        }
        g_perDrawStereoTargetCleared8[eye] = true;
    }
    return true;
}

static void DuplicatePrimitiveForStereoProbe(DWORD primitiveType,
    UINT startVertex, UINT primitiveCount)
{
    if (!g_havePerDrawStereoMatrices8)
        return;
    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginStereoPairPass(&originalColor, &originalDepth))
        return;

    ClearStereoPairEyeIfNeeded(0);
    o_D3D8_SetVertexShaderConstant(g_device8, 2,
        g_perDrawCenterViewProjection8, 4);
    o_D3D8_DrawPrimitive(g_device8, primitiveType, startVertex, primitiveCount);

    if (SwitchStereoPairEye(1))
    {
        ClearStereoPairEyeIfNeeded(1);
        o_D3D8_SetVertexShaderConstant(g_device8, 2,
            g_perDrawRightViewProjection8, 4);
        o_D3D8_DrawPrimitive(g_device8, primitiveType, startVertex,
            primitiveCount);
    }
    o_D3D8_SetVertexShaderConstant(g_device8, 2,
        g_perDrawOriginalViewProjection8, 4);
    EndStereoPairPass(originalColor, originalDepth);
    InterlockedIncrement(&g_perDrawStereoProbeDraws8);
}

static void DuplicateIndexedPrimitiveForStereoProbe(DWORD primitiveType,
    UINT minIndex, UINT vertexCount, UINT startIndex, UINT primitiveCount)
{
    if (!g_havePerDrawStereoMatrices8)
        return;
    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginStereoPairPass(&originalColor, &originalDepth))
        return;

    ClearStereoPairEyeIfNeeded(0);
    o_D3D8_SetVertexShaderConstant(g_device8, 2,
        g_perDrawCenterViewProjection8, 4);
    o_D3D8_DrawIndexedPrimitive(g_device8, primitiveType, minIndex,
        vertexCount, startIndex, primitiveCount);

    if (SwitchStereoPairEye(1))
    {
        ClearStereoPairEyeIfNeeded(1);
        o_D3D8_SetVertexShaderConstant(g_device8, 2,
            g_perDrawRightViewProjection8, 4);
        o_D3D8_DrawIndexedPrimitive(g_device8, primitiveType, minIndex,
            vertexCount, startIndex, primitiveCount);
    }
    o_D3D8_SetVertexShaderConstant(g_device8, 2,
        g_perDrawOriginalViewProjection8, 4);
    EndStereoPairPass(originalColor, originalDepth);
    InterlockedIncrement(&g_perDrawStereoProbeDraws8);
}

static void DuplicatePrimitiveUPForStereoProbe(DWORD primitiveType,
    UINT primitiveCount, const void* vertices, UINT stride)
{
    if (!g_havePerDrawStereoMatrices8)
        return;
    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginStereoPairPass(&originalColor, &originalDepth))
        return;

    ClearStereoPairEyeIfNeeded(0);
    o_D3D8_SetVertexShaderConstant(g_device8, 2,
        g_perDrawCenterViewProjection8, 4);
    o_D3D8_DrawPrimitiveUP(g_device8, primitiveType, primitiveCount,
        vertices, stride);

    if (SwitchStereoPairEye(1))
    {
        ClearStereoPairEyeIfNeeded(1);
        o_D3D8_SetVertexShaderConstant(g_device8, 2,
            g_perDrawRightViewProjection8, 4);
        o_D3D8_DrawPrimitiveUP(g_device8, primitiveType, primitiveCount,
            vertices, stride);
    }
    o_D3D8_SetVertexShaderConstant(g_device8, 2,
        g_perDrawOriginalViewProjection8, 4);
    EndStereoPairPass(originalColor, originalDepth);
    InterlockedIncrement(&g_perDrawStereoProbeDraws8);
}

static void DuplicateIndexedPrimitiveUPForStereoProbe(DWORD primitiveType,
    UINT minIndex, UINT vertexCount, UINT primitiveCount, const void* indices,
    DWORD indexFormat, const void* vertices, UINT stride)
{
    if (!g_havePerDrawStereoMatrices8)
        return;
    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginStereoPairPass(&originalColor, &originalDepth))
        return;

    ClearStereoPairEyeIfNeeded(0);
    o_D3D8_SetVertexShaderConstant(g_device8, 2,
        g_perDrawCenterViewProjection8, 4);
    o_D3D8_DrawIndexedPrimitiveUP(g_device8, primitiveType, minIndex,
        vertexCount, primitiveCount, indices, indexFormat, vertices, stride);

    if (SwitchStereoPairEye(1))
    {
        ClearStereoPairEyeIfNeeded(1);
        o_D3D8_SetVertexShaderConstant(g_device8, 2,
            g_perDrawRightViewProjection8, 4);
        o_D3D8_DrawIndexedPrimitiveUP(g_device8, primitiveType, minIndex,
            vertexCount, primitiveCount, indices, indexFormat, vertices,
            stride);
    }
    o_D3D8_SetVertexShaderConstant(g_device8, 2,
        g_perDrawOriginalViewProjection8, 4);
    EndStereoPairPass(originalColor, originalDepth);
    InterlockedIncrement(&g_perDrawStereoProbeDraws8);
}

static bool IsUiPretransformedFvf(DWORD shader)
{
    // These are the fixed-function/pre-transformed vertex formats observed in
    // SH3's subtitle, notification, and HUD passes. World geometry uses
    // shader handles and is intentionally not treated as UI here.
    switch (shader)
    {
    case 0x00000004u: // XYZRHW
    case 0x00000044u: // XYZRHW | DIFFUSE
    case 0x00000104u: // XYZRHW | TEX1
    case 0x00000144u: // XYZRHW | DIFFUSE | TEX1
        return true;
    default:
        return false;
    }
}

static UINT UiVertexCountForPrimitive(DWORD primitiveType,
    UINT primitiveCount)
{
    switch (primitiveType)
    {
    case D3DPT_POINTLIST:
        return primitiveCount;
    case D3DPT_LINELIST:
        return primitiveCount * 2u;
    case D3DPT_LINESTRIP:
        return primitiveCount + 1u;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
        return primitiveCount + 2u;
    case D3DPT_TRIANGLELIST:
        return primitiveCount * 3u;
    default:
        return 0;
    }
}

static bool BuildUiOverlayVertices(const D3D8ViewportLocal& sourceViewport,
    const void* vertices, UINT vertexCount, UINT stride,
    std::uint32_t eye, std::vector<std::uint8_t>* transformed)
{
    if (!vertices || !transformed || vertexCount == 0 || stride < 16 ||
        stride > 1024 || sourceViewport.Width == 0 ||
        sourceViewport.Height == 0 || g_stereoTargetWidth8 == 0 ||
        g_stereoTargetHeight8 == 0 || eye > 1)
    {
        return false;
    }

    const std::size_t byteCount = static_cast<std::size_t>(vertexCount) *
        static_cast<std::size_t>(stride);
    if (byteCount > 16u * 1024u * 1024u)
        return false;

    transformed->resize(byteCount);
    std::memcpy(transformed->data(), vertices, byteCount);

    // Keep the original UI aspect ratio. The native Quest eye texture is
    // taller than the game's 16:9 desktop viewport, so the UI is letterboxed
    // into a centered rectangle instead of being vertically stretched.
    const float sourceWidth = static_cast<float>(sourceViewport.Width);
    const float sourceHeight = static_cast<float>(sourceViewport.Height);
    const float targetWidth = static_cast<float>(g_stereoTargetWidth8);
    const float targetHeight = static_cast<float>(g_stereoTargetHeight8);
    const float scale = (std::min)(targetWidth / sourceWidth,
        targetHeight / sourceHeight);
    const float left = (targetWidth - sourceWidth * scale) * 0.5f;
    const float top = (targetHeight - sourceHeight * scale) * 0.5f;

    // The Host samples each symmetric native eye texture through a different
    // asymmetric OpenXR FOV rectangle.  Identical raw pixel coordinates in
    // both eyes therefore do not describe the same final display ray.  Map a
    // common, aspect-preserved UI position through each eye's source rectangle
    // so the Host's later crop maps both copies back to exactly the same final
    // position (zero disparity).  This adjusts the existing subtitle pass; it
    // does not introduce an additional centered subtitle copy.
    float eyeRects[2][4] = {};
    if (!Interop8_ReadProjectionUvRects(eyeRects))
        return false;
    const float* rect = eyeRects[eye];
    const float rectWidth = rect[2] - rect[0];
    const float rectHeight = rect[3] - rect[1];
    if (!std::isfinite(rect[0]) || !std::isfinite(rect[1]) ||
        !std::isfinite(rect[2]) || !std::isfinite(rect[3]) ||
        rectWidth <= 0.01f || rectHeight <= 0.01f ||
        rect[0] < -0.25f || rect[1] < -0.25f ||
        rect[2] > 1.25f || rect[3] > 1.25f)
    {
        return false;
    }

    for (UINT index = 0; index < vertexCount; ++index)
    {
        std::uint8_t* vertex = transformed->data() +
            static_cast<std::size_t>(index) * stride;
        float x = 0.0f;
        float y = 0.0f;
        std::memcpy(&x, vertex + 0, sizeof(x));
        std::memcpy(&y, vertex + 4, sizeof(y));
        const float finalX =
            ((x - static_cast<float>(sourceViewport.X)) * scale + left) /
            targetWidth;
        const float finalY =
            ((y - static_cast<float>(sourceViewport.Y)) * scale + top) /
            targetHeight;
        // Monocular subtitle comfort layout requested for Quest 3.  Shrink
        // uniformly toward the bottom-right safe area, keeping even the
        // longest original lines completely inside the right-eye image.
        constexpr float monocularScale = 0.84f;
        constexpr float safeRightBottom = 0.98f;
        const float monocularX = safeRightBottom -
            (1.0f - finalX) * monocularScale;
        const float monocularY = safeRightBottom -
            (1.0f - finalY) * monocularScale;
        x = targetWidth * (rect[0] + monocularX * rectWidth);
        y = targetHeight * (rect[1] + monocularY * rectHeight);
        std::memcpy(vertex + 0, &x, sizeof(x));
        std::memcpy(vertex + 4, &y, sizeof(y));
    }
    return true;
}

static bool SetUiOverlayViewport()
{
    D3D8ViewportLocal viewport = {};
    viewport.Width = g_stereoTargetWidth8;
    viewport.Height = g_stereoTargetHeight8;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    return SUCCEEDED(D3D8Slot<PFN8_SetViewport>(g_device8, 40)(g_device8,
        &viewport));
}

static void LogUiStereoOverlayFailure8(const char* stage)
{
    if (g_loggedUiStereoOverlayFailure8)
        return;
    g_loggedUiStereoOverlayFailure8 = true;
    Log("Stereo UI overlay candidate was not replayed at stage: %s", stage);
}

static bool DuplicateUiPrimitiveUPForStereo(DWORD primitiveType,
    UINT primitiveCount, const void* vertices, UINT stride)
{
    const UINT vertexCount = UiVertexCountForPrimitive(primitiveType,
        primitiveCount);
    if (vertexCount == 0 || vertexCount > 65536u)
    {
        LogUiStereoOverlayFailure8("invalid UP vertex count");
        return false;
    }

    D3D8ViewportLocal sourceViewport = {};
    if (!g_device8 || FAILED(D3D8Slot<PFN8_GetViewport>(g_device8, 41)(
        g_device8, &sourceViewport)))
    {
        LogUiStereoOverlayFailure8("GetViewport for UP draw");
        return false;
    }

    std::vector<std::uint8_t> transformedRight;
    if (!BuildUiOverlayVertices(sourceViewport, vertices, vertexCount, stride,
            1, &transformedRight))
    {
        LogUiStereoOverlayFailure8("BuildUiOverlayVertices for UP draw");
        return false;
    }

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginUiStereoPairPass(&originalColor, &originalDepth))
    {
        LogUiStereoOverlayFailure8("BeginUiStereoPairPass for UP draw");
        return false;
    }

    // Deliberately do not draw subtitles into the left eye.  A monocular
    // subtitle cannot acquire contradictory binocular disparity.
    const bool leftComplete = true;
    bool rightComplete = false;
    if (SwitchStereoPairEye(1) && SetUiOverlayViewport())
    {
        rightComplete = SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8,
            primitiveType, primitiveCount, transformedRight.data(), stride));
    }
    EndStereoPairPass(originalColor, originalDepth);

    if (leftComplete && rightComplete)
    {
        InterlockedIncrement(&g_uiStereoOverlayDraws8);
        if (!g_loggedUiStereoOverlay8)
        {
            g_loggedUiStereoOverlay8 = true;
            Log("Stereo UI overlay enabled: FVF 0x%08X, source viewport "
                "%ux%u, native target %ux%u, right-eye-only scaled safe "
                "layout", g_currentVertexShader8, sourceViewport.Width,
                sourceViewport.Height, g_stereoTargetWidth8,
                g_stereoTargetHeight8);
        }
    }
    else
    {
        LogUiStereoOverlayFailure8("eye draw or eye switch for UP draw");
    }
    return leftComplete && rightComplete;
}

static bool DuplicateGamePostProcessPrimitiveUPForStereo(
    DWORD primitiveType, UINT primitiveCount, const void* vertices,
    UINT stride)
{
    const UINT vertexCount = UiVertexCountForPrimitive(primitiveType,
        primitiveCount);
    if (!vertices || vertexCount == 0 || vertexCount > 16u || stride < 16u ||
        stride > 256u || !g_device8)
    {
        return false;
    }

    D3D8ViewportLocal sourceViewport = {};
    if (FAILED(D3D8Slot<PFN8_GetViewport>(g_device8, 41)(g_device8,
        &sourceViewport)) || sourceViewport.Width == 0 ||
        sourceViewport.Height == 0 || g_stereoTargetWidth8 == 0 ||
        g_stereoTargetHeight8 == 0)
    {
        return false;
    }

    const std::size_t byteCount = static_cast<std::size_t>(vertexCount) *
        static_cast<std::size_t>(stride);
    std::vector<std::uint8_t> transformed(byteCount);
    std::memcpy(transformed.data(), vertices, byteCount);

    // A post-process quad must cover the complete native eye. Unlike HUD
    // geometry, it is intentionally stretched to the eye target aspect.
    const float scaleX = static_cast<float>(g_stereoTargetWidth8) /
        static_cast<float>(sourceViewport.Width);
    const float scaleY = static_cast<float>(g_stereoTargetHeight8) /
        static_cast<float>(sourceViewport.Height);
    for (UINT index = 0; index < vertexCount; ++index)
    {
        std::uint8_t* vertex = transformed.data() +
            static_cast<std::size_t>(index) * stride;
        float x = 0.0f;
        float y = 0.0f;
        std::memcpy(&x, vertex + 0, sizeof(x));
        std::memcpy(&y, vertex + 4, sizeof(y));
        x = (x - static_cast<float>(sourceViewport.X)) * scaleX;
        y = (y - static_cast<float>(sourceViewport.Y)) * scaleY;
        std::memcpy(vertex + 0, &x, sizeof(x));
        std::memcpy(vertex + 4, &y, sizeof(y));
    }

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginUiStereoPairPass(&originalColor, &originalDepth))
        return false;

    bool leftComplete = SetUiOverlayViewport() &&
        SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8, primitiveType,
            primitiveCount, transformed.data(), stride));
    bool rightComplete = false;
    if (SwitchStereoPairEye(1) && SetUiOverlayViewport())
    {
        rightComplete = SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8,
            primitiveType, primitiveCount, transformed.data(), stride));
    }
    EndStereoPairPass(originalColor, originalDepth);

    if (leftComplete && rightComplete)
    {
        InterlockedIncrement(&g_uiStereoOverlayDraws8);
        if (!g_loggedGamePostProcess8)
        {
            g_loggedGamePostProcess8 = true;
            Log("Game post-process stereo pass replayed: shader/FVF "
                "0x%08X, source viewport %ux%u, native target %ux%u",
                g_currentVertexShader8,
                sourceViewport.Width, sourceViewport.Height,
                g_stereoTargetWidth8, g_stereoTargetHeight8);
        }
    }
    return leftComplete && rightComplete;
}

static bool BuildFixedFunctionStereoTransforms8(bool eyeOffset,
    std::uint32_t eye, D3DMATRIX* view, D3DMATRIX* projection)
{
    if (!view || !projection)
        return false;

    const bool savedEyeOffset = g_applyStereoEyeOffset8;
    const std::uint32_t savedEye = g_renderEye8;
    g_applyStereoEyeOffset8 = eyeOffset;
    g_renderEye8 = eye;
    const bool haveView = ApplyHeadRotationToMainCamera(g_lastView8, *view);
    const bool haveProjection = BuildImmersiveProjection(g_lastProjection8,
        *projection);
    g_applyStereoEyeOffset8 = savedEyeOffset;
    g_renderEye8 = savedEye;
    return haveView && haveProjection;
}

struct FirearmReticleVertex8
{
    float x;
    float y;
    float z;
    DWORD diffuse;
};

static bool BuildWeaponGuideTargetLocal8(int profileIndex, float target[3])
{
    if (!target || profileIndex < 0 ||
        profileIndex >= SH3VR_WEAPON_PROFILE_COUNT8)
    {
        return false;
    }

    PollWeaponIniHotReload8();
    float gripOrientation[4] = {};
    float gripPosition[3] = {};
    if (!ReadRightHandRelativePoseForWeapon8(gripOrientation, gripPosition))
        return false;

    const WeaponPoseProfile8& profile = g_weaponPoseProfiles8[profileIndex];
    const float weaponWorldCompensation = g_worldScale8 /
        SH3VR_DEFAULT_WORLD_SCALE;
    const float calibratedOffset[3] = {
        profile.position[0] * weaponWorldCompensation,
        profile.position[1] * weaponWorldCompensation,
        profile.position[2] * weaponWorldCompensation
    };
    float rotatedOffset[3] = {};
    RotateVectorByQuaternion(gripOrientation, calibratedOffset,
        rotatedOffset);
    for (int axis = 0; axis < 3; ++axis)
        gripPosition[axis] += rotatedOffset[axis];

    float forward[3] = {};
    float distanceMeters = 0.0f;
    if (profileIndex >= 4)
    {
        float aimOrientation[4] = {};
        float unusedAimPosition[3] = {};
        if (!ReadRightHandRelativeAimPose8(aimOrientation,
            unusedAimPosition))
        {
            return false;
        }
        const float pitch = profile.aimPitchRadians;
        const float yaw = profile.aimYawRadians;
        const float calibratedForward[3] = {
            std::sin(yaw) * std::cos(pitch),
            -std::sin(pitch),
            std::cos(yaw) * std::cos(pitch)
        };
        RotateVectorByQuaternion(aimOrientation, calibratedForward, forward);
        distanceMeters = profileIndex == 4 ? 2.0f : 8.0f;
    }
    else
    {
        const float controllerForward[3] = { 0.0f, 0.0f, 1.0f };
        RotateVectorByQuaternion(gripOrientation, controllerForward, forward);
        distanceMeters = profileIndex == 0 ? 0.38f :
            (profileIndex == 1 ? 0.82f :
                (profileIndex == 2 ? 0.76f : 0.95f));
    }

    const float forwardLength = std::sqrt(forward[0] * forward[0] +
        forward[1] * forward[1] + forward[2] * forward[2]);
    if (!std::isfinite(forwardLength) || forwardLength < 0.001f)
        return false;
    for (int axis = 0; axis < 3; ++axis)
    {
        forward[axis] /= forwardLength;
        target[axis] = gripPosition[axis] + forward[axis] *
            distanceMeters * g_worldScale8;
    }
    return true;
}

static bool BuildWeaponGuideTransforms8(std::uint32_t eye, D3DMATRIX* view,
    D3DMATRIX* projection)
{
    if (!view || !projection || eye > 2 || !g_haveProjection8)
        return false;

    std::memset(view, 0, sizeof(*view));
    view->m[0][0] = 1.0f;
    view->m[1][1] = 1.0f;
    view->m[2][2] = 1.0f;
    view->m[3][3] = 1.0f;
    if (eye < 2)
    {
        const float eyeSign = eye == 0 ? -1.0f : 1.0f;
        view->m[3][0] = -eyeSign * 0.5f * SH3VR_IPD_METERS * g_worldScale8;
    }

    if (!BuildImmersiveProjection(g_lastProjection8, *projection))
        return false;
    const UINT width = eye < 2 ? g_stereoTargetWidth8 :
        g_gameRenderTargetWidth8;
    const UINT height = eye < 2 ? g_stereoTargetHeight8 :
        g_gameRenderTargetHeight8;
    if (width != 0 && height != 0)
    {
        const float aspect = static_cast<float>(width) /
            static_cast<float>(height);
        projection->m[0][0] = std::copysign(
            SH3VR_IMMERSIVE_VERTICAL_SCALE / aspect,
            projection->m[0][0]);
    }
    return true;
}

static bool RenderFirearmReticleStereo8()
{
    if (!g_device8 || !g_haveProjection8)
        return false;

    PollWeaponIniHotReload8();
    const int profileIndex = g_activeWeaponPoseProfile8;
    if (profileIndex < 0 || profileIndex >= SH3VR_WEAPON_PROFILE_COUNT8)
        return false;
    if (!g_weaponPoseProfiles8[profileIndex].showGuideDot)
        return false;

    float target[3] = {};
    if (!BuildWeaponGuideTargetLocal8(profileIndex, target))
        return false;

    const float halfSize = (profileIndex < 4 ? 0.005f : 0.040f) *
        g_worldScale8;
    const float right[3] = { halfSize, 0.0f, 0.0f };
    const float up[3] = { 0.0f, halfSize, 0.0f };

    constexpr DWORD color = 0xFFFFFFFFu;
    const FirearmReticleVertex8 vertices[4] = {
        { target[0] - right[0] + up[0],
          target[1] - right[1] + up[1],
          target[2] - right[2] + up[2], color },
        { target[0] + right[0] + up[0],
          target[1] + right[1] + up[1],
          target[2] + right[2] + up[2], color },
        { target[0] - right[0] - up[0],
          target[1] - right[1] - up[1],
          target[2] - right[2] - up[2], color },
        { target[0] + right[0] - up[0],
          target[1] + right[1] - up[1],
          target[2] + right[2] - up[2], color }
    };

    D3DMATRIX leftView = {};
    D3DMATRIX leftProjection = {};
    D3DMATRIX rightView = {};
    D3DMATRIX rightProjection = {};
    D3DMATRIX desktopView = {};
    D3DMATRIX desktopProjection = {};
    if (!BuildWeaponGuideTransforms8(0, &leftView,
        &leftProjection) ||
        !BuildWeaponGuideTransforms8(1, &rightView,
            &rightProjection) ||
        !BuildWeaponGuideTransforms8(2, &desktopView,
            &desktopProjection))
    {
        return false;
    }

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginExistingStereoPairPass(&originalColor, &originalDepth))
        return false;

    DWORD stateBlock = 0;
    const bool haveStateBlock = SUCCEEDED(
        D3D8Slot<PFN8_CreateStateBlock>(g_device8, VT8_CreateStateBlock)(
            g_device8, 1u, &stateBlock));
    bool leftComplete = false;
    bool rightComplete = false;
    bool desktopComplete = false;
    if (haveStateBlock)
    {
        const auto setRenderState = D3D8Slot<PFN8_SetRenderState>(g_device8,
            VT8_SetRenderState);
        const auto setTextureStage = D3D8Slot<PFN8_SetTextureStageState>(
            g_device8, VT8_SetTextureStageState);
        const auto setTexture = o_D3D8_SetTexture ? o_D3D8_SetTexture
            : D3D8Slot<PFN_D3D8_SetTexture>(g_device8, VT8_SetTexture);
        const auto setVertexShader = o_D3D8_SetVertexShader
            ? o_D3D8_SetVertexShader
            : D3D8Slot<PFN_D3D8_SetVertexShader>(g_device8,
                VT8_SetVertexShader);

        D3DMATRIX identity = {};
        identity.m[0][0] = 1.0f;
        identity.m[1][1] = 1.0f;
        identity.m[2][2] = 1.0f;
        identity.m[3][3] = 1.0f;
        setTexture(g_device8, 0, nullptr);
        setVertexShader(g_device8, 0x42u); // XYZ | DIFFUSE
        setRenderState(g_device8, 7u, FALSE);   // ZENABLE
        setRenderState(g_device8, 14u, FALSE);  // ZWRITEENABLE
        setRenderState(g_device8, 22u, 1u);     // CULLMODE = NONE
        setRenderState(g_device8, 27u, FALSE);  // ALPHABLENDENABLE
        setRenderState(g_device8, 80u, FALSE);  // FOGENABLE
        setTextureStage(g_device8, 0, 1u, 2u);  // COLOROP = SELECTARG1
        setTextureStage(g_device8, 0, 2u, 0u);  // COLORARG1 = DIFFUSE
        setTextureStage(g_device8, 0, 4u, 2u);  // ALPHAOP = SELECTARG1
        setTextureStage(g_device8, 0, 5u, 0u);  // ALPHAARG1 = DIFFUSE
        setTextureStage(g_device8, 1, 1u, 1u);  // COLOROP = DISABLE
        setTextureStage(g_device8, 1, 4u, 1u);  // ALPHAOP = DISABLE

        leftComplete = SUCCEEDED(o_D3D8_SetTransform(g_device8,
            SH3VR_D3DTS_WORLD, &identity)) &&
            SUCCEEDED(o_D3D8_SetTransform(g_device8,
                SH3VR_D3DTS_VIEW, &leftView)) &&
            SUCCEEDED(o_D3D8_SetTransform(g_device8,
                SH3VR_D3DTS_PROJECTION, &leftProjection)) &&
            SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8, 5u, 2u,
                vertices, sizeof(FirearmReticleVertex8)));

        if (SwitchStereoPairEye(1))
        {
            rightComplete = SUCCEEDED(o_D3D8_SetTransform(g_device8,
                SH3VR_D3DTS_WORLD, &identity)) &&
                SUCCEEDED(o_D3D8_SetTransform(g_device8,
                    SH3VR_D3DTS_VIEW, &rightView)) &&
                SUCCEEDED(o_D3D8_SetTransform(g_device8,
                    SH3VR_D3DTS_PROJECTION, &rightProjection)) &&
                SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8, 5u, 2u,
                vertices, sizeof(FirearmReticleVertex8)));
        }

        // Native eye surfaces are separate from the monitor mirror. Render a
        // zero-IPD copy into the real backbuffer, with depth disabled, so the
        // guide remains visible in the desktop view as well.
        IDirect3DSurface8* desktopBackBuffer = nullptr;
        const HRESULT backBufferResult = D3D8Slot<PFN8_GetBackBuffer>(
            g_device8, 16)(g_device8, 0, 0, &desktopBackBuffer);
        if (SUCCEEDED(backBufferResult) && desktopBackBuffer)
        {
            const HRESULT bindResult = D3D8Slot<PFN8_SetRenderTarget>(
                g_device8, 31)(g_device8, desktopBackBuffer, nullptr);
            if (SUCCEEDED(bindResult))
            {
                D3D8ViewportLocal desktopViewport = {};
                desktopViewport.Width = g_gameRenderTargetWidth8;
                desktopViewport.Height = g_gameRenderTargetHeight8;
                desktopViewport.MinZ = 0.0f;
                desktopViewport.MaxZ = 1.0f;
                const bool viewportReady = desktopViewport.Width != 0 &&
                    desktopViewport.Height != 0 &&
                    SUCCEEDED(D3D8Slot<PFN8_SetViewport>(g_device8,
                        VT8_SetViewport)(g_device8, &desktopViewport));
                desktopComplete = viewportReady &&
                    SUCCEEDED(o_D3D8_SetTransform(g_device8,
                        SH3VR_D3DTS_WORLD, &identity)) &&
                    SUCCEEDED(o_D3D8_SetTransform(g_device8,
                        SH3VR_D3DTS_VIEW, &desktopView)) &&
                    SUCCEEDED(o_D3D8_SetTransform(g_device8,
                        SH3VR_D3DTS_PROJECTION, &desktopProjection)) &&
                    SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8, 5u, 2u,
                        vertices, sizeof(FirearmReticleVertex8)));
            }
            ReleaseD3D8Surface(desktopBackBuffer);
        }

        D3D8Slot<PFN8_ApplyStateBlock>(g_device8, VT8_ApplyStateBlock)(
            g_device8, stateBlock);
        D3D8Slot<PFN8_DeleteStateBlock>(g_device8, VT8_DeleteStateBlock)(
            g_device8, stateBlock);
    }
    EndStereoPairPass(originalColor, originalDepth);

    if (leftComplete && rightComplete && !g_firearmReticleLogged8)
    {
        g_firearmReticleLogged8 = true;
        Log("MotionControls: weapon guide dot is rendering in both native eyes (desktop=%d)",
            desktopComplete ? 1 : 0);
    }
    return leftComplete && rightComplete;
}

static bool DuplicateFixedFunctionPrimitiveUPForStereo(
    DWORD primitiveType, UINT primitiveCount, const void* vertices,
    UINT stride)
{
    if (!g_device8 || g_currentVertexShader8 != 0x00000002u || !vertices ||
        stride < 12 || stride > 256)
    {
        return false;
    }

    const UINT vertexCount = UiVertexCountForPrimitive(primitiveType,
        primitiveCount);
    if (vertexCount == 0 || vertexCount > 65536u)
        return false;

    D3DMATRIX leftView = {};
    D3DMATRIX leftProjection = {};
    D3DMATRIX rightView = {};
    D3DMATRIX rightProjection = {};
    if (!BuildFixedFunctionStereoTransforms8(true, 0, &leftView,
        &leftProjection) ||
        !BuildFixedFunctionStereoTransforms8(true, 1, &rightView,
            &rightProjection))
    {
        return false;
    }

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginStereoPairPass(&originalColor, &originalDepth))
        return false;

    const bool leftBound =
        SUCCEEDED(o_D3D8_SetTransform(g_device8, SH3VR_D3DTS_VIEW,
            &leftView)) &&
        SUCCEEDED(o_D3D8_SetTransform(g_device8, SH3VR_D3DTS_PROJECTION,
            &leftProjection));
    const bool leftComplete = leftBound &&
        SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8, primitiveType,
            primitiveCount, vertices, stride));

    bool rightComplete = false;
    if (SwitchStereoPairEye(1))
    {
        const bool rightBound =
            SUCCEEDED(o_D3D8_SetTransform(g_device8, SH3VR_D3DTS_VIEW,
                &rightView)) &&
            SUCCEEDED(o_D3D8_SetTransform(g_device8,
                SH3VR_D3DTS_PROJECTION, &rightProjection));
        rightComplete = rightBound &&
            SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8, primitiveType,
                primitiveCount, vertices, stride));
    }

    D3DMATRIX centerView = {};
    D3DMATRIX centerProjection = {};
    if (BuildFixedFunctionStereoTransforms8(false, 0, &centerView,
        &centerProjection))
    {
        o_D3D8_SetTransform(g_device8, SH3VR_D3DTS_VIEW, &centerView);
        o_D3D8_SetTransform(g_device8, SH3VR_D3DTS_PROJECTION,
            &centerProjection);
    }
    EndStereoPairPass(originalColor, originalDepth);

    if (leftComplete && rightComplete)
    {
        InterlockedIncrement(&g_perDrawStereoProbeDraws8);
        if (!g_loggedFixedFunctionShadow8)
        {
            g_loggedFixedFunctionShadow8 = true;
            Log("Fixed-function 3D shadow candidate replayed: FVF 0x00000002, "
                "primitive type %u, primitives %u", primitiveType,
                primitiveCount);
        }
    }
    return leftComplete && rightComplete;
}

static bool DuplicateUiPrimitiveForStereo(DWORD primitiveType,
    UINT startVertex, UINT primitiveCount)
{
    if (!g_device8 || !IsLikelyUiAtlasDraw8())
        return false;

    const UINT vertexCount = UiVertexCountForPrimitive(primitiveType,
        primitiveCount);
    if (vertexCount == 0 || vertexCount > 65536u)
        return false;

    D3D8ViewportLocal sourceViewport = {};
    if (FAILED(D3D8Slot<PFN8_GetViewport>(g_device8, 41)(g_device8,
        &sourceViewport)))
    {
        return false;
    }

    IDirect3DVertexBuffer8* vertexBuffer = nullptr;
    UINT stride = 0;
    const HRESULT sourceResult = D3D8Slot<PFN8_GetStreamSource>(g_device8,
        VT8_GetStreamSource)(g_device8, 0, &vertexBuffer, &stride);
    if (FAILED(sourceResult) || !vertexBuffer || stride < 16 || stride > 1024)
    {
        if (vertexBuffer)
            D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
        return false;
    }

    const std::size_t byteCount = static_cast<std::size_t>(vertexCount) *
        static_cast<std::size_t>(stride);
    if (byteCount > 16u * 1024u * 1024u ||
        static_cast<std::size_t>(startVertex) * stride > 0xFFFFFFFFu -
            byteCount)
    {
        D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
        return false;
    }

    BYTE* locked = nullptr;
    const UINT lockOffset = startVertex * stride;
    const HRESULT lockResult = D3D8Slot<PFNV8_Lock>(vertexBuffer, 11)(
        vertexBuffer, lockOffset, static_cast<UINT>(byteCount), &locked,
        0x00000010u);
    if (FAILED(lockResult) || !locked)
    {
        D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);
        return false;
    }

    std::vector<std::uint8_t> source(byteCount);
    std::memcpy(source.data(), locked, byteCount);
    D3D8Slot<PFNV8_Unlock>(vertexBuffer, 12)(vertexBuffer);
    D3D8Slot<PFNV8_Release>(vertexBuffer, 2)(vertexBuffer);

    std::vector<std::uint8_t> transformedRight;
    if (!BuildUiOverlayVertices(sourceViewport, source.data(), vertexCount,
            stride, 1, &transformedRight))
    {
        return false;
    }

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginUiStereoPairPass(&originalColor, &originalDepth))
        return false;

    const bool leftComplete = true;
    bool rightComplete = false;
    if (SwitchStereoPairEye(1) && SetUiOverlayViewport())
    {
        rightComplete = SUCCEEDED(o_D3D8_DrawPrimitiveUP(g_device8,
            primitiveType, primitiveCount, transformedRight.data(), stride));
    }
    EndStereoPairPass(originalColor, originalDepth);

    if (leftComplete && rightComplete)
    {
        InterlockedIncrement(&g_uiStereoOverlayDraws8);
        if (!g_loggedUiStereoOverlay8)
        {
            g_loggedUiStereoOverlay8 = true;
            Log("Stereo UI vertex-buffer overlay enabled: shader/FVF "
                "0x%08X, atlas 512x512, source viewport %ux%u, native "
                "target %ux%u, right-eye-only scaled safe layout",
                g_currentVertexShader8,
                sourceViewport.Width, sourceViewport.Height,
                g_stereoTargetWidth8, g_stereoTargetHeight8);
        }
    }
    return leftComplete && rightComplete;
}

static bool DuplicateUiIndexedPrimitiveUPForStereo(DWORD primitiveType,
    UINT minIndex, UINT vertexCount, UINT primitiveCount, const void* indices,
    DWORD indexFormat, const void* vertices, UINT stride)
{
    if (!indices || vertexCount == 0 || vertexCount > 65536u)
        return false;

    D3D8ViewportLocal sourceViewport = {};
    if (!g_device8 || FAILED(D3D8Slot<PFN8_GetViewport>(g_device8, 41)(
        g_device8, &sourceViewport)))
    {
        return false;
    }

    std::vector<std::uint8_t> transformedRight;
    if (!BuildUiOverlayVertices(sourceViewport, vertices, vertexCount, stride,
            1, &transformedRight))
    {
        return false;
    }

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginUiStereoPairPass(&originalColor, &originalDepth))
        return false;

    const bool leftComplete = true;
    bool rightComplete = false;
    if (SwitchStereoPairEye(1) && SetUiOverlayViewport())
    {
        rightComplete = SUCCEEDED(o_D3D8_DrawIndexedPrimitiveUP(g_device8,
            primitiveType, minIndex, vertexCount, primitiveCount, indices,
            indexFormat, transformedRight.data(), stride));
    }
    EndStereoPairPass(originalColor, originalDepth);

    if (leftComplete && rightComplete)
    {
        InterlockedIncrement(&g_uiStereoOverlayDraws8);
        if (!g_loggedUiStereoOverlay8)
        {
            g_loggedUiStereoOverlay8 = true;
            Log("Stereo indexed UI overlay enabled: FVF 0x%08X, source "
                "viewport %ux%u, native target %ux%u, right-eye-only "
                "scaled safe layout", g_currentVertexShader8,
                sourceViewport.Width, sourceViewport.Height,
                g_stereoTargetWidth8, g_stereoTargetHeight8);
        }
    }
    return leftComplete && rightComplete;
}

static HRESULT WINAPI hk_D3D8_SetTransform(IDirect3DDevice8* device, DWORD state,
    const D3DMATRIX* matrix)
{
    const LONG callOrdinal = g_enableRuntimeDiagnostics8
        ? ++g_transformCallOrdinal8 : 0;
    if (matrix)
    {
        if (state == SH3VR_D3DTS_VIEW)
        {
            std::memcpy(&g_lastView8, matrix, sizeof(g_lastView8));
            g_haveView8 = true;
            if (g_enableRuntimeDiagnostics8)
            {
                ++g_viewCallsThisFrame8;
                CaptureD3D8TransformSample(g_viewSamples8, g_viewSampleCount8,
                    *matrix, callOrdinal);
            }
        }
        else if (state == SH3VR_D3DTS_PROJECTION)
        {
            std::memcpy(&g_lastProjection8, matrix, sizeof(g_lastProjection8));
            g_haveProjection8 = true;
            if (g_enableRuntimeDiagnostics8)
            {
                ++g_projectionCallsThisFrame8;
                CaptureD3D8TransformSample(g_projectionSamples8,
                    g_projectionSampleCount8, *matrix, callOrdinal);
            }
        }
    }

    if (matrix && state == SH3VR_D3DTS_PROJECTION)
    {
        D3DMATRIX vrProjection = {};
        if (BuildImmersiveProjection(*matrix, vrProjection))
            return o_D3D8_SetTransform(device, state, &vrProjection);
    }

    if (matrix && state == SH3VR_D3DTS_VIEW)
    {
        D3DMATRIX vrView = {};
        if (g_enableExperimentalColumnViewHeadRotation8 &&
            ApplyHeadRotationToColumnView(*matrix, vrView))
        {
            const HRESULT result = o_D3D8_SetTransform(device, state, &vrView);
            if (SUCCEEDED(result))
            {
                g_flashlightBaseView8 = *matrix;
                g_flashlightVrView8 = vrView;
                g_haveFlashlightViewPair8 = true;
                RefreshHeadTrackedFlashlightProjection8(device);
            }
            return result;
        }
        if (g_enableExperimentalHeadRotation8 &&
            ApplyHeadRotationToMainCamera(*matrix, vrView))
        {
            const HRESULT result = o_D3D8_SetTransform(device, state, &vrView);
            if (SUCCEEDED(result))
            {
                g_flashlightBaseView8 = *matrix;
                g_flashlightVrView8 = vrView;
                g_haveFlashlightViewPair8 = true;
                RefreshHeadTrackedFlashlightProjection8(device);
            }
            return result;
        }
    }

    return o_D3D8_SetTransform(device, state, matrix);
}

static void LogRenderCaller(const char* operation, const void* returnAddress)
{
    HMODULE module = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExA(flags,
        reinterpret_cast<LPCSTR>(returnAddress), &module))
    {
        Log("%s caller: address 0x%08X, module unavailable", operation,
            static_cast<unsigned>(reinterpret_cast<UINT_PTR>(returnAddress)));
        return;
    }

    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(module, modulePath, static_cast<DWORD>(sizeof(modulePath)));
    const UINT_PTR address = reinterpret_cast<UINT_PTR>(returnAddress);
    const UINT_PTR base = reinterpret_cast<UINT_PTR>(module);
    Log("%s caller: address 0x%08X, module base 0x%08X, RVA 0x%08X, %s",
        operation, static_cast<unsigned>(address), static_cast<unsigned>(base),
        static_cast<unsigned>(address - base), modulePath);
}

static void LogRenderCallStack(const char* operation)
{
    void* frames[16] = {};
    const USHORT frameCount = CaptureStackBackTrace(0,
        static_cast<DWORD>(_countof(frames)), frames, nullptr);
    Log("%s call stack: %u frames", operation,
        static_cast<unsigned>(frameCount));
    for (USHORT index = 0; index < frameCount; ++index)
    {
        HMODULE module = nullptr;
        const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(frames[index]),
            &module))
        {
            Log("  frame %u: address 0x%08X", static_cast<unsigned>(index),
                static_cast<unsigned>(reinterpret_cast<UINT_PTR>(frames[index])));
            continue;
        }

        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(module, modulePath,
            static_cast<DWORD>(sizeof(modulePath)));
        const UINT_PTR address = reinterpret_cast<UINT_PTR>(frames[index]);
        const UINT_PTR base = reinterpret_cast<UINT_PTR>(module);
        Log("  frame %u: address 0x%08X, RVA 0x%08X, %s",
            static_cast<unsigned>(index), static_cast<unsigned>(address),
            static_cast<unsigned>(address - base), modulePath);
    }
}

static HRESULT WINAPI hk_D3D8_Present(IDirect3DDevice8* device, const RECT* src,
    const RECT* dest, HWND window, const void* dirty)
{
    g_device8 = device;
    ProbeD3D8Device(device);
    UpdateMotionWeaponDrawCapture8();
    TryAutoLoadCameraModFirstPerson();
    EnsureProxyVirtualFPSMode4();
    ApplyProxyVirtualFrameTimeTuple90Hz();
    ApplyProxyFrameTimeState90Hz();

    const LONG frame = InterlockedIncrement(&c_present8);
    UpdateGamePostProcessState8();
    PollNativeEyeAsyncCopy();
    const bool use90FpsTiming = Apply90FpsTimingPatch();
    const bool useProxyNativeTiming = g_proxyNativeTimingUnlockApplied8;
    const bool useProxyFrameTimeOverride =
        g_proxyFrameTimeOverrideApplied8;
    const bool useProxyVirtualMode4 = g_proxyVirtualMode4Applied8;
    LogD3D12QueueDiagnostics(frame);
    if (frame == 60 || frame == 300 || frame == 600)
        LogGameTimingState(frame);
    if (g_enableFixedStep90Test8 && frame % 600 == 0)
    {
        Log("Fixed-step 90 Hz counters at Present %d: phase %d, "
            "simulation updates %d, skipped preparations %d, post updates %d, "
            "post skips %d",
            frame, g_fixedStep90Phase8, g_fixedStep90UpdateCalls8,
            g_fixedStep90SkippedCalls8, g_fixedStep90PostUpdateCalls8,
            g_fixedStep90PostSkippedCalls8);
    }
    if (frame == 60)
    {
        Log("Backend probe at frame 60: d3d9 %s, d3d11 %s, d3d12 %s, "
            "dxgi %s, d3d9on12 %s",
            GetModuleHandleA("d3d9.dll") ? "loaded" : "absent",
            GetModuleHandleA("d3d11.dll") ? "loaded" : "absent",
            GetModuleHandleA("d3d12.dll") ? "loaded" : "absent",
            GetModuleHandleA("dxgi.dll") ? "loaded" : "absent",
            GetModuleHandleA("d3d9on12.dll") ? "loaded" : "absent");
    }
    const LONG renderCalls = g_enableRuntimeDiagnostics8
        ? InterlockedExchange(&c_sh3RenderCallsSincePresent, 0) : 0;
    const LONG prepareCalls = g_enableRuntimeDiagnostics8
        ? InterlockedExchange(&c_sh3PrepareCallsSincePresent, 0) : 0;
    if (g_enableRuntimeDiagnostics8 &&
        !g_loggedSh3RenderCadence && renderCalls != 0)
    {
        g_loggedSh3RenderCadence = true;
        Log("SH3 renderer cadence detected: %d call(s) before Present frame %d",
            renderCalls, frame);
    }
    if (g_enableRuntimeDiagnostics8 &&
        !g_loggedSh3PrepareCadence && prepareCalls != 0)
    {
        g_loggedSh3PrepareCadence = true;
        Log("SH3 frame preparation cadence detected: %d call(s) before "
            "Present frame %d", prepareCalls, frame);
    }
    if (g_enableRuntimeDiagnostics8 && frame % 600 == 0)
    {
        Log("SH3 renderer total %d, calls before this Present %d",
            c_sh3RenderFrame, renderCalls);
        Log("SH3 frame preparation total %d, calls before this Present %d",
            c_sh3PrepareFrame, prepareCalls);
    }
    if (g_enableRuntimeDiagnostics8 && frame == 1)
        LogRenderCaller("D3D8 Present", _ReturnAddress());
    if (g_enableRuntimeDiagnostics8 &&
        (frame == 1 || frame == 60 || frame % 600 == 0))
        Log("hk_D3D8_Present frame %d, device 0x%08X",
            frame, (unsigned)(UINT_PTR)device);

    if (g_enableRuntimeDiagnostics8)
    {
        LogD3D8TransformDiagnostics(frame);
        LogFirstGameplayTransformSet(frame);
        LogFirstGameplayShaderSet(frame);
    }
    // Immersive mode is intentionally evaluated per game frame. Pause/menu
    // frames do not always submit the gameplay view-projection, so latching
    // this state would keep the previous native eye texture on screen and hide
    // the newly opened pause menu.
    const bool immersive = g_viewProjectionAppliedThisFrame8;
    if (immersive != g_previousImmersiveFrame8)
    {
        // A save/room load replaces shader objects and may leave a loading or
        // menu frame between two gameplay scenes. Do not carry partially
        // filled eye targets, an overflow fallback, or a cached intermediate
        // render target across that boundary: doing so mixes geometry from
        // two scenes and appears as severe stereo ghosting.
        ResetBatchedFog8();
        g_perDrawStereoProbeDraws8 = 0;
        g_uiStereoOverlayDraws8 = 0;
        g_perDrawStereoReplayOverflow8 = false;
        g_consecutiveStereoReplayOverflowFrames8 = 0;
        g_perDrawStereoTargetCleared8[0] = false;
        g_perDrawStereoTargetCleared8[1] = false;
        g_havePerDrawStereoMatrices8 = false;
        g_havePerDrawGameMatrix8 = false;
        g_heavyFullSceneStereo8 = false;
        g_fullSceneStereoPairReady8 = false;
        g_fullSceneStereoReplayActive8 = false;
        g_fullScenePrimaryTargetBound8 = false;
        g_forceWaterFullSceneStereo8 = false;
        g_holdPreviousNativeEyeFrame8 = false;
        g_transientEffectPresentPreviousFrame8 = false;
        g_recentOffscreenTargetCount8 = 0;
        std::memset(g_recentOffscreenTargets8, 0,
            sizeof(g_recentOffscreenTargets8));
        ReleaseD3D8Surface(g_stereoReplayTarget8);
        if (g_stereoReplayTargetContainer8)
        {
            g_stereoReplayTargetContainer8->Release();
            g_stereoReplayTargetContainer8 = nullptr;
        }
        g_stereoReplayTargetFrame8 = -1;
        g_primaryTargetRefreshFrame8 = -1;
        Log(immersive
            ? "Stereo scene state reset before entering gameplay"
            : "Stereo scene state reset after leaving gameplay");
        g_previousImmersiveFrame8 = immersive;
    }
    UpdateRoomscaleMovement(immersive);
    if (!immersive)
        ResetBatchedFog8();
    else if (g_perDrawStereoProbeActive8)
        ReplayBatchedFogForStereo8();
    else
        ResetBatchedFog8();
    const bool transientEffectPresent = g_holdPreviousNativeEyeFrame8;
    const bool holdPreviousNativeEyeFrame = transientEffectPresent &&
        !g_transientEffectPresentPreviousFrame8 && immersive;
    g_transientEffectPresentPreviousFrame8 = transientEffectPresent;
    if (holdPreviousNativeEyeFrame && !g_loggedTransientEyeFrameHeld8)
    {
        g_loggedTransientEyeFrameHeld8 = true;
        Log("Transient effect frame was not published to native eyes; the "
            "previous complete stereo pair remains active");
    }
    if (!immersive && g_heavyFullSceneStereo8)
    {
        g_heavyFullSceneStereo8 = false;
        g_fullSceneStereoPairReady8 = false;
        g_loggedHeavyFullSceneStereo8 = false;
        Log("Heavy-scene full stereo replay reset after leaving immersive "
            "gameplay");
    }
    if (immersive && g_fullSceneStereoPairReady8)
    {
        if (!holdPreviousNativeEyeFrame)
            PrepareNativeEyeAsyncCopy();
        g_fullSceneStereoPairReady8 = false;
        g_perDrawStereoProbeDraws8 = 0;
        g_uiStereoOverlayDraws8 = 0;
        g_perDrawStereoReplayOverflow8 = false;
        g_perDrawStereoTargetCleared8[0] = false;
        g_perDrawStereoTargetCleared8[1] = false;
        g_havePerDrawStereoMatrices8 = false;
        g_havePerDrawGameMatrix8 = false;
    }
    if (g_perDrawStereoProbeActive8 && g_perDrawStereoProbeDraws8 > 0)
    {
        if (!g_perDrawStereoProbeComplete8)
        {
            g_perDrawStereoProbeComplete8 = true;
            Log("Continuous stereo draw replay started with %d selected 3D "
                "draw calls", g_perDrawStereoProbeDraws8);
            for (int index = 0; index < g_shaderDrawGroupCount8; ++index)
            {
                const D3D8ShaderDrawGroup& group = g_shaderDrawGroups8[index];
                if (IsStereoReplayShader(group.shader))
                {
                    Log("  replay shader 0x%08X: draws %d, indexed %d, "
                        "primitives %u", group.shader, group.drawCalls,
                        group.indexedDrawCalls,
                        static_cast<unsigned>(group.primitives));
                }
            }
        }
        if (!g_stereoShaderCensusLogged8 &&
            ++g_stereoShaderCensusFrames8 >= 120)
        {
            g_stereoShaderCensusLogged8 = true;
            Log("=== Continuous stereo shader census ===");
            for (int index = 0; index < g_shaderDrawGroupCount8; ++index)
            {
                const D3D8ShaderDrawGroup& group = g_shaderDrawGroups8[index];
                Log("  shader 0x%08X: draws %d, indexed %d, primitives %u, "
                    "replayed %s", group.shader, group.drawCalls,
                    group.indexedDrawCalls,
                    static_cast<unsigned>(group.primitives),
                    IsStereoReplayShader(group.shader) ? "yes" : "no");
            }
            Log("=== Continuous stereo shader census end ===");
        }
        if (g_perDrawStereoReplayOverflow8)
        {
            if (!g_heavyFullSceneStereo8)
            {
                g_heavyFullSceneStereo8 = true;
                Log("Per-draw demand exceeded the bounded budget; heavy-scene "
                    "full stereo replay will start on the next immersive "
                    "frame");
            }
            ++g_consecutiveStereoReplayOverflowFrames8;
            if (g_consecutiveStereoReplayOverflowFrames8 == 1)
            {
                Log("Incomplete native eye frame was not published after "
                    "replay budget overflow; the previous complete stereo "
                    "pair remains active");
                LONG selectedDraws = 0;
                Log("=== Over-budget stereo shader census ===");
                for (int index = 0; index < g_shaderDrawGroupCount8; ++index)
                {
                    const D3D8ShaderDrawGroup& group =
                        g_shaderDrawGroups8[index];
                    if (!IsStereoReplayShader(group.shader))
                        continue;
                    selectedDraws += group.drawCalls;
                    Log("  replay shader 0x%08X: draws %d, indexed %d, "
                        "primitives %u", group.shader, group.drawCalls,
                        group.indexedDrawCalls,
                        static_cast<unsigned>(group.primitives));
                }
                Log("  selected replay demand: %d draw calls; budget %d",
                    selectedDraws, kPerDrawStereoReplayBudget8);
                Log("=== Over-budget stereo shader census end ===");
            }
            else if (g_consecutiveStereoReplayOverflowFrames8 == 60 ||
                g_consecutiveStereoReplayOverflowFrames8 % 300 == 0)
            {
                Log("Native eye replay has remained over budget for %d "
                    "consecutive frames; partial eye targets are still not "
                    "being published",
                    g_consecutiveStereoReplayOverflowFrames8);
            }
        }
        else if (!holdPreviousNativeEyeFrame)
        {
            if (g_consecutiveStereoReplayOverflowFrames8 > 0)
            {
                Log("Complete native eye replay resumed after %d overflow "
                    "frame(s)", g_consecutiveStereoReplayOverflowFrames8);
                g_consecutiveStereoReplayOverflowFrames8 = 0;
            }
            PrepareNativeEyeAsyncCopy();
        }
        g_perDrawStereoProbeDraws8 = 0;
        g_uiStereoOverlayDraws8 = 0;
        g_perDrawStereoReplayOverflow8 = false;
        g_perDrawStereoTargetCleared8[0] = false;
        g_perDrawStereoTargetCleared8[1] = false;
        g_havePerDrawStereoMatrices8 = false;
        g_havePerDrawGameMatrix8 = false;
    }
    else if (g_perDrawStereoProbeActive8 &&
        InterlockedCompareExchange(&g_uiStereoOverlayDraws8, 0, 0) > 0)
    {
        // Menus and subtitle-only frames may not contain a selected 3D draw,
        // but their native eye targets still need to be published.
        if (!holdPreviousNativeEyeFrame)
            PrepareNativeEyeAsyncCopy();
        g_uiStereoOverlayDraws8 = 0;
    }
    g_holdPreviousNativeEyeFrame8 = false;
    if (immersive && g_enablePerDrawStereoProbe8 &&
        !g_perDrawStereoProbeActive8 && !g_perDrawStereoProbeComplete8)
    {
        if (g_perDrawStereoProbeWaitFrames8 > 0)
            --g_perDrawStereoProbeWaitFrames8;
        if (g_perDrawStereoProbeWaitFrames8 == 0)
        {
            g_perDrawStereoProbeActive8 = true;
            g_perDrawStereoTargetCleared8[0] = false;
            g_perDrawStereoTargetCleared8[1] = false;
            g_havePerDrawStereoMatrices8 = false;
            g_havePerDrawGameMatrix8 = false;
            g_perDrawStereoProbeDraws8 = 0;
            g_perDrawStereoBudgetLogged8 = false;
            g_perDrawStereoReplayOverflow8 = false;
            Log("Continuous native stereo draw replay armed for the next "
                "gameplay frame with 64 mm IPD");
        }
    }
    const bool synchronizedStereo = immersive &&
        g_enableSynchronizedStereo8 &&
        g_stereoPairPublishedThisFrame8;
    if (immersive && g_probePresentBackBufferThisFrame8)
    {
        ProbeStereoBackBuffer("explicit backbuffer immediately before Present");
        g_probePresentBackBufferThisFrame8 = false;
    }
    Interop8_SetRenderMode(immersive
        ? (synchronizedStereo || g_enableAlternatingStereo8
            ? SH3VR_RENDER_IMMERSIVE_STEREO
            : SH3VR_RENDER_IMMERSIVE_MONO)
        : SH3VR_RENDER_CINEMA);
    Interop8_SetRenderFlags(SH3VR_RENDER_FLAG_NONE);
    Interop8_SetRenderEye(synchronizedStereo ? 1 :
        (immersive && g_enableAlternatingStereo8
        ? g_renderEye8 : 0));
    g_viewProjectionAppliedThisFrame8 = false;
    g_haveFlashlightViewPair8 = false;
    g_viewCallsThisFrame8 = 0;
    g_projectionCallsThisFrame8 = 0;
    g_transformCallOrdinal8 = 0;
    g_viewSampleCount8 = 0;
    g_projectionSampleCount8 = 0;
    g_vertexShaderChangesThisFrame8 = 0;
    g_vertexShaderConstantCallsThisFrame8 = 0;
    g_shaderConstantGroupCount8 = 0;
    g_shaderDrawGroupCount8 = 0;

    // Publish every frame into the shared section for sh3vr_host64.exe.
    if (!synchronizedStereo)
        Interop8_GrabFrame(device);
    g_haveLatchedFrameHeadPose8 = false;
    g_stereoPairPublishedThisFrame8 = false;

    if (immersive && g_enableAlternatingStereo8)
        g_renderEye8 ^= 1u;

    InterlockedExchange(&g_insideD3D8Present, 1);
    const HRESULT result = o_D3D8_Present(device, src, dest, window, dirty);
    InterlockedExchange(&g_insideD3D8Present, 0);
    D3D12QueueDiagnostic* activeQueue =
        FindD3D12QueueDiagnostic(g_d3d12DirectQueue);
    if (activeQueue && activeQueue->executeOriginal)
    {
        TrySubmitNativeEyeAsyncCopy(g_d3d12DirectQueue,
            activeQueue->executeOriginal);
    }

    // PC Fix FPSMode=1 remains the simulation clock. This optional proxy path
    // publishes one additional presentation on every second immersive game
    // frame, producing a 2:1 90 Hz presentation cadence from the stable 60 Hz
    // game loop. It does not skip updates, change frameSeconds, or call any
    // game update routine. The host can therefore refresh its OpenXR pose at
    // 90 Hz without requiring GPU-driver or RivaTuner configuration.
    const bool useProxyPresentationUnlock =
        g_enableProxyPresentationUnlock90Hz8 && immersive &&
        !g_proxyNativeTimingUnlockApplied8 &&
        !g_proxyFrameTimeOverrideApplied8 && !useProxyVirtualMode4 &&
        g_nativeEyeHandlesPublished &&
        !synchronizedStereo &&
        (frame % 2 == 0) && SUCCEEDED(result);
    if (useProxyPresentationUnlock)
    {
        Sh3VrHeadPose latestPose = {};
        if (Interop8_ReadHeadPose(&latestPose))
            Interop8_SetFrameRenderPose(latestPose);
        Interop8_GrabFrame(device);

        InterlockedExchange(&g_insideD3D8Present, 1);
        const HRESULT extraResult = o_D3D8_Present(device, src, dest, window,
            dirty);
        InterlockedExchange(&g_insideD3D8Present, 0);
        if (SUCCEEDED(extraResult))
        {
            const LONG extraCount = InterlockedIncrement(
                &g_proxyExtraPresentCount8);
            if (!g_proxyPresentationUnlockLogged8)
            {
                g_proxyPresentationUnlockLogged8 = true;
                Log("Proxy presentation unlock active: one extra D3D8 "
                    "Present every second immersive game frame; PC Fix "
                    "simulation remains at 60 Hz");
            }
            if (extraCount == 1 || extraCount % 600 == 0)
                Log("Proxy presentation unlock extra Present %d after game "
                    "Present %d", extraCount, frame);

            activeQueue = FindD3D12QueueDiagnostic(g_d3d12DirectQueue);
            if (activeQueue && activeQueue->executeOriginal)
            {
                TrySubmitNativeEyeAsyncCopy(g_d3d12DirectQueue,
                    activeQueue->executeOriginal);
            }
        }
        else if (!g_proxyPresentationUnlockFailureLogged8)
        {
            g_proxyPresentationUnlockFailureLogged8 = true;
            g_enableProxyPresentationUnlock90Hz8 = false;
            Log("Proxy presentation unlock disabled after extra D3D8 Present "
                "failed with hr 0x%08X", static_cast<unsigned>(extraResult));
        }
    }
    if (use90FpsTiming || useProxyNativeTiming || useProxyFrameTimeOverride ||
        useProxyVirtualMode4)
        PaceGameFrame();
    return result;
}

static HRESULT WINAPI hk_D3D8_EndScene(IDirect3DDevice8* device)
{
    g_device8 = device;
    PollWeaponIniHotReload8();

    if (InterlockedIncrement(&c_endScene8) == 1)
    {
        Log("hk_D3D8_EndScene first call, device 0x%08X", (unsigned)(UINT_PTR)device);
        LogRenderCaller("D3D8 EndScene", _ReturnAddress());
    }

    // Append the tracked hand while the game's D3D8 scene is still open.
    // Starting a fresh scene from Present is rejected by several D3D8-to-D3D12
    // wrappers and left the resources loaded without ever issuing a visible draw.
    const LONG presentFrame = InterlockedCompareExchange(&c_present8, 0, 0);
    if (g_viewProjectionAppliedThisFrame8 &&
        g_leftHandRenderedPresent8 != presentFrame &&
        RenderLeftHandStereo8())
    {
        g_leftHandRenderedPresent8 = presentFrame;
    }
    if ((g_viewProjectionAppliedThisFrame8 || g_previousImmersiveFrame8) &&
        g_firearmReticleRenderedPresent8 != presentFrame &&
        RenderFirearmReticleStereo8())
    {
        g_firearmReticleRenderedPresent8 = presentFrame;
    }

    return o_D3D8_EndScene(device);
}

static HRESULT WINAPI hk_D3D8_Reset(IDirect3DDevice8* device, void* presentParams)
{
    Log("hk_D3D8_Reset called");
    ReleaseLeftHandResources8();
    g_haveFlashlightViewPair8 = false;
    g_haveFlashlightProjectionSource8 = false;
    g_firearmReticleRenderedPresent8 = -1;
    g_firearmReticleLogged8 = false;
    g_flashlightProjectionSeenFrame8 = -1000;
    g_flashlightProjectionStrength8 = 0.0f;
    g_loggedLeftHandFlashlight8 = false;
    std::memset(g_currentTextures8, 0, sizeof(g_currentTextures8));
    std::memset(g_currentTextureIsRenderTarget8, 0,
        sizeof(g_currentTextureIsRenderTarget8));
    std::memset(g_currentTextureWidths8, 0,
        sizeof(g_currentTextureWidths8));
    std::memset(g_currentTextureHeights8, 0,
        sizeof(g_currentTextureHeights8));
    std::memset(g_recentOffscreenTargets8, 0,
        sizeof(g_recentOffscreenTargets8));
    g_recentOffscreenTargetCount8 = 0;
    g_framePacingDeadline8 = 0;
    ReleaseStereoRenderTargets();
    Interop8_OnDeviceReset();
    return o_D3D8_Reset(device, presentParams);
}

static void HookD3D8Device(IDirect3DDevice8* device)
{
    if (g_d8DeviceHooked || !device)
        return;

    void** vtable = *reinterpret_cast<void***>(device);
    Log("--- IDirect3DDevice8 vtable at 0x%08X ---", (unsigned)(UINT_PTR)vtable);

    HookOne("IDirect3DDevice8::Present", vtable[VT8_Present], &hk_D3D8_Present,
        reinterpret_cast<void**>(&o_D3D8_Present));
    HookOne("IDirect3DDevice8::Reset", vtable[VT8_Reset], &hk_D3D8_Reset,
        reinterpret_cast<void**>(&o_D3D8_Reset));
    HookOne("IDirect3DDevice8::Clear", vtable[VT8_Clear], &hk_D3D8_Clear,
        reinterpret_cast<void**>(&o_D3D8_Clear));
    HookOne("IDirect3DDevice8::SetRenderTarget",
        vtable[VT8_SetRenderTarget], &hk_D3D8_SetRenderTarget,
        reinterpret_cast<void**>(&o_D3D8_SetRenderTarget));
    HookOne("IDirect3DDevice8::SetViewport", vtable[VT8_SetViewport],
        &hk_D3D8_SetViewport,
        reinterpret_cast<void**>(&o_D3D8_SetViewport));
    HookOne("IDirect3DDevice8::EndScene", vtable[VT8_EndScene],
        &hk_D3D8_EndScene,
        reinterpret_cast<void**>(&o_D3D8_EndScene));
    HookOne("IDirect3DDevice8::SetTransform", vtable[VT8_SetTransform],
        &hk_D3D8_SetTransform, reinterpret_cast<void**>(&o_D3D8_SetTransform));
    if (g_enableRuntimeDiagnostics8 || g_enablePerDrawStereoProbe8)
    {
        HookOne("IDirect3DDevice8::DrawPrimitive", vtable[VT8_DrawPrimitive],
            &hk_D3D8_DrawPrimitive,
            reinterpret_cast<void**>(&o_D3D8_DrawPrimitive));
        HookOne("IDirect3DDevice8::DrawIndexedPrimitive",
            vtable[VT8_DrawIndexedPrimitive], &hk_D3D8_DrawIndexedPrimitive,
            reinterpret_cast<void**>(&o_D3D8_DrawIndexedPrimitive));
        HookOne("IDirect3DDevice8::DrawPrimitiveUP",
            vtable[VT8_DrawPrimitiveUP], &hk_D3D8_DrawPrimitiveUP,
            reinterpret_cast<void**>(&o_D3D8_DrawPrimitiveUP));
        HookOne("IDirect3DDevice8::DrawIndexedPrimitiveUP",
            vtable[VT8_DrawIndexedPrimitiveUP],
            &hk_D3D8_DrawIndexedPrimitiveUP,
            reinterpret_cast<void**>(&o_D3D8_DrawIndexedPrimitiveUP));
        HookOne("IDirect3DDevice8::SetVertexShader", vtable[VT8_SetVertexShader],
            &hk_D3D8_SetVertexShader,
            reinterpret_cast<void**>(&o_D3D8_SetVertexShader));
        HookOne("IDirect3DDevice8::SetTexture", vtable[VT8_SetTexture],
            &hk_D3D8_SetTexture,
            reinterpret_cast<void**>(&o_D3D8_SetTexture));
    }
    if (g_enableShaderDumps8)
    {
        HookOne("IDirect3DDevice8::CreateVertexShader",
            vtable[VT8_CreateVertexShader], &hk_D3D8_CreateVertexShader,
            reinterpret_cast<void**>(&o_D3D8_CreateVertexShader));
    }
    HookOne("IDirect3DDevice8::SetVertexShaderConstant",
        vtable[VT8_SetVertexShaderConstant], &hk_D3D8_SetVertexShaderConstant,
        reinterpret_cast<void**>(&o_D3D8_SetVertexShaderConstant));

    if (MH_EnableHook(MH_ALL_HOOKS) == MH_OK)
    {
        g_d8DeviceHooked = true;
        Log("D3D8 device hooks armed");
    }
    else
    {
        Log("ERROR: MH_EnableHook failed while arming D3D8 device hooks");
    }
}

static HRESULT WINAPI hk_D3D8_CreateDevice(IDirect3D8* self, UINT adapter, DWORD deviceType,
    HWND focusWindow, DWORD behaviorFlags,
    void* presentParams, IDirect3DDevice8** out)
{
    Log("hk_D3D8_CreateDevice: adapter %u, type %u, flags 0x%08X, focus 0x%08X",
        adapter, (unsigned)deviceType, (unsigned)behaviorFlags,
        (unsigned)(UINT_PTR)focusWindow);

    const HRESULT hr = o_D3D8_CreateDevice(self, adapter, deviceType, focusWindow,
        behaviorFlags, presentParams, out);

    if (SUCCEEDED(hr) && out && *out)
    {
        Log("hk_D3D8_CreateDevice: device 0x%08X created", (unsigned)(UINT_PTR)*out);
        g_device8 = *out;
        HookD3D8Device(*out);
    }
    else
    {
        Log("hk_D3D8_CreateDevice failed, hr = 0x%08X", (unsigned)hr);
    }

    return hr;
}

static IDirect3D8* WINAPI hk_Direct3DCreate8(UINT sdkVersion)
{
    Log("hk_Direct3DCreate8 called, SDK version %u", sdkVersion);

    IDirect3D8* d3d8 = o_Direct3DCreate8(sdkVersion);
    if (!d3d8)
    {
        Log("hk_Direct3DCreate8: wrapper returned null");
        return nullptr;
    }

    void** vtable = *reinterpret_cast<void***>(d3d8);
    Log("--- IDirect3D8 object 0x%08X, vtable 0x%08X ---",
        (unsigned)(UINT_PTR)d3d8, (unsigned)(UINT_PTR)vtable);

    HookOne("IDirect3D8::CreateDevice", vtable[VT8_CreateDevice], &hk_D3D8_CreateDevice,
        reinterpret_cast<void**>(&o_D3D8_CreateDevice));

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
        Log("ERROR: MH_EnableHook failed after hooking IDirect3D8::CreateDevice");

    return d3d8;
}

// =============================================================================
//  Chain B: Direct3D 9, the API the wrapper renders with
// =============================================================================

static HRESULT WINAPI hk_D3D9_Present(IDirect3DDevice9* device, const RECT* src,
    const RECT* dest, HWND window, const RGNDATA* dirty)
{
    const LONG frame = InterlockedIncrement(&c_present9);
    if (frame == 1 || frame % 600 == 0)
        Log("hk_D3D9_Present frame %d, device 0x%08X",
            frame, (unsigned)(UINT_PTR)device);
    return o_D3D9_Present(device, src, dest, window, dirty);
}

static HRESULT WINAPI hk_D3D9_PresentEx(IDirect3DDevice9Ex* device, const RECT* src,
    const RECT* dest, HWND window,
    const RGNDATA* dirty, DWORD flags)
{
    const LONG frame = InterlockedIncrement(&c_presentEx);
    if (frame == 1 || frame % 600 == 0)
        Log("hk_D3D9_PresentEx frame %d, device 0x%08X",
            frame, (unsigned)(UINT_PTR)device);
    return o_D3D9_PresentEx(device, src, dest, window, dirty, flags);
}

static HRESULT WINAPI hk_D3D9_EndScene(IDirect3DDevice9* device)
{
    return o_D3D9_EndScene(device);
}

static HRESULT WINAPI hk_D3D9_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp)
{
    Log("hk_D3D9_Reset called, %ux%u windowed %d",
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
        pp ? pp->Windowed : -1);
    return o_D3D9_Reset(device, pp);
}

// Captures the wrapper's real rendering device. This pointer is what the
// interop layer of step 2.3 needs.
static void HookD3D9Device(IDirect3DDevice9* device, const char* origin)
{
    if (!device)
        return;

    if (!g_realDevice9)
    {
        g_realDevice9 = device;
        Log("REAL D3D9 DEVICE CAPTURED via %s: 0x%08X",
            origin, (unsigned)(UINT_PTR)device);

        IDirect3DDevice9Ex* ex = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
            reinterpret_cast<void**>(&ex))) && ex)
        {
            g_realDevice9Ex = ex;
            Log("  device supports IDirect3DDevice9Ex (fast shared-surface path)");
        }
        else
        {
            Log("  device is plain IDirect3DDevice9 (no Ex interface)");
        }
    }

    if (g_d9DeviceHooked)
        return;

    void** vtable = *reinterpret_cast<void***>(device);
    Log("--- IDirect3DDevice9 vtable at 0x%08X (via %s) ---",
        (unsigned)(UINT_PTR)vtable, origin);

    HookOne("IDirect3DDevice9::Present", vtable[VT9_Present], &hk_D3D9_Present,
        reinterpret_cast<void**>(&o_D3D9_Present));
    HookOne("IDirect3DDevice9::EndScene", vtable[VT9_EndScene], &hk_D3D9_EndScene,
        reinterpret_cast<void**>(&o_D3D9_EndScene));
    HookOne("IDirect3DDevice9::Reset", vtable[VT9_Reset], &hk_D3D9_Reset,
        reinterpret_cast<void**>(&o_D3D9_Reset));

    if (g_realDevice9Ex)
        HookOne("IDirect3DDevice9Ex::PresentEx", vtable[VT9Ex_PresentEx],
            &hk_D3D9_PresentEx, reinterpret_cast<void**>(&o_D3D9_PresentEx));

    if (MH_EnableHook(MH_ALL_HOOKS) == MH_OK)
    {
        g_d9DeviceHooked = true;
        Log("D3D9 device hooks armed");
    }
    else
    {
        Log("ERROR: MH_EnableHook failed while arming D3D9 device hooks");
    }
}

static void LogPresentParams(D3DPRESENT_PARAMETERS* pp)
{
    if (!pp)
        return;

    Log("  present params: %ux%u, format %u, count %u, windowed %d, "
        "swap effect %u, interval 0x%08X, device window 0x%08X",
        pp->BackBufferWidth, pp->BackBufferHeight,
        (unsigned)pp->BackBufferFormat, pp->BackBufferCount, pp->Windowed,
        (unsigned)pp->SwapEffect, (unsigned)pp->PresentationInterval,
        (unsigned)(UINT_PTR)pp->hDeviceWindow);
}

static HRESULT WINAPI hk_D3D9_CreateDevice_plain(IDirect3D9* self, UINT adapter,
    D3DDEVTYPE type, HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** out)
{
    Log("hk_D3D9_CreateDevice[plain]: adapter %u, type %u, flags 0x%08X, focus 0x%08X",
        adapter, (unsigned)type, (unsigned)behaviorFlags,
        (unsigned)(UINT_PTR)focusWindow);
    LogPresentParams(pp);

    const HRESULT hr = o_D3D9_CreateDevice_plain(self, adapter, type, focusWindow,
        behaviorFlags, pp, out);
    if (SUCCEEDED(hr) && out && *out)
        HookD3D9Device(*out, "IDirect3D9::CreateDevice");
    else
        Log("  failed, hr = 0x%08X", (unsigned)hr);

    return hr;
}

static HRESULT WINAPI hk_D3D9_CreateDevice_ex(IDirect3D9* self, UINT adapter,
    D3DDEVTYPE type, HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** out)
{
    Log("hk_D3D9_CreateDevice[on Ex object]: adapter %u, type %u, flags 0x%08X, "
        "focus 0x%08X", adapter, (unsigned)type, (unsigned)behaviorFlags,
        (unsigned)(UINT_PTR)focusWindow);
    LogPresentParams(pp);

    const HRESULT hr = o_D3D9_CreateDevice_ex(self, adapter, type, focusWindow,
        behaviorFlags, pp, out);
    if (SUCCEEDED(hr) && out && *out)
        HookD3D9Device(*out, "IDirect3D9Ex::CreateDevice");
    else
        Log("  failed, hr = 0x%08X", (unsigned)hr);

    return hr;
}

static HRESULT WINAPI hk_D3D9Ex_CreateDeviceEx(IDirect3D9Ex* self, UINT adapter,
    D3DDEVTYPE type, HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* pp,
    D3DDISPLAYMODEEX* fullscreenMode,
    IDirect3DDevice9Ex** out)
{
    Log("hk_D3D9Ex_CreateDeviceEx: adapter %u, type %u, flags 0x%08X, focus 0x%08X",
        adapter, (unsigned)type, (unsigned)behaviorFlags,
        (unsigned)(UINT_PTR)focusWindow);
    LogPresentParams(pp);

    const HRESULT hr = o_D3D9Ex_CreateDeviceEx(self, adapter, type, focusWindow,
        behaviorFlags, pp, fullscreenMode, out);
    if (SUCCEEDED(hr) && out && *out)
        HookD3D9Device(*out, "IDirect3D9Ex::CreateDeviceEx");
    else
        Log("  failed, hr = 0x%08X", (unsigned)hr);

    return hr;
}

static IDirect3D9* WINAPI hk_Direct3DCreate9(UINT sdkVersion)
{
    Log("hk_Direct3DCreate9 called, SDK version %u", sdkVersion);

    IDirect3D9* d3d9 = o_Direct3DCreate9(sdkVersion);
    if (!d3d9)
    {
        Log("  Direct3DCreate9 returned null");
        return nullptr;
    }

    void** vtable = *reinterpret_cast<void***>(d3d9);
    Log("--- IDirect3D9 object 0x%08X, vtable 0x%08X ---",
        (unsigned)(UINT_PTR)d3d9, (unsigned)(UINT_PTR)vtable);

    HookOne("IDirect3D9::CreateDevice", vtable[VT9_CreateDevice],
        &hk_D3D9_CreateDevice_plain,
        reinterpret_cast<void**>(&o_D3D9_CreateDevice_plain));

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
        Log("ERROR: MH_EnableHook failed after hooking IDirect3D9::CreateDevice");

    return d3d9;
}

static HRESULT WINAPI hk_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    Log("hk_Direct3DCreate9Ex called, SDK version %u", sdkVersion);

    const HRESULT hr = o_Direct3DCreate9Ex(sdkVersion, out);
    if (FAILED(hr) || !out || !*out)
    {
        Log("  Direct3DCreate9Ex failed, hr = 0x%08X", (unsigned)hr);
        return hr;
    }

    void** vtable = *reinterpret_cast<void***>(*out);
    Log("--- IDirect3D9Ex object 0x%08X, vtable 0x%08X ---",
        (unsigned)(UINT_PTR)*out, (unsigned)(UINT_PTR)vtable);

    // Slot 16 of an Ex object is a DIFFERENT implementation than slot 16 of a
    // plain object. This is exactly the path version 4 was blind to.
    HookOne("IDirect3D9Ex::CreateDevice", vtable[VT9_CreateDevice],
        &hk_D3D9_CreateDevice_ex,
        reinterpret_cast<void**>(&o_D3D9_CreateDevice_ex));
    HookOne("IDirect3D9Ex::CreateDeviceEx", vtable[VT9Ex_CreateDeviceEx],
        &hk_D3D9Ex_CreateDeviceEx,
        reinterpret_cast<void**>(&o_D3D9Ex_CreateDeviceEx));

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
        Log("ERROR: MH_EnableHook failed after hooking the IDirect3D9Ex factory");

    return hr;
}

// =============================================================================
//  Export hooking
// =============================================================================

static bool HookExport(const char* moduleName, const char* exportName,
    void* detour, void** original)
{
    HMODULE module = GetModuleHandleA(moduleName);
    if (!module)
    {
        Log("HookExport: %s is not loaded yet", moduleName);
        return false;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(module, exportName));
    if (!target)
    {
        Log("HookExport: %s!%s not found", moduleName, exportName);
        return false;
    }

    Log("HookExport: %s at 0x%08X, %s at 0x%08X",
        moduleName, (unsigned)(UINT_PTR)module, exportName,
        (unsigned)(UINT_PTR)target);

    char label[128];
    wsprintfA(label, "%s!%s", moduleName, exportName);

    if (!HookOne(label, target, detour, original))
        return false;

    return MH_EnableHook(target) == MH_OK;
}

// Called from DllMain while the process is still single-threaded.
void EarlyHook_D3D8()
{
    if (MH_Initialize() != MH_OK)
    {
        Log("EarlyHook: MH_Initialize failed");
        return;
    }
    g_minHookReady = true;

    // Chain A: the API the game calls.
    if (HookExport("d3d8.dll", "Direct3DCreate8", &hk_Direct3DCreate8,
        reinterpret_cast<void**>(&o_Direct3DCreate8)))
        Log("EarlyHook: Direct3DCreate8 armed before the game touched Direct3D");

    // Chain B: the API the wrapper renders with. Both factories must be hooked,
    // because the wrapper may take either one.
    if (HookExport("d3d9.dll", "Direct3DCreate9", &hk_Direct3DCreate9,
        reinterpret_cast<void**>(&o_Direct3DCreate9)))
        Log("EarlyHook: Direct3DCreate9 armed");

    if (HookExport("d3d9.dll", "Direct3DCreate9Ex", &hk_Direct3DCreate9Ex,
        reinterpret_cast<void**>(&o_Direct3DCreate9Ex)))
        Log("EarlyHook: Direct3DCreate9Ex armed");
}

// Retry from VrThread for anything that was not mapped during DllMain.
static void LateHooks()
{
    if (!o_Direct3DCreate8)
        HookExport("d3d8.dll", "Direct3DCreate8", &hk_Direct3DCreate8,
            reinterpret_cast<void**>(&o_Direct3DCreate8));
    if (!o_Direct3DCreate9)
        HookExport("d3d9.dll", "Direct3DCreate9", &hk_Direct3DCreate9,
            reinterpret_cast<void**>(&o_Direct3DCreate9));
    if (!o_Direct3DCreate9Ex)
        HookExport("d3d9.dll", "Direct3DCreate9Ex", &hk_Direct3DCreate9Ex,
            reinterpret_cast<void**>(&o_Direct3DCreate9Ex));
}

// =============================================================================
//  Heartbeat
// =============================================================================

static const char* ModuleState(const char* name)
{
    return GetModuleHandleA(name) ? "yes" : "no";
}

static DWORD WINAPI HeartbeatThread(LPVOID)
{
    for (int i = 0; i < 60; ++i)
    {
        Sleep(5000);
        if (InterlockedCompareExchange(&g_stopHeartbeat, 0, 0) != 0)
            return 0;

        Log("heartbeat: D3D8 Present %d / EndScene %d | D3D9 Present %d, PresentEx %d "
            "| real d3d9 device %s | d3d11 %s, dxgi %s, vulkan %s",
            c_present8, c_endScene8, c_present9, c_presentEx,
            g_realDevice9 ? "captured" : "MISSING",
            ModuleState("d3d11.dll"), ModuleState("dxgi.dll"),
            ModuleState("vulkan-1.dll"));

        if (i == 3 && !g_realDevice9)
        {
            Log("DIAGNOSIS: the wrapper renders without creating a D3D9 device.");
            Log("  Check the heartbeat line for d3d11 / dxgi / vulkan to identify");
            Log("  the real backend, then step 2.3.3 (backbuffer copy) applies.");
        }
    }
    return 0;
}

// =============================================================================
//  Public entry points
// =============================================================================

bool D3D9Hook_Install()
{
    if (!g_minHookReady && MH_Initialize() != MH_OK)
    {
        Log("FATAL: MH_Initialize failed");
        return false;
    }
    g_minHookReady = true;

    char executablePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, executablePath,
        static_cast<DWORD>(sizeof(executablePath)));
    char* finalSlash = strrchr(executablePath, '\\');
    if (finalSlash)
        strcpy_s(finalSlash + 1,
            static_cast<size_t>(executablePath + sizeof(executablePath) -
                (finalSlash + 1)), "Silent_Hill_3_PC_Fix.ini");
    const bool useDirectX12 = GetPrivateProfileIntA("DirectX", "UseDirectX12",
        0, executablePath) != 0;
    g_enableFixedStep90Test8 = ReadFixedStep90Setting();
    g_enableThirteenAGFrameRateFix8 = ReadThirteenAGFrameRateFixSetting();
    g_enableProxyPresentationUnlock90Hz8 = ReadProxyPresentationUnlockSetting();
    g_enableProxyNativeTimingUnlock90Hz8 =
        ReadProxyNativeTimingUnlockSetting();
    g_enableProxyFrameTimeOverride90Hz8 =
        ReadProxyFrameTimeOverrideSetting();
    g_enableProxyVirtualMode490Hz8 = ReadProxyVirtualMode4Setting();
    g_enablePerDrawStereoProbe8 = ReadPerDrawStereoReplaySetting();
    g_enableFullPassStereo8 = ReadFullPassStereoSetting();
    g_enableGamePostProcess8 = ReadGamePostProcessSetting();
    g_stereoReplayWorldOnly8 = ReadStereoReplayWorldOnlySetting();
    g_enableWeaponRenderProbe8 =
        ReadIniIntSetting("MotionControls", "WeaponRenderProbe", 0) != 0;
    g_enableWeaponPosePrototype8 = ReadIniIntSetting("MotionControls",
        "EnableWeaponPosePrototype", 0) != 0;
    g_enableWeaponPoseRotation8 = ReadIniIntSetting("MotionControls",
        "WeaponPoseRotation", 0) != 0;
    g_weaponPoseRotateSecondaryBone8 = ReadIniIntSetting("MotionControls",
        "WeaponPoseRotateSecondaryBone", 0) != 0;
    g_weaponPoseAbsolutePosition8 = ReadIniIntSetting("MotionControls",
        "WeaponPoseAbsolutePosition", 1) != 0;
    LoadWeaponPoseProfiles8();
    LoadLeftHandPoseProfile8();
    InitializeWeaponIniHotReload8();
    g_activeWeaponPoseProfile8 = -1;
    g_weaponPoseMinimumForward8 = 0.0f;
    g_weaponPoseDisableClipping8 = true;
    if (g_enableWeaponRenderProbe8)
    {
        Log("MotionControls: WeaponRenderProbe=1; press the right grip while "
            "an equipped melee weapon is visible to capture its draw passes");
    }
    if (g_enableWeaponPosePrototype8)
    {
        Log("MotionControls: EnableWeaponPosePrototype=1; %d weapon profiles loaded from sh3vr_weapons.ini (rotation %s, absolute position %s)",
            SH3VR_WEAPON_PROFILE_COUNT8,
            g_enableWeaponPoseRotation8 ? "enabled" : "disabled",
            g_weaponPoseAbsolutePosition8 ? "enabled" : "disabled");
    }
    if (g_leftHandPoseProfile8.enabled)
    {
        Log("LeftHand: enabled from sh3vr_weapons.ini (scale %d%%, local position %d/%d/%d)",
            static_cast<int>(std::lround(g_leftHandPoseProfile8.scale * 100.0f)),
            static_cast<int>(g_leftHandPoseProfile8.position[0]),
            static_cast<int>(g_leftHandPoseProfile8.position[1]),
            static_cast<int>(g_leftHandPoseProfile8.position[2]));
    }
    g_enableRoomscale8 =
        ReadIniIntSetting("Roomscale", "Enable", 1) != 0;
    int roomscalePlayerHeightCm =
        ReadIniIntSetting("Roomscale", "PlayerHeightCm", 165);
    roomscalePlayerHeightCm = std::clamp(roomscalePlayerHeightCm, 120, 220);
    g_roomscalePlayerHeightMeters8 =
        static_cast<float>(roomscalePlayerHeightCm) / 100.0f;
    int roomscaleFullKeySpeedCmPerSecond = ReadIniIntSetting(
        "Roomscale", "FullSpeedCmPerSecond", 150);
    roomscaleFullKeySpeedCmPerSecond = std::clamp(
        roomscaleFullKeySpeedCmPerSecond, 50, 400);
    g_roomscaleFullKeySpeedMetersPerSecond8 =
        static_cast<float>(roomscaleFullKeySpeedCmPerSecond) / 100.0f;
    int worldScale = ReadIniIntSetting("Roomscale", "WorldScale",
        static_cast<int>(SH3VR_DEFAULT_WORLD_SCALE));
    worldScale = std::clamp(worldScale, 200, 600);
    // WorldScale is a user-facing perceived-size control: values below the
    // 360 baseline must make the world smaller. Internally the renderer needs
    // the inverse quantity (SH3 game units per physical metre) for IPD, head
    // translation, controllers, weapons, and the tracked hand. Keeping the
    // conversion in one place prevents the world and held objects from
    // drifting to different physical scales.
    g_worldScale8 = (SH3VR_DEFAULT_WORLD_SCALE *
        SH3VR_DEFAULT_WORLD_SCALE) / static_cast<float>(worldScale);
    g_autoLoadCameraModFirstPerson8 =
        ReadIniIntSetting("CameraMod", "AutoLoadFirstPerson", 1) != 0;
    int cameraModAutoLoadDelaySeconds = ReadIniIntSetting("CameraMod",
        "AutoLoadFirstPersonDelaySeconds", 180);
    cameraModAutoLoadDelaySeconds = std::clamp(
        cameraModAutoLoadDelaySeconds, 0, 600);
    g_cameraModAutoLoadDelaySeconds8 = static_cast<DWORD>(
        cameraModAutoLoadDelaySeconds);
    g_cameraModStartupTick8 = GetTickCount64();
    g_cameraModAutoLoadDelayLogged8 = false;
    g_cameraModAutoLoadDone8 = false;
    g_cameraModImmersiveFrameStreak8 = 0;
    g_enableCameraModSnapTurn8 =
        ReadInputSetting("EnableCameraModRightStick", 1) != 0;
    g_enableHeadTrackedFlashlight8 =
        ReadInputSetting("HeadTrackedFlashlight", 0) != 0;
    int cameraModSnapActivation =
        ReadInputSetting("CameraModSnapTurnActivation", 65);
    cameraModSnapActivation = std::clamp(cameraModSnapActivation, 20, 95);
    g_cameraModSnapTurnActivation8 =
        static_cast<float>(cameraModSnapActivation) / 100.0f;
    int cameraModSnapDegrees =
        ReadInputSetting("CameraModSnapTurnDegrees", 45);
    cameraModSnapDegrees = std::clamp(cameraModSnapDegrees, 15, 180);
    g_cameraModSnapTurnDegrees8 = static_cast<float>(cameraModSnapDegrees);
    int cameraModCharacterAlignMilliseconds =
        ReadInputSetting("CameraModCharacterAlignMilliseconds", 34);
    cameraModCharacterAlignMilliseconds = std::clamp(
        cameraModCharacterAlignMilliseconds, 16, 100);
    g_cameraModCharacterAlignMilliseconds8 =
        static_cast<DWORD>(cameraModCharacterAlignMilliseconds);
    if (g_enableFullPassStereo8)
    {
        // The full-scene path is deliberately mutually exclusive with the
        // per-draw target-switching path. It reuses the game's back buffer,
        // renders the already prepared scene twice, and captures a pair of
        // eyes without creating native D3D8 eye targets.
        if (g_enablePerDrawStereoProbe8)
        {
            Log("Full-pass stereo is enabled; disabling per-draw replay to "
                "keep the two stereo paths mutually exclusive");
            g_enablePerDrawStereoProbe8 = false;
        }
        g_enableCompositeDuplicateTest8 = true;
        g_enableSynchronizedStereo8 = true;
        g_enableNativeEyeTargetProbe8 = false;
    }
    else if (!g_enablePerDrawStereoProbe8)
    {
        g_enableNativeEyeTargetProbe8 = false;
    }
    const int pcFixFpsMode = ReadPcFixFpsMode();
    if (pcFixFpsMode == 4 && !g_enableFixedStep90Test8)
    {
        Log("PC Fix FPSMode=4 detected: native unlocked timing path is active; "
            "the VR proxy will not apply a fixed-step or QPC pacing override");
    }
    else if (pcFixFpsMode == 1 && g_enableThirteenAGFrameRateFix8)
    {
        Log("PC Fix FPSMode=1 detected: ThirteenAG 60 Hz fluctuation fix is "
            "eligible; native unlocked timing is not active");
    }
    else
    {
        Log("PC Fix FPSMode=%d detected; proxy timing overrides: "
            "fixedStep90=%s, thirteenAG=%s", pcFixFpsMode,
            g_enableFixedStep90Test8 ? "on" : "off",
            g_enableThirteenAGFrameRateFix8 ? "on" : "off");
    }
    if (g_enableProxyFrameTimeOverride90Hz8 &&
        g_enableProxyNativeTimingUnlock90Hz8)
    {
        Log("Proxy frame-time override and native timing unlock are both "
            "enabled; native timing unlock will be ignored");
        g_enableProxyNativeTimingUnlock90Hz8 = false;
    }
    if (g_enableProxyVirtualMode490Hz8)
    {
        // The virtual mode-4 path is intentionally mutually exclusive with
        // the other experimental timing paths. It switches only the in-memory
        // PC Fix selector and then uses the proxy's 90 Hz QPC deadline; no
        // internal SH3 limiter globals are written.
        g_enableProxyPresentationUnlock90Hz8 = false;
        g_enableProxyNativeTimingUnlock90Hz8 = false;
        g_enableProxyFrameTimeOverride90Hz8 = false;
    }
    ApplyProxyVirtualFPSMode4();
    ApplyProxyFrameTimeOverride90Hz();
    ApplyProxyNativeTimingUnlock90Hz();
    ApplyThirteenAGFrameRateFix();
    Log("Loaded [Timing] ProxyPresentationUnlock90Hz=%s; it duplicates only "
        "D3D8 Present after native immersive eye sources are ready",
        g_enableProxyPresentationUnlock90Hz8 ? "1" : "0");
    Log("Loaded [Timing] ProxyNativeTimingUnlock90Hz=%s; native PC Fix timing "
        "patch %s", g_enableProxyNativeTimingUnlock90Hz8 ? "1" : "0",
        g_proxyNativeTimingUnlockApplied8 ? "applied" : "inactive");
    Log("Loaded [Timing] ProxyFrameTimeOverride90Hz=%s; live SH3 frame "
        "override %s", g_enableProxyFrameTimeOverride90Hz8 ? "1" : "0",
        g_proxyFrameTimeOverrideApplied8 ? "applied" : "inactive");
    Log("Loaded [Timing] ProxyVirtualFPSMode4=%s; runtime mode-4 path %s",
        g_enableProxyVirtualMode490Hz8 ? "1" : "0",
        g_proxyVirtualMode4Applied8 ? "applied" : "inactive");
    Log("Loaded [Stereo] EnablePerDrawReplay=%s; continuous per-draw eye "
        "replay %s", g_enablePerDrawStereoProbe8 ? "1" : "0",
        g_enablePerDrawStereoProbe8 ? "enabled" : "disabled");
    Log("Loaded [Stereo] EnableFullPassStereo=%s; synchronized full-scene "
        "stereo %s", g_enableFullPassStereo8 ? "1" : "0",
        g_enableFullPassStereo8 ? "enabled" : "disabled");
    Log("Loaded [Image] EnableGamePostProcess=%s; per-eye game color "
        "parameter transfer %s", g_enableGamePostProcess8 ? "1" : "0",
        g_enableGamePostProcess8 ? "enabled" : "disabled");
    Log("Loaded [Stereo] ReplayWorldOnly=%s; per-draw replay shader "
        "filter %s", g_stereoReplayWorldOnly8 ? "1" : "0",
        g_stereoReplayWorldOnly8 ? "world shader 0x2D only" : "full allowlist");
    Log("Loaded [Roomscale] WorldScale=%d perceived-size units; internal "
        "tracking scale %.1f game units per meter",
        worldScale, g_worldScale8);
    Log("Loaded [Input] Camera Mod integrated snap turn=%s; activation %d%%, "
        "angle %d degrees, Heather alignment pulse %d ms",
        g_enableCameraModSnapTurn8 ? "1" : "0",
        cameraModSnapActivation, cameraModSnapDegrees,
        cameraModCharacterAlignMilliseconds);
    Log("Loaded [Input] HeadTrackedFlashlight=%s; SH3 flashlight projection "
        "c80-c82 follows the headset",
        g_enableHeadTrackedFlashlight8 ? "1" : "0");
    Log("Loaded [CameraMod] AutoLoadFirstPerson=%s",
        g_autoLoadCameraModFirstPerson8 ? "1" : "0");
    Log("Loaded [CameraMod] AutoLoadFirstPersonDelaySeconds=%u",
        static_cast<unsigned>(g_cameraModAutoLoadDelaySeconds8));
    Log("Loaded [Roomscale] Enable=%s; PlayerHeightCm=%d; "
        "FullSpeedCmPerSecond=%d; follow radius %d cm",
        g_enableRoomscale8 ? "1" : "0", roomscalePlayerHeightCm,
        roomscaleFullKeySpeedCmPerSecond,
        static_cast<int>(std::lround(
            SH3VR_ROOMSCALE_FOLLOW_RADIUS_METERS * 100.0f)));
    if (g_enableFixedStep90Test8)
    {
        Log("Loaded [Timing] FixedStep90Test=1 from sh3vr.ini; "
            "FPSMode=4 is required for the guarded 90 Hz test");
    }
    if (useDirectX12)
    {
        LoadLibraryA("d3d12.dll");
        LoadLibraryA("dxgi.dll");
        HookExport("d3d12.dll", "D3D12CreateDevice", &hk_D3D12CreateDevice,
            reinterpret_cast<void**>(&o_D3D12CreateDevice));
        HookExport("dxgi.dll", "CreateDXGIFactory2", &hk_CreateDXGIFactory2,
            reinterpret_cast<void**>(&o_CreateDXGIFactory2));
        Log("D3D12/DXGI creation diagnostics armed before D3D8 device startup");
    }

    LateHooks();

    if (g_enableRuntimeDiagnostics8 || g_enableCompositeDuplicateTest8 ||
        g_enableStereoSurfaceProbe8 || g_enableSynchronizedStereo8 ||
        g_enableNativeEyeTargetProbe8)
    {
        HMODULE executable = GetModuleHandleA(nullptr);
        void* sh3PrepareTarget = reinterpret_cast<BYTE*>(executable) + 0x00285B30;
        if (HookOne("SH3 frame preparation RVA 0x00285B30", sh3PrepareTarget,
            &hk_SH3_PrepareFrame, reinterpret_cast<void**>(&o_SH3_PrepareFrame)))
        {
            if (MH_EnableHook(sh3PrepareTarget) == MH_OK)
                Log("SH3 frame preparation cadence hook enabled");
            else
                Log("ERROR: failed to enable the SH3 frame preparation cadence hook");
        }

        void* sh3CompositeTarget = reinterpret_cast<BYTE*>(executable) + 0x00285B60;
        if (HookOne("SH3 frame composite RVA 0x00285B60", sh3CompositeTarget,
            &hk_SH3_RenderComposite,
            reinterpret_cast<void**>(&o_SH3_RenderComposite)))
        {
            if (MH_EnableHook(sh3CompositeTarget) == MH_OK)
                Log("SH3 frame composite hook enabled");
            else
                Log("ERROR: failed to enable the SH3 frame composite hook");
        }

        void* sh3RenderTarget = reinterpret_cast<BYTE*>(executable) + 0x00059220;
        if (HookOne("SH3 frame renderer RVA 0x00059220", sh3RenderTarget,
            &hk_SH3_RenderFrame, reinterpret_cast<void**>(&o_SH3_RenderFrame)))
        {
            if (MH_EnableHook(sh3RenderTarget) == MH_OK)
                Log("SH3 frame renderer cadence hook enabled");
            else
                Log("ERROR: failed to enable the SH3 frame renderer cadence hook");
        }

        if (g_enableFixedStep90Test8)
        {
            void* sh3PostFrameTarget = reinterpret_cast<BYTE*>(executable) +
                0x00285F90;
            if (HookOne("SH3 post-frame object update RVA 0x00285F90",
                sh3PostFrameTarget, &hk_SH3_PostFrame,
                reinterpret_cast<void**>(&o_SH3_PostFrame)))
            {
                if (MH_EnableHook(sh3PostFrameTarget) == MH_OK)
                    Log("SH3 post-frame object update hook enabled");
                else
                    Log("ERROR: failed to enable the SH3 post-frame object update hook");
            }
        }
    }

    Log("D3D9Hook_Install: %d target(s) hooked total", g_hookedCount);

    if (g_enableRuntimeDiagnostics8)
        g_heartbeat = CreateThread(nullptr, 0, HeartbeatThread, nullptr, 0, nullptr);
    return g_hookedCount > 0;
}

void D3D9Hook_Remove()
{
    InterlockedExchange(&g_stopHeartbeat, 1);

    if (g_heartbeat)
    {
        CloseHandle(g_heartbeat);
        g_heartbeat = nullptr;
    }

    if (g_minHookReady)
    {
        ReleaseLeftHandResources8();
        ReleaseStereoRenderTargets();
        ReleaseD3D12EyeTextureSharedHandles();
        ReleaseDxgiBackBufferSharedHandles();
        Interop8_Shutdown();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_minHookReady = false;
    }

    g_device8 = nullptr;
    g_hookedCount = 0;
    Log("D3D9Hook_Remove: hooks removed");
}
