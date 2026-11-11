# BullyRT

BullyRT is a replacement `d3d9.dll` for **Bully: Scholarship Edition** on PC. It runs the game on Direct3D 12 through Microsoft's D3D9On12 and adds hardware ray-traced lighting: sun shadows, global illumination, reflections, the game's own lamps and volumetric light.

The game still renders its own scene. BullyRT reads what the game draws, rebuilds it for ray tracing in a separate 64-bit process and lights it there, then puts the result back into the frame.

> [!NOTE]
> This is a fan project. It is not affiliated with or endorsed by Rockstar Games. It contains no game files: you need your own copy of the game.

## Features

- Direct3D 9 on Direct3D 12 through D3D9On12, the component that ships with Windows 10 and 11.
- Ray-traced sun and moon shadows, denoised with NVIDIA NRD (SIGMA).
- Diffuse global illumination with a world-space radiance cache (SHaRC), denoised with NRD (REBLUR).
- Ray-traced reflections on glossy surfaces.
- Local lights sampled with ReSTIR (RTXDI): street lamps, wall lamps, ceiling fixtures, signs and car headlights.
- A light catalog built from the game's own models and 2dfx light data. Lamps turn on and off with the game's clock, as the game decides.
- Volumetric light and fog, a physical sky with clouds, and weather and time of day taken from the game's time cycle.
- An in-game menu, opened with **Insert**, for every setting. It can also set the game's clock, season and weather, and edit the time cycle live.
- Game fixes: longer draw distance, larger object pools and heap, and D3DX effects that load above 2 GB of address space.

## Requirements

- Bully: Scholarship Edition for PC, version 1.200 (Steam).
- Windows 10 or 11, 64-bit.
- A GPU with DirectX 12 ray tracing (DXR 1.1). Development and testing were done on an NVIDIA GeForce RTX 4070 Laptop GPU.

## Installation

1. Download the latest build from the [Releases](../../releases) page.
2. Copy everything in the archive into the game folder, next to `Bully.exe`: `d3d9.dll`, `BullyDX12.ini` and the `BullyDX12` folder.
3. Start the game. Press **Insert** to open the menu.

To uninstall, delete `d3d9.dll`, `BullyDX12.ini` and the `BullyDX12` folder.

Another `d3d9.dll` (an ASI loader, ENB) cannot be used at the same time. ReShade installed as `dxgi.dll` is not supported either, because BullyRT renders through DXGI itself.

## Settings

Everything is in `BullyDX12.ini` and in the in-game menu. **Info > Reset all settings** puts the menu's settings back to their defaults.

| Setting | Where | What it does |
|---|---|---|
| Ray resolution | `[RaytracingBridge] RtScale` | 1 full, 2 half, 3 third. The largest effect on performance. |
| Denoiser | `[RTOptions] Denoiser` | 0 NRD (default), 1 DLSS Ray Reconstruction. |
| Lighting | `[RTOptions] Lighting` | 0 off, 1 medium, 2 ultra, 3 psycho: how much indirect light is traced. |
| Draw distance | `[Culling] DrawDistance` | How much farther objects keep their detailed model. 1 is the game's own distance. |
| Renderer | `[Renderer] Backend` | `D3D9On12` (default), or `Native` to run the stock Direct3D 9 without any of the above. |

DLSS Ray Reconstruction needs NVIDIA's `nvngx_dlssd.dll` in `BullyDX12\bridge`. Without it, NRD is used.

## Building

You need Visual Studio 2022 or 2026 with the **Desktop development with C++** workload and the Windows SDK, which provides `fxc` and `dxc`. Python 3 is only needed for the tools.

```
git clone --recursive https://github.com/lucid29a/BullyRT.git
cd BullyRT
build.bat Release "C:\Program Files (x86)\Steam\steamapps\common\Bully Scholarship Edition"
```

`build.bat` builds the 32-bit part (`d3d9.dll` and the extensions) and the 64-bit ray tracing process (`BullyRTBridge.exe`). When a game folder is given, the build is copied there. Copy `dist\BullyDX12.ini` next to `Bully.exe` the first time.

The light catalog is generated from your copy of the game:

```
python tools\lampcatalog.py "<game folder>" "<game folder>\BullyDX12\bridge\lamps.dat"
```

## How it works

```
Bully.exe (32-bit)
  d3d9.dll           Direct3D 9 proxy on D3D9On12, extension host
    PostFX.dll           tone and sharpening
    RaytracingBridge.dll captures the scene, game patches, menu
        | shared memory and shared D3D12 resources
BullyRTBridge.exe (64-bit)
  DXR 1.1 inline ray tracing, NRD, RTXDI, SHaRC, volumetrics
```

GPU drivers do not expose DXR to 32-bit processes, and `Bully.exe` is 32-bit. That is why the ray tracing runs in a separate 64-bit process on the same GPU. The game's frame, depth and G-buffer are shared with it, and the lit image comes back the same way.

| Folder | Contents |
|---|---|
| `src` | The `d3d9.dll` proxy, D3D9On12 backend, display handling, extension manager. |
| `include` | The extension API (`BullyDX12.h`). |
| `extensions` | `PostFX`, `RaytracingBridge` (scene capture, game patches, menu), `RaytracingFoundation`. |
| `bridge` | The 64-bit ray tracing process and its shaders. |
| `tools` | Scripts that read the game's data: light catalog, 2dfx lights. |
| `tests` | Replay of captured frames, integration tests. |

## Reporting problems

Please attach:

- `BullyDX12.log` from the game folder, or from `%LOCALAPPDATA%\BullyDX12` if the game folder is not writable.
- `%LOCALAPPDATA%\BullyDX12\startup.txt` if the game does not start. Its last line shows the last step that was reached.
- `BullyDX12\bridge\BullyRTBridge.log`.
- For a visual problem, a capture made with **Ctrl+Alt+F12** at that spot. It is saved in `BullyDX12\bridge\dumps`.
- Your GPU, driver version and Windows version.

## Known issues

- On some Windows 10 22H2 systems the game closes while `d3d9.dll` is loading, before anything is logged. This is being investigated.
- Performance depends heavily on the ray resolution. If the frame rate is low, set Ray resolution to Third.
- Moving characters are lit from fewer samples than the world and can look softer.

## Credits and license

BullyRT is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).

It uses:

- [D3D9On12](https://github.com/microsoft/D3D9On12) by Microsoft, the system component.
- [NRD](https://github.com/NVIDIA-RTX/NRD), [NRI](https://github.com/NVIDIA-RTX/NRI), [RTXDI](https://github.com/NVIDIA-RTX/RTXDI-Library), [RTXGI / SHaRC](https://github.com/NVIDIA-RTX/RTXGI) and [DLSS](https://github.com/NVIDIA/DLSS) by NVIDIA.
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut.

These are included as git submodules under `external`, each with its own license.
