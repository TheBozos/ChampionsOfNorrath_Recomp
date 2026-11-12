#include "con_water.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace con::water
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu;
        constexpr uint32_t kRows = 32, kColumns = 32, kHeightRow = 0x50, kVelocityRow = 0x40, kBank = 0x40;

        using Row = std::array<int16_t, kColumns>;

        int16_t sat(int32_t v) { return int16_t(std::clamp(v, -32768, 32767)); }

        Row load(const uint8_t *rdram, uint32_t address)
        {
            Row row;
            std::memcpy(row.data(), rdram + (address & kRamMask), sizeof(row));
            return row;
        }

        void store(uint8_t *rdram, uint32_t address, const Row &row) { std::memcpy(rdram + (address & kRamMask), row.data(), sizeof(row)); }

        Row horizontal(const Row &h)
        {
            Row out;
            for (uint32_t c = 0; c < kColumns; ++c) {
                const int32_t right = c + 1 < kColumns ? h[c + 1] : 0, left = c ? h[c - 1] : 0;
                out[c] = sat(sat(h[c] + right) + left);
            }
            return out;
        }
    }

    void step(uint8_t *rdram, uint32_t heights, uint32_t velocities, uint32_t mask)
    {
        Row previous{}, around{}, aroundBefore{};
        for (uint32_t i = 0; i < kRows; ++i) {
            const Row current = load(rdram, heights + i * kHeightRow);
            const Row here = horizontal(current);
            const uint32_t velocityAt = velocities + i * kVelocityRow - kVelocityRow;
            const Row velocity = load(rdram, velocityAt);
            const uint32_t bank = mask + (i / 16) * kBank, bit = i % 16;
            Row newVelocity, newHeight;
            for (uint32_t c = 0; c < kColumns; ++c) {
                int16_t s = sat(sat(here[c] + around[c]) + aroundBefore[c]);
                s = sat(s - previous[c]);
                s = int16_t(s >> 3);
                s = sat(s - previous[c]);
                s = int16_t(s >> 2);
                int16_t v = sat(velocity[c] + s);
                int16_t m;
                std::memcpy(&m, rdram + ((bank + c * 2) & kRamMask), 2);
                if (!(uint16_t(m << bit) & 0x8000u))
                    v = 0;
                v = sat(v - (v >> 7));
                newVelocity[c] = v;
                newHeight[c] = sat(previous[c] + v);
            }
            store(rdram, velocityAt, newVelocity);
            store(rdram, heights + i * kHeightRow - kHeightRow, newHeight);
            aroundBefore = around;
            around = here;
            previous = current;
        }
    }
}
