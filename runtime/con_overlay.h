#pragma once

#include "bozo/port.h"
#include "bozo/render/renderer.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace con::overlay
{
    // Screen pixels of the 640x448 2D screen, as the game's 2D calls give them.
    struct Clip
    {
        float x0 = 0.0f, y0 = 0.0f, x1 = 640.0f, y1 = 448.0f;
    };

    void install(bozo::port::Registry &runtime);
    void endFrame(const uint8_t *rdram, const R5900Context *ctx);
    // A game texture (its in-memory lump) decoded for the renderer, cached by its contents.
    std::shared_ptr<const bozo::render::Texture> texture(const uint8_t *rdram, const R5900Context *ctx, uint32_t address);
    // Triangles in screen pixels, drawn in the 2D list's layer order, clipped to the rectangle.
    void queue(int32_t layer, std::shared_ptr<const bozo::render::Texture> texture, std::vector<bozo::render::Vertex> triangles,
               const Clip &clip);
}
