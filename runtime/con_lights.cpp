#include "con_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace con::lights
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu;

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & kRamMask), sizeof(T));
            return value;
        }

        constexpr uint32_t kLightGrid = 0x004f40f0u, kFillDirection = 0x004f40e0u, kSunDirection = 0x004f40d0u;
        constexpr uint32_t kView = 0x0037ef00u;
        constexpr int32_t kLightGroup = -0x3f44, kLightScale = -0x1188, kFillColour = -0x1190, kSunColour = -0x1198,
                          kAmbientColour = -0x11a0;

        Vec3 colourAt(const uint8_t *rdram, uint32_t address, float scale = 1.0f)
        {
            return {float(guest<int16_t>(rdram, address)) * scale, float(guest<int16_t>(rdram, address + 2)) * scale,
                    float(guest<int16_t>(rdram, address + 4)) * scale};
        }

    }

    Lights forCharacter(const uint8_t *rdram, uint32_t gp, const Vec3 &at)
    {
        Lights lights;
        const int32_t group = int32_t(guest<uint32_t>(rdram, gp + uint32_t(kLightGroup)));
        const auto cell = [](float v, int32_t offset) { return std::clamp((int32_t(v) + offset) >> 8, 0, 0x59); };
        std::vector<std::pair<float, uint32_t>> found;
        for (int32_t y = cell(at[1], 0x2bd4); y <= cell(at[1], 0x2e2c); ++y)
            for (int32_t x = cell(at[0], 0x2bd4); x <= cell(at[0], 0x2e2c); ++x)
                for (uint32_t light = guest<uint32_t>(rdram, kLightGrid + uint32_t(y * 90 + x) * 4); light && found.size() < 31;
                     light = guest<uint32_t>(rdram, light)) {
                    const float radius = guest<float>(rdram, light + 0x1c);
                    bool inside = guest<int8_t>(rdram, light + 8) == group;
                    float d2 = 0.0f;
                    for (uint32_t k = 0; k < 3 && inside; ++k) {
                        const float d = at[k] - guest<float>(rdram, light + 0x30 + k * 4);
                        inside = std::fabs(d) <= radius;
                        d2 += d * d;
                    }
                    if (inside)
                        found.emplace_back(d2 - radius * radius, light);
                }
        std::sort(found.begin(), found.end());
        const Vec3 scale{guest<float>(rdram, gp + uint32_t(kLightScale)), guest<float>(rdram, gp + uint32_t(kLightScale) + 4),
                         guest<float>(rdram, gp + uint32_t(kLightScale) + 8)};
        for (size_t i = 0; i < 2 && i < found.size(); ++i) {
            const uint32_t light = found[i].second;
            Vec3 d{};
            float d2 = 0.0f;
            for (uint32_t k = 0; k < 3; ++k) {
                d[k] = guest<float>(rdram, light + 0x30 + k * 4) - at[k];
                d2 += d[k] * d[k];
            }
            const float radius = guest<float>(rdram, light + 0x1c);
            const float strength = std::clamp((radius * radius - d2) * guest<float>(rdram, light + 0x20), 0.0f, 1.0f);
            const float inverse = d2 > 0.0f ? 1.0f / std::sqrt(d2) : 0.0f;
            Vec3 camera{};
            for (uint32_t k = 0; k < 3; ++k)
                for (uint32_t c = 0; c < 3; ++c)
                    camera[k] += d[c] * inverse * guest<float>(rdram, kView + (c * 3 + k) * 4);
            lights.direction[i] = {camera[0], camera[1], -camera[2]};
            const Vec3 rgb = colourAt(rdram, light + 0xa);
            for (uint32_t k = 0; k < 3; ++k)
                lights.colour[i][k] = float(int32_t(rgb[k] * strength * scale[k]));
        }
        for (uint32_t k = 0; k < 3; ++k) {
            lights.direction[2][k] = guest<float>(rdram, kFillDirection + k * 4);
            lights.direction[3][k] = guest<float>(rdram, kSunDirection + k * 4);
        }
        lights.colour[2] = colourAt(rdram, gp + uint32_t(kFillColour));
        lights.colour[3] = colourAt(rdram, gp + uint32_t(kSunColour));
        lights.ambient = colourAt(rdram, gp + uint32_t(kAmbientColour));
        return lights;
    }

    uint32_t shade(const Lights &lights, const Vec3 &normal)
    {
        Vec3 colour = lights.ambient;
        for (size_t l = 0; l < 4; ++l) {
            const float facing = std::max(0.0f, lights.direction[l][0] * normal[0] + lights.direction[l][1] * normal[1] +
                                                    lights.direction[l][2] * normal[2]);
            for (uint32_t k = 0; k < 3; ++k)
                colour[k] += facing * lights.colour[l][k];
        }
        const auto channel = [&](uint32_t k) { return uint32_t(std::clamp(colour[k], 0.0f, 255.0f)); };
        return channel(0) | channel(1) << 8 | channel(2) << 16 | 0x80000000u;
    }
}
