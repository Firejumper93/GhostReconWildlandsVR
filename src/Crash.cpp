// Crash.cpp - turn "it crashed" into "it crashed at X, in module Y, because Z".
//
// Rationale: we are hooking a Denuvo-protected game we cannot debug attached,
// in a process that spawns three times and shows no modules externally. Without
// this, every crash costs a guess-and-retest cycle, and guesses have already
// cost real time tonight.
//
// A vectored exception handler runs BEFORE SEH unwinding and before any of the
// game's own handlers, so we see the fault even if the game swallows it. We log
// and then decline to handle, so the game's normal crash path still runs and we
// have not changed behaviour, only observed it.
//
// Critically, we report whether the faulting address is inside OUR module. That
// single fact separates "our bug" from "the game's bug that our timing exposed",
// which are very different investigations.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include "Log.h"

#pragma comment(lib, "psapi.lib")

namespace grwxr {
namespace crash {
namespace {

PVOID g_handler = nullptr;
HMODULE g_self = nullptr;
volatile LONG g_reported = 0;

const char* code_name(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
        case 0xE06D7363:                      return "C++ exception";
        default:                              return "(other)";
    }
}

// Which module owns an address, and the offset within it.
bool module_for(void* addr, char* out_name, size_t name_len, uintptr_t* out_offset,
                HMODULE* out_mod) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod) || !mod) {
        return false;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    strncpy_s(out_name, name_len, base ? base + 1 : path, _TRUNCATE);
    *out_offset = (uintptr_t)addr - (uintptr_t)mod;
    *out_mod = mod;
    return true;
}

LONG CALLBACK on_exception(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;

    // WHITELIST, not a blacklist. The first version blacklisted a few known-noisy
    // codes and still drowned the log in handled COM failures: REGDB_E_IIDNOTREG
    // (0x80040155) raised as SEH by KERNELBASE and swallowed by combase, coming
    // from msctfp.dll and the Ubisoft overlay64.dll. Those are normal interface
    // negotiation, not crashes. Only genuine hardware faults are interesting.
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_INVALID_HANDLE:
        case 0xC0000409:   // STATUS_STACK_BUFFER_OVERRUN, also used by __fastfail
            break;         // interesting, fall through and report
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }

    // Report only the first few, and never re-enter.
    if (InterlockedIncrement(&g_reported) > 3) return EXCEPTION_CONTINUE_SEARCH;

    void* addr = ep->ExceptionRecord->ExceptionAddress;

    LOG_ERROR("");
    LOG_ERROR("################ EXCEPTION ################");
    LOG_ERROR("  code    : 0x%08lX  %s", (unsigned long)code, code_name(code));
    LOG_ERROR("  address : 0x%p", addr);
    LOG_ERROR("  thread  : %lu", GetCurrentThreadId());

    char name[MAX_PATH] = "(unknown)";
    uintptr_t off = 0;
    HMODULE mod = nullptr;
    if (module_for(addr, name, sizeof(name), &off, &mod)) {
        LOG_ERROR("  module  : %s + 0x%llX  (base 0x%p)",
                  name, (unsigned long long)off, (void*)mod);
        if (mod == g_self) {
            // NOT "this is our bug", and NOT "the run crashed". This handler is
            // VECTORED, so it fires FIRST CHANCE and then declines (below), and
            // an exception someone upstream catches still prints a full dump
            // here. Corpus check 2026-08-22: 120 of the 126 logs carrying a dump
            // kept heartbeating for a median of five more minutes. The old
            // wording is why logreport.py graded every one of them as a crash.
            LOG_ERROR("  >>> FAULT ADDRESS IS INSIDE OUR OWN DLL (first chance). <<<");
            LOG_ERROR("      If heartbeats continue below, it was handled and the");
            LOG_ERROR("      run is still valid. Worth fixing, not a crash.");
        } else {
            LOG_ERROR("  (fault is not in our module; likely the game, but our");
            LOG_ERROR("   timing or state may still have caused it)");
        }
    } else {
        LOG_ERROR("  module  : address belongs to no loaded module");
        LOG_ERROR("  >>> likely a corrupted call target or freed memory <<<");
    }

    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
        const ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
        const ULONG_PTR at = ep->ExceptionRecord->ExceptionInformation[1];
        LOG_ERROR("  operation: %s", op == 0 ? "READ" : (op == 1 ? "WRITE" : "EXECUTE"));
        LOG_ERROR("  bad addr : 0x%p", (void*)at);
        if (at < 0x10000) LOG_ERROR("  >>> near-null: a null pointer was dereferenced <<<");
    }

#ifdef _M_X64
    const CONTEXT* c = ep->ContextRecord;
    LOG_ERROR("  rip=0x%p rsp=0x%p rbp=0x%p", (void*)c->Rip, (void*)c->Rsp, (void*)c->Rbp);
    LOG_ERROR("  rcx=0x%p rdx=0x%p r8 =0x%p r9 =0x%p",
              (void*)c->Rcx, (void*)c->Rdx, (void*)c->R8, (void*)c->R9);
    LOG_ERROR("  rax=0x%p rbx=0x%p", (void*)c->Rax, (void*)c->Rbx);

    // Walk the return addresses on the stack and name the modules. Not a real
    // unwind, just a scan, but on a game with no symbols it is usually enough to
    // say "this went through our DLL" or "it did not".
    LOG_ERROR("  --- stack scan (return addresses by module) ---");
    uintptr_t* sp = (uintptr_t*)c->Rsp;
    int printed = 0;
    for (int i = 0; i < 256 && printed < 12; ++i) {
        uintptr_t v = 0;
        // Guarded read: the stack may be damaged.
        __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v < 0x10000) continue;
        char mn[MAX_PATH] = {};
        uintptr_t mo = 0;
        HMODULE mm = nullptr;
        if (module_for((void*)v, mn, sizeof(mn), &mo, &mm)) {
            LOG_ERROR("    [%02d] 0x%p  %s + 0x%llX%s", printed, (void*)v, mn,
                      (unsigned long long)mo, (mm == g_self) ? "   <-- OURS" : "");
            printed++;
        }
    }
#endif
    LOG_ERROR("###########################################");
    LOG_ERROR("");

    // Do not swallow it. Let the game's own handling proceed unchanged.
    return EXCEPTION_CONTINUE_SEARCH;
}

// Second net. A vectored handler sees exceptions as they are raised, but a
// process can also die by ExitProcess, TerminateProcess or __fastfail, none of
// which raise anything we would see. This catches the unhandled case, and the
// atexit hook catches an orderly exit. Between the three, a silent death
// becomes a logged one.
LONG WINAPI on_unhandled(PEXCEPTION_POINTERS ep) {
    LOG_ERROR("!!! UNHANDLED EXCEPTION FILTER: code 0x%08lX at 0x%p",
              (unsigned long)ep->ExceptionRecord->ExceptionCode,
              ep->ExceptionRecord->ExceptionAddress);
    on_exception(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_exit_hook() {
    LOG_INFO("process is exiting normally (atexit). Not a crash.");
}

}  // namespace

void install(HMODULE self) {
    g_self = self;
    // 1 = call us first, before anything else gets a look.
    g_handler = AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
    atexit(on_exit_hook);
    LOG_INFO("crash reporter installed (%s) + unhandled filter + atexit",
             g_handler ? "ok" : "FAILED");
}

void remove() {
    if (g_handler) { RemoveVectoredExceptionHandler(g_handler); g_handler = nullptr; }
}

}  // namespace crash
}  // namespace grwxr
