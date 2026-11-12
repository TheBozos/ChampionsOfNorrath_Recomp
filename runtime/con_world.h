#pragma once

#include "bozo/port.h"

#include <cstdint>
#include <string>

namespace con::world
{
    void setDiscImage(const std::string &path);
    void drawChunk(const uint8_t *rdram, const R5900Context *ctx);
    void drawModel(const uint8_t *rdram, const R5900Context *ctx);
    void endFrame(uint32_t clearColour);
}
