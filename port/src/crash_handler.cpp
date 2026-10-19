// Prints the exception and a symbolized call stack when the process crashes (Windows),
// so crashes in recompiled code show which game function they happened in.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <mutex>

static std::mutex crash_mutex;

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

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    std::lock_guard lock{ crash_mutex };
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    fprintf(stderr, "\n[crash] exception 0x%08lX at %p, thread %lu\n",
            rec->ExceptionCode, rec->ExceptionAddress, GetCurrentThreadId());
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        fprintf(stderr, "[crash] %s address 0x%016llX\n",
                rec->ExceptionInformation[0] == 1 ? "write to" : rec->ExceptionInformation[0] == 8 ? "execute at" : "read from",
                (unsigned long long)rec->ExceptionInformation[1]);
    }

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    fprintf(stderr, "[crash] stack:\n");
    for (int i = 0; i < 48; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) {
            break;
        }
        print_frame(process, frame.AddrPC.Offset);
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void mt_install_crash_handler() {
    SetUnhandledExceptionFilter(crash_filter);
}
#else
void mt_install_crash_handler() {}
#endif
