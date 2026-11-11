#pragma once

#include "bozo/port.h"

#include <cstddef>
#include <cstdint>

namespace con::characters
{
    void draw(const uint8_t *rdram, const R5900Context *ctx);
    void drawAttachment(const uint8_t *rdram, const R5900Context *ctx);
    void submit();
    size_t pending();
    void clear();
}
