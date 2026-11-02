#include "game_overrides.h"
#include "ps2_runtime.h"

namespace
{
    constexpr uint32_t kConEntry = 0x00100008u;

    void applyCon(PS2Runtime &)
    {
    }
}

PS2_REGISTER_GAME_OVERRIDE("Champions of Norrath (USA)", "SLUS_205.65", kConEntry, 0u, &applyCon)
