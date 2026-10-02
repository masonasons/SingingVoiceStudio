#include "app/crash.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <mutex>

#include "core/paths.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace svs {

namespace {

std::mutex g_mu;
std::deque<std::string> g_lines;
const size_t kKeep = 40;

#ifdef _WIN32

// Loaded by hand: dbghelp is on every Windows, but what it offers varies.
typedef BOOL(WINAPI *WriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
typedef BOOL(WINAPI *Walk)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                           PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64,
                           PTRANSLATE_ADDRESS_ROUTINE64);
typedef PVOID(WINAPI *TableAccess)(HANDLE, DWORD64);
typedef DWORD64(WINAPI *ModuleBase)(HANDLE, DWORD64);
typedef BOOL(WINAPI *SymInit)(HANDLE, PCSTR, BOOL);

std::string module_of(DWORD64 address) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(uintptr_t)address, &mod) || !mod)
        return "?";
    wchar_t name[MAX_PATH] = {0};
    GetModuleFileNameW(mod, name, MAX_PATH);
    char buf[MAX_PATH + 64];
    std::snprintf(buf, sizeof buf, "%s+0x%llx", base_name(narrow(name)).c_str(),
                  (unsigned long long)(address - (DWORD64)(uintptr_t)mod));
    return buf;
}

const char *describe(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000409: return "stack buffer overrun / fail-fast";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

void write_report(EXCEPTION_POINTERS *ep, const char *why) {
    std::string dir = settings_dir();
    make_dirs(dir);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    std::string base = join_path(dir, std::string("crash-") + stamp);

    FILE *f = _wfopen(widen(base + ".txt").c_str(), L"w");
    if (f) {
        std::fprintf(f, "Singing Voice Studio crashed: %s\nbuilt %s %s\n", why, __DATE__, __TIME__);
        if (ep && ep->ExceptionRecord) {
            EXCEPTION_RECORD *r = ep->ExceptionRecord;
            std::fprintf(f, "exception 0x%08lx (%s) at %s\n", (unsigned long)r->ExceptionCode,
                         describe(r->ExceptionCode), module_of((DWORD64)(uintptr_t)r->ExceptionAddress).c_str());
            if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
                std::fprintf(f, "  %s address 0x%llx\n",
                             r->ExceptionInformation[0] == 8 ? "executing" : r->ExceptionInformation[0] ? "writing" : "reading",
                             (unsigned long long)r->ExceptionInformation[1]);
        }
        std::fprintf(f, "thread %lu%s\n", (unsigned long)GetCurrentThreadId(),
                     GetCurrentThreadId() == GetWindowThreadProcessId(GetActiveWindow(), nullptr) ? "" : "");
        // the call stack, walked from the moment of the fault
        HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
        if (dbg && ep && ep->ContextRecord) {
            auto init = (SymInit)(void *)GetProcAddress(dbg, "SymInitialize");
            auto walk = (Walk)(void *)GetProcAddress(dbg, "StackWalk64");
            auto table = (TableAccess)(void *)GetProcAddress(dbg, "SymFunctionTableAccess64");
            auto modbase = (ModuleBase)(void *)GetProcAddress(dbg, "SymGetModuleBase64");
            if (init && walk && table && modbase) {
                HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
                init(proc, nullptr, TRUE);
                CONTEXT ctx = *ep->ContextRecord;
                STACKFRAME64 frame;
                std::memset(&frame, 0, sizeof frame);
#ifdef _M_X64
                DWORD machine = IMAGE_FILE_MACHINE_AMD64;
                frame.AddrPC.Offset = ctx.Rip;
                frame.AddrFrame.Offset = ctx.Rbp;
                frame.AddrStack.Offset = ctx.Rsp;
#elif defined(__x86_64__)
                DWORD machine = IMAGE_FILE_MACHINE_AMD64;
                frame.AddrPC.Offset = ctx.Rip;
                frame.AddrFrame.Offset = ctx.Rbp;
                frame.AddrStack.Offset = ctx.Rsp;
#else
                DWORD machine = IMAGE_FILE_MACHINE_I386;
                frame.AddrPC.Offset = ctx.Eip;
                frame.AddrFrame.Offset = ctx.Ebp;
                frame.AddrStack.Offset = ctx.Esp;
#endif
                frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
                std::fprintf(f, "stack:\n");
                for (int i = 0; i < 48; ++i) {
                    if (!walk(machine, proc, thread, &frame, &ctx, nullptr, table, modbase, nullptr)) break;
                    if (!frame.AddrPC.Offset) break;
                    std::fprintf(f, "  %2d  %s\n", i, module_of(frame.AddrPC.Offset).c_str());
                }
            }
        }
        std::fprintf(f, "\nlast messages:\n");
        {
            // no lock: the crash may have happened while it was held, and a
            // torn line is better than no report
            for (const std::string &l : g_lines) std::fprintf(f, "  %s\n", l.c_str());
        }
        std::fclose(f);
    }

    // the minidump: threads, stacks and the modules, enough for a debugger
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    auto dump = dbg ? (WriteDump)(void *)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
    if (dump) {
        HANDLE file = CreateFileW(widen(base + ".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info;
            info.ThreadId = GetCurrentThreadId();
            info.ExceptionPointers = ep;
            info.ClientPointers = FALSE;
            dump(GetCurrentProcess(), GetCurrentProcessId(), file,
                 MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), ep ? &info : nullptr,
                 nullptr, nullptr);
            CloseHandle(file);
        }
    }
}

LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) write_report(ep, "unhandled exception");
    return EXCEPTION_CONTINUE_SEARCH;      // and let Windows end it as it would have
}

void on_terminate() {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) {
        std::string what = "std::terminate";
        if (std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception &x) {
                what += std::string(": ") + x.what();
            } catch (...) {
                what += ": an exception of unknown type";
            }
        }
        write_report(nullptr, what.c_str());
    }
    std::abort();
}

#endif  // _WIN32

}  // namespace

void install_crash_reporter() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_crash);
    std::set_terminate(on_terminate);
#endif
}

void crash_note(const std::string &line) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lines.push_back(line);
    while (g_lines.size() > kKeep) g_lines.pop_front();
}

std::vector<std::string> unseen_crash_reports() {
    std::vector<std::string> out;
#ifdef _WIN32
    std::string dir = settings_dir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(join_path(dir, "crash-*.txt")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string name = narrow(fd.cFileName);
        std::string seen = join_path(dir, name + ".seen");
        if (file_exists(seen)) continue;
        out.push_back(join_path(dir, name));
        FILE *m = _wfopen(widen(seen).c_str(), L"w");
        if (m) std::fclose(m);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#endif
    return out;
}

}  // namespace svs
