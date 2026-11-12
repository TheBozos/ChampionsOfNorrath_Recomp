#include "con_particles.h"
#include "con_water.h"

#include "bozo/context.h"
#include "bozo/port.h"

#include <cstdint>
#include <cstring>

namespace con::graphics
{
    void install(bozo::port::Registry &runtime);
}

namespace con::overlay
{
    void install(bozo::port::Registry &runtime);
}

namespace con::vu0
{
    void install(bozo::port::Registry &runtime);
}

namespace con::movies
{
    void install(bozo::port::Registry &runtime);
}

namespace
{
    constexpr uint32_t kConEntry = 0x00100008u;
    constexpr uint32_t kInitVif = 0x0018F118u;
    constexpr uint32_t kWaterStep = 0x00187798u;

    constexpr uint32_t kLgKbMInit = 0x002E6ED0u;
    constexpr uint32_t kLgAudEnumHint = 0x002EA150u;
    constexpr uint32_t kLgReturnZero[] = {
        0x002E6D70u, 0x002E7070u, 0x002E70F8u, 0x002E7138u, 0x002E7188u, 0x002E9098u, 0x002E9550u, 0x002E96B8u,
        0x002E9828u, 0x002E9998u, 0x002E9B08u, 0x002E9BF8u, 0x002E9D70u, 0x002E9EF8u, 0x002EA080u, 0x002EA1C0u,
        0x002EA380u, 0x002EA448u, 0x002EA520u, 0x002EA5F8u, 0x002EA718u, 0x002EA830u, 0x002EA8F8u, 0x002EA9C0u,
        0x002EAA88u, 0x002EAB50u, 0x002EAC18u, 0x002EAD10u, 0x002EAE08u};

    void returnFrom(R5900Context *ctx, int32_t value)
    {
        setReturnS32(ctx, value);
        ctx->pc = getRegU32(ctx, 31);
    }

    void returnZero(uint8_t *, R5900Context *ctx, Runtime *)
    {
        returnFrom(ctx, 0);
    }

    void returnVoid(uint8_t *, R5900Context *ctx, Runtime *)
    {
        ctx->pc = getRegU32(ctx, 31);
    }

    void keyboardAbsent(uint8_t *, R5900Context *ctx, Runtime *)
    {
        returnFrom(ctx, 1);
    }

    void headsetHintNone(uint8_t *rdram, R5900Context *ctx, Runtime *)
    {
        const uint32_t out = getRegU32(ctx, 4);
        if (out)
        {
            const uint32_t none = 0;
            std::memcpy(rdram + (out & 0x01FFFFFCu), &none, sizeof(none));
        }
        returnFrom(ctx, 0);
    }

    // The game's MMI loop keeps data in $sp, so an interrupt taken inside it would find no stack.
    void waterStep(uint8_t *rdram, R5900Context *ctx, Runtime *)
    {
        con::water::step(rdram, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6));
        ctx->pc = getRegU32(ctx, 31);
    }

    void install(bozo::port::Registry &runtime)
    {
        con::graphics::install(runtime);
        con::overlay::install(runtime);
        con::vu0::install(runtime);
        con::movies::install(runtime);
        con::particles::install(runtime);
        runtime.registerFunction(kWaterStep, &waterStep);
        runtime.registerFunction(kInitVif, &returnVoid);
        runtime.registerFunction(kLgKbMInit, &keyboardAbsent);
        runtime.registerFunction(kLgAudEnumHint, &headsetHintNone);
        for (uint32_t address : kLgReturnZero)
            runtime.registerFunction(address, &returnZero);
    }
}

BOZO_PORT("Champions of Norrath (USA)", "SLUS_205.65", kConEntry, &install)
