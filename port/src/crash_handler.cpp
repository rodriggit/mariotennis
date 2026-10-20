// Debugging aids (Windows):
//  - on a crash, print the exception and a symbolized call stack, so crashes in recompiled
//    code show which game function they happened in;
//  - with MT_DUMP_STACKS=<seconds>, after that time print the stack of every thread
//    (to find where a hung or spinning game is stuck);
//  - with MT_PROFILE=<seconds>, a sampling profiler that reports every that many seconds
//    (to find what makes the game slow down);
//  - with MT_WATCH=<addr>:<size>,..., RDRAM watchpoints: which code reads or writes there.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

// Unwinds a suspended thread with RtlVirtualUnwind only: no allocation or locks, so it
// can't deadlock on a lock the suspended thread holds. Symbols are resolved afterwards.
static int capture_frames(const CONTEXT& start, DWORD64* frames, int max_frames) {
    CONTEXT context = start;
    int count = 0;
    while (count < max_frames && context.Rip != 0) {
        frames[count++] = context.Rip;
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // leaf function: the return address is at rsp
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        } else {
            PVOID handler_data = nullptr;
            DWORD64 establisher_frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                             &handler_data, &establisher_frame, nullptr);
        }
    }
    return count;
}

struct ThreadDump {
    DWORD id;
    wchar_t name[64];
    double cpu_s;
    DWORD64 frames[24];
    int num_frames;
};

static void dump_all_threads() {
    static ThreadDump dumps[256];
    int num_dumps = 0;
    DWORD self = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{ sizeof(THREADENTRY32) };
    for (BOOL ok = Thread32First(snapshot, &entry); ok && num_dumps < 256; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        ThreadDump& d = dumps[num_dumps++];
        d.id = entry.th32ThreadID;
        d.name[0] = 0;
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &name)) && name != nullptr) {
            wcsncpy_s(d.name, name, _TRUNCATE);
            LocalFree(name);
        }
        FILETIME created, exited, kernel, user;
        GetThreadTimes(thread, &created, &exited, &kernel, &user);
        d.cpu_s = ((ULARGE_INTEGER{ { user.dwLowDateTime, user.dwHighDateTime } }).QuadPart +
                   (ULARGE_INTEGER{ { kernel.dwLowDateTime, kernel.dwHighDateTime } }).QuadPart) / 1e7;
        d.num_frames = 0;
        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                d.num_frames = capture_frames(context, d.frames, 24);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);

    // busiest first
    std::sort(dumps, dumps + num_dumps, [](const ThreadDump& a, const ThreadDump& b) { return a.cpu_s > b.cpu_s; });
    std::lock_guard lock{ dbghelp_mutex };
    init_symbols();
    fprintf(stderr, "\n[stacks] ---- all threads, busiest first ----\n");
    for (int i = 0; i < num_dumps; i++) {
        fprintf(stderr, "[stacks] thread %lu \"%ls\" cpu %.1fs\n", dumps[i].id, dumps[i].name, dumps[i].cpu_s);
        for (int j = 0; j < dumps[i].num_frames; j++) {
            print_frame(GetCurrentProcess(), dumps[i].frames[j]);
        }
    }
    fflush(stderr);
}

// ---- Sampling profiler (MT_PROFILE=<seconds per report>) ----
// About every millisecond, each thread that used CPU since the previous sample is suspended
// and its stack captured. Every window prints one line to stderr (frame rates, busiest
// threads) and appends a report to mt_profile.txt: per busy thread, the hottest functions
// by self time (innermost frame), by innermost recompiled game function, and inclusive.

extern std::atomic<uint32_t> mt_dl_count;   // rt64_context.cpp
extern std::atomic<uint32_t> mt_vi_count;
extern std::atomic<uint8_t*> mt_rdram;      // for the watchpoints below

namespace {

constexpr int PROF_MAX_FRAMES = 64;
constexpr ULONG64 PROF_MIN_CYCLES = 100000;   // below this the thread was waiting

struct ProfThread {
    DWORD id;
    HANDLE handle;
    std::wstring name;
    ULONG64 last_cycles;
    ULONG64 start_cpu;   // 100 ns units
    std::vector<std::vector<DWORD64>> stacks;   // function start per frame, innermost first
};

struct ProfSymbol {
    std::string name;
    bool game;   // recompiled game code (defined in RecompiledFuncs)
};

ULONG64 thread_cpu_time(HANDLE thread) {
    FILETIME created, exited, kernel, user;
    GetThreadTimes(thread, &created, &exited, &kernel, &user);
    return (ULARGE_INTEGER{ { user.dwLowDateTime, user.dwHighDateTime } }).QuadPart +
           (ULARGE_INTEGER{ { kernel.dwLowDateTime, kernel.dwHighDateTime } }).QuadPart;
}

// Function start of each frame (the frame address itself when there's no unwind entry).
void frames_to_functions(const DWORD64* frames, int count, std::vector<DWORD64>& out) {
    out.clear();
    for (int i = 0; i < count; i++) {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(frames[i], &image_base, nullptr);
        out.push_back(function != nullptr ? image_base + function->BeginAddress : frames[i]);
    }
}

const ProfSymbol& lookup_symbol(std::unordered_map<DWORD64, ProfSymbol>& cache, DWORD64 addr) {
    auto it = cache.find(addr);
    if (it != cache.end()) {
        return it->second;
    }
    HANDLE process = GetCurrentProcess();
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    ProfSymbol result;
    if (SymFromAddr(process, addr, &displacement, symbol)) {
        result.name = symbol->Name;
    } else {
        char hex[32];
        snprintf(hex, sizeof(hex), "0x%016llX", addr);
        result.name = hex;
    }
    IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
    DWORD line_displacement = 0;
    result.game = SymGetLineFromAddr64(process, addr, &line_displacement, &line) &&
                  strstr(line.FileName, "RecompiledFuncs") != nullptr;
    return cache.emplace(addr, std::move(result)).first->second;
}

void print_top(FILE* out, const char* title, const std::unordered_map<DWORD64, uint32_t>& counts,
               size_t total, std::unordered_map<DWORD64, ProfSymbol>& symbols, int limit) {
    std::vector<std::pair<DWORD64, uint32_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    fprintf(out, "    %s:\n", title);
    for (int i = 0; i < (int)sorted.size() && i < limit; i++) {
        fprintf(out, "      %5.1f%%  %s\n", 100.0 * sorted[i].second / total, lookup_symbol(symbols, sorted[i].first).name.c_str());
    }
}

void profile_loop(double window_s) {
    timeBeginPeriod(1);
    FILE* out = fopen("mt_profile.txt", "w");
    if (out == nullptr) {
        fprintf(stderr, "[prof] can't open mt_profile.txt\n");
        return;
    }
    fprintf(stderr, "[prof] sampling every ~1 ms, report every %.1f s to mt_profile.txt\n", window_s);
    {
        std::lock_guard lock{ dbghelp_mutex };
        init_symbols();
    }
    std::unordered_map<DWORD64, ProfSymbol> symbols;
    const DWORD self = GetCurrentThreadId();
    const auto t0 = std::chrono::steady_clock::now();
    static DWORD64 frames[PROF_MAX_FRAMES];

    for (;;) {
        // threads come and go: take a fresh list every window
        std::vector<ProfThread> threads;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{ sizeof(THREADENTRY32) };
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
                continue;
            }
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (handle == nullptr) {
                continue;
            }
            ProfThread t{ entry.th32ThreadID, handle };
            PWSTR name = nullptr;
            if (SUCCEEDED(GetThreadDescription(handle, &name)) && name != nullptr) {
                t.name = name;
                LocalFree(name);
            }
            QueryThreadCycleTime(handle, &t.last_cycles);
            t.start_cpu = thread_cpu_time(handle);
            threads.push_back(std::move(t));
        }
        CloseHandle(snapshot);

        const uint32_t dl_start = mt_dl_count.load();
        const uint32_t vi_start = mt_vi_count.load();
        const auto window_start = std::chrono::steady_clock::now();
        const auto window_end = window_start + std::chrono::duration<double>(window_s);
        std::vector<DWORD64> functions;
        while (std::chrono::steady_clock::now() < window_end) {
            for (ProfThread& t : threads) {
                ULONG64 cycles = 0;
                QueryThreadCycleTime(t.handle, &cycles);
                bool ran = cycles - t.last_cycles >= PROF_MIN_CYCLES;
                t.last_cycles = cycles;
                if (!ran || SuspendThread(t.handle) == (DWORD)-1) {
                    continue;
                }
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                int count = GetThreadContext(t.handle, &context) ? capture_frames(context, frames, PROF_MAX_FRAMES) : 0;
                ResumeThread(t.handle);
                if (count > 0) {
                    frames_to_functions(frames, count, functions);
                    t.stacks.push_back(functions);
                }
            }
            Sleep(1);
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - window_start).count();
        const double at = std::chrono::duration<double>(window_start - t0).count();
        const double dl_rate = (mt_dl_count.load() - dl_start) / elapsed;
        const double vi_rate = (mt_vi_count.load() - vi_start) / elapsed;

        struct Busy { ProfThread* thread; double cpu; };
        std::vector<Busy> busy;
        for (ProfThread& t : threads) {
            double cpu = (thread_cpu_time(t.handle) - t.start_cpu) / 1e7 / elapsed;
            if (cpu >= 0.05) {
                busy.push_back({ &t, cpu });
            }
        }
        std::sort(busy.begin(), busy.end(), [](const Busy& a, const Busy& b) { return a.cpu > b.cpu; });

        fprintf(stderr, "[prof] %6.1fs  display lists %5.1f/s  VI %5.1f/s  |", at, dl_rate, vi_rate);
        for (size_t i = 0; i < busy.size() && i < 4; i++) {
            fprintf(stderr, " %ls %.0f%%", busy[i].thread->name.empty() ? L"?" : busy[i].thread->name.c_str(), busy[i].cpu * 100);
        }
        fprintf(stderr, "\n");

        fprintf(out, "==== %.1fs - %.1fs   display lists %.1f/s   VI %.1f/s ====\n", at, at + elapsed, dl_rate, vi_rate);
        std::lock_guard lock{ dbghelp_mutex };
        for (const Busy& b : busy) {
            const ProfThread& t = *b.thread;
            fprintf(out, "  thread %lu \"%ls\"  cpu %.0f%%  samples %zu\n", t.id, t.name.c_str(), b.cpu * 100, t.stacks.size());
            if (t.stacks.empty()) {
                continue;
            }
            std::unordered_map<DWORD64, uint32_t> self_counts, game_counts, inclusive_counts;
            for (const auto& stack : t.stacks) {
                self_counts[stack[0]]++;
                for (DWORD64 f : stack) {
                    if (lookup_symbol(symbols, f).game) {
                        game_counts[f]++;
                        break;
                    }
                }
                std::vector<DWORD64> unique = stack;
                std::sort(unique.begin(), unique.end());
                unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
                for (DWORD64 f : unique) {
                    inclusive_counts[f]++;
                }
            }
            print_top(out, "self (innermost function)", self_counts, t.stacks.size(), symbols, 15);
            print_top(out, "innermost game function", game_counts, t.stacks.size(), symbols, 15);
            print_top(out, "inclusive", inclusive_counts, t.stacks.size(), symbols, 25);
        }
        fprintf(out, "\n");
        fflush(out);
        for (ProfThread& t : threads) {
            CloseHandle(t.handle);
        }
    }
}

// ---- RDRAM watchpoints (MT_WATCH=<addr>:<size>[,<addr>:<size>...], hex physical addresses) ----
// The watched pages are made inaccessible every 100 ms; the first access to each one faults,
// is recorded (read/write, call stack) and the page is opened again so the access completes.
// Every 2 s the accesses are printed grouped by call stack: which game function, runtime or
// RT64 code reads or writes that memory (4 KB granularity: neighbours on the same pages
// show up too).

struct WatchRange {
    uint32_t start;
    uint32_t end;
};
std::vector<WatchRange> watch_ranges;

constexpr int WATCH_FRAMES = 6;
constexpr uint32_t WATCH_MAX_HITS = 4096;

struct WatchHit {
    uint32_t addr;
    bool write;
    int num_frames;
    DWORD64 frames[WATCH_FRAMES];
};
WatchHit watch_hits[WATCH_MAX_HITS];
std::atomic<uint32_t> watch_hit_count{0};

uint32_t watched_offset(uint8_t* rdram, ULONG_PTR address, bool* watched) {
    *watched = false;
    if (rdram == nullptr || address < (ULONG_PTR)rdram || address >= (ULONG_PTR)rdram + 0x800000) {
        return 0;
    }
    uint32_t offset = (uint32_t)(address - (ULONG_PTR)rdram);
    for (const WatchRange& range : watch_ranges) {
        if (offset >= (range.start & ~0xFFFu) && offset < ((range.end + 0xFFF) & ~0xFFFu)) {
            *watched = true;
        }
    }
    return offset;
}

LONG CALLBACK watch_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uint8_t* rdram = mt_rdram.load();
    bool watched;
    uint32_t offset = watched_offset(rdram, rec->ExceptionInformation[1], &watched);
    if (!watched) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uint32_t index = watch_hit_count.fetch_add(1);
    if (index < WATCH_MAX_HITS) {
        WatchHit& hit = watch_hits[index];
        hit.addr = offset;
        hit.write = rec->ExceptionInformation[0] == 1;
        hit.num_frames = capture_frames(*info->ContextRecord, hit.frames, WATCH_FRAMES);
    }
    DWORD old;
    VirtualProtect(rdram + (offset & ~0xFFFu), 0x1000, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void watch_protect(uint8_t* rdram) {
    for (const WatchRange& range : watch_ranges) {
        uint32_t start = range.start & ~0xFFFu;
        uint32_t end = (range.end + 0xFFF) & ~0xFFFu;
        DWORD old;
        VirtualProtect(rdram + start, end - start, PAGE_NOACCESS, &old);
    }
}

void watch_loop() {
    uint8_t* rdram;
    while ((rdram = mt_rdram.load()) == nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    {
        std::lock_guard lock{ dbghelp_mutex };
        init_symbols();
    }
    std::unordered_map<DWORD64, ProfSymbol> symbols;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        for (int i = 0; i < 20; i++) {
            watch_protect(rdram);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        uint32_t count = (std::min)(watch_hit_count.exchange(0), WATCH_MAX_HITS);
        if (count == 0) {
            continue;
        }
        // group by access type and call stack
        struct Group {
            uint32_t hits;
            uint32_t min_addr;
            uint32_t max_addr;
        };
        std::unordered_map<std::string, Group> groups;
        std::lock_guard lock{ dbghelp_mutex };
        for (uint32_t i = 0; i < count; i++) {
            const WatchHit& hit = watch_hits[i];
            std::string key = hit.write ? "W " : "R ";
            for (int f = 0; f < hit.num_frames; f++) {
                key += (f == 0 ? "" : " < ") + lookup_symbol(symbols, hit.frames[f]).name;
            }
            auto [it, inserted] = groups.try_emplace(key, Group{ 0, hit.addr, hit.addr });
            it->second.hits++;
            it->second.min_addr = (std::min)(it->second.min_addr, hit.addr);
            it->second.max_addr = (std::max)(it->second.max_addr, hit.addr);
        }
        const double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "[watch] %.1fs: %u page faults\n", at, count);
        for (const auto& [key, group] : groups) {
            fprintf(stderr, "[watch]   %4u  0x%06X-0x%06X  %s\n", group.hits, group.min_addr, group.max_addr, key.c_str());
        }
        fflush(stderr);
    }
}

void watch_start(const char* spec) {
    const char* p = spec;
    for (;;) {
        char* next;
        uint32_t start = (uint32_t)strtoul(p, &next, 16) & 0x7FFFFF;
        if (next == p) {
            break;
        }
        uint32_t size = 0x1000;
        if (*next == ':') {
            size = (uint32_t)strtoul(next + 1, &next, 16);
        }
        watch_ranges.push_back({ start, start + size });
        fprintf(stderr, "[watch] RDRAM 0x%06X-0x%06X\n", start, start + size);
        if (*next != ',') {
            break;
        }
        p = next + 1;
    }
    if (!watch_ranges.empty()) {
        AddVectoredExceptionHandler(1, watch_handler);
        std::thread(watch_loop).detach();
    }
}

} // namespace

void mt_install_crash_handler() {
    SetUnhandledExceptionFilter(crash_filter);
    if (const char* spec = std::getenv("MT_WATCH")) {
        watch_start(spec);
    }
    if (const char* window = std::getenv("MT_PROFILE")) {
        double seconds = std::atof(window);
        std::thread(profile_loop, seconds > 0 ? seconds : 2.0).detach();
    }
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
