// Floor decals (blood, scorch marks, footprints): the records floorAdd keeps per 256-unit cell, drawn natively after the
// game's floorDraw has grown and faded them.
#include "con_decals.h"

#include "con_functions.h"
#include "con_textures.h"

#include "bozo/context.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace con::decals
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu;
        constexpr uint32_t kFloorDraw = 0x001922b8u, kCells = 0x007ac1f0u, kSpriteMap = 0x007940a0u;
        constexpr uint32_t kViewProjection = 0x0037ef30u;
        constexpr int32_t kRunning = -0x3510, kTotal = -0x3518;
        constexpr uint32_t kGrid = 30, kPerCell = 31, kRecord = 0x18, kCount = 0x2f8, kFirstRectangle = 0x3d3;
        constexpr float kGuard = 1000.0f;

        // The texture rectangles floorDraw uploads, by decal type: u0, v0, u1, v1 in 1/16 texels.
        constexpr std::array<std::array<uint16_t, 4>, 11> kRectangles = {{
            {0x008, 0x008, 0x400, 0x400},
            {0x408, 0x408, 0x600, 0x600},
            {0x408, 0x608, 0x600, 0x800},
            {0x408, 0x008, 0x800, 0x400},
            {0x608, 0x408, 0x800, 0x600},
            {0x608, 0x608, 0x800, 0x800},
            {0x808, 0x008, 0xc00, 0x400},
            {0x808, 0x408, 0xa00, 0x600},
            {0x808, 0x608, 0xa00, 0x800},
            {0x008, 0x408, 0x400, 0x800},
            {0xc08, 0x008, 0x1000, 0x400},
        }};

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & kRamMask), sizeof(T));
            return value;
        }

        con::textures::Mapped g_sprite;
        std::array<float, 16> g_matrix{};
        std::vector<bozo::render::Vertex> g_vertices;

        void collect(const uint8_t *rdram, uint32_t gp)
        {
            if (!guest<int8_t>(rdram, gp + uint32_t(kRunning)) || !guest<int32_t>(rdram, gp + uint32_t(kTotal)))
                return;
            const auto *texture = con::textures::mapped(rdram, gp, kSpriteMap, g_sprite);
            if (!texture)
                return;
            std::memcpy(g_matrix.data(), rdram + kViewProjection, sizeof(g_matrix));
            const auto &m = g_matrix;
            const float texelU = 1.0f / (16.0f * float(texture->width)), texelV = 1.0f / (16.0f * float(texture->height));
            for (uint32_t c = 0; c < kGrid * kGrid; ++c) {
                const uint32_t cell = guest<uint32_t>(rdram, kCells + c * 4);
                if (!cell)
                    continue;
                const uint32_t count = std::min(guest<uint32_t>(rdram, cell + kCount), kPerCell);
                for (uint32_t n = 0; n < count; ++n) {
                    const uint32_t record = cell + n * kRecord;
                    const uint32_t rectangle = (guest<uint16_t>(rdram, record + 0x14) - kFirstRectangle) / 2u;
                    if (rectangle >= kRectangles.size())
                        continue;
                    const auto &r = kRectangles[rectangle];
                    uint32_t rgba = 0;
                    for (uint32_t k = 0; k < 4; ++k)
                        rgba |= uint32_t(std::clamp<int32_t>(guest<int16_t>(rdram, record + k * 2), 0, 255)) << (k * 8);
                    std::array<float, 3> centre{};
                    for (uint32_t k = 0; k < 3; ++k)
                        centre[k] = float(guest<int16_t>(rdram, record + 8 + k * 2)) * 0.25f;
                    const float cs = float(guest<int16_t>(rdram, record + 0x10)) * 0.25f;
                    const float sn = float(guest<int16_t>(rdram, record + 0x12)) * 0.25f;
                    const std::array<std::array<float, 2>, 4> offsets = {{{cs, sn}, {-sn, cs}, {sn, -cs}, {-cs, -sn}}};
                    const std::array<std::array<uint16_t, 2>, 4> uvs = {{{r[0], r[1]}, {r[2], r[1]}, {r[0], r[3]}, {r[2], r[3]}}};
                    std::array<bozo::render::Vertex, 4> corner;
                    bool inside = true;
                    for (uint32_t k = 0; k < 4 && inside; ++k) {
                        auto &v = corner[k];
                        v.x = centre[0] + offsets[k][0];
                        v.y = centre[1] + offsets[k][1];
                        v.z = centre[2];
                        const float w = v.x * m[3] + v.y * m[7] + v.z * m[11] + m[15];
                        const float sx = (v.x * m[0] + v.y * m[4] + v.z * m[8] + m[12]) / w;
                        const float sy = (v.x * m[1] + v.y * m[5] + v.z * m[9] + m[13]) / w;
                        inside = w > 0.0f && std::fabs(sx) <= kGuard && std::fabs(sy) <= kGuard;
                        v.u = float(uvs[k][0]) * texelU;
                        v.v = float(uvs[k][1]) * texelV;
                        v.rgba = rgba;
                    }
                    if (!inside)
                        continue;
                    for (uint32_t k : {0u, 1u, 2u, 1u, 3u, 2u})
                        g_vertices.push_back(corner[k]);
                }
            }
        }

        void floorDraw(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            const uint32_t gp = getRegU32(ctx, 28);
            floorDraw__Fv_0x1922b8(rdram, ctx, runtime);
            collect(rdram, gp);
        }
    }

    void install(bozo::port::Registry &runtime) { runtime.registerFunction(kFloorDraw, &floorDraw); }

    void submit()
    {
        if (g_vertices.empty() || !g_sprite.texture)
            return;
        bozo::render::Material material;
        material.tex0 = g_sprite.texture->key;
        material.blend = bozo::render::Material::Blend::Alpha;
        material.alphaTest = true;
        material.alphaRef = 1;
        material.depthWrite = false;
        bozo::render::Renderer::instance().draw(material, g_matrix.data(), g_vertices.data(), uint32_t(g_vertices.size()),
                                                g_sprite.texture);
    }

    size_t pending() { return g_vertices.empty() ? 0u : 1u; }

    void clear() { g_vertices.clear(); }
}
