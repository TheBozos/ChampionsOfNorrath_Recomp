#include "con_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
        constexpr uint32_t kView = 0x0037ef00u, kFixedAmbient = 0x004841b8u, kSceneSunDirection = 0x004f406cu;
        constexpr int32_t kSceneSunScale = -0x38c8;
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

    Lights forScenery(const uint8_t *rdram, uint32_t gp, const Vec3 &at, uint32_t word)
    {
        if (word & 1)
            return forCharacter(rdram, gp, at);
        Lights lights;
        if (word & 2) {
            lights.ambient = colourAt(rdram, gp + uint32_t(kSunColour));
            return lights;
        }
        if (word & 8) {
            lights.ambient = colourAt(rdram, kFixedAmbient);
            return lights;
        }
        lights.ambient = colourAt(rdram, gp + uint32_t(kAmbientColour));
        for (uint32_t k = 0; k < 3; ++k) {
            lights.direction[2][k] = guest<float>(rdram, kFillDirection + k * 4);
            lights.direction[3][k] = guest<float>(rdram, kSceneSunDirection + k * 4);
        }
        lights.colour[2] = colourAt(rdram, gp + uint32_t(kFillColour));
        lights.colour[3] = colourAt(rdram, gp + uint32_t(kSunColour), guest<float>(rdram, gp + uint32_t(kSceneSunScale)));
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

    namespace
    {
        constexpr uint32_t kWorldLightLevel = 0x004f4094u;
        constexpr int32_t kAmbientReach = -0x7d30, kTurnScale = -0x7d2c;
        constexpr uint32_t kQuarterTurns = 0x1, kFlash = 0x100, kUnlit = 0x200, kBright = 0x2000;
        constexpr size_t kFound = 31;

        std::vector<uint32_t> lightsInBox(const uint8_t *rdram, uint32_t gp, const float (&low)[3], const float (&high)[3])
        {
            const int32_t group = int32_t(guest<uint32_t>(rdram, gp + uint32_t(kLightGroup)));
            const auto cell = [](float v, int32_t offset) { return std::clamp((int32_t(v) + offset) >> 8, 0, 0x59); };
            std::vector<uint32_t> found;
            for (int32_t y = cell(low[1], 0x2bd4); y <= cell(high[1], 0x2e2c); ++y)
                for (int32_t x = cell(low[0], 0x2bd4); x <= cell(high[0], 0x2e2c); ++x)
                    for (uint32_t light = guest<uint32_t>(rdram, kLightGrid + uint32_t(y * 90 + x) * 4); light;
                         light = guest<uint32_t>(rdram, light)) {
                        const float radius = guest<float>(rdram, light + 0x24);
                        if (radius == 0.0f)
                            continue;
                        bool overlaps = true;
                        for (uint32_t k = 0; k < 3 && overlaps; ++k) {
                            const float at = guest<float>(rdram, light + 0x30 + k * 4);
                            overlaps = !(at + radius < low[k]) && !(high[k] < at - radius);
                        }
                        if (overlaps && found.size() < kFound && guest<int8_t>(rdram, light + 8) == group)
                            found.push_back(light);
                    }
            return found;
        }

        int32_t truncated(float v) { return v < 2147483648.0f ? int32_t(v) : std::numeric_limits<int32_t>::max(); }
    }

    ChunkLights forChunk(const uint8_t *rdram, uint32_t gp, uint32_t record)
    {
        ChunkLights lights;
        lights.base = float(guest<int32_t>(rdram, kWorldLightLevel));
        const uint32_t flags = guest<uint32_t>(rdram, record + 0x24);
        if (flags & (kFlash | kBright | kUnlit)) {
            const float level = flags & kFlash ? -230.0f : flags & kBright ? 230.0f : 0.0f;
            lights.colour[0] = {level, level, level, 0.0f};
            lights.reach[0] = guest<float>(rdram, gp + uint32_t(kAmbientReach));
            lights.falloff[0] = -1.0f;
            return lights;
        }
        float low[3], high[3];
        for (uint32_t k = 0; k < 3; ++k) {
            low[k] = guest<float>(rdram, record + 0x8 + k * 4);
            high[k] = guest<float>(rdram, record + 0x14 + k * 4);
        }
        const auto found = lightsInBox(rdram, gp, low, high);
        int32_t nearest[4] = {0x1000001f, 0x1000001f, 0x1000001f, 0x1000001f};
        for (size_t i = 0; i < found.size(); ++i) {
            float d2 = 0.0f;
            for (uint32_t k = 0; k < 3; ++k) {
                const float d = (low[k] + high[k]) * 0.5f - guest<float>(rdram, found[i] + 0x30 + k * 4);
                d2 += d * d;
            }
            int32_t key = int32_t(uint32_t(truncated(d2)) << 7 | uint32_t(i));
            for (int32_t &kept : nearest)
                if (key < kept)
                    std::swap(key, kept);
        }
        const uint16_t turn = guest<uint16_t>(rdram, record + 0x36);
        const float scale = guest<float>(rdram, gp + uint32_t(kTurnScale));
        const float cosine = float(int16_t(turn)) * scale, sine = float(guest<int16_t>(rdram, record + 0x38)) * scale;
        int32_t first = 0, last = 3;
        for (int32_t key : nearest) {
            const size_t index = size_t(key & 0x1f);
            if (index >= found.size())
                break;
            const uint32_t light = found[index];
            const float floor = guest<float>(rdram, light + 0x2c);
            const int32_t slot = 1.0f <= floor ? last-- : first++;
            for (uint32_t k = 0; k < 4; ++k)
                lights.colour[size_t(slot)][k] = float(guest<int16_t>(rdram, light + 0x12 + k * 2));
            lights.floor[size_t(slot)] = floor;
            const float px = guest<float>(rdram, light + 0x30), py = guest<float>(rdram, light + 0x34);
            auto &at = lights.position[size_t(slot)];
            if (flags & kQuarterTurns) {
                const float a = (turn & 4 ? py : px) * 0.25f, b = (turn & 4 ? px : py) * 0.25f;
                at[0] = turn & 1 ? -a : a;
                at[1] = turn & 2 ? -b : b;
            } else {
                at[0] = (cosine * px + sine * py) * 0.25f;
                at[1] = (cosine * py - sine * px) * 0.25f;
            }
            at[2] = guest<float>(rdram, light + 0x38) * 0.25f;
            const float radius = guest<float>(rdram, light + 0x24);
            lights.reach[size_t(slot)] = radius * radius * 0.0625f;
            lights.falloff[size_t(slot)] = -guest<float>(rdram, light + 0x28) * 16.0f;
        }
        return lights;
    }

    ChunkLights within(const ChunkLights &lights, const Vec3 &low, const Vec3 &high)
    {
        ChunkLights out = lights;
        for (size_t i = 0; i < 4; ++i) {
            if (!(lights.falloff[i] < 0.0f))
                continue;
            float d2 = 0.0f;
            for (uint32_t k = 0; k < 3; ++k) {
                const float d = std::max({low[k] - lights.position[i][k], lights.position[i][k] - high[k], 0.0f});
                d2 += d * d;
            }
            if (d2 >= lights.reach[i]) {
                out.colour[i] = {};
                out.reach[i] = 0.0f;
                out.falloff[i] = 0.0f;
            }
        }
        return out;
    }

    void shade(const ChunkLights &lights, const std::vector<Vec3> &positions, const std::vector<Vec3> &normals,
               std::vector<uint32_t> &out)
    {
        std::array<size_t, 4> used{};
        size_t count = 0;
        for (size_t i = 0; i < 4; ++i)
            if (lights.falloff[i] != 0.0f || lights.reach[i] > 0.0f)
                used[count++] = i;
        const auto colour = [&](const std::array<float, 4> &amount, bool lit) {
            uint32_t rgba = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                float v = k < 3 ? lights.base : 128.0f;
                for (size_t n = 0; n < count; ++n)
                    v += amount[used[n]] * lights.colour[used[n]][k];
                int32_t channel = std::min(int32_t(v + (v < 0.0f ? -0.5f : 0.5f)), 255);
                if (channel < 0)
                    channel = lit ? 0 : (channel * 2) & 0xff;
                rgba |= uint32_t(channel) << (k * 8);
            }
            return rgba;
        };
        const uint32_t plain = colour({}, false);
        out.resize(positions.size());
        for (size_t v = 0; v < positions.size(); ++v) {
            std::array<float, 4> amount{}, distance{};
            std::array<Vec3, 4> towards{};
            bool lit = false, reached = false;
            for (size_t n = 0; n < count; ++n) {
                const size_t i = used[n];
                for (uint32_t k = 0; k < 3; ++k) {
                    towards[i][k] = lights.position[i][k] - positions[v][k];
                    distance[i] += towards[i][k] * towards[i][k];
                }
                amount[i] = std::max((distance[i] - lights.reach[i]) * lights.falloff[i], 0.0f);
                lit |= lights.floor[i] < 1.0f && distance[i] < lights.reach[i];
                reached |= amount[i] > 0.0f;
            }
            if (!reached && !lit) {
                out[v] = plain;
                continue;
            }
            for (size_t n = 0; n < count; ++n) {
                const size_t i = used[n];
                if (lit) {
                    const float length = std::sqrt(distance[i]);
                    const Vec3 &normal = normals[v];
                    const float facing = length > 0.0f
                                             ? (towards[i][0] * normal[0] + towards[i][1] * normal[1] + towards[i][2] * normal[2]) *
                                                   255.0f / (length * 32768.0f)
                                             : 0.0f;
                    amount[i] *= std::min(std::max(facing, lights.floor[i]), 1.0f);
                }
                amount[i] = std::min(amount[i], 1.0f);
            }
            out[v] = colour(amount, lit);
        }
    }
}
