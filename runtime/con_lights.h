// The lights the game's vertex programs apply to models, computed natively from the game's own light state.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace con::lights
{
    using Vec3 = std::array<float, 3>;

    // A light set in camera space, as the game uploads it: four directional lights and an ambient, 0..255 per channel.
    struct Lights
    {
        std::array<Vec3, 4> direction{};
        std::array<Vec3, 4> colour{};
        Vec3 ambient{};
    };

    // lightSetupForCharacter: the two nearest point lights of the light grid, the fill, the sun, the ambient.
    Lights forCharacter(const uint8_t *rdram, uint32_t gp, const Vec3 &at);
    // drawWorldObject's set for a placed model, by its chunk's lighting word: 1 the character set, 2 the sun as ambient,
    // 8 a fixed ambient, otherwise the ambient, the fill and the sun.
    Lights forScenery(const uint8_t *rdram, uint32_t gp, const Vec3 &at, uint32_t word);
    // The vertex colour for a camera-space unit normal: ambient + max(light . normal, 0) x colour, clamped.
    uint32_t shade(const Lights &lights, const Vec3 &normal);

    // The four slots a level chunk's vertex program reads, in chunk space (world / 4, before the chunk's rotation).
    struct ChunkLights
    {
        std::array<std::array<float, 4>, 4> colour{};
        std::array<float, 4> floor{1.0f, 1.0f, 1.0f, 1.0f};
        std::array<Vec3, 4> position{};
        std::array<float, 4> reach{}, falloff{};
        float base = 128.0f;

        bool operator==(const ChunkLights &) const = default;
    };

    // lightSetupForWorld for the chunk record, or lightSetupAmbientWorld's constant term for a flagged chunk.
    ChunkLights forChunk(const uint8_t *rdram, uint32_t gp, uint32_t record);
    // The slots that cannot reach the chunk-space box left out: no vertex inside it gets anything from them.
    ChunkLights within(const ChunkLights &lights, const Vec3 &low, const Vec3 &high);
    // The chunk program's colours for chunk-space positions and their stored normals (length 127).
    void shade(const ChunkLights &lights, const std::vector<Vec3> &positions, const std::vector<Vec3> &normals,
               std::vector<uint32_t> &out);
}
