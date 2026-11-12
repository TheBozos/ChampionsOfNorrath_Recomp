// The particle systems drawn natively: each live particle of drawPS's lists becomes a camera-facing sprite.
#include "con_particles.h"

#include "con_textures.h"

#include "bozo/context.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace con::particles
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu;
        constexpr uint32_t kDrawPS = 0x00185c38u;
        constexpr uint32_t kMainSystem = 0x0037f730u, kViewProjection = 0x0037ef30u, kLifetimes = 0x0037f720u;
        constexpr uint32_t kPages = 0x00780080u, kPageData = 0x00700080u, kTypes = 0x00780300u, kNow = 0x004869f4u;
        constexpr uint32_t kAtlasMap = 0x006ec800u;
        constexpr uint32_t kLocalMatrix = 0x100u;
        constexpr int32_t kGeneration = -0x3a68;
        constexpr float kColourBias = 8388608.0f;

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & kRamMask), sizeof(T));
            return value;
        }

        struct Batch
        {
            bozo::render::Material material;
            std::vector<bozo::render::Vertex> vertices;
        };

        std::vector<Batch> g_batches;
        std::shared_ptr<const bozo::render::Texture> g_atlas;
        uint32_t g_atlasLump = 0;

        const bozo::render::Texture *atlas(const uint8_t *rdram, const R5900Context *ctx)
        {
            const uint32_t generation = guest<uint32_t>(rdram, getRegU32(ctx, 28) + uint32_t(kGeneration));
            if (guest<uint32_t>(rdram, kAtlasMap) != generation)
                return nullptr;
            const uint32_t lump = guest<uint32_t>(rdram, kAtlasMap + 0xc);
            if (!lump)
                return nullptr;
            if (lump != g_atlasLump || !g_atlas) {
                g_atlas.reset();
                g_atlasLump = lump;
                bozo::render::Texture decoded;
                if (!con::textures::decodeLump(rdram, lump, decoded) || !decoded.width || !decoded.height)
                    return nullptr;
                decoded.key = 0x5041525400000000ull | lump;
                decoded.version = 1;
                g_atlas = std::make_shared<bozo::render::Texture>(std::move(decoded));
            }
            return g_atlas.get();
        }

        using Matrix = std::array<float, 16>;

        Matrix systemMatrix(const uint8_t *rdram, uint32_t system)
        {
            Matrix view;
            std::memcpy(view.data(), rdram + kViewProjection, sizeof(view));
            if (system == kMainSystem)
                return view;
            std::array<float, 12> local;
            std::memcpy(local.data(), rdram + ((system + kLocalMatrix) & kRamMask), sizeof(local));
            Matrix out{};
            for (uint32_t row = 0; row < 4; ++row)
                for (uint32_t j = 0; j < 4; ++j) {
                    float sum = row == 3 ? view[12 + j] : 0.0f;
                    for (uint32_t k = 0; k < 3; ++k)
                        sum += local[row * 3 + k] * view[k * 4 + j];
                    out[row * 4 + j] = sum;
                }
            return out;
        }

        uint8_t channel(float value, bool biased)
        {
            const float c = std::floor(biased ? value - kColourBias : value);
            return uint8_t(std::clamp(c, 0.0f, 255.0f));
        }

        struct Particle
        {
            std::array<float, 3> origin{}, velocity{};
            float age = 0.0f;
            uint32_t type = 0;

            std::array<float, 3> at(float t, float gravity) const
            {
                return {origin[0] + velocity[0] * t, origin[1] + velocity[1] * t, origin[2] + velocity[2] * t + gravity * t * t};
            }
        };

        bool read(const uint8_t *rdram, uint32_t slot, uint16_t now, int32_t lifetime, Particle &out)
        {
            const uint16_t age = uint16_t(now - guest<uint16_t>(rdram, slot + 6));
            if (int32_t(age) > lifetime)
                return false;
            for (uint32_t k = 0; k < 3; ++k) {
                out.origin[k] = float(guest<int16_t>(rdram, slot + k * 2));
                out.velocity[k] = float(guest<int16_t>(rdram, slot + 8 + k * 2)) / 4096.0f;
            }
            out.age = float(age);
            out.type = kTypes + ((guest<uint16_t>(rdram, slot + 14) >> 2) & 31u) * 0x40u;
            return true;
        }

        std::array<float, 4> project(const Matrix &m, const std::array<float, 3> &p)
        {
            std::array<float, 4> clip{};
            for (uint32_t j = 0; j < 4; ++j)
                clip[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
            return clip;
        }

        uint32_t colourAt(const uint8_t *rdram, uint32_t type, float t)
        {
            uint32_t colour = 0;
            for (uint32_t c = 0; c < 4; ++c)
                colour |= uint32_t(channel(guest<float>(rdram, type + (8 + c) * 4) + guest<float>(rdram, type + (12 + c) * 4) * t, true)) << (c * 8);
            return colour;
        }

        void sprite(const uint8_t *rdram, const Matrix &m, const Particle &p, float width, float height,
                    std::vector<bozo::render::Vertex> &out)
        {
            const auto field = [&](uint32_t i) { return guest<float>(rdram, p.type + i * 4); };
            const auto clip = project(m, p.at(p.age, field(2)));
            if (clip[3] <= 1.0f)
                return;
            const float size = field(7) + field(3) * p.age;
            const uint32_t colour = colourAt(rdram, p.type, p.age);
            if (size <= 0.0f || !(colour >> 24))
                return;
            const float iw = 1.0f / clip[3];
            const float cx = clip[0] * iw, cy = clip[1] * iw, cz = clip[2] * iw, hx = 2.0f * size * iw, hy = size * iw;
            const float u0 = float(guest<int32_t>(rdram, p.type)) / (16.0f * width),
                        v0 = float(guest<int32_t>(rdram, p.type + 4)) / (16.0f * height),
                        u1 = float(guest<int32_t>(rdram, p.type + 16)) / (16.0f * width),
                        v1 = float(guest<int32_t>(rdram, p.type + 20)) / (16.0f * height);
            const auto corner = [&](float sx, float sy, float u, float v) {
                bozo::render::Vertex vertex;
                vertex.x = cx + sx * hx;
                vertex.y = cy + sy * hy;
                vertex.z = cz;
                vertex.u = u;
                vertex.v = v;
                vertex.rgba = colour;
                return vertex;
            };
            const auto a = corner(-1, -1, u0, v0), b = corner(1, -1, u1, v0), c = corner(1, 1, u1, v1), d = corner(-1, 1, u0, v1);
            out.insert(out.end(), {a, b, c, a, c, d});
        }

        void line(const uint8_t *rdram, const Matrix &m, const Particle &p, std::vector<bozo::render::Vertex> &out)
        {
            const float gravity = guest<float>(rdram, p.type), lag = guest<float>(rdram, p.type + 12);
            const auto head = project(m, p.at(p.age, gravity)), tail = project(m, p.at(p.age - lag, gravity));
            if (head[3] <= 1.0f || tail[3] <= 1.0f)
                return;
            const uint32_t colour = colourAt(rdram, p.type, p.age);
            if (!(colour >> 24))
                return;
            const float hx = head[0] / head[3], hy = head[1] / head[3], tx = tail[0] / tail[3], ty = tail[1] / tail[3];
            const float dx = (tx - hx) * 0.5f, dy = ty - hy, length = std::sqrt(dx * dx + dy * dy);
            const float nx = length > 0.0f ? -dy / length : 0.0f, ny = length > 0.0f ? dx / length : 1.0f;
            const auto end = [&](float x, float y, float z, float side) {
                bozo::render::Vertex vertex;
                vertex.x = x + side * nx;
                vertex.y = y + side * ny * 0.5f;
                vertex.z = z;
                vertex.rgba = colour;
                return vertex;
            };
            const float hz = head[2] / head[3], tz = tail[2] / tail[3];
            const auto a = end(hx, hy, hz, -1), b = end(hx, hy, hz, 1), c = end(tx, ty, tz, 1), d = end(tx, ty, tz, -1);
            out.insert(out.end(), {a, b, c, a, c, d});
        }

        template <typename Emit>
        void drawList(const uint8_t *rdram, uint32_t list, uint16_t now, int32_t lifetime, const Emit &emit)
        {
            const uint32_t head = guest<uint32_t>(rdram, list), tail = guest<uint32_t>(rdram, list + 4);
            const uint32_t first = guest<uint32_t>(rdram, list + 8), count = guest<uint32_t>(rdram, list + 12);
            uint32_t page = head;
            Particle particle;
            for (uint32_t guard = 0; page && guard < 4096; ++guard) {
                const uint32_t begin = page == head ? first : 0, end = page == tail ? std::min(count, 0x100u) : 0x100u;
                const uint32_t data = kPageData + ((page - kPages) << 10);
                for (uint32_t n = begin; n < end; ++n)
                    if ((n & 31u) && (n & 31u) != 31u && read(rdram, data + n * 16u, now, lifetime, particle))
                        emit(particle);
                if (page == tail)
                    break;
                page = guest<uint32_t>(rdram, page);
            }
        }

        void drawPS(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const auto *texture = atlas(rdram, ctx);
            if (!texture)
                return;
            const uint32_t system = getRegU32(ctx, 5);
            const Matrix m = systemMatrix(rdram, system);
            const uint16_t now = guest<uint16_t>(rdram, kNow);
            const float width = float(texture->width), height = float(texture->height);
            for (uint32_t pass = 0; pass < 3; ++pass) {
                Batch batch;
                batch.material.tex0 = pass == 2 ? 0 : texture->key;
                batch.material.textured = pass != 2;
                batch.material.directColour = pass == 2;
                batch.material.blend = pass ? bozo::render::Material::Blend::Additive : bozo::render::Material::Blend::Alpha;
                batch.material.depthWrite = false;
                batch.material.nearest = true;
                for (uint32_t k = 0; k < 4; ++k)
                    drawList(rdram, system + pass * 0x40u + k * 0x10u, now, guest<int32_t>(rdram, kLifetimes + k * 4u),
                             [&](const Particle &p) {
                                 if (pass == 2)
                                     line(rdram, m, p, batch.vertices);
                                 else
                                     sprite(rdram, m, p, width, height, batch.vertices);
                             });
                if (!batch.vertices.empty())
                    g_batches.push_back(std::move(batch));
            }
        }
    }

    void install(bozo::port::Registry &runtime) { runtime.registerFunction(kDrawPS, &drawPS); }

    void submit()
    {
        static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        auto &renderer = bozo::render::Renderer::instance();
        for (const Batch &batch : g_batches)
            renderer.draw(batch.material, kIdentity, batch.vertices.data(), uint32_t(batch.vertices.size()),
                          batch.material.textured ? g_atlas : nullptr);
    }

    size_t pending() { return g_batches.size(); }

    void clear() { g_batches.clear(); }
}
