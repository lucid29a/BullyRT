// BullyDX12 integration test: loads d3d9.dll (BullyDX12) by path, renders frames the way a D3D9
// game does (managed VB/IB, vertex declaration, SM3 shaders, state blocks, Reset) and checks the
// result by reading back the back buffer.
// TestHarness.exe <path-to-d3d9.dll> <frames>
// Exit code 0 = success. Prints the centre pixel (BGRA) of the last frame.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3d9on12.h>
#include <cstdio>

#include "TestShaders_VSMain.h"
#include "TestShaders_PSMain.h"

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace {
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    printf("CRASH: exception 0x%08lX at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Eip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Ebp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Esp; sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, proc, GetCurrentThread(), &sf, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset)
            break;
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line = {sizeof(line)};
        DWORD ldisp = 0;
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(sf.AddrPC.Offset)), &hm))
            GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* modName = strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod;
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym)) {
            if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ldisp, &line))
                printf("  #%02d %s!%s+0x%llx  (%s:%lu)\n", i, modName, sym->Name, disp, line.FileName, line.LineNumber);
            else
                printf("  #%02d %s!%s+0x%llx\n", i, modName, sym->Name, disp);
        } else {
            printf("  #%02d %s+0x%llx\n", i, modName,
                   sf.AddrPC.Offset - reinterpret_cast<uintptr_t>(hm));
        }
    }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        HRESULT _hr = (expr);                                                               \
        if (FAILED(_hr)) {                                                                  \
            printf("FAILED: %s -> 0x%08lX (line %d)\n", #expr, static_cast<unsigned long>(_hr), __LINE__); \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

struct Vertex { float x, y, z; DWORD color; };
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        printf("usage: TestHarness <d3d9.dll> <frames>\n");
        return 2;
    }
    const int frames = _wtoi(argv[2]);
    SetUnhandledExceptionFilter(CrashFilter);

    HMODULE d3d9 = LoadLibraryW(argv[1]);
    if (!d3d9) { printf("cannot load %ls\n", argv[1]); return 1; }
    using PFN = IDirect3D9*(WINAPI*)(UINT);
    auto create = reinterpret_cast<PFN>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!create) { printf("no Direct3DCreate9\n"); return 1; }

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BullyDX12Test";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"BullyDX12 test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr,
                             wc.hInstance, nullptr);

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 returned null\n"); return 1; }

    D3DADAPTER_IDENTIFIER9 id = {};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    printf("adapter: %s\n", id.Description);

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed             = TRUE;
    pp.SwapEffect           = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat     = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth      = 640;
    pp.BackBufferHeight     = 480;
    pp.BackBufferCount      = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow        = wnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* dev = nullptr;
    CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));

    IUnknown* on12 = nullptr;
    const GUID iid9On12 = {0xe7fda234, 0xb589, 0x4049, {0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8}};
    printf("IDirect3DDevice9On12: %s\n", SUCCEEDED(dev->QueryInterface(iid9On12, reinterpret_cast<void**>(&on12))) ? "yes" : "no");
    if (on12) on12->Release();

    const Vertex verts[] = {
        {-0.8f, -0.8f, 0.5f, 0xFFFF0000}, {-0.8f, 0.8f, 0.5f, 0xFFFF0000},
        { 0.8f,  0.8f, 0.5f, 0xFFFF0000}, { 0.8f, -0.8f, 0.5f, 0xFFFF0000}};
    const WORD idx[] = {0, 1, 2, 0, 2, 3};

    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9*  ib = nullptr;
    CHECK(dev->CreateVertexBuffer(sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr));
    CHECK(dev->CreateIndexBuffer(sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr));
    void* p = nullptr;
    CHECK(vb->Lock(0, 0, &p, 0)); memcpy(p, verts, sizeof(verts)); vb->Unlock();
    CHECK(ib->Lock(0, 0, &p, 0)); memcpy(p, idx, sizeof(idx)); ib->Unlock();

    const D3DVERTEXELEMENT9 decl[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()};
    IDirect3DVertexDeclaration9* vdecl = nullptr;
    CHECK(dev->CreateVertexDeclaration(decl, &vdecl));

    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9*  ps = nullptr;
    CHECK(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_TestVS), &vs));
    CHECK(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_TestPS), &ps));

    IDirect3DStateBlock9* sb = nullptr;
    auto recordStateBlock = [&]() -> HRESULT {
        HRESULT hr = dev->BeginStateBlock();
        if (FAILED(hr)) return hr;
        dev->SetVertexDeclaration(vdecl);
        dev->SetStreamSource(0, vb, 0, sizeof(Vertex));
        dev->SetIndices(ib);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        return dev->EndStateBlock(&sb);
    };
    CHECK(recordStateBlock());

    for (int f = 0; f < frames; ++f) {
        if (f == frames / 2) {
            sb->Release();
            sb = nullptr;
            CHECK(dev->Reset(&pp));
            CHECK(recordStateBlock());
        }
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF0000FF, 1.0f, 0));
        CHECK(dev->BeginScene());
        CHECK(sb->Apply());
        CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
        CHECK(dev->EndScene());
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }

    {
        IDirect3DDevice9On12* d9 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&d9)))) {
            ID3D12Device* d12 = nullptr;
            d9->GetD3D12Device(__uuidof(ID3D12Device), reinterpret_cast<void**>(&d12));
            ID3D12CommandQueue* q = nullptr;
            D3D12_COMMAND_QUEUE_DESC qd = {};
            if (d12) d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&q));
            IDirect3DSurface9* surfs[2] = {};
            dev->GetDepthStencilSurface(&surfs[0]);
            dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surfs[1]);
            const char* names[2] = {"depth", "backbuffer"};
            for (int i = 0; i < 2 && q; ++i) {
                ID3D12Resource* r = nullptr;
                HRESULT hr = d9->UnwrapUnderlyingResource(surfs[i], q, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r));
                if (SUCCEEDED(hr) && r) {
                    D3D12_RESOURCE_DESC rd = r->GetDesc();
                    printf("probe %s: fmt=%d flags=0x%x %llux%u samples=%u\n", names[i], rd.Format, rd.Flags, rd.Width, rd.Height, rd.SampleDesc.Count);
                    r->Release();
                    d9->ReturnUnderlyingResource(surfs[i], 0, nullptr, nullptr);
                } else {
                    printf("probe %s: unwrap failed 0x%08lX\n", names[i], static_cast<unsigned long>(hr));
                }
                surfs[i]->Release();
            }
            if (q) q->Release();
            if (d12) d12->Release();
            d9->Release();
        }
    }

    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* sys = nullptr;
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    CHECK(dev->CreateOffscreenPlainSurface(640, 480, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    CHECK(dev->GetRenderTargetData(bb, sys));
    D3DLOCKED_RECT lr = {};
    CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
    const DWORD centre = static_cast<const DWORD*>(static_cast<const void*>(static_cast<const BYTE*>(lr.pBits) + 240 * lr.Pitch))[320];
    const DWORD corner = static_cast<const DWORD*>(lr.pBits)[5];
    sys->UnlockRect();
    printf("centre pixel: R=%lu G=%lu B=%lu\n", (centre >> 16) & 0xFF, (centre >> 8) & 0xFF, centre & 0xFF);
    printf("corner pixel: R=%lu G=%lu B=%lu\n", (corner >> 16) & 0xFF, (corner >> 8) & 0xFF, corner & 0xFF);

    sys->Release();
    bb->Release();
    sb->Release();
    ps->Release();
    vs->Release();
    vdecl->Release();
    ib->Release();
    vb->Release();
    const ULONG devRefs = dev->Release();
    const ULONG d3dRefs = d3d->Release();
    printf("final refs: device=%lu d3d=%lu\n", devRefs, d3dRefs);
    DestroyWindow(wnd);
    FreeLibrary(d3d9);
    printf("OK\n");
    return 0;
}
