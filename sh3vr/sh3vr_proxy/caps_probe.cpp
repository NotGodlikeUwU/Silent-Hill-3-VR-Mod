// =============================================================================
//  sh3vr - caps_probe.cpp  (version 2)
//
//  Runs once on the first intercepted frame and answers a single question:
//  what does the Steam006 d3d8 wrapper actually render with?
//
//  Version 5 of d3d9_hook.cpp hooks the Direct3DCreate9 / Direct3DCreate9Ex
//  exports, so in the normal case the real rendering device is already captured
//  by the time this probe runs and g_realDevice9 is non-null. The manual object
//  scan below is only a fallback for the case where the wrapper obtained its
//  device through a path we did not intercept.
//
//  Notes:
//    * Interface GUIDs are taken through __uuidof so that dxguid.lib is not
//      required at link time.
//    * MODULEENTRY32W plus an explicit conversion is used because this project
//      is built with UNICODE defined; the ANSI formatter would otherwise print
//      only the first character of each module name.
// =============================================================================

#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>

struct IDirect3DDevice8;

extern void Log(const char* format, ...);

// Owned here, written either by the factory hooks in d3d9_hook.cpp or by the
// fallback scan below. Consumed by the interop layer of step 2.3.
IDirect3DDevice9* g_realDevice9 = nullptr;
IDirect3DDevice9Ex* g_realDevice9Ex = nullptr;

// -----------------------------------------------------------------------------
//  Low level memory helpers
// -----------------------------------------------------------------------------

static bool IsReadable(const void* address, SIZE_T size)
{
    if (!address)
        return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;

    const BYTE* regionStart = static_cast<const BYTE*>(mbi.BaseAddress);
    const BYTE* regionEnd = regionStart + mbi.RegionSize;
    return static_cast<const BYTE*>(address) + size <= regionEnd;
}

static bool IsExecutable(const void* address)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executable) != 0;
}

// Resolves the module that owns an address, for readable diagnostics.
static void DescribeAddress(const char* label, const void* address)
{
    if (!address)
    {
        Log("  %-22s : null", label);
        return;
    }

    HMODULE module = nullptr;
    char name[MAX_PATH] = "<unknown>";

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(address), &module) && module)
    {
        char full[MAX_PATH] = {};
        if (GetModuleFileNameA(module, full, MAX_PATH))
        {
            const char* slash = strrchr(full, '\\');
            (void)lstrcpynA(name, slash ? slash + 1 : full, MAX_PATH);
        }
    }

    Log("  %-22s : 0x%08X  in %s (base 0x%08X, offset 0x%X)",
        label, (unsigned)(UINT_PTR)address, name,
        (unsigned)(UINT_PTR)module,
        (unsigned)((UINT_PTR)address - (UINT_PTR)module));
}

static void LogModuleMap()
{
    Log("--- module map ---");

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        Log("  CreateToolhelp32Snapshot failed, error %u", GetLastError());
        return;
    }

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            char name[MAX_PATH] = {};
            WideCharToMultiByte(CP_ACP, 0, entry.szModule, -1, name, MAX_PATH,
                nullptr, nullptr);

            Log("  %-30s base 0x%08X  size 0x%08X",
                name,
                (unsigned)(UINT_PTR)entry.modBaseAddr,
                (unsigned)entry.modBaseSize);
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    Log("--- module map end ---");
}

// Reports which graphics backends are present, to identify the real renderer if
// no D3D9 device can be found at all.
static void LogBackendCandidates()
{
    static const char* const candidates[] =
    {
        "d3d8.dll", "d3d9.dll", "d3d9on12.dll", "d3d10.dll", "d3d11.dll",
        "d3d12.dll", "dxgi.dll", "vulkan-1.dll", "opengl32.dll",
        "nvd3dum.dll", "nvoglv32.dll", "dxvk_d3d9.dll", "ddraw.dll",
        "Silent_Hill_3_PC_Fix.dll"
    };

    Log("--- backend candidates ---");
    for (int i = 0; i < ARRAYSIZE(candidates); ++i)
    {
        HMODULE module = GetModuleHandleA(candidates[i]);
        Log("  %-26s : %s (0x%08X)", candidates[i],
            module ? "LOADED" : "absent", (unsigned)(UINT_PTR)module);
    }
    Log("--- backend candidates end ---");
}

// -----------------------------------------------------------------------------
//  Fallback: finding a real D3D9 device inside the wrapper object
// -----------------------------------------------------------------------------

static bool VtableBelongsTo(void** vtable, HMODULE module, int slotsToCheck)
{
    if (!module || !vtable)
        return false;

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const UINT_PTR low = reinterpret_cast<UINT_PTR>(module);
    const UINT_PTR high = low + nt->OptionalHeader.SizeOfImage;

    for (int i = 0; i < slotsToCheck; ++i)
    {
        if (!IsReadable(&vtable[i], sizeof(void*)))
            return false;

        const UINT_PTR fn = reinterpret_cast<UINT_PTR>(vtable[i]);
        if (fn < low || fn >= high)
            return false;
        if (!IsExecutable(vtable[i]))
            return false;
    }
    return true;
}

// Tests one candidate pointer: its vtable must live inside d3d9.dll, and
// QueryInterface must confirm the interface.
static IDirect3DDevice9* TryCandidate(void* candidate, HMODULE d3d9,
    const char* where, unsigned offset)
{
    if (!candidate || (reinterpret_cast<UINT_PTR>(candidate) & 3) != 0)
        return nullptr;
    if (!IsReadable(candidate, sizeof(void*)))
        return nullptr;

    void** vtable = *static_cast<void***>(candidate);
    if (!IsReadable(vtable, sizeof(void*) * 20))
        return nullptr;
    if (!VtableBelongsTo(vtable, d3d9, 20))
        return nullptr;

    Log("  candidate in %s at offset 0x%03X -> object 0x%08X, vtable 0x%08X",
        where, offset, (unsigned)(UINT_PTR)candidate, (unsigned)(UINT_PTR)vtable);

    // The vtable lives inside d3d9.dll, so calling QueryInterface is safe.
    IUnknown* unknown = static_cast<IUnknown*>(candidate);
    IDirect3DDevice9* device = nullptr;

    if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDirect3DDevice9),
        reinterpret_cast<void**>(&device))) && device)
    {
        Log("  CONFIRMED: IDirect3DDevice9 at 0x%08X", (unsigned)(UINT_PTR)device);
        return device;   // caller keeps this reference
    }

    Log("  candidate rejected: not an IDirect3DDevice9");
    return nullptr;
}

// Scans the wrapper device object, then one level deeper through any pointer
// fields it contains, because translation layers often keep the D3D9 device in
// a nested state structure rather than directly in the device object.
static IDirect3DDevice9* FindRealD3D9Device(void* wrapperObject, SIZE_T scanBytes)
{
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9)
    {
        Log("  d3d9.dll is not loaded, cannot search for a D3D9 device");
        return nullptr;
    }

    Log("--- scanning wrapper object 0x%08X for a d3d9 device (0x%X bytes) ---",
        (unsigned)(UINT_PTR)wrapperObject, (unsigned)scanBytes);

    void** fields = static_cast<void**>(wrapperObject);
    const SIZE_T slots = scanBytes / sizeof(void*);

    // Pass one: direct fields of the wrapper device object.
    for (SIZE_T i = 0; i < slots; ++i)
    {
        if (!IsReadable(&fields[i], sizeof(void*)))
            break;

        IDirect3DDevice9* device = TryCandidate(fields[i], d3d9, "wrapper device",
            (unsigned)(i * sizeof(void*)));
        if (device)
            return device;
    }

    Log("  pass one found nothing, descending one level");

    // Pass two: fields of any nested object the wrapper points at.
    for (SIZE_T i = 0; i < slots; ++i)
    {
        if (!IsReadable(&fields[i], sizeof(void*)))
            break;

        void* nested = fields[i];
        if (!nested || (reinterpret_cast<UINT_PTR>(nested) & 3) != 0)
            continue;
        if (!IsReadable(nested, sizeof(void*) * 64))
            continue;

        void** nestedFields = static_cast<void**>(nested);
        for (SIZE_T j = 0; j < 64; ++j)
        {
            IDirect3DDevice9* device = TryCandidate(nestedFields[j], d3d9,
                "nested object",
                (unsigned)(j * sizeof(void*)));
            if (device)
                return device;
        }
    }

    Log("  no IDirect3DDevice9 found in either pass");
    return nullptr;
}

// -----------------------------------------------------------------------------
//  Capability dump
// -----------------------------------------------------------------------------

static void DumpDevice9Capabilities(IDirect3DDevice9* device)
{
    Log("--- D3D9 device capabilities ---");

    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(device->GetCreationParameters(&cp)))
    {
        Log("  adapter          : %u", cp.AdapterOrdinal);
        Log("  device type      : %u", (unsigned)cp.DeviceType);
        Log("  behavior flags   : 0x%08X", (unsigned)cp.BehaviorFlags);
        Log("  focus window     : 0x%08X", (unsigned)(UINT_PTR)cp.hFocusWindow);
    }

    IDirect3DSwapChain9* chain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain)
    {
        D3DPRESENT_PARAMETERS pp = {};
        if (SUCCEEDED(chain->GetPresentParameters(&pp)))
        {
            Log("  backbuffer       : %ux%u, format %u, count %u",
                pp.BackBufferWidth, pp.BackBufferHeight,
                (unsigned)pp.BackBufferFormat, pp.BackBufferCount);
            Log("  windowed         : %d", pp.Windowed);
            Log("  swap effect      : %u", (unsigned)pp.SwapEffect);
            Log("  present interval : 0x%08X", (unsigned)pp.PresentationInterval);
            Log("  device window    : 0x%08X", (unsigned)(UINT_PTR)pp.hDeviceWindow);
        }
        chain->Release();
    }

    IDirect3DSurface9* renderTarget = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &renderTarget)) && renderTarget)
    {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(renderTarget->GetDesc(&desc)))
            Log("  render target 0  : %ux%u, format %u, usage 0x%08X, pool %u, "
                "multisample %u",
                desc.Width, desc.Height, (unsigned)desc.Format,
                (unsigned)desc.Usage, (unsigned)desc.Pool,
                (unsigned)desc.MultiSampleType);
        renderTarget->Release();
    }

    D3DCAPS9 caps = {};
    if (SUCCEEDED(device->GetDeviceCaps(&caps)))
    {
        Log("  vertex shader    : %u.%u",
            (unsigned)((caps.VertexShaderVersion >> 8) & 0xFF),
            (unsigned)(caps.VertexShaderVersion & 0xFF));
        Log("  pixel shader     : %u.%u",
            (unsigned)((caps.PixelShaderVersion >> 8) & 0xFF),
            (unsigned)(caps.PixelShaderVersion & 0xFF));
        Log("  max texture      : %ux%u", caps.MaxTextureWidth, caps.MaxTextureHeight);
        Log("  simultaneous RTs : %u", caps.NumSimultaneousRTs);
    }

    // Shared-surface support decides which branch of step 2.3 we take.
    if (!g_realDevice9Ex)
    {
        IDirect3DDevice9Ex* ex = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
            reinterpret_cast<void**>(&ex))) && ex)
            g_realDevice9Ex = ex;   // keep the reference for the interop layer
    }

    Log("  IDirect3DDevice9Ex : %s", g_realDevice9Ex
        ? "AVAILABLE (fast shared-surface path is usable)"
        : "NOT available (plain D3D9 device)");

    // Can this device create a shareable render target at all?
    IDirect3DSurface9* probe = nullptr;
    HANDLE shared = nullptr;
    const HRESULT hr = device->CreateRenderTarget(256, 256, D3DFMT_X8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE,
        &probe, &shared);
    Log("  CreateRenderTarget(shared) hr = 0x%08X, handle 0x%08X",
        (unsigned)hr, (unsigned)(UINT_PTR)shared);
    if (probe)
        probe->Release();

    // A plain texture with a shared handle is the actual mechanism used by the
    // interop bridge, so probe that too.
    IDirect3DTexture9* sharedTexture = nullptr;
    HANDLE textureHandle = nullptr;
    const HRESULT texHr = device->CreateTexture(256, 256, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &sharedTexture, &textureHandle);
    Log("  CreateTexture(shared) hr = 0x%08X, handle 0x%08X",
        (unsigned)texHr, (unsigned)(UINT_PTR)textureHandle);
    if (sharedTexture)
        sharedTexture->Release();

    Log("--- capabilities end ---");
}

// -----------------------------------------------------------------------------
//  Entry point, called once from hk_D3D8_Present
// -----------------------------------------------------------------------------

void VR_ProbeDeviceCapabilities(IDirect3DDevice8* wrapperDevice)
{
    Log("=== caps probe start ===");

    if (!wrapperDevice)
    {
        Log("  wrapper device is null, aborting probe");
        Log("=== caps probe end ===");
        return;
    }

    LogModuleMap();
    LogBackendCandidates();

    void** wrapperVtable = *reinterpret_cast<void***>(wrapperDevice);
    Log("--- wrapper device 0x%08X ---", (unsigned)(UINT_PTR)wrapperDevice);
    DescribeAddress("wrapper vtable", wrapperVtable);
    DescribeAddress("wrapper Reset", wrapperVtable[14]);
    DescribeAddress("wrapper Present", wrapperVtable[15]);
    DescribeAddress("wrapper EndScene", wrapperVtable[35]);

    if (g_realDevice9)
    {
        Log("  real D3D9 device already captured by the factory hook: 0x%08X",
            (unsigned)(UINT_PTR)g_realDevice9);
    }
    else
    {
        Log("  factory hooks did not capture a device, falling back to a scan");
        g_realDevice9 = FindRealD3D9Device(wrapperDevice, 0x800);
    }

    if (g_realDevice9)
    {
        DumpDevice9Capabilities(g_realDevice9);
    }
    else
    {
        Log("  FALLBACK REQUIRED: no D3D9 device behind the wrapper.");
        Log("  Check the backend candidate list above to identify the real");
        Log("  renderer. Step 2.3.3 (GetBackBuffer plus system-memory copy)");
        Log("  applies in that case.");
    }

    Log("=== caps probe end ===");
}