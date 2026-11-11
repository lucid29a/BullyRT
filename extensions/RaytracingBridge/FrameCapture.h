// Helpers of the frame capture (Ctrl+Alt+F12): textures to .tga and the image of the frame to .bmp.

#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace framecap {

inline void Rgb565(uint16_t c, uint8_t o[3])
{
    o[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    o[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    o[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
}

inline void ColorBlock(const uint8_t* b, uint8_t out[16][4], bool bc1)
{
    const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8)), c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    uint8_t pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    for (int k = 0; k < 3; ++k) {
        if (!bc1 || c0 > c1) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (bc1 && c0 <= c1) pal[3][3] = 0;
    const uint32_t bits = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

inline bool DecodeLevel(const uint8_t* src, int pitch, D3DFORMAT fmt, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba)
{
    rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* s = src + static_cast<size_t>(y) * pitch + x * 4;
                uint8_t* d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = fmt == D3DFMT_X8R8G8B8 ? 255 : s[3];
            }
        return true;
    }
    const bool bc1 = fmt == D3DFMT_DXT1, bc2 = fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3, bc3 = fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5;
    if (!bc1 && !bc2 && !bc3) return false;
    const uint32_t blockBytes = bc1 ? 8 : 16;
    for (uint32_t by = 0; by < (h + 3) / 4; ++by)
        for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx) {
            const uint8_t* b = src + static_cast<size_t>(by) * pitch + bx * blockBytes;
            uint8_t px[16][4];
            ColorBlock(bc1 ? b : b + 8, px, bc1);
            if (bc2)
                for (int i = 0; i < 16; ++i) px[i][3] = static_cast<uint8_t>(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            if (bc3) {
                uint8_t a[8] = {b[0], b[1]};
                for (int k = 2; k < 8; ++k)
                    a[k] = a[0] > a[1] ? static_cast<uint8_t>(((8 - k) * a[0] + (k - 1) * a[1]) / 7)
                                       : (k < 6 ? static_cast<uint8_t>(((6 - k) * a[0] + (k - 1) * a[1]) / 5) : (k == 6 ? 0 : 255));
                uint64_t bits = 0;
                for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
                for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) memcpy(&rgba[(static_cast<size_t>(y) * w + x) * 4], px[i], 4);
            }
        }
    return true;
}

inline bool WriteTga(const std::wstring& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    uint8_t hdr[18] = {0, 0, 2};
    hdr[12] = static_cast<uint8_t>(w & 255); hdr[13] = static_cast<uint8_t>(w >> 8);
    hdr[14] = static_cast<uint8_t>(h & 255); hdr[15] = static_cast<uint8_t>(h >> 8);
    hdr[16] = 32; hdr[17] = 0x28;
    fwrite(hdr, 1, 18, f);
    std::vector<uint8_t> bgra(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        bgra[i] = rgba[i + 2]; bgra[i + 1] = rgba[i + 1]; bgra[i + 2] = rgba[i]; bgra[i + 3] = rgba[i + 3];
    }
    fwrite(bgra.data(), 1, bgra.size(), f);
    fclose(f);
    return true;
}

inline bool WriteSurfaceBmp(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const std::wstring& path)
{
    if (!dev || !surface) return false;
    D3DSURFACE_DESC sd = {};
    if (FAILED(surface->GetDesc(&sd)) || (sd.Format != D3DFMT_A8R8G8B8 && sd.Format != D3DFMT_X8R8G8B8)) return false;
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return false;
    bool ok = false;
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(dev->GetRenderTargetData(surface, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        if (FILE* out = _wfopen(path.c_str(), L"wb")) {
            const int w = static_cast<int>(sd.Width), h = static_cast<int>(sd.Height), row = w * 3, pad = (4 - row % 4) % 4;
            BITMAPFILEHEADER fh = {0x4D42, static_cast<DWORD>(54 + (row + pad) * h), 0, 0, 54};
            BITMAPINFOHEADER ih = {40, w, h, 1, 24, BI_RGB, 0, 2835, 2835, 0, 0};
            fwrite(&fh, sizeof(fh), 1, out);
            fwrite(&ih, sizeof(ih), 1, out);
            std::vector<unsigned char> buf(static_cast<size_t>(row + pad), 0);
            for (int y = h - 1; y >= 0; --y) {
                const DWORD* src = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch);
                for (int x = 0; x < w; ++x) {
                    buf[x * 3] = src[x] & 0xFF; buf[x * 3 + 1] = (src[x] >> 8) & 0xFF; buf[x * 3 + 2] = (src[x] >> 16) & 0xFF;
                }
                fwrite(buf.data(), 1, buf.size(), out);
            }
            fclose(out);
            ok = true;
        }
        sys->UnlockRect();
    }
    sys->Release();
    return ok;
}

}
