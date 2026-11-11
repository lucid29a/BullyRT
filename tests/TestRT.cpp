// BullyDX12 ray tracing bridge integration test.
// BullyDX12_TestRT.exe <path-to-d3d9.dll> <frames>
// Scene: ground plane (y = 0) + floating occluder quad (y = 1), sun from +X+Y. Renders like the
// game does (managed buffers, Gamebryo-named constants, HUD drawn without Z after the scene) and
// checks by readback that a ground point predicted to be in the occluder's shadow is darker than
// a lit one. Exit 0 = pass.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "TestRTShaders_VSScene.h"
#include "TestRTShaders_PSScene.h"
#include "TestRTShaders_VSHud.h"
#include "TestRTShaders_PSHud.h"
#include "TestRTShaders_VSSceneW.h"
#include "TestRTShaders_VSNoCam.h"
#include "TestRTShaders_PSPointLit.h"
#include "TestRTShaders_PSShadowRecv.h"
#include <vector>
#include <cstdio>
#include "TestSkin_VSSkin.h"
#include "TestSkin_PSSkin.h"
#include "TestSkin_VSAlpha.h"
#include "TestSkin_PSAlpha.h"
#include "TestSkin_PSAlphaClip.h"

namespace {
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

#define CHECK(expr)                                                                                     \
    do {                                                                                                \
        HRESULT _hr = (expr);                                                                           \
        if (FAILED(_hr)) {                                                                              \
            printf("FAILED: %s -> 0x%08lX (line %d)\n", #expr, static_cast<unsigned long>(_hr), __LINE__); \
            return 1;                                                                                   \
        }                                                                                               \
    } while (0)

struct V { float x, y, z; float nx, ny, nz; DWORD c; };
struct M { float m[4][4]; };

M Mul(const M& a, const M& b)
{
    M r = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
M Transpose(const M& a)
{
    M r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
    return r;
}
M Translation(float x, float y, float z) { return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {x, y, z, 1}}}; }
M LookAtLH(const float e[3], const float t[3], const float u[3])
{
    float z[3] = {t[0] - e[0], t[1] - e[1], t[2] - e[2]};
    float l = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float& v : z) v /= l;
    float x[3] = {u[1] * z[2] - u[2] * z[1], u[2] * z[0] - u[0] * z[2], u[0] * z[1] - u[1] * z[0]};
    l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (float& v : x) v /= l;
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    return {{{x[0], y[0], z[0], 0}, {x[1], y[1], z[1], 0}, {x[2], y[2], z[2], 0}, {-dot(x, e), -dot(y, e), -dot(z, e), 1}}};
}
M PerspectiveFovLH(float fovy, float aspect, float zn, float zf)
{
    const float ys = 1.0f / std::tan(fovy * 0.5f), xs = ys / aspect;
    return {{{xs, 0, 0, 0}, {0, ys, 0, 0}, {0, 0, zf / (zf - zn), 1}, {0, 0, -zn * zf / (zf - zn), 0}}};
}
void Project(const M& vp, float x, float y, float z, int W, int H, int* px, int* py)
{
    float c[4];
    for (int j = 0; j < 4; ++j) c[j] = x * vp.m[0][j] + y * vp.m[1][j] + z * vp.m[2][j] + vp.m[3][j];
    *px = static_cast<int>((c[0] / c[3] * 0.5f + 0.5f) * W);
    *py = static_cast<int>((0.5f - c[1] / c[3] * 0.5f) * H);
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { printf("usage: BullyDX12_TestRT <d3d9.dll> <frames> [internal]\n"); return 2; }
    const int frames = _wtoi(argv[2]);
    const bool internalMode = argc > 3 && wcscmp(argv[3], L"internal") == 0;
    int W = 800, H = 600;

    HMODULE d3d9 = LoadLibraryW(argv[1]);
    if (!d3d9) { printf("cannot load %ls\n", argv[1]); return 1; }
    auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9, "Direct3DCreate9"));

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BullyDX12TestRT";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"BullyDX12 RT test", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr,
                             wc.hInstance, nullptr);

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed               = FALSE;
    pp.SwapEffect             = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth        = W;
    pp.BackBufferHeight       = H;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow          = wnd;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));

    const DWORD gray = 0xFFA0A0A0, red = 0xFFC04040, hud = 0xFF20FF20;
    const V verts[] = {
        {-5, 0, -5, 0, 1, 0, gray}, {-5, 0, 5, 0, 1, 0, gray}, {5, 0, 5, 0, 1, 0, gray}, {5, 0, -5, 0, 1, 0, gray},
        {-1, 0, -1, 0, 1, 0, red}, {-1, 0, 1, 0, 1, 0, red}, {1, 0, 1, 0, 1, 0, red}, {1, 0, -1, 0, 1, 0, red},
        {-1, 1, 0, 0, 0, 1, hud}, {-0.8f, 1, 0, 0, 0, 1, hud}, {-0.8f, 0.8f, 0, 0, 0, 1, hud}, {-1, 0.8f, 0, 0, 0, 1, hud}};
    const WORD idx[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11};

    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9*  ib = nullptr;
    CHECK(dev->CreateVertexBuffer(sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr));
    CHECK(dev->CreateIndexBuffer(sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr));
    void* p = nullptr;
    CHECK(vb->Lock(0, 0, &p, 0)); memcpy(p, verts, sizeof(verts)); vb->Unlock();
    CHECK(ib->Lock(0, 0, &p, 0)); memcpy(p, idx, sizeof(idx)); ib->Unlock();

    const D3DVERTEXELEMENT9 decl[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                      {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                      {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                      D3DDECL_END()};
    IDirect3DVertexDeclaration9* vdecl = nullptr;
    CHECK(dev->CreateVertexDeclaration(decl, &vdecl));
    IDirect3DVertexShader9 *vsScene = nullptr, *vsHud = nullptr;
    IDirect3DPixelShader9 *psScene = nullptr, *psHud = nullptr;
    CHECK(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSScene), &vsScene));
    CHECK(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSScene), &psScene));
    CHECK(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSHud), &vsHud));
    CHECK(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSHud), &psHud));
    IDirect3DPixelShader9* psPointLit = nullptr;
    CHECK(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSPointLit), &psPointLit));
    IDirect3DVertexShader9* vsNoCam = nullptr;
    CHECK(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSNoCam), &vsNoCam));
    IDirect3DVertexShader9* vsSceneW = nullptr;
    IDirect3DPixelShader9*  psRecv   = nullptr;
    CHECK(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSSceneW), &vsSceneW));
    CHECK(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSShadowRecv), &psRecv));
    IDirect3DTexture9* shadowMap = nullptr;
    CHECK(dev->CreateTexture(512, 512, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &shadowMap, nullptr));
    {
        D3DLOCKED_RECT r;
        CHECK(shadowMap->LockRect(0, &r, nullptr, 0));
        for (int y = 0; y < 512; ++y) memset(static_cast<BYTE*>(r.pBits) + y * r.Pitch, 0xFF, 512 * 4);
        shadowMap->UnlockRect(0);
    }
    struct SV { float x, y, z, w0, w1, w2; DWORD bones; float nx, ny, nz, u, v; DWORD c; };
    const SV skinV[] = {
        {-1, 0, -1, 1, 0, 0, 0, 0, 1, 0, 0, 0, gray}, {-1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, gray},
        {1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1, gray},   {1, 0, -1, 1, 0, 0, 0, 0, 1, 0, 1, 0, gray},
        {-0.4f, 0, -0.5f, 1, 0, 0, 1, 0, 1, 0, 0, 0, gray}, {-0.4f, 0, 0.5f, 1, 0, 0, 1, 0, 1, 0, 0, 1, gray},
        {0.4f, 0, 0.5f, 1, 0, 0, 1, 0, 1, 0, 1, 1, gray},   {0.4f, 0, -0.5f, 1, 0, 0, 1, 0, 1, 0, 1, 0, gray}};
    const WORD skinI[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    IDirect3DVertexBuffer9* skinVb = nullptr;
    IDirect3DIndexBuffer9*  skinIb = nullptr;
    CHECK(dev->CreateVertexBuffer(sizeof(skinV), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &skinVb, nullptr));
    CHECK(dev->CreateIndexBuffer(sizeof(skinI), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &skinIb, nullptr));
    CHECK(skinVb->Lock(0, 0, &p, 0)); memcpy(p, skinV, sizeof(skinV)); skinVb->Unlock();
    CHECK(skinIb->Lock(0, 0, &p, 0)); memcpy(p, skinI, sizeof(skinI)); skinIb->Unlock();
    const D3DVERTEXELEMENT9 skinDecl[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                          {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
                                          {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
                                          {0, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                          {0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                          {0, 48, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                          D3DDECL_END()};
    IDirect3DVertexDeclaration9* skinD = nullptr;
    CHECK(dev->CreateVertexDeclaration(skinDecl, &skinD));
    IDirect3DVertexShader9* vsSkin = nullptr;
    IDirect3DPixelShader9*  psSkin = nullptr;
    CHECK(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSSkin), &vsSkin));
    CHECK(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSSkin), &psSkin));
    float bones[6][4] = {{1, 0, 0, 0},    {0, 1, 0, 1},    {0, 0, 1, -3},
                               {1, 0, 0, -1.6f}, {0, 1, 0, 0.2f}, {0, 0, 1, 1}};

    struct AV { float x, y, z, nx, ny, nz, u, v; DWORD c; };
    const AV alphaV[] = {{-1, 0, -1, 0, 1, 0, 0, 0, gray}, {-1, 0, 1, 0, 1, 0, 0, 1, gray},
                         {1, 0, 1, 0, 1, 0, 1, 1, gray},   {1, 0, -1, 0, 1, 0, 1, 0, gray}};
    const WORD alphaI[] = {0, 1, 2, 0, 2, 3};
    IDirect3DVertexBuffer9* alphaVb = nullptr;
    IDirect3DIndexBuffer9*  alphaIb = nullptr;
    CHECK(dev->CreateVertexBuffer(sizeof(alphaV), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &alphaVb, nullptr));
    CHECK(dev->CreateIndexBuffer(sizeof(alphaI), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &alphaIb, nullptr));
    CHECK(alphaVb->Lock(0, 0, &p, 0)); memcpy(p, alphaV, sizeof(alphaV)); alphaVb->Unlock();
    CHECK(alphaIb->Lock(0, 0, &p, 0)); memcpy(p, alphaI, sizeof(alphaI)); alphaIb->Unlock();
    const D3DVERTEXELEMENT9 alphaDecl[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                           {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                           {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                           {0, 32, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                           D3DDECL_END()};
    IDirect3DVertexDeclaration9* alphaD = nullptr;
    CHECK(dev->CreateVertexDeclaration(alphaDecl, &alphaD));
    IDirect3DVertexShader9* vsAlpha = nullptr;
    IDirect3DPixelShader9*  psAlpha = nullptr;
    CHECK(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSAlpha), &vsAlpha));
    CHECK(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSAlpha), &psAlpha));
    const bool clipRun = GetEnvironmentVariableW(L"BDX_TEST_CLIP", nullptr, 0) > 0;
    IDirect3DPixelShader9* psClip = nullptr;
    CHECK(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSAlphaClip), &psClip));
    IDirect3DTexture9* cutout = nullptr;
    CHECK(dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cutout, nullptr));
    {
        D3DLOCKED_RECT r;
        CHECK(cutout->LockRect(0, &r, nullptr, 0));
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits) + y * r.Pitch)[x] = x < 32 ? 0x00FFFFFF : 0xFFFFFFFF;
        cutout->UnlockRect(0);
    }
    const M worldAlpha = Translation(-3, 1, 1);
    const M waT = Transpose(worldAlpha);

    const float lightPos[4] = {50, 50, 1, 1};
    const float noBias[4]   = {0.001f, 0, 0, 0};

    float eye[3] = {0, 6, -6};
    const float at[3] = {0, 0, 0}, up[3] = {0, 1, 0};
    M view = LookAtLH(eye, at, up);
    const M proj = PerspectiveFovLH(1.0471976f, static_cast<float>(W) / H, 0.5f, 100.0f);
    M vp  = Mul(view, proj);
    M vpT = Transpose(vp);
    const M worldGround = Translation(0, 0, 0), worldOcc = Translation(0, 1, 1);
    const M wgT = Transpose(worldGround);
    M woT = Transpose(worldOcc);
    const bool moveRun = GetEnvironmentVariableW(L"BDX_TEST_MOVE", nullptr, 0) > 0;
    const float bone0x = bones[0][3], bone1x = bones[3][3], occX = woT.m[0][3];
    float camera[4] = {eye[0], eye[1], eye[2], 1};
    const float lightDir[4] = {-0.70710678f, -0.70710678f, 0, 0};

    auto gameSeesRect = [&](const char* when) {
        RECT rc = {}, rw = {};
        GetClientRect(wnd, &rc);
        GetWindowRect(wnd, &rw);
        const bool ok = !pp.Windowed && rc.right == W && rc.bottom == H && rw.right - rw.left == W && rw.bottom - rw.top == H;
        printf("%s: game's Windowed=%d, client %ldx%ld, window %ldx%ld -> %s\n", when, pp.Windowed, rc.right, rc.bottom,
               rw.right - rw.left, rw.bottom - rw.top, ok ? "OK" : "WRONG");
        return ok;
    };
    bool resolutionOk = gameSeesRect("at start");

    const int moveStart = frames / 4, moveEnd = moveStart + 40;
    int movingFrames = 0, movingWrong = 0;
    const bool lampRun = GetEnvironmentVariableW(L"BDX_TEST_LAMP", nullptr, 0) > 0;
    const bool roofRun = GetEnvironmentVariableW(L"BDX_TEST_ROOF", nullptr, 0) > 0;
    const M roofWorld = Transpose(Translation(5, 0.6f, 0));
    const bool nanRun = GetEnvironmentVariableW(L"BDX_TEST_NAN", nullptr, 0) > 0;
    const bool seamRun = GetEnvironmentVariableW(L"BDX_TEST_SEAM", nullptr, 0) > 0;
    const M seamWorld = Transpose(Translation(3, 0.08f, 1));
    AV zeroV[4];
    memcpy(zeroV, alphaV, sizeof(zeroV));
    for (AV& v : zeroV) { v.nx = v.ny = v.nz = 0; v.c = 0xFFFFFFFF; }
    IDirect3DVertexBuffer9* zeroVb = nullptr;
    CHECK(dev->CreateVertexBuffer(sizeof(zeroV), 0, 0, D3DPOOL_MANAGED, &zeroVb, nullptr));
    CHECK(zeroVb->Lock(0, 0, &p, 0)); memcpy(p, zeroV, sizeof(zeroV)); zeroVb->Unlock();
    const M zeroWorld = Transpose(Translation(3, 0.02f, 1));
    const bool flipRun = GetEnvironmentVariableW(L"BDX_TEST_FLIP", nullptr, 0) > 0;
    if (flipRun) {
        for (AV& v : zeroV) { v.nx = -0.55f; v.ny = -0.4f; v.nz = -0.73f; }
        CHECK(zeroVb->Lock(0, 0, &p, 0)); memcpy(p, zeroV, sizeof(zeroV)); zeroVb->Unlock();
    }

    IDirect3DSurface9* mirrorRT = nullptr, *mirrorDS = nullptr, *postCopy = nullptr;
    auto makeMirror = [&]() {
        CHECK(dev->CreateRenderTarget(W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &mirrorRT, nullptr));
        CHECK(dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &mirrorDS, nullptr));
        CHECK(dev->CreateRenderTarget(W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &postCopy, nullptr));
        return 0;
    };
    makeMirror();
    const float mirrorEye[3] = {4, -7, 5};
    const M mirrorVpT = Transpose(Mul(LookAtLH(mirrorEye, at, up), proj));
    const float mirrorCamera[4] = {mirrorEye[0], mirrorEye[1], mirrorEye[2], 1};

    struct FV { float p[3]; float n[3]; DWORD c; };
    const DWORD fenceCol = 0x80202020;
    const FV fenceV[4] = {{{0.6f, 0.9f, 0.5f}, {0, 0, -1}, fenceCol}, {{0.9f, 0.9f, 0.5f}, {0, 0, -1}, fenceCol},
                          {{0.6f, 0.6f, 0.5f}, {0, 0, -1}, fenceCol}, {{0.9f, 0.6f, 0.5f}, {0, 0, -1}, fenceCol}};
    IDirect3DVertexBuffer9* fenceVb = nullptr;
    CHECK(dev->CreateVertexBuffer(sizeof(fenceV), 0, 0, D3DPOOL_MANAGED, &fenceVb, nullptr));
    {
        void* vp0 = nullptr;
        CHECK(fenceVb->Lock(0, 0, &vp0, 0));
        memcpy(vp0, fenceV, sizeof(fenceV));
        fenceVb->Unlock();
    }
    AV puddleV[4];
    memcpy(puddleV, alphaV, sizeof(puddleV));
    for (AV& v : puddleV) v.c = 0x00FFFFFF;
    IDirect3DVertexBuffer9* puddleVb = nullptr;
    CHECK(dev->CreateVertexBuffer(sizeof(puddleV), 0, 0, D3DPOOL_MANAGED, &puddleVb, nullptr));
    CHECK(puddleVb->Lock(0, 0, &p, 0)); memcpy(p, puddleV, sizeof(puddleV)); puddleVb->Unlock();
    const M puddleWorld = Transpose(Translation(3, 0.3f, 1));
    const M fenceWorld = Transpose(Translation(-9, 0, 12));
    const M lodGround = Transpose(Translation(0, 0.2f, 0));

    for (int f = 0; f < frames; ++f) {
        const bool moving = !moveRun && f >= moveStart && f < moveEnd;
        if (moveRun) {
            const float walk = 1.5f * sinf(static_cast<float>(f) * 0.04f);
            bones[0][3] = bone0x + walk;
            bones[3][3] = bone1x - walk;
            woT.m[0][3] = occX + walk * 0.5f;
        }
        eye[0]    = moving ? 1.5f * sinf(static_cast<float>(f - moveStart) * 0.3f) : 0.0f;
        camera[0] = eye[0];
        view = LookAtLH(eye, at, up);
        vp   = Mul(view, proj);
        vpT  = Transpose(vp);
        if (!moveRun && f == frames / 2) {
            W = 1024;
            H = 768;
            pp.BackBufferWidth  = W;
            pp.BackBufferHeight = H;
            mirrorRT->Release();
            mirrorDS->Release();
            postCopy->Release();
            CHECK(dev->Reset(&pp));
            makeMirror();
            MoveWindow(wnd, 0, 0, W, H, TRUE);
            resolutionOk = gameSeesRect("after resolution change") && resolutionOk;
        }
        {
            IDirect3DSurface9* mainRT = nullptr, *mainDS = nullptr;
            CHECK(dev->GetRenderTarget(0, &mainRT));
            CHECK(dev->GetDepthStencilSurface(&mainDS));
            CHECK(dev->SetRenderTarget(0, mirrorRT));
            CHECK(dev->SetDepthStencilSurface(mirrorDS));
            CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
            CHECK(dev->BeginScene());
            dev->SetVertexDeclaration(vdecl);
            dev->SetStreamSource(0, vb, 0, sizeof(V));
            dev->SetIndices(ib);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            dev->SetVertexShader(vsScene);
            dev->SetPixelShader(psScene);
            dev->SetVertexShaderConstantF(4, &mirrorVpT.m[0][0], 4);
            dev->SetVertexShaderConstantF(8, mirrorCamera, 1);
            dev->SetVertexShaderConstantF(0, &wgT.m[0][0], 4);
            dev->SetPixelShaderConstantF(0, lightDir, 1);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            CHECK(dev->EndScene());
            CHECK(dev->SetRenderTarget(0, mainRT));
            CHECK(dev->SetDepthStencilSurface(mainDS));
            mainRT->Release();
            mainDS->Release();
        }
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF4060A0, 1.0f, 0));
        CHECK(dev->BeginScene());
        dev->SetVertexDeclaration(vdecl);
        dev->SetStreamSource(0, vb, 0, sizeof(V));
        dev->SetIndices(ib);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetVertexShader(internalMode ? vsSceneW : vsScene);
        dev->SetPixelShader(internalMode ? psRecv : psScene);
        if (internalMode) {
            dev->SetTexture(0, shadowMap);
            dev->SetPixelShaderConstantF(0, &vpT.m[0][0], 4);
            dev->SetPixelShaderConstantF(4, noBias, 1);
            dev->SetPixelShaderConstantF(5, lightPos, 1);
        }
        dev->SetVertexShaderConstantF(4, &vpT.m[0][0], 4);
        dev->SetVertexShaderConstantF(8, camera, 1);
        if (!internalMode) dev->SetPixelShaderConstantF(0, lightDir, 1);
        dev->SetVertexShaderConstantF(0, &wgT.m[0][0], 4);
        CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
        dev->SetVertexShaderConstantF(0, &woT.m[0][0], 4);
        static const bool ghostRun = GetEnvironmentVariableW(L"BDX_TEST_GHOST", nullptr, 0) > 0;
        if (!ghostRun || f < frames - 40) CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 4, 6, 2));
        dev->SetVertexDeclaration(skinD);
        dev->SetStreamSource(0, skinVb, 0, sizeof(SV));
        dev->SetIndices(skinIb);
        dev->SetVertexShader(vsSkin);
        dev->SetPixelShader(psSkin);
        dev->SetVertexShaderConstantF(0, &bones[0][0], 6);
        dev->SetVertexShaderConstantF(195, &vpT.m[0][0], 4);
        dev->SetVertexShaderConstantF(199, camera, 1);
        CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
        CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 4, 6, 2));
        dev->SetVertexDeclaration(alphaD);
        dev->SetStreamSource(0, alphaVb, 0, sizeof(AV));
        dev->SetIndices(alphaIb);
        dev->SetVertexShader(vsAlpha);
        dev->SetPixelShader(clipRun ? psClip : psAlpha);
        dev->SetTexture(0, cutout);
        dev->SetVertexShaderConstantF(200, &waT.m[0][0], 4);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, clipRun ? FALSE : TRUE);
        dev->SetRenderState(D3DRS_ALPHAREF, 128);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetTexture(0, nullptr);
        dev->SetVertexDeclaration(vdecl);
        dev->SetStreamSource(0, vb, 0, sizeof(V));
        dev->SetIndices(ib);
        {
            static const float degenerate[16] = {0};
            dev->SetVertexShader(vsNoCam);
            dev->SetPixelShader(psScene);
            dev->SetVertexShaderConstantF(4, degenerate, 4);
            dev->SetVertexShaderConstantF(0, &wgT.m[0][0], 4);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            dev->SetVertexShaderConstantF(4, &vpT.m[0][0], 4);
        }
        {
            dev->SetVertexDeclaration(alphaD);
            dev->SetStreamSource(0, puddleVb, 0, sizeof(AV));
            dev->SetIndices(alphaIb);
            dev->SetVertexShader(vsAlpha);
            dev->SetPixelShader(psAlpha);
            dev->SetTexture(0, shadowMap);
            dev->SetVertexShaderConstantF(200, &puddleWorld.m[0][0], 4);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetTexture(0, nullptr);
            dev->SetVertexDeclaration(vdecl);
            dev->SetStreamSource(0, vb, 0, sizeof(V));
            dev->SetIndices(ib);
            dev->SetVertexShader(vsScene);
            dev->SetPixelShader(psHud);
            dev->SetVertexShaderConstantF(0, &fenceWorld.m[0][0], 4);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        }
        if (seamRun) {
            dev->SetVertexDeclaration(alphaD);
            dev->SetStreamSource(0, alphaVb, 0, sizeof(AV));
            dev->SetIndices(alphaIb);
            dev->SetVertexShader(vsAlpha);
            dev->SetPixelShader(psAlpha);
            dev->SetTexture(0, shadowMap);
            dev->SetVertexShaderConstantF(200, &seamWorld.m[0][0], 4);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetTexture(0, nullptr);
            dev->SetVertexDeclaration(vdecl);
            dev->SetStreamSource(0, vb, 0, sizeof(V));
            dev->SetIndices(ib);
        }
        if (nanRun || flipRun) {
            dev->SetVertexDeclaration(alphaD);
            dev->SetStreamSource(0, zeroVb, 0, sizeof(AV));
            dev->SetIndices(alphaIb);
            dev->SetVertexShader(vsAlpha);
            dev->SetPixelShader(psAlpha);
            dev->SetTexture(0, shadowMap);
            dev->SetVertexShaderConstantF(200, &zeroWorld.m[0][0], 4);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            dev->SetTexture(0, nullptr);
            dev->SetVertexDeclaration(vdecl);
            dev->SetStreamSource(0, vb, 0, sizeof(V));
            dev->SetIndices(ib);
        }
        if (roofRun) {
            dev->SetVertexShader(vsScene);
            dev->SetPixelShader(psScene);
            dev->SetVertexShaderConstantF(0, &roofWorld.m[0][0], 4);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        }
        {
            const float lampPos[4] = {0, 3.0f, 1, 1}, lampCol[4] = {lampRun ? 3.0f : 0.0f, 0.0f, 0.0f, 1}, lampAtt[4] = {1, 0.05f, 0.02f, 0};
            dev->SetVertexShader(vsScene);
            dev->SetPixelShader(psPointLit);
            dev->SetPixelShaderConstantF(10, lampPos, 1);
            dev->SetPixelShaderConstantF(11, lampCol, 1);
            dev->SetPixelShaderConstantF(12, lampAtt, 1);
            dev->SetVertexShaderConstantF(0, &wgT.m[0][0], 4);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        }
        if (f < frames / 8) {
            dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            dev->SetVertexShader(vsScene);
            dev->SetPixelShader(psScene);
            dev->SetVertexShaderConstantF(0, &lodGround.m[0][0], 4);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        }
        {
            dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->SetVertexShader(vsHud);
            dev->SetPixelShader(psHud);
            dev->SetStreamSource(0, fenceVb, 0, sizeof(FV));
            CHECK(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
            dev->SetStreamSource(0, vb, 0, sizeof(V));
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        }
        {
            const M proxyWorld = Transpose(Translation(3, 1.0f, 1));
            dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
            dev->SetVertexShader(vsScene);
            dev->SetPixelShader(psScene);
            dev->SetVertexShaderConstantF(0, &proxyWorld.m[0][0], 4);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 4, 6, 2));
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        }
        IDirect3DSurface9* sceneNow = nullptr;
        CHECK(dev->GetRenderTarget(0, &sceneNow));
        CHECK(dev->StretchRect(sceneNow, nullptr, postCopy, nullptr, D3DTEXF_POINT));
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetVertexShader(vsHud);
        dev->SetPixelShader(psHud);
        CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 8, 4, 12, 2));
        {
            const int rw = W / 8, rh = H / 8;
            const RECT above = {0, 0, W, H - rh}, right = {rw, H - rh, W, H};
            CHECK(dev->StretchRect(postCopy, &above, sceneNow, &above, D3DTEXF_POINT));
            CHECK(dev->StretchRect(postCopy, &right, sceneNow, &right, D3DTEXF_POINT));
        }
        sceneNow->Release();
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0));
        {
            const M farAway = Transpose(Translation(40, 0, 0));
            dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            dev->SetVertexShader(vsScene);
            dev->SetPixelShader(psScene);
            dev->SetVertexShaderConstantF(0, &farAway.m[0][0], 4);
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
        }
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetVertexShader(vsHud);
        dev->SetPixelShader(psHud);
        CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 8, 4, 12, 2));
        CHECK(dev->EndScene());
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
        Sleep(16);
        if (moving) {
            IDirect3DSurface9 *bbM = nullptr, *sysM = nullptr;
            dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbM);
            dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysM, nullptr);
            dev->GetRenderTargetData(bbM, sysM);
            D3DLOCKED_RECT lm = {};
            sysM->LockRect(&lm, nullptr, D3DLOCK_READONLY);
            auto lumM = [&](int x, int y) {
                const DWORD c = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lm.pBits) + y * lm.Pitch)[x];
                return ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF);
            };
            int ax, ay, bx, by;
            Project(vp, -1.0f, 0, 1.0f, W, H, &ax, &ay);
            Project(vp, 3.0f, 0, 1.0f, W, H, &bx, &by);
            const int s = lumM(ax, ay), l = lumM(bx, by);
            ++movingFrames;
            if (s >= l * 0.8) { ++movingWrong; printf("moving frame %d: shadow %d, lit %d -> WRONG\n", f, s, l); }
            sysM->UnlockRect();
            sysM->Release();
            bbM->Release();
        }
        if (f == 3) {
            IDirect3DSurface9 *bb0 = nullptr, *sys0 = nullptr;
            dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb0);
            dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys0, nullptr);
            dev->GetRenderTargetData(bb0, sys0);
            D3DLOCKED_RECT l0 = {};
            sys0->LockRect(&l0, nullptr, D3DLOCK_READONLY);
            auto lum = [&](int x, int y) {
                const DWORD c = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(l0.pBits) + y * l0.Pitch)[x];
                return ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF);
            };
            int ax, ay, bx, by;
            Project(vp, -1.0f, 0, 1.0f, W, H, &ax, &ay);
            Project(vp, 3.0f, 0, 1.0f, W, H, &bx, &by);
            printf("frame 3: shadow %d, lit %d\n", lum(ax, ay), lum(bx, by));
            sys0->UnlockRect();
            sys0->Release();
            bb0->Release();
        }
    }

    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    CHECK(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    CHECK(dev->GetRenderTargetData(bb, sys));
    D3DLOCKED_RECT lr = {};
    CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
    {
        wchar_t savePath[512] = {};
        if (GetEnvironmentVariableW(L"BDX_TEST_SAVE", savePath, 512) > 0) {
            FILE* sf = nullptr;
            _wfopen_s(&sf, savePath, L"wb");
            if (sf) {
                const int row = (W * 3 + 3) & ~3;
                const unsigned size = 54 + row * H;
                unsigned char hdr[54] = {'B', 'M'};
                memcpy(hdr + 2, &size, 4);
                const unsigned off = 54, dib = 40;
                memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
                const int bw = static_cast<int>(W), bh = static_cast<int>(H);
                memcpy(hdr + 18, &bw, 4); memcpy(hdr + 22, &bh, 4);
                hdr[26] = 1; hdr[28] = 24;
                fwrite(hdr, 1, 54, sf);
                std::vector<unsigned char> line(row);
                for (int y = bh - 1; y >= 0; --y) {
                    const unsigned char* s = static_cast<const unsigned char*>(lr.pBits) + y * lr.Pitch;
                    for (int x = 0; x < bw; ++x) { line[x * 3] = s[x * 4]; line[x * 3 + 1] = s[x * 4 + 1]; line[x * 3 + 2] = s[x * 4 + 2]; }
                    fwrite(line.data(), 1, row, sf);
                }
                fclose(sf);
            }
        }
    }
    auto px = [&](int x, int y) {
        const DWORD c = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x];
        return ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF);
    };
    int sx, sy, lx, ly, hx = 40, hy = 30;
    Project(vp, -1.0f, 0, 1.0f, W, H, &sx, &sy);
    Project(vp, 3.0f, 0, 1.0f, W, H, &lx, &ly);
    const int shadow = px(sx, sy), lit = px(lx, ly), hudPx = px(hx, hy);
    {
        const int y = ly;
        int x0 = 0;
        for (; x0 < W; ++x0) {
            const DWORD c = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x0];
            if (((c >> 16) & 0xFF) == (c & 0xFF)) break;
        }
        printf("ground edge at x=%d: luminance %d %d %d, interior %d\n", x0, px(x0, y), px(x0 + 1, y), px(x0 + 2, y), px(x0 + 20, y));
    }
    {
        const DWORD c = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + ly * lr.Pitch)[lx];
        printf("lit point RGB = (%lu, %lu, %lu)\n", (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }
    auto lumAt = [&](float x, float y, float z) { int a, b; Project(vp, x, y, z, W, H, &a, &b); return px(a, b); };
    const int skinShadow = lumAt(-1.9f, 0, -3), skinLit = lumAt(3, 0, -3);
    const int recvShadow = lumAt(-1.6f, 0.2f, 1), recvLit = lumAt(-1.95f, 0.2f, 1);
    const int cutSolid = lumAt(-3.5f, 0, 1), cutHole = lumAt(-4.5f, 0, 1);
    printf("skinned casts:    shadow %d, lit %d\n", skinShadow, skinLit);
    printf("skinned receives: shadow %d, lit %d\n", recvShadow, recvLit);
    printf("alpha cut-out:    solid part %d, transparent part %d\n", cutSolid, cutHole);
    int rx, ry, gx, gy;
    Project(vp, -0.7f, 0, 1.0f, W, H, &rx, &ry);
    Project(vp, -1.9f, 0, -3, W, H, &gx, &gy);
    auto rgbAt = [&](int x, int y, int* r, int* g, int* b) {
        const DWORD c = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x];
        *r = (c >> 16) & 0xFF; *g = (c >> 8) & 0xFF; *b = c & 0xFF;
    };
    int r1, g1, b1, r2, g2, b2;
    rgbAt(rx, ry, &r1, &g1, &b1);
    rgbAt(gx, gy, &r2, &g2, &b2);
    printf("shadow colour: under red (%d %d %d), under grey (%d %d %d)\n", r1, g1, b1, r2, g2, b2);
    const bool bleed = r1 > g1 + 3 && r2 <= g2 + 3;
    int fr, fg, fb, nr, ng, nb;
    rgbAt(W * 17 / 20, H * 5 / 40, &fr, &fg, &fb);
    rgbAt(W * 14 / 20, H * 5 / 40, &nr, &ng, &nb);
    const bool fenceOk = fr + fg + fb < nr + ng + nb - 30;
    int ux, uy, sx2, sy2, ur, ug, ub, sr2, sg2, sb2;
    Project(vp, 0, 0, 1, W, H, &ux, &uy);
    Project(vp, 3.0f, 0, -2.5f, W, H, &sx2, &sy2);
    rgbAt(ux, uy, &ur, &ug, &ub);
    rgbAt(sx2, sy2, &sr2, &sg2, &sb2);
    printf("lamp: under the occluder (%d %d %d), beside it (%d %d %d)\n", ur, ug, ub, sr2, sg2, sb2);
    const bool lampOk = !lampRun || (sr2 - sg2) > (ur - ug) + 100;
    printf("lamp shadow -> %s\n", lampOk ? "OK" : "WRONG");
    if (lampRun) { printf(lampOk ? "PASS lamp\n" : "FAIL lamp\n"); return lampOk ? 0 : 3; }
    if (seamRun) {
        int ax2, ay2, bx2, by2, ar, ag, ab, br3, bg3, bb3;
        Project(vp, 3.0f, 0, 1.0f, W, H, &ax2, &ay2);
        Project(vp, 3.0f, 0, -2.5f, W, H, &bx2, &by2);
        rgbAt(ax2, ay2, &ar, &ag, &ab);
        rgbAt(bx2, by2, &br3, &bg3, &bb3);
        const bool seamOk = ar + ag + ab > (br3 + bg3 + bb3) * 0.85;
        printf("under an overlapping road piece: (%d %d %d), open ground (%d %d %d)\n%s\n", ar, ag, ab, br3, bg3, bb3, seamOk ? "PASS seam" : "FAIL seam (dark patch)");
        return seamOk ? 0 : 3;
    }
    if (flipRun) {
        int a1x, a1y, a2x, a2y, cr, cg, cb, nr2, ng2, nb2;
        Project(vp, 3.0f, 0, 1.0f, W, H, &a1x, &a1y);
        Project(vp, 3.0f, 0, -1.5f, W, H, &a2x, &a2y);
        rgbAt(a1x, a1y, &cr, &cg, &cb);
        rgbAt(a2x, a2y, &nr2, &ng2, &nb2);
        const bool ok = cr + cg + cb > (nr2 + ng2 + nb2) * 0.7;
        printf("decal with normals into the ground: (%d %d %d), ground beside it (%d %d %d)\n%s\n", cr, cg, cb, nr2, ng2, nb2, ok ? "PASS flip" : "FAIL flip (black decal)");
        return ok ? 0 : 3;
    }
    if (nanRun) {
        int a1x, a1y, a2x, a2y, cr, cg, cb, nr2, ng2, nb2;
        Project(vp, 3.0f, 0, 1.0f, W, H, &a1x, &a1y);
        Project(vp, 3.0f, 0, -1.5f, W, H, &a2x, &a2y);
        rgbAt(a1x, a1y, &cr, &cg, &cb);
        rgbAt(a2x, a2y, &nr2, &ng2, &nb2);
        const bool nanOk = cr + cg + cb > 240 && nr2 + ng2 + nb2 > 240;
        printf("surface without normals: (%d %d %d), ground beside it (%d %d %d)\n%s\n", cr, cg, cb, nr2, ng2, nb2, nanOk ? "PASS nan" : "FAIL nan (black stain)");
        return nanOk ? 0 : 3;
    }
    if (roofRun) {
        int lx2, ly2, roofR, roofG, roofB;
        Project(vp, 3.0f, 0, 1.0f, W, H, &lx2, &ly2);
        rgbAt(lx2, ly2, &roofR, &roofG, &roofB);
        const bool roofOk = roofR + roofG + roofB < 400;
        printf("under a roof seen from inside: (%d %d %d)\n%s\n", roofR, roofG, roofB, roofOk ? "PASS roof" : "FAIL roof (the sun leaks in)");
        return roofOk ? 0 : 3;
    }
    int bx, by, br2, bg2, bb2;
    Project(vp, -9, 0, 12, W, H, &bx, &by);
    rgbAt(bx, by, &br2, &bg2, &bb2);
    const bool fenceLit = std::abs(br2 - 160) + std::abs(bg2 - 160) + std::abs(bb2 - 160) > 20;
    printf("blended plane against the sky: (%d %d %d) -> %s\n", br2, bg2, bb2, fenceLit ? "OK" : "WRONG (no ray traced light)");
    printf("transparent plane: (%d %d %d), sky beside it (%d %d %d) -> %s\n", fr, fg, fb, nr, ng, nb, fenceOk ? "OK" : "WRONG");
    const bool extraOk = skinShadow < skinLit * 0.8 && recvShadow < recvLit * 0.8 && cutSolid < cutHole * 0.8 && bleed && resolutionOk && movingFrames > 0 && movingWrong == 0 && fenceOk && fenceLit && lampOk;
    printf("camera moving: %d frames, %d wrong\n", movingFrames, movingWrong);
    { const int sp[3][2] = {{W - 6, 6}, {W / 2, 6}, {W - 6, H / 3}}; for (const auto& q : sp) { int sr, sg, sb; rgbAt(q[0], q[1], &sr, &sg, &sb); printf("sky pixel (%d,%d): (%d %d %d)\n", q[0], q[1], sr, sg, sb); } }
    sys->UnlockRect();
    printf("shadow point (%d,%d) luminance %d | lit point (%d,%d) luminance %d | HUD %d\n", sx, sy, shadow, lx, ly, lit, hudPx);
    const double ratio = lit > 0 ? static_cast<double>(shadow) / lit : 1.0;
    printf("shadow/lit ratio: %.3f\n", ratio);

    sys->Release(); bb->Release();
    mirrorRT->Release(); mirrorDS->Release(); postCopy->Release(); fenceVb->Release(); puddleVb->Release(); zeroVb->Release();
    shadowMap->Release(); psRecv->Release(); vsSceneW->Release(); vsNoCam->Release(); psPointLit->Release();
    psHud->Release(); vsHud->Release(); psScene->Release(); vsScene->Release();
    vdecl->Release(); ib->Release(); vb->Release();
    const ULONG refs = dev->Release();
    d3d->Release();
    DestroyWindow(wnd);
    printf("final device refs: %lu\n", refs);
    if (ratio < 0.8 && extraOk) { printf("PASS: static, skinned and alpha-tested geometry all block rays; skinned receives\n"); return 0; }
    printf("FAIL: no shadow detected\n");
    return 3;
}
