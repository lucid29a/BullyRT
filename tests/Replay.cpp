// BullyDX12 replay: renders a world dump (BullyRTBridge Ctrl+Alt+F12: world_*.obj + world_*.txt) through the
// real d3d9 proxy and ray tracing pipeline, from the camera of the game, with the lights of that frame.
// Every surface is a plain grey material, like the Blender reference built from the same dump.

#include <set>
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "TestRTShaders_VSScene.h"
#include "TestRTShaders_PSScene.h"
#include "TestRTShaders_PSPointLit.h"
#include "TestRTShaders_VSHud.h"
#include "TestRTShaders_PSHud.h"
#include "TestSkin_VSAlpha.h"
#include "TestSkin_PSAlpha.h"
#include "TestSkin_PSGlass.h"

namespace {
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

struct V { float x, y, z, nx, ny, nz, u, v; DWORD c; };
struct Group {
    std::string name, tex; std::vector<V> tris; IDirect3DVertexBuffer9* vb = nullptr; IDirect3DTexture9* t = nullptr; bool alpha = false, glass = false;
    bool hasState = false, raysOnly = false; DWORD state[11] = {}; int drawId = -1; std::wstring texDir;
};
struct Light { float pos[3], col[3], att[3]; };

bool Check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) printf("FAILED %s 0x%08lx\n", what, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) { printf("usage: BullyDX12_Replay <d3d9.dll> <world.obj> <out.bmp> [frames] [width]\n"); return 2; }
    const int frames = argc > 4 ? _wtoi(argv[4]) : 400;
    int W = argc > 5 ? _wtoi(argv[5]) : 1280;

    std::vector<std::vector<float>> posPool;
    std::vector<Group> groups;
    auto loadObj = [&](const std::wstring& objPath, bool raysOnly) -> bool {
        std::vector<float> pos, uvs;
        std::vector<DWORD> cols;
        const std::wstring objDir = objPath.substr(0, objPath.find_last_of(L"\\/") + 1);
        FILE* f = _wfopen(objPath.c_str(), L"r");
        if (!f) { printf("cannot open %ls\n", objPath.c_str()); return false; }
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == 'g') {
                groups.push_back({});
                groups.back().name = line + 2;
                groups.back().raysOnly = raysOnly;
                groups.back().texDir = objDir;
                if (!strncmp(line + 2, "draw", 4)) groups.back().drawId = atoi(line + 6);
                const char* fl = strstr(line, "_flags");
                groups.back().alpha = fl && (atoi(fl + 6) & 1);
                groups.back().glass = fl && (atoi(fl + 6) & 8);
            } else if (!strncmp(line, "usemtl ", 7) && !groups.empty()) {
                groups.back().tex = line + 7;
                while (!groups.back().tex.empty() && (groups.back().tex.back() == '\n' || groups.back().tex.back() == '\r')) groups.back().tex.pop_back();
            } else if (line[0] == 'v' && line[1] == 'c') {
                unsigned c = 0xFFFFFFFFu;
                if (sscanf_s(line + 3, "%x", &c) == 1) cols.push_back(c);
            } else if (line[0] == 'v' && line[1] == 't') {
                float u, v;
                if (sscanf_s(line + 3, "%f %f", &u, &v) == 2) { uvs.push_back(u); uvs.push_back(1.0f - v); }
            } else if (line[0] == 'v') {
                float x, y, z;
                if (sscanf_s(line + 2, "%f %f %f", &x, &y, &z) == 3) { pos.push_back(x); pos.push_back(y); pos.push_back(z); }
            } else if (line[0] == 'f' && !groups.empty()) {
                unsigned a, b, c, ta = 0, tb = 0, tc = 0;
                if (sscanf_s(line + 2, "%u/%u %u/%u %u/%u", &a, &ta, &b, &tb, &c, &tc) != 6) {
                    if (sscanf_s(line + 2, "%u %u %u", &a, &b, &c) != 3) continue;
                    ta = tb = tc = 0;
                }
                const unsigned tv[3] = {ta, tb, tc};
                const unsigned vi[3] = {a, b, c};
                const float* p[3] = {&pos[(a - 1) * 3], &pos[(b - 1) * 3], &pos[(c - 1) * 3]};
                float e1[3], e2[3], n[3];
                for (int i = 0; i < 3; ++i) { e1[i] = p[1][i] - p[0][i]; e2[i] = p[2][i] - p[0][i]; }
                n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
                const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l < 1e-12f) continue;
                for (float& v : n) v /= l;
                for (int k = 0; k < 3; ++k) {
                    const float u = tv[k] && tv[k] * 2 <= uvs.size() ? uvs[(tv[k] - 1) * 2] : 0.f, v = tv[k] && tv[k] * 2 <= uvs.size() ? uvs[(tv[k] - 1) * 2 + 1] : 0.f;
                    groups.back().tris.push_back({p[k][0], p[k][1], p[k][2], n[0], n[1], n[2], u, v, vi[k] <= cols.size() ? cols[vi[k] - 1] : ((groups.back().tex.empty() || groups.back().tex == "plain") ? 0xFFA0A0A0 : 0xFFFFFFFF)});
                }
            }
        }
        fclose(f);
        return true;
    };
    if (!loadObj(argv[2], false)) return 1;
    if (const char* ra = getenv("REPLAY_RAYS")) {
        const size_t own = groups.size();
        std::wstring rp(ra, ra + strlen(ra));
        if (loadObj(rp, true)) {
            std::set<std::string> seen;
            auto keyOf = [](const Group& g) { char s[96]; sprintf_s(s, "%.1f %.1f %.1f %zu", g.tris[0].x, g.tris[0].y, g.tris[0].z, g.tris.size()); return std::string(s); };
            for (size_t i = 0; i < own; ++i) if (!groups[i].tris.empty()) seen.insert(keyOf(groups[i]));
            size_t dropped = 0;
            for (size_t i = own; i < groups.size(); ++i)
                if (!groups[i].tris.empty() && seen.count(keyOf(groups[i]))) { groups[i].tris.clear(); ++dropped; }
            printf("rays beyond the frame: %zu groups, %zu already in it\n", groups.size() - own, dropped);
        }
    }
    std::wstring txtPath = argv[2];
    txtPath = txtPath.substr(0, txtPath.size() - 4) + L".txt";
    float cam[4] = {0, 0, 0, 1}, vp[16] = {};
    std::vector<Light> lights;
    {
        FILE* f = _wfopen(txtPath.c_str(), L"r");
        if (!f) { printf("cannot open %ls\n", txtPath.c_str()); return 1; }
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "camera ", 7)) sscanf_s(line + 7, "%f %f %f", &cam[0], &cam[1], &cam[2]);
            if (!strncmp(line, "drawstate ", 10)) {
                int id = -1;
                DWORD s[11] = {};
                if (sscanf_s(line + 10, "%d %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &id, &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6], &s[7], &s[8], &s[9], &s[10]) == 12)
                    for (Group& g : groups) if (g.drawId == id) { memcpy(g.state, s, sizeof(s)); g.hasState = true; break; }
            }
            if (!strncmp(line, "viewproj ", 9)) {
                char* p = line + 9;
                for (float& v : vp) v = strtof(p, &p);
            }
            if (!strncmp(line, "light ", 6)) {
                Light l = {};
                float r = 0;
                if (sscanf_s(line + 6, "%f %f %f radius %f colour %f %f %f att %f %f %f", &l.pos[0], &l.pos[1], &l.pos[2], &r, &l.col[0], &l.col[1],
                             &l.col[2], &l.att[0], &l.att[1], &l.att[2]) == 10)
                    lights.push_back(l);
            }
        }
        fclose(f);
    }
    if (const char* mx = getenv("REPLAY_MIRRORX")) {
        const float x0 = static_cast<float>(atof(mx));
        for (int r = 0; r < 4; ++r) { vp[r * 4 + 3] += vp[r * 4 + 0] * 2.0f * x0; vp[r * 4 + 0] = -vp[r * 4 + 0]; }
        cam[0] = 2.0f * x0 - cam[0];
        printf("camera mirrored about x = %.3f\n", x0);
    }
    std::vector<std::string> glassTex;
    if (const char* gl = getenv("REPLAY_GLASS")) {
        std::string s = gl;
        size_t a = 0;
        while (a < s.size()) { size_t b = s.find(',', a); if (b == std::string::npos) b = s.size(); glassTex.push_back("tex" + s.substr(a, b - a)); a = b + 1; }
    }
    for (Group& g : groups) for (const std::string& gt : glassTex) if (g.tex == gt) g.glass = true;
    const float sx = std::sqrt(vp[0] * vp[0] + vp[1] * vp[1] + vp[2] * vp[2]);
    const float sy = std::sqrt(vp[4] * vp[4] + vp[5] * vp[5] + vp[6] * vp[6]);
    int H = static_cast<int>(W * sx / sy) & ~1;
    printf("dump: %zu groups, %zu lights, camera (%.1f %.1f %.1f), %dx%d\n", groups.size(), lights.size(), cam[0], cam[1], cam[2], W, H);

    HMODULE d3d9 = LoadLibraryW(argv[1]);
    if (!d3d9) { printf("cannot load %ls\n", argv[1]); return 1; }
    auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9, "Direct3DCreate9"));
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BullyDX12Replay";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"BullyDX12 replay", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed               = TRUE;
    pp.SwapEffect             = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth        = W;
    pp.BackBufferHeight       = H;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow          = wnd;
    if (getenv("REPLAY_NOVSYNC")) pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    if (!Check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev), "CreateDevice")) return 1;

    const D3DVERTEXELEMENT9 decl[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                      {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                      {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                      {0, 32, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                      D3DDECL_END()};
    IDirect3DVertexDeclaration9* vdecl = nullptr;
    dev->CreateVertexDeclaration(decl, &vdecl);
    IDirect3DVertexShader9 *vsScene = nullptr, *vsHud = nullptr;
    IDirect3DPixelShader9 *psScene = nullptr, *psLight = nullptr, *psHud = nullptr;
    dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSScene), &vsScene);
    dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSHud), &vsHud);
    dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSScene), &psScene);
    dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSPointLit), &psLight);
    dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSHud), &psHud);
    IDirect3DVertexShader9* vsTex = nullptr;
    IDirect3DPixelShader9*  psTex = nullptr;
    dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_VSAlpha), &vsTex);
    dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSAlpha), &psTex);
    IDirect3DPixelShader9* psGlass = nullptr;
    dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PSGlass), &psGlass);
    std::wstring dir = argv[2];
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    size_t textured = 0;
    for (Group& g : groups) {
        if (g.tex.empty() || g.tex == "plain") continue;
        FILE* tf = _wfopen(((g.texDir.empty() ? dir : g.texDir) + L"textures/" + std::wstring(g.tex.begin(), g.tex.end()) + L".tga").c_str(), L"rb");
        if (!tf) continue;
        unsigned char h[18];
        fread(h, 1, 18, tf);
        const int tw = h[12] | (h[13] << 8), th = h[14] | (h[15] << 8);
        std::vector<unsigned char> px(static_cast<size_t>(tw) * th * 4);
        fread(px.data(), 1, px.size(), tf);
        fclose(tf);
        static const bool useMips = getenv("REPLAY_MIPS") != nullptr;
        if (FAILED(dev->CreateTexture(tw, th, useMips ? 0 : 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g.t, nullptr))) continue;
        int lw = tw, lh = th;
        const DWORD levels = g.t->GetLevelCount();
        for (DWORD lv = 0; lv < levels; ++lv) {
            D3DLOCKED_RECT r;
            g.t->LockRect(lv, &r, nullptr, 0);
            for (int y = 0; y < lh; ++y) memcpy(static_cast<BYTE*>(r.pBits) + y * r.Pitch, &px[static_cast<size_t>(y) * lw * 4], lw * 4);
            g.t->UnlockRect(lv);
            if (lv + 1 < levels) {
                const int nw = (std::max)(lw / 2, 1), nh = (std::max)(lh / 2, 1);
                std::vector<unsigned char> half(static_cast<size_t>(nw) * nh * 4);
                for (int y = 0; y < nh; ++y)
                    for (int x = 0; x < nw; ++x)
                        for (int c = 0; c < 4; ++c) {
                            int sum = 0;
                            for (int dy = 0; dy < 2; ++dy)
                                for (int dx = 0; dx < 2; ++dx) sum += px[(static_cast<size_t>((std::min)(y * 2 + dy, lh - 1)) * lw + (std::min)(x * 2 + dx, lw - 1)) * 4 + c];
                            half[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<unsigned char>(sum / 4);
                        }
                px.swap(half);
                lw = nw; lh = nh;
            }
        }
        ++textured;
    }
    printf("textured groups: %zu\n", textured);

    size_t drawn = 0;
    for (Group& g : groups) {
        if (g.tris.size() < 6) continue;
        const UINT bytes = static_cast<UINT>(g.tris.size() * sizeof(V));
        if (FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g.vb, nullptr))) continue;
        void* p = nullptr;
        g.vb->Lock(0, 0, &p, 0);
        memcpy(p, g.tris.data(), bytes);
        g.vb->Unlock();
        ++drawn;
    }
    const V hudQuad[6] = {{-1, 1, 0, 0, 0, 1, 0xFF000000}, {-0.99f, 1, 0, 0, 0, 1, 0xFF000000}, {-1, 0.99f, 0, 0, 0, 1, 0xFF000000},
                          {-0.99f, 1, 0, 0, 0, 1, 0xFF000000}, {-0.99f, 0.99f, 0, 0, 0, 1, 0xFF000000}, {-1, 0.99f, 0, 0, 0, 1, 0xFF000000}};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    const bool blendCutouts = getenv("REPLAY_BLEND") != nullptr;
    std::vector<std::string> blendTex;
    if (const char* bt = getenv("REPLAY_BLENDTEX")) {
        std::string s = bt;
        size_t a = 0;
        while (a < s.size()) { size_t b = s.find(',', a); if (b == std::string::npos) b = s.size(); blendTex.push_back("tex" + s.substr(a, b - a)); a = b + 1; }
    }
    std::vector<std::string> addTex;
    if (const char* at = getenv("REPLAY_ADDTEX")) {
        std::string s = at;
        size_t a = 0;
        while (a < s.size()) { size_t b = s.find(',', a); if (b == std::string::npos) b = s.size(); addTex.push_back("tex" + s.substr(a, b - a)); a = b + 1; }
    }
    int seqStart = -1, seqStep = 1, seqCount = 0;
    if (const char* sq = getenv("REPLAY_SEQ")) sscanf_s(sq, "%d,%d,%d", &seqStart, &seqStep, &seqCount);
    float walk[3] = {};
    int walkFirst = 0, walkLast = -1;
    if (const char* wk = getenv("REPLAY_WALK")) sscanf_s(wk, "%f,%f,%f,%d,%d", &walk[0], &walk[1], &walk[2], &walkFirst, &walkLast);
    auto saveBmp = [&](const std::wstring& path) {
        IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        dev->GetRenderTargetData(bb, sys);
        D3DLOCKED_RECT lr = {};
        sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        FILE* out = _wfopen(path.c_str(), L"wb");
        if (out) {
            const int row = W * 3, pad = (4 - row % 4) % 4;
            BITMAPFILEHEADER fh = {0x4D42, static_cast<DWORD>(54 + (row + pad) * H), 0, 0, 54};
            BITMAPINFOHEADER ih = {40, W, H, 1, 24, BI_RGB, 0, 2835, 2835, 0, 0};
            fwrite(&fh, sizeof(fh), 1, out);
            fwrite(&ih, sizeof(ih), 1, out);
            std::vector<unsigned char> buf(row + pad, 0);
            for (int y = H - 1; y >= 0; --y) {
                const DWORD* src = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch);
                for (int x = 0; x < W; ++x) { buf[x * 3] = src[x] & 0xFF; buf[x * 3 + 1] = (src[x] >> 8) & 0xFF; buf[x * 3 + 2] = (src[x] >> 16) & 0xFF; }
                fwrite(buf.data(), 1, buf.size(), out);
            }
            fclose(out);
        }
        sys->UnlockRect();
        sys->Release(); bb->Release();
    };
    for (int f = 0; f < frames; ++f) {
        if (f >= walkFirst && f <= walkLast) {
            for (int r = 0; r < 4; ++r) vp[r * 4 + 3] -= vp[r * 4] * walk[0] + vp[r * 4 + 1] * walk[1] + vp[r * 4 + 2] * walk[2];
            for (int c = 0; c < 3; ++c) cam[c] += walk[c];
        }
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF8090A0, 1.0f, 0);
        dev->BeginScene();
        dev->SetVertexDeclaration(vdecl);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        if (getenv("REPLAY_MIPS")) {
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        }
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetVertexShader(vsScene);
        dev->SetPixelShader(psScene);
        dev->SetVertexShaderConstantF(0, identity, 4);
        dev->SetVertexShaderConstantF(4, vp, 4);
        dev->SetVertexShaderConstantF(8, cam, 1);
        for (Group& g : groups) {
            if (!g.vb) continue;
            if (g.t) {
                dev->SetVertexShader(vsTex);
                dev->SetPixelShader(g.glass ? psGlass : psTex);
                dev->SetVertexShaderConstantF(195, vp, 4);
                dev->SetVertexShaderConstantF(199, cam, 1);
                dev->SetVertexShaderConstantF(200, identity, 4);
                dev->SetTexture(0, g.t);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, g.alpha);
                dev->SetRenderState(D3DRS_ALPHAREF, 128);
                dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
                bool blendThis = blendCutouts && g.alpha;
                for (const std::string& bn : blendTex) blendThis = blendThis || g.tex == bn;
                if (blendThis) {
                    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                } else {
                    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                }
            } else {
                dev->SetVertexShader(vsScene);
                dev->SetPixelShader(psScene);
                dev->SetTexture(0, nullptr);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            }
            if (g.hasState) {
                dev->SetRenderState(D3DRS_ZENABLE, g.state[0]);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, g.state[1]);
                dev->SetRenderState(D3DRS_ZFUNC, g.state[2]);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, g.state[3]);
                dev->SetRenderState(D3DRS_SRCBLEND, g.state[4]);
                dev->SetRenderState(D3DRS_DESTBLEND, g.state[5]);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, g.state[6]);
                dev->SetRenderState(D3DRS_ALPHAREF, g.state[7]);
                dev->SetRenderState(D3DRS_ALPHAFUNC, g.state[8]);
                static const bool noCull = getenv("REPLAY_NOCULL") != nullptr;
                dev->SetRenderState(D3DRS_CULLMODE, noCull ? static_cast<DWORD>(D3DCULL_NONE) : g.state[9]);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, g.state[10]);
            }
            if (g.raysOnly) dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
            dev->SetStreamSource(0, g.vb, 0, sizeof(V));
            dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(g.tris.size() / 3));
            if (g.hasState || g.raysOnly) {
                dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
                dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
                dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            }
        }
        if (!addTex.empty()) {
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            for (Group& g : groups) {
                bool addThis = false;
                for (const std::string& an : addTex) addThis = addThis || g.tex == an;
                if (!addThis || !g.vb || !g.t) continue;
                dev->SetVertexShader(vsTex);
                dev->SetPixelShader(psTex);
                dev->SetTexture(0, g.t);
                dev->SetStreamSource(0, g.vb, 0, sizeof(V));
                dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(g.tris.size() / 3));
            }
            dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        }
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetVertexShader(vsScene);
        dev->SetPixelShader(psLight);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
        for (const Light& l : lights) {
            const float p4[4] = {l.pos[0], l.pos[1], l.pos[2], 1}, c4[4] = {l.col[0], l.col[1], l.col[2], 1}, a4[4] = {l.att[0], l.att[1], l.att[2], 0};
            dev->SetPixelShaderConstantF(10, p4, 1);
            dev->SetPixelShaderConstantF(11, c4, 1);
            dev->SetPixelShaderConstantF(12, a4, 1);
            for (Group& g : groups)
                if (g.vb) { dev->SetStreamSource(0, g.vb, 0, sizeof(V)); dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2); break; }
        }
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetVertexShader(vsHud);
        dev->SetPixelShader(psHud);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, hudQuad, sizeof(V));
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        if (seqStart >= 0 && f >= seqStart && (f - seqStart) % seqStep == 0 && (f - seqStart) / seqStep < seqCount) {
            std::wstring seqPath = argv[3];
            seqPath = seqPath.substr(0, seqPath.size() - 4) + L"_" + std::to_wstring(f) + L".bmp";
            saveBmp(seqPath);
        }
    }

    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT lr = {};
    sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    FILE* out = _wfopen(argv[3], L"wb");
    if (out) {
        const int row = W * 3, pad = (4 - row % 4) % 4;
        BITMAPFILEHEADER fh = {0x4D42, static_cast<DWORD>(54 + (row + pad) * H), 0, 0, 54};
        BITMAPINFOHEADER ih = {40, W, H, 1, 24, BI_RGB, 0, 2835, 2835, 0, 0};
        fwrite(&fh, sizeof(fh), 1, out);
        fwrite(&ih, sizeof(ih), 1, out);
        std::vector<unsigned char> buf(row + pad, 0);
        for (int y = H - 1; y >= 0; --y) {
            const DWORD* src = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch);
            for (int x = 0; x < W; ++x) { buf[x * 3] = src[x] & 0xFF; buf[x * 3 + 1] = (src[x] >> 8) & 0xFF; buf[x * 3 + 2] = (src[x] >> 16) & 0xFF; }
            fwrite(buf.data(), 1, buf.size(), out);
        }
        fclose(out);
    }
    sys->UnlockRect();
    printf("rendered %zu groups, %d frames -> %ls\n", drawn, frames, argv[3]);
    sys->Release(); bb->Release();
    for (Group& g : groups) { if (g.vb) g.vb->Release(); if (g.t) g.t->Release(); }
    vsTex->Release(); psTex->Release(); psGlass->Release();
    vdecl->Release(); vsScene->Release(); vsHud->Release(); psScene->Release(); psLight->Release(); psHud->Release();
    dev->Release(); d3d->Release();
    DestroyWindow(wnd);
    return 0;
}
