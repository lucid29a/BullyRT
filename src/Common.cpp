#include "Common.h"

#include <cstdio>

namespace bdx12 {

namespace {
std::mutex g_logMutex;
FILE*      g_logFile  = nullptr;
LogLevel   g_logLevel = LogLevel::Info;

const char* LevelTag(LogLevel l)
{
    switch (l) {
    case LogLevel::Trace:   return "TRACE";
    case LogLevel::Info:    return "INFO ";
    case LogLevel::Warning: return "WARN ";
    default:                return "ERROR";
    }
}
}

void Startup(const char* step)
{
    static bool first = true;
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) return;
    wchar_t dir[MAX_PATH + 32], file[MAX_PATH + 64];
    wsprintfW(dir, L"%s\\BullyDX12", local);
    CreateDirectoryW(dir, nullptr);
    wsprintfW(file, L"%s\\startup.txt", dir);
    HANDLE h = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, first ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    first = false;
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[512];
    const int n = wsprintfA(line, "[%02u:%02u:%02u.%03u] pid %lu thread %lu: %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                            GetCurrentProcessId(), GetCurrentThreadId(), step);
    DWORD written = 0;
    WriteFile(h, line, static_cast<DWORD>(n), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

void LogInit(const std::wstring& path, LogLevel minLevel)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLevel = minLevel;
    if (!g_logFile)
        g_logFile = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    if (!g_logFile) {
        wchar_t local[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
            std::wstring dir = std::wstring(local) + L"\\BullyDX12";
            CreateDirectoryW(dir.c_str(), nullptr);
            g_logFile = _wfsopen((dir + L"\\BullyDX12.log").c_str(), L"w", _SH_DENYWR);
            if (g_logFile) fprintf(g_logFile, "(the game folder is not writable: this log is in %%LOCALAPPDATA%%\\BullyDX12)\n");
        }
    }
}

void LogShutdown()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void LogV(LogLevel level, const char* fmt, va_list args)
{
    if (level < g_logLevel)
        return;

    char message[2048];
    vsnprintf(message, sizeof(message), fmt, args);

    SYSTEMTIME t;
    GetLocalTime(&t);

    char line[2200];
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] [%s] [T%05lu] %s\n", t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, LevelTag(level), GetCurrentThreadId(), message);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
    OutputDebugStringA(line);
}

void LogF(LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV(level, fmt, args);
    va_end(args);
}

const char* HResultName(HRESULT hr)
{
    switch (hr) {
    case S_OK:                          return "S_OK";
    case E_FAIL:                        return "E_FAIL";
    case E_INVALIDARG:                  return "E_INVALIDARG";
    case E_OUTOFMEMORY:                 return "E_OUTOFMEMORY";
    case E_NOINTERFACE:                 return "E_NOINTERFACE";
    case E_NOTIMPL:                     return "E_NOTIMPL";
    case D3DERR_INVALIDCALL:            return "D3DERR_INVALIDCALL";
    case D3DERR_NOTAVAILABLE:           return "D3DERR_NOTAVAILABLE";
    case D3DERR_DEVICELOST:             return "D3DERR_DEVICELOST";
    case D3DERR_DEVICENOTRESET:         return "D3DERR_DEVICENOTRESET";
    case D3DERR_OUTOFVIDEOMEMORY:       return "D3DERR_OUTOFVIDEOMEMORY";
    case DXGI_ERROR_DEVICE_REMOVED:     return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_HUNG:        return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_RESET:       return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_UNSUPPORTED:        return "DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_NOT_FOUND:          return "DXGI_ERROR_NOT_FOUND";
    default: {
        static char ring[8][16];
        static volatile long next = 0;
        char* buf = ring[static_cast<unsigned long>(InterlockedIncrement(&next)) & 7u];
        snprintf(buf, 16, "0x%08lX", static_cast<unsigned long>(hr));
        return buf;
    }
    }
}

const std::wstring& GameDirectory()
{
    static const std::wstring dir = [] {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        std::wstring s(path);
        const size_t slash = s.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring(L".\\") : s.substr(0, slash + 1);
    }();
    return dir;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& s)
{
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

uint64_t Fnv1a64(const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

}
