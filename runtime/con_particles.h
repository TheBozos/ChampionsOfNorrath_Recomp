#pragma once

#include "bozo/port.h"

#include <cstddef>
#include <cstdint>

namespace con::particles
{
    void install(bozo::port::Registry &runtime);
    void drawTrails(const uint8_t *rdram, const R5900Context *ctx);
    void submit();
    size_t pending();
    void clear();
}
