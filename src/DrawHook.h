// DrawHook.h - detour the D3D11 draw functions in d3d11.dll itself.
//
// WHY THIS EXISTS (build 36, session 23, 2026-08-04)
// -------------------------------------------------
// Build 33.1's palette probe patched the draw slots of the game's immediate
// context vtable AND of a throwaway deferred context's vtable, then sat through
// 81 seconds of rendering at 72 fps and counted ZERO draws through all eight
// slots (grwxr-28060.log, 17:53:49). That is not a sparse scene; it is the
// wrong interception layer, and the log says why:
//
//     pal: immediate context vtable 0x00000000567D56F0 patched
//     pal: deferred  context vtable 0x000000016D205040 patched
//
// Both "vtables" are HEAP addresses, not addresses inside d3d11.dll (which
// loads in the 0x00007FF... range). The D3D11 runtime gives each device context
// object its OWN heap-allocated vtable, so patching one context's vtable
// affects exactly that one object and nothing else. It is not a class vtable.
// Whatever contexts the game actually records its scene draws on, they each
// carry a different heap vtable and never saw our patch.
//
// The fix is to stop patching pointers and patch the CODE. Every context, of
// every kind, created at any time, funnels into the same handful of functions
// inside d3d11.dll, so a detour at the function entry catches all of them. That
// is also why this is a friendlier target than it sounds: d3d11.dll is
// Microsoft's own module, not Denuvo-protected game code, and the entry
// addresses are read out of a live vtable rather than guessed.
//
// HOW THE PATCH AVOIDS A TORN INSTRUCTION STREAM. Draw functions are hot and
// worker threads may be inside them while we patch. Rather than suspend threads
// (heavy, and risky under anti-tamper), every entry is patched with a SINGLE
// 8-byte aligned atomic store: E9 rel32 plus three NOPs. Function entries are
// 16-byte aligned, so an 8-byte store there is aligned and therefore atomic; no
// thread can ever observe half a jump. To make 8 bytes safe to overwrite, the
// installer decodes whole instructions until it has stolen at least 8 bytes.
//
// Instruction decoding is a WHITELIST, not a general disassembler: only the
// handful of position-independent prologue forms these functions actually use
// are recognised, and anything unrecognised aborts the install and logs the
// first 16 bytes so the whitelist can be extended in the next build. Nothing is
// written unless every target decodes cleanly (project rule 7).
//
// install() is reversible: restore() puts the original 8 bytes back through the
// same atomic store.

#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace grwxr {
namespace drawhook {

// The entry points we intercept, as they arrive at our recorder.
// Build 37 adds Dispatch: RE-notes ("THE GPU SKINNING PALETTE, READ FROM THE
// GAME'S OWN SHIPPED SHADERS") proves the engine ships a COMPUTE pre-skinning
// path whose bone palette is a stride-48 structured SRV at t3. If characters
// go down that path, the palette is bound around a Dispatch and no draw hook
// would ever see it.
enum class Kind : uint8_t {
    DrawIndexed, Draw, DrawIndexedInstanced, DrawInstanced,
    Dispatch, DispatchIndirect,
    kCount
};

// Called on the calling thread for every draw, before the real function runs.
// Must obey rule 8: no logging, no locks, no allocation.
using Recorder = void (*)(ID3D11DeviceContext* ctx, Kind kind, uint32_t vertex_or_index_count);

// Resolve the draw functions from the game's immediate context and from a
// throwaway deferred context (the two classes use different implementations),
// then detour each unique function. Returns the number of functions detoured,
// 0 on failure with the reason logged. Nothing is written on failure.
int install(ID3D11Device* dev, ID3D11DeviceContext* ctx, Recorder rec);

// Undo every detour. Safe to call if install() failed.
void restore();

// Per-kind lifetime call counts, so a run always says which paths fired.
void counts(uint64_t out[(int)Kind::kCount]);

}  // namespace drawhook
}  // namespace grwxr
