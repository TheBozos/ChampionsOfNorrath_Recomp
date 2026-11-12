#pragma once

#include "bozo/port.h"

#include <cstddef>
#include <cstdint>

namespace con::decals
{
    void install(bozo::port::Registry &runtime);
    void submit();
    size_t pending();
    void clear();
}
