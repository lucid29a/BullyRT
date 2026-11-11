// =====================================================================================
// BullyDX12 ray tracing bridge - shared protocol

#pragma once
#include <cstdint>

namespace bdx12::bridge {

constexpr uint32_t kMagic             = 0x42545242;
constexpr uint32_t kProtocolVersion   = 14;
constexpr uint32_t kMaxLights         = 32;
constexpr uint32_t kMaxDynamicGeoms   = 1024;
constexpr uint32_t kMaxTexOps         = 64;
constexpr uint64_t kMaxDynamicBytes   = 8ull * 1024 * 1024;
constexpr uint32_t kMaxInstances      = 16384;
constexpr uint32_t kMaxMeshOps        = 512;
constexpr uint64_t kSlotDataBytes     = 16ull * 1024 * 1024;

enum : uint32_t { kSlotFree = 0, kSlotWriting = 1, kSlotReady = 2, kSlotProcessing = 3 };
enum : uint32_t { kMeshCreate = 1, kMeshDestroy = 2 };
enum : uint32_t { kEffectAO = 1u << 0, kEffectSunShadow = 1u << 1 };
enum : uint32_t { kInstanceAlphaTest = 1u << 0, kInstanceTwoSided = 1u << 1, kInstanceFrontCCW = 1u << 2, kInstanceGlass = 1u << 3, kInstanceGlow = 1u << 4, kInstanceMirror = 1u << 5,
                    kInstanceGameLamp = 1u << 6 };
constexpr uint32_t kInstanceEmissiveShift = 8;
constexpr uint32_t kMaxTexMaterials = 4096;

#pragma pack(push, 8)

struct MeshOp {
    uint32_t type;
    uint32_t meshId;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint64_t dataOffset;
};

struct Instance {
    uint32_t meshId;
    uint32_t mask;
    float    transform[3][4];
    uint32_t textureId;
    uint32_t flags;
    float    alphaRef;
    uint32_t reserved;
};

struct TexOp {
    uint32_t id;
    uint32_t width, height;
    uint32_t dxgiFormat;
    uint32_t rowBytes;
    uint32_t rows;
    uint64_t dataOffset;
};

struct DynGeom {
    uint32_t vertexCount;
    uint32_t indexCount;
    uint64_t dataOffset;
    uint32_t textureId;
    uint32_t reserved;
};

struct FrameSlot {
    volatile uint32_t state;
    uint32_t          slotIndex;
    uint64_t          frameId;
    uint64_t          gameFenceValue;
    uint64_t          outputFreeValue;

    float             viewProj[4][4];
    float             cameraPos[4];
    float             sunDirection[4];
    uint32_t          width, height;
    uint32_t          effects;
    uint32_t          aoSamples;
    float             aoRadius;
    float             shadowMaxDistance;
    float             normalBias;
    float             hitBakedRadiance;

    float             sunColor[4];
    float             skyColor[4];
    float             strength[4];
    float             atmosphere[4];
    float             trueSun[4];
    float             clouds[4];
    float             weatherTop[4];
    float             weatherBot[4];
    float             weatherFog[4];
    uint32_t          lightCount;
    uint32_t          reserved2;
    float             lightPos[kMaxLights][4];
    float             lightCol[kMaxLights][4];
    float             lightDir[kMaxLights][4];
    float             lightAtt[kMaxLights][4];
    float             tanSunRadius;
    float             denoisingRange;
    uint32_t          resetHistory;
    uint32_t          fullRayTracing;

    uint32_t          meshOpCount;
    uint32_t          instanceCount;
    uint32_t          texOpCount;
    uint32_t          dynamicGeomCount;
    MeshOp            meshOps[kMaxMeshOps];
    TexOp             texOps[kMaxTexOps];
    DynGeom           dynamicGeoms[kMaxDynamicGeoms];
    Instance          instances[kMaxInstances];
};

struct Header {
    uint32_t          magic;
    uint32_t          version;
    uint32_t          gamePid;
    uint32_t          adapterLuidLow;
    int32_t           adapterLuidHigh;
    uint32_t          headerBytes;
    uint64_t          slotDataOffset[2];
    uint64_t          mappingBytes;

    volatile uint32_t resourceGeneration;
    uint32_t          resourceWidth, resourceHeight;
    volatile uint32_t bridgeGeneration;
    uint32_t          reserved0;

    volatile uint32_t bridgeState;
    volatile uint32_t shutdownRequested;
    volatile uint64_t completedFrame;
    volatile uint64_t bridgeHeartbeat;
    uint32_t          bridgeRaytracingTier;
    uint32_t          bridgeMeshCount;
    uint32_t          bridgeLastInstanceCount;
    float             bridgeGpuMs;
    char              bridgeMessage[256];
    volatile uint32_t rtDisabled;
    volatile uint8_t  texEmissive[kMaxTexMaterials];

    FrameSlot         slots[2];
};

#pragma pack(pop)

constexpr uint64_t MappingBytes()
{
    return ((sizeof(Header) + 4095) & ~4095ull) + 2 * kSlotDataBytes;
}

}
