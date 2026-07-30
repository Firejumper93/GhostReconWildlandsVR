// ThunkHook.cpp - see ThunkHook.h for why a thunk rewrite beats a prologue
// detour on this target.

#include "ThunkHook.h"

#include "Log.h"

#include <cstring>

namespace grwxr {
namespace hook {

bool ThunkHook::install(uint8_t* thunk, void* expected_target, void* replacement,
                        const char* name) {
    name_ = name;
    if (installed_) {
        LOG_ERROR("hook(%s): already installed. Refusing to double-install.", name);
        return false;
    }
    if (!thunk || !expected_target || !replacement) {
        LOG_ERROR("hook(%s): null argument. Not installing.", name);
        return false;
    }

    // --- verify the slot really is a padded jump thunk ----------------------
    if (thunk[0] != 0xE9) {
        LOG_ERROR("hook(%s): thunk at 0x%p does not start with E9 (found 0x%02X).",
                  name, (void*)thunk, thunk[0]);
        return false;
    }
    for (int i = 5; i < 14; ++i) {
        if (thunk[i] != 0xCC) {
            LOG_ERROR("hook(%s): thunk slot at 0x%p is not int3-padded at +%d "
                      "(found 0x%02X). Refusing to write past the stub.",
                      name, (void*)thunk, i, thunk[i]);
            return false;
        }
    }

    int32_t rel = 0;
    memcpy(&rel, thunk + 1, sizeof(rel));
    uint8_t* resolved = thunk + 5 + rel;
    if (resolved != (uint8_t*)expected_target) {
        LOG_ERROR("hook(%s): thunk at 0x%p jumps to 0x%p but the signature scan "
                  "found the function at 0x%p. Not installing.",
                  name, (void*)thunk, (void*)resolved, expected_target);
        return false;
    }

    original_ = expected_target;
    thunk_    = thunk;
    memcpy(saved_, thunk, sizeof(saved_));

    // --- write jmp qword ptr [rip+0]; <absolute target> ---------------------
    uint8_t patch[14] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const uint64_t dest = (uint64_t)replacement;
    memcpy(patch + 6, &dest, sizeof(dest));

    DWORD old = 0;
    if (!VirtualProtect(thunk, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("hook(%s): VirtualProtect failed on 0x%p (err %lu). Not installing.",
                  name, (void*)thunk, GetLastError());
        return false;
    }
    memcpy(thunk, patch, sizeof(patch));
    VirtualProtect(thunk, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(patch));

    installed_ = true;
    LOG_INFO("hook(%s): thunk 0x%p redirected -> 0x%p (original 0x%p)",
             name, (void*)thunk, replacement, original_);
    return true;
}

void ThunkHook::restore() {
    if (!installed_) return;
    DWORD old = 0;
    if (VirtualProtect(thunk_, sizeof(saved_), PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(thunk_, saved_, sizeof(saved_));
        VirtualProtect(thunk_, sizeof(saved_), old, &old);
        FlushInstructionCache(GetCurrentProcess(), thunk_, sizeof(saved_));
    }
    installed_ = false;
    LOG_INFO("hook(%s): thunk restored", name_);
}

}  // namespace hook
}  // namespace grwxr
