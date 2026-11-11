#include "ShaderOverride.h"
#include "Config.h"
#include "ExtensionManager.h"

#include <cstdio>

namespace bdx12 {

ShaderRegistry& ShaderRegistry::Get()
{
    static ShaderRegistry s;
    return s;
}

size_t ShaderRegistry::BytecodeSize(const DWORD* tokens)
{
    if (!tokens) return 0;
    const DWORD version = tokens[0];
    const DWORD major   = (version >> 8) & 0xFF;
    constexpr size_t kMaxTokens = 1u << 20;

    size_t i = 1;
    while (i < kMaxTokens) {
        const DWORD t = tokens[i];
        if (t == 0x0000FFFF) return (i + 1) * sizeof(DWORD);
        if ((t & 0xFFFF) == 0xFFFE) {
            i += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        if (major >= 2) {
            i += 1 + ((t >> 24) & 0x0F);
        } else {
            ++i;
        }
    }
    return 0;
}

const DWORD* ShaderRegistry::Resolve(ShaderStage stage, const DWORD* original, uint64_t* outHash)
{
    const size_t size = BytecodeSize(original);
    if (size == 0) {
        if (outHash) *outHash = 0;
        return original;
    }
    const uint64_t hash = Fnv1a64(original, size);
    if (outHash) *outHash = hash;

    const Config& cfg    = GetConfig();
    const wchar_t* prefix = stage == ShaderStage::Vertex ? L"vs_" : L"ps_";
    wchar_t name[64];
    swprintf_s(name, L"%s%016llx.cso", prefix, static_cast<unsigned long long>(hash));

    std::lock_guard<std::mutex> lock(m_mutex);

    if (cfg.dumpShaders && !m_dumped[hash]) {
        m_dumped[hash] = true;
        CreateDirectoryW((GameDirectory() + L"BullyDX12").c_str(), nullptr);
        CreateDirectoryW((GameDirectory() + L"BullyDX12\\shaders").c_str(), nullptr);
        CreateDirectoryW(cfg.shaderDumpDir.c_str(), nullptr);
        const std::wstring path = cfg.shaderDumpDir + name;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
            fwrite(original, 1, size, f);
            fclose(f);
            BDX_TRACE("Dumped shader %s", Narrow(path).c_str());
        }
    }

    if (const DWORD* ext = ExtensionManager::Get().DispatchCreateShader(stage, hash, original, size)) {
        BDX_INFO("Shader %s%016llx replaced by an extension", stage == ShaderStage::Vertex ? "vs_" : "ps_",
                 static_cast<unsigned long long>(hash));
        return ext;
    }

    if (cfg.overrideShaders) {
        auto it = m_fileOverrides.find(hash);
        if (it != m_fileOverrides.end()) return it->second.get();

        const std::wstring path = cfg.shaderOverrideDir + name;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
            fseek(f, 0, SEEK_END);
            const long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len >= 8 && (len % 4) == 0) {
                std::unique_ptr<DWORD[]> buf(new DWORD[static_cast<size_t>(len) / 4]);
                const size_t read = fread(buf.get(), 1, static_cast<size_t>(len), f);
                const DWORD expectedType = stage == ShaderStage::Vertex ? 0xFFFE0000u : 0xFFFF0000u;
                if (read == static_cast<size_t>(len) && (buf[0] & 0xFFFF0000u) == expectedType &&
                    BytecodeSize(buf.get()) != 0) {
                    BDX_INFO("Shader override loaded: %s", Narrow(path).c_str());
                    const DWORD* p = buf.get();
                    m_fileOverrides.emplace(hash, std::move(buf));
                    fclose(f);
                    return p;
                }
                BDX_ERROR("Shader override %s is not valid %s bytecode (compile with fxc /T %s_3_0)",
                          Narrow(path).c_str(), stage == ShaderStage::Vertex ? "vertex" : "pixel",
                          stage == ShaderStage::Vertex ? "vs" : "ps");
            }
            fclose(f);
        }
    }
    return original;
}

std::vector<ShaderRegistry::Constant> ShaderRegistry::ParseConstantTable(const DWORD* code)
{
    std::vector<Constant> out;
    const size_t size = BytecodeSize(code);
    if (size < 8) return out;
    const size_t tokens = size / 4;

    struct CTabHeader { DWORD Size, Creator, Version, Constants, ConstantInfo, Flags, Target; };
    struct CInfo { DWORD Name; WORD RegisterSet, RegisterIndex, RegisterCount, Reserved; DWORD TypeInfo, DefaultValue; };

    for (size_t i = 1; i < tokens;) {
        const DWORD t = code[i];
        if (t == 0x0000FFFF) break;
        if ((t & 0xFFFF) == 0xFFFE) {
            const size_t len = (t >> 16) & 0x7FFF;
            if (len >= 1 + sizeof(CTabHeader) / 4 && i + 1 + len <= tokens && code[i + 1] == MAKEFOURCC('C', 'T', 'A', 'B')) {
                const auto*  base  = reinterpret_cast<const uint8_t*>(&code[i + 2]);
                const size_t bytes = (len - 1) * 4;
                const auto*  h     = reinterpret_cast<const CTabHeader*>(base);
                if (h->ConstantInfo + static_cast<uint64_t>(h->Constants) * sizeof(CInfo) <= bytes) {
                    const auto* ci = reinterpret_cast<const CInfo*>(base + h->ConstantInfo);
                    for (DWORD c = 0; c < h->Constants; ++c) {
                        if (ci[c].Name >= bytes) continue;
                        const char*  nm  = reinterpret_cast<const char*>(base + ci[c].Name);
                        const size_t max = bytes - ci[c].Name;
                        out.push_back({std::string(nm, strnlen(nm, max)), ci[c].RegisterSet, ci[c].RegisterIndex,
                                       ci[c].RegisterCount});
                    }
                }
                return out;
            }
            i += 1 + len;
            continue;
        }
        break;
    }
    return out;
}

void ShaderRegistry::RegisterConstants(ShaderStage stage, uint64_t hash, const DWORD* code)
{
    if (!hash) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& map = m_constants[stage == ShaderStage::Vertex ? 0 : 1];
    if (map.count(hash)) return;
    map.emplace(hash, ParseConstantTable(code));
}

bool ShaderRegistry::FindConstant(ShaderStage stage, uint64_t hash, const char* name, Constant* out) const
{
    if (!hash || !name) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto& map = m_constants[stage == ShaderStage::Vertex ? 0 : 1];
    auto it = map.find(hash);
    if (it == map.end()) return false;
    for (const Constant& c : it->second) {
        if (c.name == name) {
            if (out) *out = c;
            return true;
        }
    }
    return false;
}

void ShaderRegistry::Register(IUnknown* shader, uint64_t hash)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hashes[shader] = hash;
}

void ShaderRegistry::Unregister(IUnknown* shader)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hashes.erase(shader);
}

uint64_t ShaderRegistry::HashOf(IUnknown* shader) const
{
    if (!shader) return 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_hashes.find(shader);
    return it == m_hashes.end() ? 0 : it->second;
}

void ShaderRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hashes.clear();
}

}
