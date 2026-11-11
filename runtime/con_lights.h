// The lights the game's vertex programs apply to models, computed natively from the game's own light state.
#pragma once

#include <array>
#include <cstdint>

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
    // The vertex colour for a camera-space unit normal: ambient + max(light . normal, 0) x colour, clamped.
    uint32_t shade(const Lights &lights, const Vec3 &normal);
}
