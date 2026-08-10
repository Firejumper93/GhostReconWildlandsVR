// GunModel.h - build 66-overlay milestone 1.
//
// A procedural, blocky placeholder rifle for the controller-held overlay gun
// (docs/PLAN-overlay-gun.md, docs/PLAN-controller-gun.md). This is ORIGINAL
// authored geometry generated in code: no game asset is extracted, loaded, or
// shipped, so the project redistribution rule holds with nothing to widen in
// the export whitelist.
//
// Model space matches the mod world basis: +X right, +Y up, +Z back, so the
// barrel points -Z (the OpenXR aim-pose forward). Units are metres. The mesh
// is authored with the grip near the origin so the model origin sits at the
// controller. Triangle list, safe under CULL_NONE (per-face winding is not
// made consistent for milestone 1).

#pragma once

#include <cstdint>
#include <vector>

namespace grwxr {
namespace gun {

struct Vertex {
    float pos[3];
    float nrm[3];
    float col[3];
};

// Fill verts/indices with the placeholder rifle. Deterministic, no allocation
// beyond the vectors, called once at init.
void build_mesh(std::vector<Vertex>& verts, std::vector<uint16_t>& indices);

}  // namespace gun
}  // namespace grwxr
