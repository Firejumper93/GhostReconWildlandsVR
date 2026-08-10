// GunModel.cpp - build 66-overlay milestone 1. See GunModel.h.

#include "GunModel.h"

namespace grwxr {
namespace gun {
namespace {

// One axis-aligned box, six faces, four verts and two triangles each, per-face
// outward normal. Winding is not made uniform: the overlay draws CULL_NONE for
// milestone 1, so both faces are visible and the private depth buffer resolves
// self-occlusion. Colour is flat per box.
void add_box(std::vector<Vertex>& V, std::vector<uint16_t>& I,
             float cx, float cy, float cz,
             float hx, float hy, float hz,
             float r, float g, float b) {
    const float x0 = cx - hx, x1 = cx + hx;
    const float y0 = cy - hy, y1 = cy + hy;
    const float z0 = cz - hz, z1 = cz + hz;
    struct Face { float n[3]; float p[4][3]; };
    const Face faces[6] = {
        {{ 1, 0, 0}, {{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}},
        {{-1, 0, 0}, {{x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}}},
        {{ 0, 1, 0}, {{x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}}},
        {{ 0,-1, 0}, {{x0, y0, z1}, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}}},
        {{ 0, 0, 1}, {{x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}}},
        {{ 0, 0,-1}, {{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}},
    };
    for (const Face& f : faces) {
        const uint16_t base = (uint16_t)V.size();
        for (int k = 0; k < 4; ++k) {
            Vertex v;
            v.pos[0] = f.p[k][0]; v.pos[1] = f.p[k][1]; v.pos[2] = f.p[k][2];
            v.nrm[0] = f.n[0];    v.nrm[1] = f.n[1];    v.nrm[2] = f.n[2];
            v.col[0] = r;         v.col[1] = g;         v.col[2] = b;
            V.push_back(v);
        }
        I.push_back(base);     I.push_back(base + 1); I.push_back(base + 2);
        I.push_back(base);     I.push_back(base + 2); I.push_back(base + 3);
    }
}

}  // namespace

void build_mesh(std::vector<Vertex>& V, std::vector<uint16_t>& I) {
    V.clear();
    I.clear();
    // Grip anchor: authored gun coordinates are shifted by -anchor so the grip
    // ends up at the model origin (which the overlay places at the controller).
    const float ax = 0.0f, ay = -0.09f, az = 0.10f;
    auto box = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                   float r, float g, float b) {
        add_box(V, I, cx - ax, cy - ay, cz - az, hx, hy, hz, r, g, b);
    };
    // receiver / body (barrel points -Z)
    box(0.00f,  0.000f,  0.00f, 0.030f, 0.050f, 0.160f, 0.20f, 0.20f, 0.23f);
    // barrel, forward
    box(0.00f,  0.015f, -0.30f, 0.011f, 0.011f, 0.160f, 0.14f, 0.14f, 0.16f);
    // handguard
    box(0.00f, -0.010f, -0.15f, 0.024f, 0.028f, 0.100f, 0.24f, 0.20f, 0.16f);
    // stock, back
    box(0.00f,  0.000f,  0.22f, 0.024f, 0.045f, 0.090f, 0.20f, 0.17f, 0.14f);
    // pistol grip, below and back
    box(0.00f, -0.090f,  0.09f, 0.020f, 0.060f, 0.028f, 0.10f, 0.09f, 0.10f);
    // magazine, below the receiver
    box(0.00f, -0.085f, -0.01f, 0.020f, 0.065f, 0.040f, 0.12f, 0.12f, 0.14f);
}

}  // namespace gun
}  // namespace grwxr
