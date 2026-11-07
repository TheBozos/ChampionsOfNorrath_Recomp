#pragma once

#include "bozo/port.h"

#include <cstdint>

namespace con::overlay
{
    void install(bozo::port::Registry &runtime);
    void endFrame();
}
