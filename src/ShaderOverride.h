#pragma once
#include "Common.h"

#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

namespace bdx12 {

class ShaderRegistry {
public:
    static ShaderRegistry& Get();

    static size_t BytecodeSize(const DWORD* tokens);

    const DWORD* Resolve(ShaderStage stage, const DWORD* original, uint64_t* outHash);

    void     Register(IUnknown* shader, uint64_t hash);
    void     Unregister(IUnknown* shader);
    uint64_t HashOf(IUnknown* shader) const;

    struct Constant {
        std::string name;
        uint16_t    registerSet;
        uint16_t    registerIndex;
        uint16_t    registerCount;
    };
    void RegisterConstants(ShaderStage stage, uint64_t hash, const DWORD* code);
    bool FindConstant(ShaderStage stage, uint64_t hash, const char* name, Constant* out) const;
    static std::vector<Constant> ParseConstantTable(const DWORD* code);

    void Clear();

private:
    mutable std::mutex                                          m_mutex;
    std::unordered_map<IUnknown*, uint64_t>                     m_hashes;
    std::unordered_map<uint64_t, std::unique_ptr<DWORD[]>>      m_fileOverrides;
    std::unordered_map<uint64_t, bool>                          m_dumped;
    std::unordered_map<uint64_t, std::vector<Constant>>         m_constants[2];
};

}
