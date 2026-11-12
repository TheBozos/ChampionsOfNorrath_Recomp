#pragma once

#include "bozo/port.h"
#include "bozo/render/texture.h"

#include <cstdint>
#include <memory>
#include <string>

namespace con::world
{
    void setDiscImage(const std::string &path);
    void drawChunk(const uint8_t *rdram, const R5900Context *ctx);
    void drawModel(const uint8_t *rdram, const R5900Context *ctx);
    void endFrame(uint32_t clearColour);
    std::shared_ptr<const bozo::render::Texture> environment(const uint8_t *rdram, uint32_t gp);
}
