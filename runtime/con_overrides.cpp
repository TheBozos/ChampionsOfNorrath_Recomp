#include "game_overrides.h"
#include "ps2_runtime.h"

namespace
{
    constexpr uint32_t kConEntry = 0x00100008u;

    constexpr uint32_t kLgKbMInit = 0x002E6ED0u;
    constexpr uint32_t kLgKbMPrepareForRebootIOP = 0x002E6D70u;
    constexpr uint32_t kLgAudInit = 0x002E9098u;
    constexpr uint32_t kLgAudPrepareForReboot = 0x002E9B08u;
    constexpr uint32_t kLgAudDeInit = 0x002EA080u;

    void disableLogitechKeyboard(PS2Runtime &runtime)
    {
        ps2_game_overrides::bindAddressHandler(runtime, kLgKbMInit, "ret1");
        ps2_game_overrides::bindAddressHandler(runtime, kLgKbMPrepareForRebootIOP, "ret0");
    }

    void disableLogitechHeadset(PS2Runtime &runtime)
    {
        ps2_game_overrides::bindAddressHandler(runtime, kLgAudInit, "ret0");
        ps2_game_overrides::bindAddressHandler(runtime, kLgAudPrepareForReboot, "ret0");
        ps2_game_overrides::bindAddressHandler(runtime, kLgAudDeInit, "ret0");
    }

    void readDiscBySector()
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        if (paths.cdImage.empty())
        {
            paths.cdImage = paths.cdRoot / "disc.iso";
        }
        paths.cdImageFileSystem = true;
        PS2Runtime::setIoPaths(paths);
    }

    void applyCon(PS2Runtime &runtime)
    {
        readDiscBySector();
        disableLogitechKeyboard(runtime);
        disableLogitechHeadset(runtime);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Champions of Norrath (USA)", "SLUS_205.65", kConEntry, 0u, &applyCon)
