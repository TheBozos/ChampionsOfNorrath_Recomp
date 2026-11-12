// Movie pictures (libmpeg, decoded natively) shown as the window's backdrop while VIMoviePlay runs.
#include "con_functions.h"

#include "bozo/port.h"
#include "bozo/render/movie.h"
#include "bozo/render/renderer.h"

#include <cstdint>
#include <cstring>
#include <memory>

namespace con::movies
{
    namespace
    {
        constexpr uint32_t kMoviePlay = 0x010dcaa0u;
        constexpr uint64_t kBackdropKey = 0x434f4e4d4f564945ull;

        uint64_t g_version = 0;

        void picture(uint32_t, uint32_t width, uint32_t height, const uint8_t *rgba)
        {
            auto texture = std::make_shared<bozo::render::Texture>();
            texture->key = kBackdropKey;
            texture->version = ++g_version;
            texture->width = width;
            texture->height = height;
            texture->rgba.resize(size_t(width) * height);
            std::memcpy(texture->rgba.data(), rgba, texture->rgba.size() * 4u);
            for (uint32_t &c : texture->rgba)
                c |= 0xff000000u;
            bozo::render::Screen screen;
            screen.originX = 0.0f;
            screen.originY = 0.0f;
            screen.width = float(width);
            screen.height = float(height);
            bozo::render::Renderer::instance().setBackdrop(std::move(texture), screen, 0.0f, 0.0f, screen.width, screen.height, true);
        }

        void moviePlay(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            VIMoviePlay__FPCcPFv_i_0x10dcaa0(rdram, ctx, runtime);
            bozo::render::Renderer::instance().clearBackdrop();
        }
    }

    void install(bozo::port::Registry &runtime)
    {
        bozo::render::setMoviePictureHook(&picture);
        runtime.registerFunction(kMoviePlay, &moviePlay);
    }
}
