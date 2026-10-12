// StallWatch.cpp - BUILD 235. See StallWatch.h for why this exists.
#include "StallWatch.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>

namespace grwxr {
namespace stallwatch {
namespace {

// Fixed storage. Every byte of this is reserved BEFORE the first thread is
// suspended, because the allocator's lock may be held by a thread we are
// about to freeze.
constexpr int kMaxThreads = 128;
constexpr int kMaxModules = 512;
constexpr int kStackWords = 1024;  // 8 KB of stack read per thread.
// 256 words (2 KB) was the first try and it was NOT enough: the
// stalltest's deliberately wedged thread, parked in WaitForSingleObject,
// resolved zero frames because its return addresses sat past the 2 KB
// mark. A thread parked in a syscall is exactly the case this tool
// exists for, so the window has to cover it.
constexpr int kMaxFrames  = 12;    // return addresses reported per thread

struct ModuleRow {
    uintptr_t base;
    uintptr_t end;
    char      name[64];
};

struct ThreadRow {
    DWORD     tid;
    HANDLE    handle;
    bool      suspended;
    bool      got_context;
    uintptr_t rip;
    uintptr_t rsp;
    int       words;                    // how many stack words were read
    uintptr_t stack[kStackWords];
};

ModuleRow g_mods[kMaxModules];
int       g_nmods = 0;
ThreadRow g_threads[kMaxThreads];
int       g_nthreads = 0;

// Resolve an address to "module+0xoffset". Returns nullptr-ish by writing
// "?" when the address belongs to no loaded module, which is itself
// information: JIT, a stack value that is not a return address, or a module
// that unloaded.
const ModuleRow* owner_of(uintptr_t addr) {
    for (int i = 0; i < g_nmods; ++i)
        if (addr >= g_mods[i].base && addr < g_mods[i].end) return &g_mods[i];
    return nullptr;
}

// Build the module table. Called before any suspension, so it may allocate
// and call anything it likes.
void snapshot_modules() {
    g_nmods = 0;
    HMODULE mods[kMaxModules];
    DWORD needed = 0;
    HANDLE self = GetCurrentProcess();
    if (!EnumProcessModules(self, mods, (DWORD)sizeof(mods), &needed)) return;
    int n = (int)(needed / sizeof(HMODULE));
    if (n > kMaxModules) n = kMaxModules;
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(self, mods[i], &mi, sizeof(mi))) continue;
        ModuleRow& r = g_mods[g_nmods];
        r.base = (uintptr_t)mi.lpBaseOfDll;
        r.end  = r.base + mi.SizeOfImage;
        r.name[0] = 0;
        GetModuleBaseNameA(self, mods[i], r.name, (DWORD)sizeof(r.name));
        if (!r.name[0]) strncpy_s(r.name, "(unnamed)", _TRUNCATE);
        ++g_nmods;
    }
}

// Collect the thread ids. Also before any suspension.
void snapshot_thread_ids() {
    g_nthreads = 0;
    DWORD me = GetCurrentProcessId();
    DWORD self_tid = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != me) continue;
            if (te.th32ThreadID == self_tid) continue;   // never suspend ourselves
            if (g_nthreads >= kMaxThreads) break;
            ThreadRow& t = g_threads[g_nthreads];
            memset(&t, 0, sizeof(t));
            t.tid = te.th32ThreadID;
            t.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                  THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            ++g_nthreads;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// THE QUIET WINDOW. No allocation, no logging, no COM, nothing that takes a
// lock another thread might be holding. Suspend, read, resume, get out.
void gather() {
    for (int i = 0; i < g_nthreads; ++i) {
        ThreadRow& t = g_threads[i];
        if (!t.handle) continue;
        if (SuspendThread(t.handle) == (DWORD)-1) continue;
        t.suspended = true;

        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(t.handle, &ctx)) {
            t.got_context = true;
            t.rip = (uintptr_t)ctx.Rip;
            t.rsp = (uintptr_t)ctx.Rsp;
            // Read the stack through ReadProcessMemory rather than a direct
            // dereference: a bad or guard page then returns false instead of
            // faulting inside a diagnostic, with every other thread frozen.
            // Try the full window, then progressively shorter ones: a thread
            // close to the end of its stack cannot serve 8 KB, and
            // ReadProcessMemory fails the whole request rather than returning
            // a partial one. Without the fallback such a thread reads as
            // "nothing on the stack", which is the silent-empty-section trap.
            static const int kTries[] = {kStackWords, 512, 128, 32};
            for (int k = 0; k < 4 && t.words == 0; ++k) {
                SIZE_T got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)t.rsp,
                                      t.stack, kTries[k] * sizeof(uintptr_t), &got))
                    t.words = (int)(got / sizeof(uintptr_t));
            }
        }
        ResumeThread(t.handle);
        t.suspended = false;
    }
}

}  // namespace

int report(const char* why) {
    LOG_WARN("stall: ======== THREAD SNAPSHOT: %s ========", why ? why : "(no reason given)");

    snapshot_modules();
    snapshot_thread_ids();

    if (g_nthreads == 0) {
        // Never exit silently. A blank section reads as an answer.
        LOG_WARN("stall: NO THREADS COULD BE ENUMERATED (CreateToolhelp32Snapshot "
                 "failed, error %lu). Nothing below this line is missing because "
                 "it was fine; it is missing because the reading failed.",
                 GetLastError());
        return 0;
    }

    gather();

    int read_ok = 0, opened = 0;
    for (int i = 0; i < g_nthreads; ++i) {
        ThreadRow& t = g_threads[i];
        if (t.handle) ++opened;

        if (!t.handle) {
            LOG_WARN("stall: thread %-6lu COULD NOT BE OPENED (error %lu): no "
                     "reading, not an empty one", t.tid, GetLastError());
            continue;
        }
        if (!t.got_context) {
            LOG_WARN("stall: thread %-6lu opened but its context could not be "
                     "read: no reading, not an empty one", t.tid);
            CloseHandle(t.handle);
            t.handle = nullptr;
            continue;
        }
        ++read_ok;

        const ModuleRow* m = owner_of(t.rip);
        if (m) {
            LOG_WARN("stall: thread %-6lu AT %s+0x%llX", t.tid, m->name,
                     (unsigned long long)(t.rip - m->base));
        } else {
            LOG_WARN("stall: thread %-6lu AT 0x%016llX (no loaded module owns "
                     "this address)", t.tid, (unsigned long long)t.rip);
        }

        // Walk the stack for return addresses. This is a scan, not a real
        // unwind: it reports every stack word that lands inside a loaded
        // module, in order, deduped by module+offset. It over-reports (stale
        // values live on stacks) and that is the right way round for a
        // diagnostic - a name that should not be there is cheap to dismiss,
        // a missing name costs a session.
        int shown = 0;
        uintptr_t last = 0;
        for (int w = 0; w < t.words && shown < kMaxFrames; ++w) {
            uintptr_t a = t.stack[w];
            if (a == last) continue;
            const ModuleRow* fm = owner_of(a);
            if (!fm) continue;
            last = a;
            LOG_WARN("stall:   via %s+0x%llX", fm->name,
                     (unsigned long long)(a - fm->base));
            ++shown;
        }
        if (shown == 0)
            LOG_WARN("stall:   (no return address on this stack falls inside a "
                     "loaded module; it is parked below our own code)");

        CloseHandle(t.handle);
        t.handle = nullptr;
    }

    LOG_WARN("stall: %d thread(s) enumerated, %d opened, %d read. %d module(s) "
             "in the table.", g_nthreads, opened, read_ok, g_nmods);
    LOG_WARN("stall: READ THIS FOR A FOREIGN NAME. An OpenXR API layer holding "
             "the render thread appears here as its own DLL (ReShade64.dll, "
             "XR_APILAYER_*.dll). If nothing foreign is on any stack, the layer "
             "is NOT what is holding this machine.");
    LOG_WARN("stall: ======== END THREAD SNAPSHOT ========");
    return read_ok;
}

}  // namespace stallwatch
}  // namespace grwxr
