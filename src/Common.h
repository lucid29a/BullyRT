#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d9on12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <mutex>

#include "BullyDX12/BullyDX12.h"

using Microsoft::WRL::ComPtr;

namespace bdx12 {

void        LogInit(const std::wstring& path, LogLevel minLevel);
void        Startup(const char* step);
void        LogShutdown();
void        LogV(LogLevel level, const char* fmt, va_list args);
void        LogF(LogLevel level, const char* fmt, ...);
const char* HResultName(HRESULT hr);

#define BDX_TRACE(...) ::bdx12::LogF(::bdx12::LogLevel::Trace,   __VA_ARGS__)
#define BDX_INFO(...)  ::bdx12::LogF(::bdx12::LogLevel::Info,    __VA_ARGS__)
#define BDX_WARN(...)  ::bdx12::LogF(::bdx12::LogLevel::Warning, __VA_ARGS__)
#define BDX_ERROR(...) ::bdx12::LogF(::bdx12::LogLevel::Error,   __VA_ARGS__)

const std::wstring& GameDirectory();
std::wstring        Widen(const std::string& s);
std::string         Narrow(const std::wstring& s);

uint64_t Fnv1a64(const void* data, size_t size);

}
