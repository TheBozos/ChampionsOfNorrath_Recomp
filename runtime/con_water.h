#pragma once

#include <cstdint>

namespace con::water
{
    void step(uint8_t *rdram, uint32_t heights, uint32_t velocities, uint32_t mask);
}
