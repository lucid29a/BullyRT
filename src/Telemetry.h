// Where the time of the game goes, measured inside the game: BullyDX12\telemetry_game.csv
// One line per second and one per frame over 100 ms. A sampling thread also records, 50 times a second, in which module the game's
// thread is and who called it, so that a stall names its culprit even when it is inside the driver or the game itself.

#pragma once
#include <windows.h>

namespace bdx12 { namespace telemetry {

enum Bucket { kDrawTotal, kDrawHook, kOther, kCreate, kBucketCount };

extern long long g_ticks[kBucketCount];
extern long long g_draws;

void Init();
void OnPresent(LARGE_INTEGER start, LARGE_INTEGER afterPipeline, LARGE_INTEGER afterLimit, LARGE_INTEGER end, HRESULT presentResult);

class Scope {
public:
    explicit Scope(Bucket b) : m_bucket(b) { QueryPerformanceCounter(&m_start); }
    ~Scope() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_ticks[m_bucket] += now.QuadPart - m_start.QuadPart;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Bucket        m_bucket;
    LARGE_INTEGER m_start;
};

}}
