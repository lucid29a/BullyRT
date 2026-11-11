#include "ShaderPatcher.h"

#include <cstring>

namespace rtpatch {
namespace {

enum : uint32_t {
    OP_LRP = 18, OP_MAX = 11, OP_MOV = 1, OP_ADD = 2, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_DP3 = 8, OP_DP4 = 9, OP_CALL = 25, OP_CALLNZ = 26,
    OP_LABEL = 30, OP_DCL = 31, OP_NRM = 36, OP_TEX = 66, OP_DEF = 81, OP_DEFI = 82, OP_DEFB = 83,
    OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};
enum : uint32_t {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_OUTPUT = 6, RT_COLOROUT = 8, RT_SAMPLER = 10,
    RT_CONST2 = 11, RT_CONST3 = 12, RT_CONST4 = 13
};
constexpr uint32_t USAGE_POSITION = 0, USAGE_NORMAL = 3, USAGE_TEXCOORD = 5;
constexpr uint32_t SWZ_XYZW = 0xE4, SWZ_XXXX = 0x00, SWZ_YYYY = 0x55, SWZ_ZZZZ = 0xAA, SWZ_WWWW = 0xFF, SWZ_ZWZW = 0xEE;
constexpr uint32_t MOD_NEG = 1;

uint32_t RegType(DWORD t) { return ((t >> 28) & 0x7) | ((t >> 8) & 0x18); }
uint32_t RegNum(DWORD t) { return t & 0x7FF; }
DWORD Dst(uint32_t type, uint32_t num, uint32_t mask)
{
    return 0x80000000u | (num & 0x7FF) | ((type & 0x7) << 28) | ((type & 0x18) << 8) | ((mask & 0xF) << 16);
}
DWORD Src(uint32_t type, uint32_t num, uint32_t swz, uint32_t mod = 0)
{
    return 0x80000000u | (num & 0x7FF) | ((type & 0x7) << 28) | ((type & 0x18) << 8) | ((swz & 0xFF) << 16) |
           ((mod & 0xF) << 24);
}
DWORD Ins(uint32_t op, uint32_t len) { return op | (len << 24); }
DWORD UsageTok(uint32_t usage, uint32_t index) { return 0x80000000u | usage | (index << 16); }

uint32_t ConstIndex(DWORD t)
{
    switch (RegType(t)) {
    case RT_CONST:  return RegNum(t);
    case RT_CONST2: return RegNum(t) + 2048;
    case RT_CONST3: return RegNum(t) + 4096;
    case RT_CONST4: return RegNum(t) + 6144;
    default:        return 0xFFFFFFFF;
    }
}

template <typename F>
bool Walk(const DWORD* code, F&& f)
{
    size_t i = 1;
    for (size_t guard = 0; guard < (1u << 20); ++guard) {
        const DWORD t = code[i];
        if (t == 0x0000FFFF) { f(i, OP_END, 0u); return true; }
        const uint32_t op = t & 0xFFFF;
        if (op == OP_COMMENT) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        const uint32_t len = (t >> 24) & 0xF;
        f(i, op, len);
        i += 1 + len;
    }
    return false;
}

size_t EndIndex(const DWORD* code)
{
    size_t end = 0;
    Walk(code, [&](size_t i, uint32_t op, uint32_t) { if (op == OP_END) end = i; });
    return end;
}

struct Emit {
    std::vector<DWORD>& r;
    void I3(uint32_t op, DWORD d, DWORD a, DWORD b) { r.push_back(Ins(op, 3)); r.push_back(d); r.push_back(a); r.push_back(b); }
    void I2(uint32_t op, DWORD d, DWORD a) { r.push_back(Ins(op, 2)); r.push_back(d); r.push_back(a); }
    void I4(uint32_t op, DWORD d, DWORD a, DWORD b, DWORD c)
    {
        r.push_back(Ins(op, 4)); r.push_back(d); r.push_back(a); r.push_back(b); r.push_back(c);
    }
    void Dcl(uint32_t usageTok, DWORD reg) { r.push_back(Ins(OP_DCL, 2)); r.push_back(usageTok); r.push_back(reg); }
};

}

bool Analyze(const DWORD* code, ShaderUsage* u)
{
    *u = ShaderUsage{};
    u->usedFloatConsts.assign(256, 0);
    u->version = code[0];
    const bool isPS = (code[0] & 0xFFFF0000u) == 0xFFFF0000u;
    if ((code[0] & 0xFFFF) != 0x0300) return false;

    return Walk(code, [&](size_t i, uint32_t op, uint32_t len) {
        if (op == OP_END) return;
        if (op == OP_CALL || op == OP_CALLNZ || op == OP_LABEL) u->hasSubroutines = true;
        if (op == 65) u->usesTexkill = true;
        if (op == OP_DCL) {
            const DWORD usage = code[i + 1], reg = code[i + 2];
            const uint32_t type = RegType(reg), num = RegNum(reg);
            const uint32_t usg = usage & 0x1F, idx = (usage >> 16) & 0xF;
            if (type == RT_SAMPLER) { u->usedSamplers |= 1u << num; return; }
            if (type == RT_INPUT) {
                u->usedInputs |= 1u << num;
                if (!isPS && usg == USAGE_POSITION && idx == 0) u->positionInput = static_cast<int>(num);
                if (!isPS && usg == USAGE_NORMAL && idx == 0) u->normalInput = static_cast<int>(num);
                if (isPS && usg == USAGE_TEXCOORD) {
                    u->usedTexcoordUsage |= 1u << idx;
                    u->texcoordInput[idx] = static_cast<int>(num);
                }
            }
            if (type == RT_OUTPUT) {
                u->usedOutputs |= 1u << num;
                if (usg == USAGE_TEXCOORD) u->usedTexcoordUsage |= 1u << idx;
            }
            return;
        }
        if (op == OP_DEF) {
            const uint32_t c = ConstIndex(code[i + 1]);
            if (c < 256) u->usedFloatConsts[c] = 1;
            return;
        }
        if (op == OP_DEFI || op == OP_DEFB) return;
        for (uint32_t k = 1; k <= len; ++k) {
            const DWORD p = code[i + k];
            if (!(p & 0x80000000u)) continue;
            const uint32_t type = RegType(p);
            if (type == RT_TEMP && RegNum(p) + 1 > u->maxTemp) u->maxTemp = RegNum(p) + 1;
            if (type == RT_SAMPLER) u->usedSamplers |= 1u << RegNum(p);
            if (type == RT_COLOROUT && RegNum(p) > 0) u->writesExtraColor = true;
            const uint32_t c = ConstIndex(p);
            if (c < 256) {
                if (p & 0x2000) for (uint32_t x = c; x < 256; ++x) u->usedFloatConsts[x] = 1;
                else u->usedFloatConsts[c] = 1;
            }
        }
    });
}

std::vector<DWORD> PatchVertexShader(const DWORD* code, uint32_t worldReg, bool* outHasNormal)
{
    if (outHasNormal) *outHasNormal = false;
    ShaderUsage u;
    if (!Analyze(code, &u) || (u.version & 0xFFFF0000u) != 0xFFFE0000u) return {};
    if (u.positionInput < 0 || u.hasSubroutines) return {};
    if (u.usedTexcoordUsage & ((1u << kWorldPosUsageIndex) | (1u << kNormalUsageIndex))) return {};
    int outPos = -1, outNrm = -1;
    for (int o = 0; o < 12; ++o) {
        if (u.usedOutputs & (1u << o)) continue;
        if (outPos < 0) outPos = o;
        else if (outNrm < 0) { outNrm = o; break; }
    }
    if (outPos < 0) return {};
    const bool normal = u.normalInput >= 0 && outNrm >= 0;

    std::vector<DWORD> r;
    Emit e{r};
    const size_t end = EndIndex(code);
    r.push_back(code[0]);
    e.Dcl(UsageTok(USAGE_TEXCOORD, kWorldPosUsageIndex), Dst(RT_OUTPUT, static_cast<uint32_t>(outPos), 0xF));
    if (normal) e.Dcl(UsageTok(USAGE_TEXCOORD, kNormalUsageIndex), Dst(RT_OUTPUT, static_cast<uint32_t>(outNrm), 0x7));
    r.insert(r.end(), code + 1, code + end);

    const uint32_t vp = static_cast<uint32_t>(u.positionInput);
    for (uint32_t row = 0; row < 3; ++row)
        e.I3(OP_DP4, Dst(RT_OUTPUT, static_cast<uint32_t>(outPos), 1u << row), Src(RT_INPUT, vp, SWZ_XYZW),
             Src(RT_CONST, worldReg + row, SWZ_XYZW));
    e.I2(OP_MOV, Dst(RT_OUTPUT, static_cast<uint32_t>(outPos), 0x8), Src(RT_INPUT, vp, SWZ_WWWW));
    if (normal) {
        const uint32_t vn = static_cast<uint32_t>(u.normalInput);
        for (uint32_t row = 0; row < 3; ++row)
            e.I3(OP_DP3, Dst(RT_OUTPUT, static_cast<uint32_t>(outNrm), 1u << row), Src(RT_INPUT, vn, SWZ_XYZW),
                 Src(RT_CONST, worldReg + row, SWZ_XYZW));
    }
    r.push_back(0x0000FFFF);
    if (outHasNormal) *outHasNormal = normal;
    return r;
}

std::vector<DWORD> PatchPixelShader(const DWORD* code, const PsPatchOptions& o)
{
    ShaderUsage u;
    if (!Analyze(code, &u) || (u.version & 0xFFFF0000u) != 0xFFFF0000u) return {};
    if (u.hasSubroutines || u.maxTemp + 3 > 32 || u.writesExtraColor) return {};
    for (uint32_t c = o.constBase; c < o.constBase + kConstCount; ++c) if (c >= 224 || u.usedFloatConsts[c]) return {};

    int inNrm = u.texcoordInput[o.nrmUsage & 15];
    const bool declNrm = inNrm < 0;
    for (int v = 0; v < 10 && declNrm && inNrm < 0; ++v) if (!(u.usedInputs & (1u << v))) inNrm = v;
    if (inNrm < 0) return {};

    const uint32_t tColor = u.maxTemp, tN = u.maxTemp + 1, tAlb = u.maxTemp + 2;
    const uint32_t C = o.constBase, vNrm = static_cast<uint32_t>(inNrm);

    std::vector<DWORD> r;
    Emit e{r};
    const size_t end = EndIndex(code);
    r.reserve(end + 64);
    r.push_back(code[0]);
    if (declNrm) e.Dcl(UsageTok(USAGE_TEXCOORD, o.nrmUsage), Dst(RT_INPUT, vNrm, 0x7));

    bool ok = true, writesColor = false, albedo = false;
    Walk(code, [&](size_t i, uint32_t op, uint32_t len) {
        if (op == OP_END || !ok) return;
        const size_t start = r.size();
        r.insert(r.end(), code + i, code + i + 1 + len);
        if (op == OP_DCL || op == OP_DEF || op == OP_DEFI || op == OP_DEFB || len < 1) return;
        DWORD& d = r[start + 1];
        if ((d & 0x80000000u) && RegType(d) == RT_COLOROUT && RegNum(d) == 0) {
            if (d & 0x2000) { ok = false; return; }
            d = Dst(RT_TEMP, tColor, (d >> 16) & 0xF) | (((d >> 20) & 0xF) << 20);
            writesColor = true;
        }
        if (!albedo && op == OP_TEX && len >= 3 && o.albedoSampler >= 0 &&
            RegType(code[i + 3]) == RT_SAMPLER && static_cast<int>(RegNum(code[i + 3])) == o.albedoSampler &&
            RegType(code[i + 1]) == RT_TEMP) {
            e.I2(OP_MOV, Dst(RT_TEMP, tAlb, 0xF), Src(RT_TEMP, RegNum(code[i + 1]), SWZ_XYZW));
            albedo = true;
        }
    });
    if (!ok || !writesColor) return {};

    if (!albedo) e.I2(OP_MOV, Dst(RT_TEMP, tAlb, 0xF), Src(RT_TEMP, tColor, SWZ_XYZW));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 0, 0xF), Src(RT_TEMP, tColor, SWZ_XYZW));
    e.I2(OP_NRM, Dst(RT_TEMP, tN, 0x7), Src(RT_INPUT, vNrm, SWZ_XYZW));
    e.I4(OP_MAD, Dst(RT_COLOROUT, 1, 0x7), Src(RT_TEMP, tN, SWZ_XYZW), Src(RT_CONST, C, SWZ_XXXX), Src(RT_CONST, C, SWZ_XXXX));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 1, 0x8), Src(RT_CONST, C + 1, SWZ_XXXX));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 2, 0x7), Src(RT_TEMP, tAlb, SWZ_XYZW));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 2, 0x8), Src(RT_CONST, C + 1, SWZ_YYYY));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 3, 0x7), Src(RT_TEMP, tColor, SWZ_XYZW));
    e.I3(OP_MAX, Dst(RT_TEMP, tN, 0x8), Src(RT_TEMP, tAlb, SWZ_WWWW), Src(RT_CONST, C + 1, SWZ_WWWW));
    e.I4(OP_LRP, Dst(RT_COLOROUT, 3, 0x8), Src(RT_CONST, C + 1, SWZ_ZZZZ), Src(RT_TEMP, tColor, SWZ_WWWW), Src(RT_TEMP, tN, SWZ_WWWW));
    r.push_back(0x0000FFFF);
    return r;
}

std::vector<DWORD> MirrorPixelShader(uint32_t C)
{
    std::vector<DWORD> r;
    Emit e{r};
    r.push_back(0xFFFF0300);
    e.Dcl(UsageTok(USAGE_TEXCOORD, kNormalUsageIndex), Dst(RT_INPUT, 0, 0x7));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 0, 0xF), Src(RT_CONST, C, SWZ_XXXX));
    e.I2(OP_NRM, Dst(RT_TEMP, 0, 0x7), Src(RT_INPUT, 0, SWZ_XYZW));
    e.I4(OP_MAD, Dst(RT_COLOROUT, 1, 0x7), Src(RT_TEMP, 0, SWZ_XYZW), Src(RT_CONST, C, SWZ_XXXX), Src(RT_CONST, C, SWZ_XXXX));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 1, 0x8), Src(RT_CONST, C + 1, SWZ_XXXX));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 2, 0x7), Src(RT_CONST, C + 1, SWZ_WWWW));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 2, 0x8), Src(RT_CONST, C + 1, SWZ_YYYY));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 3, 0x7), Src(RT_CONST, C, SWZ_XXXX));
    e.I2(OP_MOV, Dst(RT_COLOROUT, 3, 0x8), Src(RT_CONST, C + 1, SWZ_WWWW));
    r.push_back(0x0000FFFF);
    return r;
}

}
