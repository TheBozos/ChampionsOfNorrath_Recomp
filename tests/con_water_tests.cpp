#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "con_water.h"

namespace
{
    int failures = 0;

    void check(bool ok, const char *what)
    {
        if (!ok) {
            std::printf("FAILED: %s\n", what);
            ++failures;
        }
    }

    constexpr uint32_t kHeights = 0x100, kVelocities = 0x1000, kMask = 0x2000;

    struct Tile
    {
        std::vector<uint8_t> ram = std::vector<uint8_t>(0x3000, 0);

        int16_t get(uint32_t address) const
        {
            int16_t v;
            std::memcpy(&v, ram.data() + address, 2);
            return v;
        }
        void set(uint32_t address, int16_t v) { std::memcpy(ram.data() + address, &v, 2); }
        int16_t height(int32_t row, uint32_t column) const { return get(uint32_t(int32_t(kHeights) + row * 0x50) + column * 2); }
        void setHeight(int32_t row, uint32_t column, int16_t v) { set(uint32_t(int32_t(kHeights) + row * 0x50) + column * 2, v); }
        int16_t velocity(int32_t row, uint32_t column) const { return get(uint32_t(int32_t(kVelocities) + row * 0x40) + column * 2); }
        void setVelocity(int32_t row, uint32_t column, int16_t v) { set(uint32_t(int32_t(kVelocities) + row * 0x40) + column * 2, v); }
        void water() { std::memset(ram.data() + kMask, 0xff, 0x80); }
        void step() { con::water::step(ram.data(), kHeights, kVelocities, kMask); }
    };

    void loneVelocity()
    {
        Tile t;
        t.water();
        t.setVelocity(5, 10, 1000);
        t.step();
        check(t.velocity(5, 10) == 993, "a lone velocity loses 1/128");
        check(t.height(5, 10) == 993, "the velocity moves its own height row");
        check(t.height(5, 9) == 0 && t.height(6, 10) == 0, "the neighbours wait a frame");
    }

    void dryCell()
    {
        Tile t;
        t.water();
        t.set(kMask + 10 * 2, int16_t(0xbfff));
        t.setVelocity(0, 10, 1000);
        t.setHeight(0, 10, 50);
        t.step();
        check(t.velocity(0, 10) == 0, "a clear mask bit stops the velocity");
        check(t.height(0, 10) == 50, "and keeps the height");
    }

    void flatWater()
    {
        Tile t;
        t.water();
        for (int32_t r = 0; r < 32; ++r)
            for (uint32_t c = 0; c < 32; ++c)
                t.setHeight(r, c, 100);
        t.step();
        check(t.height(10, 10) == 100 && t.velocity(10, 10) == 0, "a flat interior cancels");
        check(t.height(31, 10) == 100, "the last row is not stepped");
        check(t.height(-1, 10) != 0 && t.height(-1, 10) == t.velocity(-1, 10), "the row before the tile takes its new velocity");
    }

    void saturation()
    {
        Tile t;
        t.water();
        t.setHeight(4, 7, 32000);
        t.setVelocity(4, 7, 32000);
        t.step();
        check(t.height(4, 7) == 32767, "heights saturate");
    }
}

int main()
{
    loneVelocity();
    dryCell();
    flatWater();
    saturation();
    if (failures)
        return 1;
    std::printf("ok\n");
    return 0;
}
