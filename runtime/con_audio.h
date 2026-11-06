// CoN's sound modules from the disc (SDRDRV, its LIBSD, HELLO's streams), compiled to native code and run by
// BozoEngine's compiled-library host over native libsd: the game's libsdr and stream RPCs reach them.
#pragma once

#include "bozo/libs/libraries.h"
#include "bozo/native/compiled_libraries.h"

#include <vector>

namespace con::audio
{
    std::vector<bozo::native::irx::Image> images();
    void attach(bozo::libs::Libraries &libraries);
}
