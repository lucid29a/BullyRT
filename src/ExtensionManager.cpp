#include "ExtensionManager.h"
#include "Config.h"
#include "D3D12Context.h"
#include "ShaderOverride.h"

#include <algorithm>
#include <cwctype>

namespace bdx12 {

ExtensionManager& ExtensionManager::Get()
{
    static ExtensionManager s;
    return s;
}

namespace {
bool MatchesFilter(const std::wstring& filter, const std::wstring& fileName)
{
    if (filter.empty() || filter == L"*") return true;
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    };
    const std::wstring f = lower(filter), n = lower(fileName);
    size_t start = 0;
    while (start <= f.size()) {
        size_t end = f.find(L',', start);
        if (end == std::wstring::npos) end = f.size();
        std::wstring tok = f.substr(start, end - start);
        tok.erase(0, tok.find_first_not_of(L" \t"));
        tok.erase(tok.find_last_not_of(L" \t") + 1);
        if (!tok.empty() && (tok == n || tok + L".dll" == n)) return true;
        start = end + 1;
    }
    return false;
}

template <typename F>
void SafeCall(const std::string& name, const char* what, F&& fn)
{
    __try {
        fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BDX_ERROR("Extension '%s' raised exception 0x%08lX in %s", name.c_str(), GetExceptionCode(), what);
    }
}
}

void ExtensionManager::LoadAll()
{
    const Config& cfg = GetConfig();
    if (!cfg.extensionsEnabled) {
        BDX_INFO("Extensions disabled in BullyDX12.ini");
        return;
    }

    WIN32_FIND_DATAW fd;
    HANDLE           h = FindFirstFileW((cfg.extensionDir + L"*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        BDX_INFO("No extensions found in %s", Narrow(cfg.extensionDir).c_str());
        return;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!MatchesFilter(cfg.extensionFilter, fd.cFileName)) continue;

        const std::wstring path = cfg.extensionDir + fd.cFileName;
        Startup(("loading extension " + Narrow(fd.cFileName)).c_str());
        HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod) {
            BDX_ERROR("Failed to load extension %s (error %lu)", Narrow(path).c_str(), GetLastError());
            continue;
        }
        auto create  = reinterpret_cast<PFN_CreateExtension>(GetProcAddress(mod, "BullyDX12_CreateExtension"));
        auto destroy = reinterpret_cast<PFN_DestroyExtension>(GetProcAddress(mod, "BullyDX12_DestroyExtension"));
        if (!create || !destroy) {
            BDX_WARN("%s is not a BullyDX12 extension (missing exports)", Narrow(path).c_str());
            FreeLibrary(mod);
            continue;
        }

        IExtension* ext = create(kApiVersion);
        if (!ext) {
            BDX_WARN("%s refused to load for API version %u", Narrow(path).c_str(), kApiVersion);
            FreeLibrary(mod);
            continue;
        }

        Loaded l;
        l.module  = mod;
        l.ext     = ext;
        l.destroy = destroy;
        l.name    = ext->GetName() ? ext->GetName() : Narrow(fd.cFileName);

        bool ok = false;
        Startup(("initializing extension " + l.name).c_str());
        SafeCall(l.name, "OnInitialize", [&] { ok = ext->OnInitialize(this); });
        Startup(("extension " + l.name + (ok ? " ready" : " declined")).c_str());
        if (!ok) {
            BDX_WARN("Extension '%s' declined to initialize", l.name.c_str());
            destroy(ext);
            FreeLibrary(mod);
            continue;
        }
        BDX_INFO("Loaded extension '%s' (priority %d) from %s", l.name.c_str(), ext->GetPriority(),
                 Narrow(fd.cFileName).c_str());
        m_extensions.push_back(std::move(l));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::stable_sort(m_extensions.begin(), m_extensions.end(),
                     [](const Loaded& a, const Loaded& b) { return a.ext->GetPriority() < b.ext->GetPriority(); });

    m_postProcessors.clear();
    for (auto& l : m_extensions)
        if (l.ext->WantsPostProcess()) m_postProcessors.push_back(l.ext);
}

void ExtensionManager::UnloadAll()
{
    for (auto it = m_extensions.rbegin(); it != m_extensions.rend(); ++it) {
        SafeCall(it->name, "OnShutdown", [&] { it->ext->OnShutdown(); });
        it->destroy(it->ext);
    }
    m_extensions.clear();
    m_postProcessors.clear();
    m_registry.clear();
}

bool ExtensionManager::AnyWantsPostProcess() const { return !m_postProcessors.empty(); }
size_t ExtensionManager::PostProcessPassCount() const { return m_postProcessors.size(); }

void ExtensionManager::DispatchDeviceCreated(const DeviceInfo& info)
{
    for (auto& l : m_extensions) SafeCall(l.name, "OnDeviceCreated", [&] { l.ext->OnDeviceCreated(info); });
}
void ExtensionManager::DispatchDeviceLost()
{
    for (auto& l : m_extensions) SafeCall(l.name, "OnDeviceLost", [&] { l.ext->OnDeviceLost(); });
}
void ExtensionManager::DispatchDeviceReset(const DeviceInfo& info)
{
    for (auto& l : m_extensions) SafeCall(l.name, "OnDeviceReset", [&] { l.ext->OnDeviceReset(info); });
}
void ExtensionManager::DispatchBeginScene()
{
    for (auto& l : m_extensions) l.ext->OnBeginScene(m_frameIndex);
}
void ExtensionManager::DispatchEndScene()
{
    for (auto& l : m_extensions) l.ext->OnEndScene(m_frameIndex);
}
void ExtensionManager::DispatchClear(DWORD flags, IDirect3DSurface9* renderTarget0, IDirect3DSurface9* depthStencil)
{
    for (auto& l : m_extensions) l.ext->OnClear(flags, renderTarget0, depthStencil);
}
void ExtensionManager::DispatchCopySurface(IDirect3DSurface9* source, IDirect3DSurface9* dest)
{
    for (auto& l : m_extensions) l.ext->OnCopySurface(source, dest);
}
DrawResult ExtensionManager::DispatchDraw(const DrawCallInfo& draw)
{
    DrawResult r = DrawResult::Continue;
    for (auto& l : m_extensions)
        if (l.ext->OnDraw(draw) == DrawResult::Skip) r = DrawResult::Skip;
    return r;
}
void ExtensionManager::DispatchSetLight(DWORD index, const D3DLIGHT9& light)
{
    for (auto& l : m_extensions) l.ext->OnSetLight(index, light);
}
void ExtensionManager::DispatchLightEnable(DWORD index, BOOL enable)
{
    for (auto& l : m_extensions) l.ext->OnLightEnable(index, enable);
}
const DWORD* ExtensionManager::DispatchCreateShader(ShaderStage stage, uint64_t hash, const DWORD* code, size_t size)
{
    for (auto& l : m_extensions) {
        const DWORD* r = nullptr;
        SafeCall(l.name, "OnCreateShader", [&] { r = l.ext->OnCreateShader(stage, hash, code, size); });
        if (r) return r;
    }
    return nullptr;
}
void ExtensionManager::DispatchPostProcess(const PostProcessContext& ctx, size_t passIndex)
{
    if (passIndex >= m_postProcessors.size()) return;
    IExtension* e = m_postProcessors[passIndex];
    SafeCall(e->GetName() ? e->GetName() : "?", "OnPostProcess", [&] { e->OnPostProcess(ctx); });
}
void ExtensionManager::DispatchPresent(const FrameInfo& frame)
{
    for (auto& l : m_extensions) SafeCall(l.name, "OnPresent", [&] { l.ext->OnPresent(frame); });
}

void ExtensionManager::Log(LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV(level, fmt, args);
    va_end(args);
}

int ExtensionManager::GetConfigInt(const char* section, const char* key, int def)
{
    return GetConfig().GetInt(section, key, def);
}

float ExtensionManager::GetConfigFloat(const char* section, const char* key, float def)
{
    return GetConfig().GetFloat(section, key, def);
}

size_t ExtensionManager::GetConfigString(const char* section, const char* key, const char* def, char* out,
                                         size_t outSize)
{
    const std::string v = Narrow(GetConfig().GetString(section, key, Widen(def ? def : "").c_str()));
    if (out && outSize) strncpy_s(out, outSize, v.c_str(), _TRUNCATE);
    return v.size();
}

const wchar_t* ExtensionManager::GetGameDirectory() const { return GameDirectory().c_str(); }

bool ExtensionManager::D3D12Available() const
{
    return m_backend == Backend::D3D9On12 && D3D12Context::Get().IsReady();
}

ID3D12Device* ExtensionManager::GetD3D12Device() { return D3D12Available() ? D3D12Context::Get().Device() : nullptr; }
ID3D12CommandQueue* ExtensionManager::GetD3D12Queue() { return D3D12Available() ? D3D12Context::Get().Queue() : nullptr; }
const D3D12Caps* ExtensionManager::GetD3D12Caps() { return D3D12Available() ? &D3D12Context::Get().Caps() : nullptr; }

HRESULT ExtensionManager::UnwrapResource(IDirect3DResource9* res9, ID3D12Resource** out)
{
    if (!D3D12Available()) return D3DERR_NOTAVAILABLE;
    return D3D12Context::Get().Unwrap(res9, out);
}

HRESULT ExtensionManager::ReturnResource(IDirect3DResource9* res9)
{
    if (!D3D12Available()) return D3DERR_NOTAVAILABLE;
    return D3D12Context::Get().Return(res9);
}

ID3D12GraphicsCommandList* ExtensionManager::BeginCommandList()
{
    return D3D12Available() ? D3D12Context::Get().BeginCommandList() : nullptr;
}

uint64_t ExtensionManager::ExecuteCommandList(ID3D12GraphicsCommandList* list)
{
    return D3D12Available() ? D3D12Context::Get().Execute(list) : 0;
}

uint64_t ExtensionManager::GetCompletedFenceValue() { return D3D12Context::Get().CompletedValue(); }
void ExtensionManager::WaitForFence(uint64_t value) { D3D12Context::Get().Wait(value); }

void ExtensionManager::DeferredRelease(IUnknown* object)
{
    if (D3D12Context::Get().IsReady()) D3D12Context::Get().DeferredRelease(object);
    else if (object) object->Release();
}

void ExtensionManager::PublishResource(const char* name, IUnknown* object)
{
    if (!name) return;
    if (object) m_registry[name] = object;
    else m_registry.erase(name);
}

IUnknown* ExtensionManager::FindResource(const char* name)
{
    if (!name) return nullptr;
    auto it = m_registry.find(name);
    return it == m_registry.end() ? nullptr : it->second.Get();
}

bool ExtensionManager::FindShaderConstant(ShaderStage stage, uint64_t hash, const char* name, UINT* reg, UINT* count,
                                          UINT* set)
{
    ShaderRegistry::Constant c;
    if (!ShaderRegistry::Get().FindConstant(stage, hash, name, &c)) return false;
    if (reg) *reg = c.registerIndex;
    if (count) *count = c.registerCount;
    if (set) *set = c.registerSet;
    return true;
}

}
