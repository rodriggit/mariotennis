// Debugging aids (Windows):
//  - on a crash, print the exception and a symbolized call stack, so crashes in recompiled
//    code show which game function they happened in;
//  - with MT_DUMP_STACKS=<seconds>, after that time print the stack of every thread
//    (to find where a hung or spinning game is stuck).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

static std::mutex dbghelp_mutex;   // DbgHelp is single-threaded

static void init_symbols() {
    static bool initialized = false;
    if (!initialized) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        initialized = true;
    }
}

static void print_frame(HANDLE process, DWORD64 addr) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
    DWORD line_displacement = 0;
    if (SymFromAddr(process, addr, &displacement, symbol)) {
        if (SymGetLineFromAddr64(process, addr, &line_displacement, &line)) {
            fprintf(stderr, "  %016llX %s+0x%llX (%s:%lu)\n", addr, symbol->Name, displacement, line.FileName, line.LineNumber);
        } else {
            fprintf(stderr, "  %016llX %s+0x%llX\n", addr, symbol->Name, displacement);
        }
    } else {
        fprintf(stderr, "  %016llX ?\n", addr);
    }
}

static void walk_stack(HANDLE thread, CONTEXT context, int max_frames) {
    HANDLE process = GetCurrentProcess();
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < max_frames; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) {
            break;
        }
        print_frame(process, frame.AddrPC.Offset);
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    std::lock_guard lock{ dbghelp_mutex };
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    fprintf(stderr, "\n[crash] exception 0x%08lX at %p, thread %lu\n",
            rec->ExceptionCode, rec->ExceptionAddress, GetCurrentThreadId());
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        fprintf(stderr, "[crash] %s address 0x%016llX\n",
                rec->ExceptionInformation[0] == 1 ? "write to" : rec->ExceptionInformation[0] == 8 ? "execute at" : "read from",
                (unsigned long long)rec->ExceptionInformation[1]);
    }
    init_symbols();
    fprintf(stderr, "[crash] stack:\n");
    walk_stack(GetCurrentThread(), *info->ContextRecord, 48);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void dump_all_threads() {
    std::lock_guard lock{ dbghelp_mutex };
    init_symbols();
    DWORD self = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{ sizeof(THREADENTRY32) };
    fprintf(stderr, "\n[stacks] ---- all threads ----\n");
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        PWSTR name = nullptr;
        GetThreadDescription(thread, &name);
        FILETIME created, exited, kernel, user;
        GetThreadTimes(thread, &created, &exited, &kernel, &user);
        double cpu_s = ((ULARGE_INTEGER{ { user.dwLowDateTime, user.dwHighDateTime } }).QuadPart +
                        (ULARGE_INTEGER{ { kernel.dwLowDateTime, kernel.dwHighDateTime } }).QuadPart) / 1e7;
        fprintf(stderr, "[stacks] thread %lu \"%ls\" cpu %.1fs\n", entry.th32ThreadID, name ? name : L"", cpu_s);
        LocalFree(name);
        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                walk_stack(thread, context, 16);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    fflush(stderr);
}

void mt_install_crash_handler() {
    SetUnhandledExceptionFilter(crash_filter);
    if (const char* delay = std::getenv("MT_DUMP_STACKS")) {
        int seconds = std::atoi(delay);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            dump_all_threads();
        }).detach();
    }
}
#else
void mt_install_crash_handler() {}
#endif
