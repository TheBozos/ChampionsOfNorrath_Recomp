// Character shadows drawn natively: the blob surface modelDrawShadow renders on the GS is built on the CPU from the same
// sample points, then laid on the ground the way the game's quad is.
#include "con_shadows.h"

#include "con_textures.h"

#include "bozo/context.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace con::shadows
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu, kMaxJoints = 64, kSurface = 128;
        constexpr uint32_t kViewProjection = 0x0037ef30u, kView = 0x0037ef00u;
        constexpr int32_t kShadowScale = -0x3970, kShadowBrightness = -0x395c;
        constexpr float kSkew = 1.0f / 384.0f, kNudge = 12.0f;

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & kRamMask), sizeof(T));
            return value;
        }

        struct Blob
        {
            uint32_t lump = 0;
            std::vector<uint32_t> rgba;
            uint32_t width = 0, height = 0;
        };

        Blob g_blob;

        const Blob *blob(const uint8_t *rdram, uint32_t gp)
        {
            const uint32_t lump = con::textures::findResource(rdram, gp, "engine.lmp", "shadowblob.tex");
            if (!lump)
                return nullptr;
            if (lump != g_blob.lump) {
                bozo::render::Texture decoded;
                if (!con::textures::decodeLump(rdram, lump, decoded) || !decoded.width || !decoded.height)
                    return nullptr;
                g_blob.lump = lump;
                g_blob.rgba = std::move(decoded.rgba);
                g_blob.width = decoded.width;
                g_blob.height = decoded.height;
            }
            return &g_blob;
        }

        struct Joint
        {
            float r[3][3];
            float t[3];
        };

        std::array<Joint, kMaxJoints> readPose(const uint8_t *rdram, uint32_t state)
        {
            std::array<Joint, kMaxJoints> joints{};
            for (auto &j : joints)
                j.r[0][0] = j.r[1][1] = j.r[2][2] = 1.0f;
            const uint32_t pose = state ? guest<uint32_t>(rdram, state) : 0;
            if (!pose)
                return joints;
            const uint32_t count = std::min<uint32_t>(uint32_t(std::max<int16_t>(guest<int16_t>(rdram, pose + 8), 0)), kMaxJoints);
            for (uint32_t j = 0; j < count; ++j) {
                const uint32_t at = pose + 0x10 + j * 0x18;
                for (uint32_t row = 0; row < 3; ++row) {
                    for (uint32_t c = 0; c < 3; ++c)
                        joints[j].r[row][c] = float(guest<int16_t>(rdram, at + row * 8 + c * 2)) / 4096.0f;
                    joints[j].t[row] = float(guest<int16_t>(rdram, at + row * 8 + 6)) / 64.0f;
                }
            }
            return joints;
        }

        struct Shadow
        {
            std::shared_ptr<const bozo::render::Texture> surface;
            bozo::render::Material material;
            std::array<bozo::render::Vertex, 6> quad{};
        };

        std::vector<Shadow> g_shadows;
        uint64_t g_version = 0;

        std::shared_ptr<bozo::render::Texture> surface(size_t index)
        {
            auto texture = std::make_shared<bozo::render::Texture>();
            texture->width = texture->height = kSurface;
            texture->rgba.assign(kSurface * kSurface, 0x80000000u);
            texture->key = 0x5348414400000000ull | index;
            texture->version = ++g_version;
            return texture;
        }

        void paint(const uint8_t *rdram, uint32_t header, const std::array<Joint, kMaxJoints> &joints, const float (&m)[9], float dx,
                   float dy, float scale, const Blob &source, std::vector<uint32_t> &out)
        {
            std::vector<uint32_t> sum(kSurface * kSurface * 3, 0);
            const uint32_t block = header + uint32_t(guest<uint8_t>(rdram, header + 0x24)) * 16u;
            const uint32_t points = std::min<uint32_t>(guest<uint32_t>(rdram, block + 4), 4096u);
            const float k = 1.5f / scale, half = 0.008f / scale;
            const float axisX[3] = {m[0], m[3], m[6] + dx * kSkew}, axisY[3] = {m[1], m[4], m[7] + dy * kSkew};
            for (uint32_t i = 0; i < points; ++i) {
                const uint32_t at = block + 0x20 + 8 * i + 16 * (i / 45);
                const float p[3] = {float(guest<int16_t>(rdram, at)) / 16.0f, float(guest<int16_t>(rdram, at + 2)) / 16.0f,
                                    float(guest<int16_t>(rdram, at + 4)) / 16.0f};
                const int16_t w = guest<int16_t>(rdram, at + 6);
                const Joint &j = joints[std::min<uint32_t>((uint32_t(w) & 0xffu) / 4u, kMaxJoints - 1)];
                float q[3];
                for (uint32_t row = 0; row < 3; ++row)
                    q[row] = j.r[row][0] * p[0] + j.r[row][1] * p[1] + j.r[row][2] * p[2] + j.t[row];
                const float x = 64.0f + k * (q[0] * axisX[0] + q[1] * axisX[1] + q[2] * axisX[2]);
                const float y = 64.0f + k * (q[0] * axisY[0] + q[1] * axisY[1] + q[2] * axisY[2]);
                const float hs = float(w) / 16.0f * half;
                if (hs <= 0.0f)
                    continue;
                const int32_t x0 = std::max(int32_t(std::ceil(x - hs)), 0), x1 = std::min(int32_t(std::ceil(x + hs)), int32_t(kSurface));
                const int32_t y0 = std::max(int32_t(std::ceil(y - hs)), 0), y1 = std::min(int32_t(std::ceil(y + hs)), int32_t(kSurface));
                for (int32_t py = y0; py < y1; ++py) {
                    const int32_t tv = std::clamp(int32_t(26.0f - (float(py) - (y - hs)) * 22.0f / (2.0f * hs)), 0, int32_t(source.height) - 1);
                    for (int32_t px = x0; px < x1; ++px) {
                        const int32_t tu = std::clamp(int32_t(26.0f - (float(px) - (x - hs)) * 22.0f / (2.0f * hs)), 0, int32_t(source.width) - 1);
                        const uint32_t texel = source.rgba[size_t(tv) * source.width + size_t(tu)];
                        uint32_t *dst = &sum[(size_t(py) * kSurface + size_t(px)) * 3];
                        for (uint32_t c = 0; c < 3; ++c)
                            dst[c] = std::min<uint32_t>(dst[c] + (texel >> (c * 8) & 0xffu), 255u) & 0xf8u;
                    }
                }
            }
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = sum[i * 3] | sum[i * 3 + 1] << 8 | sum[i * 3 + 2] << 16 | 0x80000000u;
        }

        std::array<float, 4> project(const float (&m)[16], const std::array<float, 3> &p)
        {
            std::array<float, 4> clip{};
            for (uint32_t j = 0; j < 4; ++j)
                clip[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
            return clip;
        }
    }

    void draw(const uint8_t *rdram, const R5900Context *ctx)
    {
        const uint32_t sp = getRegU32(ctx, 29), gp = getRegU32(ctx, 28);
        const uint32_t header = guest<uint32_t>(rdram, sp + 0x210), state = guest<uint32_t>(rdram, sp + 0x218);
        const uint32_t matrix = getRegU32(ctx, 23);
        const int32_t threshold = int32_t(guest<uint32_t>(rdram, sp + 0x220));
        const float ground = ctx->f[22];
        const float scale = guest<float>(rdram, gp + uint32_t(kShadowScale)), brightness = guest<float>(rdram, gp + uint32_t(kShadowBrightness));
        const Blob *source = blob(rdram, gp);
        if (!source || scale <= 0.0f)
            return;
        float m[9];
        for (uint32_t i = 0; i < 9; ++i)
            m[i] = guest<float>(rdram, matrix + i * 4);
        const std::array<float, 3> position{guest<float>(rdram, matrix + 0x24), guest<float>(rdram, matrix + 0x28),
                                            guest<float>(rdram, matrix + 0x2c)};
        float vp[16];
        std::memcpy(vp, rdram + kViewProjection, sizeof(vp));
        const std::array<float, 3> axis{guest<float>(rdram, kView + 8), guest<float>(rdram, kView + 0x14), guest<float>(rdram, kView + 0x20)};
        const auto joints = readPose(rdram, state);
        for (uint32_t light = 0; light < 4; ++light) {
            const int8_t dxRaw = guest<int8_t>(rdram, sp + 0xa0 + light * 3), dyRaw = guest<int8_t>(rdram, sp + 0xa1 + light * 3);
            const int8_t intensity = guest<int8_t>(rdram, sp + 0xa2 + light * 3);
            if (intensity < threshold)
                continue;
            const float dx = -float(dxRaw), dy = -float(dyRaw);
            const float lift = position[2] - ground, a = 42.0f * scale;
            const float cx = position[0] + lift * dx * kSkew, cy = position[1] + lift * dy * kSkew;
            const uint32_t c = uint32_t(std::clamp(int32_t(float(intensity) * brightness), 0, 255));
            Shadow shadow;
            bool visible = true;
            const auto corner = [&](float sx, float sy, float u, float v) {
                const std::array<float, 3> p{cx + sx * a, cy + sy * a, ground};
                const auto clip = project(vp, p);
                const auto nudged = project(vp, {p[0] - kNudge * axis[0], p[1] - kNudge * axis[1], p[2] - kNudge * axis[2]});
                if (clip[3] <= 1.0f || nudged[3] <= 1.0f || nudged[2] < 0.0f)
                    visible = false;
                bozo::render::Vertex out;
                out.x = clip[0] / clip[3];
                out.y = clip[1] / clip[3];
                out.z = nudged[3] > 0.0f ? nudged[2] / nudged[3] : 0.0f;
                out.q = clip[3] > 0.0f ? 1.0f / clip[3] : 0.0f;
                out.u = u;
                out.v = v;
                out.rgba = c | c << 8 | c << 16 | 0x80000000u;
                return out;
            };
            const auto v0 = corner(-1, -1, 0, 0), v1 = corner(-1, 1, 0, 1), v2 = corner(1, -1, 1, 0), v3 = corner(1, 1, 1, 1);
            if (!visible)
                continue;
            shadow.quad = {v0, v1, v2, v1, v2, v3};
            auto painted = surface(g_shadows.size());
            paint(rdram, header, joints, m, dx, dy, scale, *source, painted->rgba);
            shadow.surface = painted;
            auto &material = shadow.material;
            material.tex0 = painted->key;
            material.textureAlpha = false;
            material.useQ = true;
            material.clampU = material.clampV = true;
            material.gsBlend = true;
            material.blendA = 1;
            material.blendB = 0;
            material.blendC = 2;
            material.blendD = 2;
            material.blendFix = 0x80;
            material.depthWrite = false;
            g_shadows.push_back(std::move(shadow));
        }
    }

    void submit()
    {
        static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        auto &renderer = bozo::render::Renderer::instance();
        for (const Shadow &shadow : g_shadows)
            renderer.draw(shadow.material, kIdentity, shadow.quad.data(), 6u, shadow.surface);
    }

    size_t pending() { return g_shadows.size(); }

    void clear() { g_shadows.clear(); }
}
