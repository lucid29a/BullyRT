#include "Telemetry.h"
#include "Config.h"
#include <psapi.h>
#include <dwmapi.h>
#include <dbghelp.h>
#pragma comment(lib, "gdi32.lib")
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace bdx12 { namespace telemetry {

long long g_ticks[kBucketCount] = {};
long long g_draws = 0;

namespace {

bool          g_enabled = false, g_started = false;
FILE*         g_file = nullptr;
double        g_toMs = 0.0;
long long     g_prevEnd = 0;
long long     g_lastTicks[kBucketCount] = {};
long long     g_lastDraws = 0;

struct Window {
    long long start = 0;
    int       frames = 0;
    double    frameSum = 0, frameMax = 0, gameSum = 0, pipelineSum = 0, limitSum = 0, presentSum = 0;
    double    bucket[kBucketCount] = {};
    long long draws = 0;
    int       foreground = 0;
} g_win;

HANDLE            g_mainThread = nullptr;
DWORD             g_mainThreadId = 0;
CRITICAL_SECTION  g_samplesLock;
std::map<std::string, int> g_top, g_caller;
int               g_samples = 0;
HMODULE           g_ownModule = nullptr;
volatile LONG64   g_lastPresentTick = 0;

volatile LONG g_screenChanges = 0, g_screenSamples = 0;
HRESULT       g_lastBadPresent = S_OK;
int           g_badPresents = 0;

std::string ModuleOf(DWORD address, DWORD* offset)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(address)), &m) || !m) return std::string();
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    if (offset) *offset = address - static_cast<DWORD>(reinterpret_cast<uintptr_t>(m));
    return name ? name + 1 : path;
}

bool IsSystemWait(const std::string& n)
{
    return _stricmp(n.c_str(), "ntdll.dll") == 0 || _stricmp(n.c_str(), "KERNELBASE.dll") == 0 || _stricmp(n.c_str(), "KERNEL32.DLL") == 0 || _stricmp(n.c_str(), "win32u.dll") == 0 || _stricmp(n.c_str(), "wow64cpu.dll") == 0;
}

void TimeNow(char* out, size_t size);

void ReportHang(const CONTEXT& c, const DWORD* stack, size_t words, ULONGLONG silentMs, bool dump)
{
    char when[32];
    TimeNow(when, sizeof(when));
    DWORD off = 0;
    std::string where = ModuleOf(c.Eip, &off);
    std::string line = "hang," + std::string(when) + ",no frame for " + std::to_string(silentMs) + " ms,eip ";
    char buf[200];
    sprintf_s(buf, "%s+0x%X", where.empty() ? "?" : where.c_str(), off);
    line += buf;
    line += ",stack";
    int listed = 0;
    for (size_t i = 0; i < words && listed < 24; ++i) {
        if (stack[i] < 0x10000) continue;
        DWORD o = 0;
        const std::string m = ModuleOf(stack[i], &o);
        if (m.empty()) continue;
        sprintf_s(buf, " %s+0x%X", m.c_str(), o);
        line += buf;
        ++listed;
    }
    EnterCriticalSection(&g_samplesLock);
    if (g_file) { fprintf(g_file, "%s\n", line.c_str()); fflush(g_file); }
    LeaveCriticalSection(&g_samplesLock);
    if (!dump) return;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) slash[1] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    sprintf_s(buf, "BullyDX12\\hang_%02d%02d%02d.dmp", st.wHour, st.wMinute, st.wSecond);
    strcat_s(path, buf);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PVOID, PVOID, PVOID);
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    auto write = dbg ? reinterpret_cast<WriteDump>(GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
    const BOOL ok = write && write(GetCurrentProcess(), GetCurrentProcessId(), f,
                                   static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                                   nullptr, nullptr, nullptr);
    CloseHandle(f);
    EnterCriticalSection(&g_samplesLock);
    if (g_file) { fprintf(g_file, "hang,%s,dump %s,%s\n", when, buf, ok ? "written" : "FAILED"); fflush(g_file); }
    LeaveCriticalSection(&g_samplesLock);
}

DWORD WINAPI SamplerThread(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ULONGLONG reportedFor = 0;
    int       reports = 0;
    for (;;) {
        Sleep(20);
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL;
        DWORD stack[96];
        SIZE_T got = 0;
        if (SuspendThread(g_mainThread) == static_cast<DWORD>(-1)) continue;
        const BOOL ok = GetThreadContext(g_mainThread, &c);
        if (ok) ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(static_cast<uintptr_t>(c.Esp)), stack, sizeof(stack), &got);
        ResumeThread(g_mainThread);
        if (!ok) continue;
        const ULONGLONG last = static_cast<ULONGLONG>(InterlockedCompareExchange64(&g_lastPresentTick, 0, 0));
        const ULONGLONG silent = last ? GetTickCount64() - last : 0;
        static const ULONGLONG kAt[3] = {5000, 15000, 30000};
        static bool dumped = false;
        if (last && last != reportedFor) reports = 0;
        if (last && reports < 3 && silent > kAt[reports]) {
            reportedFor = last;
            const bool dump = reports == 1 && !dumped;
            if (dump) dumped = true;
            ++reports;
            ReportHang(c, stack, got / sizeof(DWORD), silent, dump);
        }
        const std::string top = ModuleOf(c.Eip, nullptr);
        std::string caller;
        const size_t words = got / sizeof(DWORD);
        for (size_t i = 0; i < words && caller.empty(); ++i) {
            if (stack[i] < 0x10000) continue;
            DWORD off = 0;
            const std::string m = ModuleOf(stack[i], &off);
            if (m.empty() || IsSystemWait(m)) continue;
            char buf[160];
            sprintf_s(buf, "%s+0x%X", m.c_str(), off);
            caller = buf;
        }
        EnterCriticalSection(&g_samplesLock);
        ++g_samples;
        ++g_top[top.empty() ? "?" : top];
        ++g_caller[caller.empty() ? "?" : caller];
        LeaveCriticalSection(&g_samplesLock);
    }
}

std::string TopOf(const std::map<std::string, int>& counts, int total, size_t howMany)
{
    std::vector<std::pair<int, std::string>> v;
    for (const auto& kv : counts) v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < howMany; ++i) {
        char buf[200];
        sprintf_s(buf, "%s%s %d%%", i ? " | " : "", v[i].second.c_str(), total ? v[i].first * 100 / total : 0);
        out += buf;
    }
    return out;
}

void TimeNow(char* out, size_t size)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    sprintf_s(out, size, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void Start()
{
    g_started = true;
    if (!g_enabled) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_toMs = 1000.0 / static_cast<double>(f.QuadPart);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) slash[1] = 0;
    strcat_s(path, "BullyDX12\\telemetry_game.csv");
    if (fopen_s(&g_file, path, "w") != 0) g_file = nullptr;
    if (!g_file) { g_enabled = false; return; }
    fprintf(g_file, "kind,time,fps,frame_ms,worst_ms,game_ms,draw_real_ms,draw_hook_ms,other_hooks_ms,create_ms,pipeline_ms,limit_ms,present_ms,draws,foreground,private_mb,handles,ram_free_mb,commit_free_mb,thread_top,thread_caller,screen_changes,screen_samples,dwm_frames,dwm_refresh_hz,bad_presents,last_bad_present,window_minimized\n");
    fflush(g_file);
    InitializeCriticalSection(&g_samplesLock);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_mainThreadId = GetCurrentThreadId();
    if (g_mainThread) {
        HANDLE t = CreateThread(nullptr, 0, &SamplerThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}

bool GameInFront()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

}

void Init()
{
    g_enabled = _wtoi(GetConfig().GetString("Telemetry", "Enabled", L"1").c_str()) != 0;
}

void OnPresent(LARGE_INTEGER start, LARGE_INTEGER afterPipeline, LARGE_INTEGER afterLimit, LARGE_INTEGER end, HRESULT presentResult)
{
    if (!g_started) Start();
    if (!g_enabled) return;
    InterlockedExchange64(&g_lastPresentTick, static_cast<LONG64>(GetTickCount64()));
    if (presentResult != S_OK) { ++g_badPresents; g_lastBadPresent = presentResult; }
    if (!g_prevEnd) {
        g_prevEnd = end.QuadPart;
        g_win.start = end.QuadPart;
        for (int b = 0; b < kBucketCount; ++b) g_lastTicks[b] = g_ticks[b];
        g_lastDraws = g_draws;
        return;
    }
    double bucket[kBucketCount];
    for (int b = 0; b < kBucketCount; ++b) {
        bucket[b] = (g_ticks[b] - g_lastTicks[b]) * g_toMs;
        g_lastTicks[b] = g_ticks[b];
    }
    const long long draws = g_draws - g_lastDraws;
    g_lastDraws = g_draws;
    const double frame    = (end.QuadPart - g_prevEnd) * g_toMs;
    const double before   = (start.QuadPart - g_prevEnd) * g_toMs;
    const double pipeline = (afterPipeline.QuadPart - start.QuadPart) * g_toMs;
    const double limit    = (afterLimit.QuadPart - afterPipeline.QuadPart) * g_toMs;
    const double present  = (end.QuadPart - afterLimit.QuadPart) * g_toMs;
    const double game     = before - bucket[kDrawTotal] - bucket[kOther] - bucket[kCreate];
    g_prevEnd = end.QuadPart;

    ++g_win.frames;
    g_win.frameSum += frame;
    g_win.frameMax = (std::max)(g_win.frameMax, frame);
    g_win.gameSum += game;
    g_win.pipelineSum += pipeline;
    g_win.limitSum += limit;
    g_win.presentSum += present;
    for (int b = 0; b < kBucketCount; ++b) g_win.bucket[b] += bucket[b];
    g_win.draws += draws;
    const bool front = GameInFront();
    if (front) ++g_win.foreground;

    char when[32];
    if (frame > 100.0) {
        TimeNow(when, sizeof(when));
        fprintf(g_file, "spike,%s,,%.1f,,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%lld,%d,,,,,,\n", when, frame, game, bucket[kDrawTotal] - bucket[kDrawHook], bucket[kDrawHook], bucket[kOther], bucket[kCreate], pipeline, limit, present, draws, front ? 1 : 0);
        fflush(g_file);
    }

    const double elapsed = (end.QuadPart - g_win.start) * g_toMs;
    if (elapsed < 1000.0) return;
    const double n = g_win.frames;
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    std::string top, caller;
    EnterCriticalSection(&g_samplesLock);
    top = TopOf(g_top, g_samples, 3);
    caller = TopOf(g_caller, g_samples, 3);
    g_top.clear();
    g_caller.clear();
    g_samples = 0;
    LeaveCriticalSection(&g_samplesLock);
    TimeNow(when, sizeof(when));
    static unsigned long long lastDwmFrames = 0;
    typedef HRESULT (WINAPI *PFN_Timing)(HWND, DWM_TIMING_INFO*);
    static PFN_Timing timing = reinterpret_cast<PFN_Timing>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetCompositionTimingInfo"));
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    double refreshHz = 0.0;
    unsigned long long dwmFrames = 0;
    if (timing && SUCCEEDED(timing(nullptr, &ti))) {
        if (ti.rateRefresh.uiDenominator) refreshHz = static_cast<double>(ti.rateRefresh.uiNumerator) / ti.rateRefresh.uiDenominator;
        dwmFrames = ti.cFrame - lastDwmFrames;
        lastDwmFrames = ti.cFrame;
    }
    const LONG screenChanges = InterlockedExchange(&g_screenChanges, 0), screenSamples = InterlockedExchange(&g_screenSamples, 0);
    HWND frontWindow = GetForegroundWindow();
    fprintf(g_file, "second,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%d,%llu,%lu,%llu,%llu,\"%s\",\"%s\",%ld,%ld,%llu,%.1f,%d,0x%08X,%d\n", when, n * 1000.0 / elapsed, g_win.frameSum / n, g_win.frameMax, g_win.gameSum / n,
            (g_win.bucket[kDrawTotal] - g_win.bucket[kDrawHook]) / n, g_win.bucket[kDrawHook] / n, g_win.bucket[kOther] / n, g_win.bucket[kCreate] / n, g_win.pipelineSum / n, g_win.limitSum / n, g_win.presentSum / n,
            static_cast<double>(g_win.draws) / n, g_win.foreground * 100 / g_win.frames, static_cast<unsigned long long>(pm.PrivateUsage >> 20), handles,
            static_cast<unsigned long long>(ms.ullAvailPhys >> 20), static_cast<unsigned long long>(ms.ullAvailPageFile >> 20), top.c_str(), caller.c_str(),
            screenChanges, screenSamples, dwmFrames, refreshHz, g_badPresents, static_cast<unsigned>(g_lastBadPresent), frontWindow && IsIconic(frontWindow) ? 1 : 0);
    g_badPresents = 0;
    fflush(g_file);
    g_win = Window();
    g_win.start = end.QuadPart;
}

}}
