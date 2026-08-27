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
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <vector>
#include "MinHook.h"
#include "shared_frame.h"

// Forward declarations for the Direct3D 8 interfaces used across the project.
struct IDirect3D8;
struct IDirect3DDevice8;
struct IDirect3DSurface8;
struct IDirect3DBaseTexture8;
struct IDirect3DVertexBuffer8;

extern void Log(const char* format, ...);

// interop_d3d8.cpp
extern void Interop8_GrabFrame(struct IDirect3DDevice8* device);
extern void Interop8_OnDeviceReset();
extern void Interop8_Shutdown();
extern bool Interop8_ReadHeadPose(Sh3VrHeadPose* output);
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
typedef HRESULT(STDMETHODCALLTYPE* PFN8_GetRenderState)(IDirect3DDevice8*,
    DWORD, DWORD*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_GetTexture)(IDirect3DDevice8*, DWORD,
    IDirect3DBaseTexture8**);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_GetTextureStageState)(
    IDirect3DDevice8*, DWORD, DWORD, DWORD*);
typedef HRESULT(STDMETHODCALLTYPE* PFN8_SetTextureStageState)(
    IDirect3DDevice8*, DWORD, DWORD, DWORD);
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
static constexpr float SH3VR_GAME_UNITS_PER_METER = 380.0f;
static constexpr float SH3VR_IPD_METERS = 0.064f;
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

static void LogRenderCallStack(const char* operation);
static void LogFirstStereoDrawForShader(const char* method, DWORD primitiveType,
    UINT primitiveCount, bool indexed, bool vertexBufferDraw);
static bool HookOne(const char* name, void* target, void* detour,
    void** original);

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
    // load order, so both are unsuitable identifiers. Screen-space effects,
    // shadows, blur, decals, and UI use the separate *UP paths and remain
    // governed by the strict allowlist. Tiny buffered strips are billboards
    // or composites and stay excluded as well.
    return primitiveCount >= (indexed ? 16u : 32u);
}

static bool IsHeavySceneDynamicReplayDraw(DWORD shader, bool indexed,
    DWORD primitiveType, UINT primitiveCount, bool vertexBufferDraw)
{
    // The batched heavy-scene pass reproduces static world geometry. Dynamic
    // actor lists are consumed by SH3's first composite call, so preserve the
    // small actor/material variants from that original pass. Shader 0x2F is a
    // high-volume room/emissive material family in the bakery (650 draws in a
    // single frame); replaying it per draw consumes the bounded budget before
    // Heather and enemy draws are reached. The full-scene pass renders 0x2F
    // once per eye, so it must stay out of the dynamic replay budget.
    if (shader == 0x2F)
        return false;
    return shader != 0x2D && ShouldReplayStereoDraw(shader, indexed,
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

static HRESULT WINAPI hk_D3D8_SetVertexShaderConstant(IDirect3DDevice8* device,
    DWORD startRegister, const void* data, DWORD registerCount)
{
    if (g_enableRuntimeDiagnostics8)
        CaptureD3D8ShaderConstants(startRegister, data, registerCount);

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
    const HRESULT result = o_D3D8_DrawIndexedPrimitive(device, primitiveType,
        minIndex, vertexCount, startIndex, primitiveCount);
    TraceScreenSpaceDraw("DrawIndexedPrimitive", true, primitiveType,
        primitiveCount, 0, _ReturnAddress());
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
    const HRESULT result = o_D3D8_DrawIndexedPrimitiveUP(device, primitiveType,
        minIndex, vertexCount, primitiveCount, indices, indexFormat, vertices,
        stride);
    TraceScreenSpaceDraw("DrawIndexedPrimitiveUP", true, primitiveType,
        primitiveCount, stride, _ReturnAddress());
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
            Log("6-DOF head pose calibration started; keep the headset level");
            return false;
        }
        if (presentFrame - g_headPoseCalibrationStartFrame8 <
            SH3VR_HEAD_POSE_CALIBRATION_FRAMES)
        {
            return false;
        }

        std::memcpy(g_headOrientationReference8, current,
            sizeof(g_headOrientationReference8));
        std::memcpy(g_headPositionReference8, pose.position,
            sizeof(g_headPositionReference8));
        g_headOrientationReferenceValid8 = true;
        Log("6-DOF head pose reference captured after the calibration delay");
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
    position[0] = headLocalDelta[0] * SH3VR_GAME_UNITS_PER_METER;
    position[1] = -headLocalDelta[1] * SH3VR_GAME_UNITS_PER_METER;
    position[2] = -headLocalDelta[2] * SH3VR_GAME_UNITS_PER_METER;

    // Render parallel stereo cameras on alternating game frames. Left is
    // negative camera X and right is positive camera X.
    if (g_enableAlternatingStereo8 || g_applyStereoEyeOffset8)
    {
        const float eyeSign = g_renderEye8 == 0 ? -1.0f : 1.0f;
        position[0] += eyeSign * 0.5f * SH3VR_IPD_METERS *
            SH3VR_GAME_UNITS_PER_METER;
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
            "at %.0f game units per meter", SH3VR_GAME_UNITS_PER_METER);
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
    std::vector<std::uint8_t>* transformed)
{
    if (!vertices || !transformed || vertexCount == 0 || stride < 16 ||
        stride > 1024 || sourceViewport.Width == 0 ||
        sourceViewport.Height == 0 || g_stereoTargetWidth8 == 0 ||
        g_stereoTargetHeight8 == 0)
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

    for (UINT index = 0; index < vertexCount; ++index)
    {
        std::uint8_t* vertex = transformed->data() +
            static_cast<std::size_t>(index) * stride;
        float x = 0.0f;
        float y = 0.0f;
        std::memcpy(&x, vertex + 0, sizeof(x));
        std::memcpy(&y, vertex + 4, sizeof(y));
        x = (x - static_cast<float>(sourceViewport.X)) * scale + left;
        y = (y - static_cast<float>(sourceViewport.Y)) * scale + top;
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

    std::vector<std::uint8_t> transformed;
    if (!BuildUiOverlayVertices(sourceViewport, vertices, vertexCount, stride,
        &transformed))
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
        if (!g_loggedUiStereoOverlay8)
        {
            g_loggedUiStereoOverlay8 = true;
            Log("Stereo UI overlay enabled: FVF 0x%08X, source viewport "
                "%ux%u, native target %ux%u, centered aspect-preserving "
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

    std::vector<std::uint8_t> transformed;
    if (!BuildUiOverlayVertices(sourceViewport, source.data(), vertexCount,
        stride, &transformed))
    {
        return false;
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
        if (!g_loggedUiStereoOverlay8)
        {
            g_loggedUiStereoOverlay8 = true;
            Log("Stereo UI vertex-buffer overlay enabled: shader/FVF "
                "0x%08X, atlas 512x512, source viewport %ux%u, native "
                "target %ux%u", g_currentVertexShader8,
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

    std::vector<std::uint8_t> transformed;
    if (!BuildUiOverlayVertices(sourceViewport, vertices, vertexCount, stride,
        &transformed))
    {
        return false;
    }

    IDirect3DSurface8* originalColor = nullptr;
    IDirect3DSurface8* originalDepth = nullptr;
    if (!BeginUiStereoPairPass(&originalColor, &originalDepth))
        return false;

    bool leftComplete = SetUiOverlayViewport() &&
        SUCCEEDED(o_D3D8_DrawIndexedPrimitiveUP(g_device8, primitiveType,
            minIndex, vertexCount, primitiveCount, indices, indexFormat,
            transformed.data(), stride));
    bool rightComplete = false;
    if (SwitchStereoPairEye(1) && SetUiOverlayViewport())
    {
        rightComplete = SUCCEEDED(o_D3D8_DrawIndexedPrimitiveUP(g_device8,
            primitiveType, minIndex, vertexCount, primitiveCount, indices,
            indexFormat, transformed.data(), stride));
    }
    EndStereoPairPass(originalColor, originalDepth);

    if (leftComplete && rightComplete)
    {
        InterlockedIncrement(&g_uiStereoOverlayDraws8);
        if (!g_loggedUiStereoOverlay8)
        {
            g_loggedUiStereoOverlay8 = true;
            Log("Stereo indexed UI overlay enabled: FVF 0x%08X, source "
                "viewport %ux%u, native target %ux%u, centered "
                "aspect-preserving layout", g_currentVertexShader8,
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
            return o_D3D8_SetTransform(device, state, &vrView);
        if (g_enableExperimentalHeadRotation8 &&
            ApplyHeadRotationToMainCamera(*matrix, vrView))
            return o_D3D8_SetTransform(device, state, &vrView);
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

    if (InterlockedIncrement(&c_endScene8) == 1)
    {
        Log("hk_D3D8_EndScene first call, device 0x%08X", (unsigned)(UINT_PTR)device);
        LogRenderCaller("D3D8 EndScene", _ReturnAddress());
    }

    return o_D3D8_EndScene(device);
}

static HRESULT WINAPI hk_D3D8_Reset(IDirect3DDevice8* device, void* presentParams)
{
    Log("hk_D3D8_Reset called");
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
