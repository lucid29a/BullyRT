// D3D9 shader model 3 bytecode patcher: brings the game's own geometry/material data out of its
// shaders (G-buffer for the RTX pipeline) and brings the ray traced lighting back into them.

#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

namespace rtpatch {

constexpr uint32_t kWorldPosUsageIndex = 15;
constexpr uint32_t kNormalUsageIndex   = 14;
constexpr uint32_t kConstCount         = 2;

struct ShaderUsage {
    uint32_t version = 0;
    uint32_t maxTemp = 0;
    uint32_t usedOutputs = 0;
    uint32_t usedInputs = 0;
    uint32_t usedSamplers = 0;
    uint32_t usedTexcoordUsage = 0;
    int      positionInput = -1;
    int      normalInput = -1;
    bool     hasSubroutines = false;
    bool     writesExtraColor = false;
    bool     usesTexkill = false;
    int      texcoordInput[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::vector<uint8_t> usedFloatConsts;
};

bool Analyze(const DWORD* code, ShaderUsage* out);

std::vector<DWORD> PatchVertexShader(const DWORD* code, uint32_t worldRegister, bool* outHasNormal);

struct PsPatchOptions {
    uint32_t constBase = 0;
    int      albedoSampler = -1;
    uint32_t nrmUsage = kNormalUsageIndex;
};
std::vector<DWORD> PatchPixelShader(const DWORD* code, const PsPatchOptions& opt);

std::vector<DWORD> MirrorPixelShader(uint32_t constBase);

}
