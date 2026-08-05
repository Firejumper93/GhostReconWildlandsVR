// PaletteProbe.h - find the GPU skinning palette with our own draw-call probe.
//
// WHY THIS EXISTS (session 23, 2026-08-04)
// ----------------------------------------
// VR arms need the skinning palette: the buffer the vertex shader reads bone
// matrices from. Static analysis exhausted the cheap routes (RE-notes: no DXBC
// in the exe, no graphics consumer of the bone gather), and RenderDoc is dead
// on this title (four routes measured, RENDERDOC-GUIDE 3b; the game dies
// before frame 1 with RenderDoc resident).
//
// But WE already own the game's D3D11 device and context, and our hooks have
// drawn no protection reaction in 22 sessions. So this module answers the
// capture question directly: hook the two indexed draw calls, and while a
// user-triggered capture window is armed, record every buffer bound to the
// vertex shader stage. The dump names each unique buffer's kind, slot, size,
// stride and element count, which are exactly the facts a RenderDoc capture
// was wanted for (IK-PLAN section 2).
//
// USE IT IN THE LOBBY MENU (menu -> continue -> load save -> pre-game lobby):
// every character there is fully rendered and nothing else is in the scene,
// so essentially every skinned draw IS a character and the candidate list is
// short. Arm with NUMPAD MINUS (hold ~1 s; the poll runs at 1 Hz).
//
// The palette candidate reads as: a VS constant buffer of roughly
// 0x3A80..0x4E00 bytes (312 bones x 0x30 or 0x40), or a structured-buffer SRV
// with stride 0x30/0x40 and a three-digit element count.

#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace grwxr {
namespace pal {

// Patch the four draw slots (12 DrawIndexed, 13 Draw, 20 DrawIndexedInstanced,
// 21 DrawInstanced) in the immediate context vtable AND in the deferred
// context class vtable (obtained via a throwaway CreateDeferredContext on the
// game's own device), because in this runtime the two classes can have
// separate vtables and AnvilNext records scene draws on deferred contexts.
// Returns false and installs nothing on failure: the game keeps running
// unmodified (project rule 7).
bool install(ID3D11Device* dev, ID3D11DeviceContext* ctx);

// 1 Hz on the init thread: the capture key edge, and the dump of a finished
// capture. All logging happens here, never in the hooks.
void poll();

}  // namespace pal
}  // namespace grwxr
