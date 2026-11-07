#pragma once

#include "bozo/port.h"

#include <cstdint>

namespace con::overlay
{
    void install(bozo::port::Registry &runtime);
    void endFrame(const uint8_t *rdram, const R5900Context *ctx);
}
