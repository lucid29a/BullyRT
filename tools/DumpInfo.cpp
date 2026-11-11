// DumpInfo - prints the exception, the faulting module and a symbolized stack of a minidump
// (MiniDumper.asi writes Bully.exe.<date>.dmp). No debugger needed.
// DumpInfo.exe <file.dmp> [symbol path]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <vector>
#include <string>
#pragma comment(lib, "dbghelp.lib")

struct Module { std::wstring name; ULONG64 base; ULONG size; };

static std::wstring ReadName(const BYTE* base, RVA rva)
{
    const auto* s = reinterpret_cast<const MINIDUMP_STRING*>(base + rva);
    return std::wstring(s->Buffer, s->Length / 2);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { printf("usage: DumpInfo <dump> [sympath]\n"); return 2; }
    HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { printf("cannot open dump\n"); return 1; }
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const BYTE* base = static_cast<const BYTE*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));

    std::vector<Module> mods;
    MINIDUMP_DIRECTORY* dir = nullptr; void* stream = nullptr; ULONG size = 0;
    if (MiniDumpReadDumpStream(const_cast<BYTE*>(base), ModuleListStream, &dir, &stream, &size)) {
        auto* ml = static_cast<MINIDUMP_MODULE_LIST*>(stream);
        for (ULONG i = 0; i < ml->NumberOfModules; ++i) {
            const MINIDUMP_MODULE& m = ml->Modules[i];
            mods.push_back({ReadName(base, m.ModuleNameRva), m.BaseOfImage, m.SizeOfImage});
        }
    }
    auto modOf = [&](ULONG64 a) -> const Module* {
        for (auto& m : mods) if (a >= m.base && a < m.base + m.size) return &m;
        return nullptr;
    };

    HANDLE proc = reinterpret_cast<HANDLE>(0x1234);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitializeW(proc, argc > 2 ? argv[2] : nullptr, FALSE);
    for (auto& m : mods) SymLoadModuleExW(proc, nullptr, m.name.c_str(), nullptr, m.base, m.size, nullptr, 0);

    auto describe = [&](ULONG64 a) {
        const Module* m = modOf(a);
        std::wstring mn = m ? m->name.substr(m->name.find_last_of(L'\\') + 1) : L"?";
        char buf[sizeof(SYMBOL_INFO) + 512] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line = {sizeof(line)};
        DWORD ld = 0;
        if (SymFromAddr(proc, a, &disp, sym)) {
            if (SymGetLineFromAddr64(proc, a, &ld, &line))
                printf("%ls!%s+0x%llx (%s:%lu)\n", mn.c_str(), sym->Name, disp, line.FileName, line.LineNumber);
            else
                printf("%ls!%s+0x%llx\n", mn.c_str(), sym->Name, disp);
        } else {
            printf("%ls+0x%llx\n", mn.c_str(), m ? a - m->base : a);
        }
    };

    if (MiniDumpReadDumpStream(const_cast<BYTE*>(base), ExceptionStream, &dir, &stream, &size)) {
        auto* es = static_cast<MINIDUMP_EXCEPTION_STREAM*>(stream);
        const MINIDUMP_EXCEPTION& e = es->ExceptionRecord;
        printf("exception 0x%08lX at 0x%llx (thread %lu)", e.ExceptionCode, e.ExceptionAddress, es->ThreadId);
        if (e.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e.NumberParameters >= 2)
            printf(" - %s 0x%llx", e.ExceptionInformation[0] ? "write" : "read", e.ExceptionInformation[1]);
        printf("\n  at ");
        describe(e.ExceptionAddress);

        const auto* ctx = reinterpret_cast<const WOW64_CONTEXT*>(base + es->ThreadContext.Rva);
        ULONG64 esp = ctx->Esp;
        if (MiniDumpReadDumpStream(const_cast<BYTE*>(base), ThreadListStream, &dir, &stream, &size)) {
            auto* tl = static_cast<MINIDUMP_THREAD_LIST*>(stream);
            for (ULONG i = 0; i < tl->NumberOfThreads; ++i) {
                const MINIDUMP_THREAD& t = tl->Threads[i];
                if (t.ThreadId != es->ThreadId) continue;
                const BYTE* mem = base + t.Stack.Memory.Rva;
                const ULONG64 start = t.Stack.StartOfMemoryRange;
                const ULONG64 end = start + t.Stack.Memory.DataSize;
                printf("  stack (return addresses found on the stack):\n");
                int shown = 0;
                for (ULONG64 a = esp; a + 4 <= end && shown < 40; a += 4) {
                    const DWORD v = *reinterpret_cast<const DWORD*>(mem + (a - start));
                    const Module* m = modOf(v);
                    if (!m) continue;
                    printf("    ");
                    describe(v);
                    ++shown;
                }
            }
        }
    } else {
        printf("no exception stream\n");
    }
    return 0;
}
