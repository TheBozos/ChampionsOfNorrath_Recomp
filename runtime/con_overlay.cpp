// The game's 2D drawn natively: what _textureDrawColor builds as GS packets goes to the renderer's overlay.
#include "con_overlay.h"

#include "con_textures.h"

#include "bozo/context.h"
#include "bozo/render/renderer.h"

#include <cstring>
#include <memory>
#include <unordered_map>

namespace con::overlay
{
    namespace
    {
        constexpr uint32_t kTextureDrawColor = 0x0016e658u;
        constexpr uint32_t kFontPrint = 0x00173958u;
        constexpr int32_t kCurrentFont = -0x1064, kFontColour = -0x1068, kFontAlpha = -0x38d8;
        constexpr int32_t kNoGlyph = 0x7ff;
        constexpr uint16_t kStoredAsFields = 0x0040;
        constexpr int32_t kFieldMode = -0x3544, kFullHeightMode = -0x357c;
        constexpr float kOriginX = 2048.0f - 320.0f, kOriginY = 2048.0f - 224.0f;
        constexpr size_t kMaxCached = 4096;

        std::unordered_map<uint64_t, std::shared_ptr<const bozo::render::Texture>> g_textures;

        uint32_t gpWord(const uint8_t *rdram, const R5900Context *ctx, int32_t offset)
        {
            uint32_t value;
            std::memcpy(&value, rdram + ((getRegU32(ctx, 28) + uint32_t(offset)) & 0x1ffffffu), 4);
            return value;
        }

        uint64_t contentKey(const uint8_t *rdram, uint32_t address)
        {
            uint64_t hash = 0xcbf29ce484222325ull ^ address;
            for (uint32_t i = 0; i < 0x20; ++i)
                hash = (hash ^ rdram[(address + 0x80 + i * 16) & 0x1ffffffu]) * 0x100000001b3ull;
            return hash;
        }

        void interleaveFields(bozo::render::Texture &texture)
        {
            const std::vector<uint32_t> stacked = texture.rgba;
            const uint32_t field = texture.height / 2;
            for (uint32_t y = 0; y < field * 2; ++y)
                std::memcpy(&texture.rgba[size_t(y) * texture.width],
                            &stacked[size_t(y / 2 + (y & 1) * field) * texture.width], texture.width * 4u);
        }

        std::shared_ptr<const bozo::render::Texture> lookUp(const uint8_t *rdram, const R5900Context *ctx, uint32_t address)
        {
            address &= 0x1ffffffu;
            uint16_t flags;
            std::memcpy(&flags, rdram + address + 8, 2);
            const bool interleave = (flags & kStoredAsFields) &&
                                    ((gpWord(rdram, ctx, kFieldMode) & 0xff) || gpWord(rdram, ctx, kFullHeightMode));
            const uint64_t key = contentKey(rdram, address) ^ uint64_t(interleave);
            if (const auto found = g_textures.find(key); found != g_textures.end())
                return found->second;
            auto texture = std::make_shared<bozo::render::Texture>();
            if (!con::textures::decodeLump(rdram, address, *texture))
                return nullptr;
            if (interleave)
                interleaveFields(*texture);
            texture->key = key;
            texture->version = 1;
            if (g_textures.size() >= kMaxCached)
                g_textures.clear();
            g_textures[key] = texture;
            return texture;
        }

        void drawQuad(const std::shared_ptr<const bozo::render::Texture> &texture, float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1, uint32_t colour)
        {
            const auto corner = [&](float x, float y, float u, float v) {
                bozo::render::Vertex vertex;
                vertex.x = kOriginX + x;
                vertex.y = kOriginY + y;
                vertex.u = u;
                vertex.v = v;
                vertex.rgba = colour;
                return vertex;
            };
            const bozo::render::Vertex quad[6] = {corner(x0, y0, u0, v0), corner(x1, y0, u1, v0), corner(x0, y1, u0, v1),
                                                  corner(x1, y0, u1, v0), corner(x1, y1, u1, v1), corner(x0, y1, u0, v1)};
            bozo::render::Material material;
            material.tex0 = texture->key;
            material.blend = bozo::render::Material::Blend::Alpha;
            material.alphaTest = true;
            material.alphaRef = 1;
            material.depthTest = false;
            material.depthWrite = false;
            material.clampU = material.clampV = true;
            bozo::render::Renderer::instance().drawOverlay(material, quad, 6u, texture);
        }

        void textureDrawColor(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            if (const auto texture = lookUp(rdram, ctx, getRegU32(ctx, 4))) {
                const float x = float(int32_t(getRegU32(ctx, 5))), y = float(int32_t(getRegU32(ctx, 6))) + 1.0f;
                drawQuad(texture, x, y, x + float(texture->width), y + float(texture->height), 0, 0, 1, 1, getRegU32(ctx, 8));
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & 0x1ffffffu), sizeof(T));
            return value;
        }

        int32_t glyphOf(const uint8_t *rdram, uint32_t font, uint16_t character)
        {
            if (character < 0x100) {
                const int16_t mapped = guest<int16_t>(rdram, font + 0x14 + character * 2u);
                return mapped >= 0 ? mapped : guest<uint16_t>(rdram, font + 0x6e);
            }
            const int32_t count = guest<int16_t>(rdram, font);
            const uint32_t table = guest<uint32_t>(rdram, font + 8);
            for (int32_t i = 0; i < count; ++i)
                if (guest<uint16_t>(rdram, table + uint32_t(i) * 16u) == character)
                    return i;
            return 0;
        }

        float kerning(const uint8_t *rdram, uint32_t font, int32_t first, int32_t previous, int32_t glyph)
        {
            const uint32_t table = guest<uint32_t>(rdram, font + 0xc);
            for (uint32_t record = table + uint32_t(first) * 6u; guest<uint16_t>(rdram, record + 2) == uint16_t(glyph); record += 6u)
                if (guest<uint16_t>(rdram, record) == uint16_t(previous))
                    return float(guest<int16_t>(rdram, record + 4)) * 0.5f;
            return 0.0f;
        }

        void fontPrint(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const int32_t top = int32_t(getRegU32(ctx, 5));
            uint32_t text = getRegU32(ctx, 6);
            uint32_t left = getRegU32(ctx, 7);
            if (uint32_t(top + 0x64) >= 0x385u || !left)
                return;
            const uint32_t font = gpWord(rdram, ctx, kCurrentFont);
            const auto texture = lookUp(rdram, ctx, guest<uint32_t>(rdram, font + 0x10));
            if (!texture)
                return;
            const uint32_t table = guest<uint32_t>(rdram, font + 8);
            const uint32_t colour = gpWord(rdram, ctx, kFontColour);
            const float width = float(texture->width), height = float(texture->height);
            float x = float(int32_t(getRegU32(ctx, 4)));
            int32_t previous = -1;
            for (uint16_t c; left && (c = guest<uint16_t>(rdram, text)) != 0; --left, text += 2) {
                if (c == '\n') {
                    previous = kNoGlyph;
                    continue;
                }
                const int32_t glyph = glyphOf(rdram, font, c);
                if (glyph <= 0) {
                    x += float(guest<int16_t>(rdram, table + 0xc));
                    previous = glyph;
                    continue;
                }
                const uint32_t record = table + uint32_t(glyph) * 16u;
                const int32_t u0 = guest<int16_t>(rdram, record + 2), v0 = guest<int16_t>(rdram, record + 6);
                const int32_t u1 = guest<int16_t>(rdram, record + 4), v1 = guest<int16_t>(rdram, record + 8);
                const int32_t first = guest<int16_t>(rdram, record + 0xe);
                if (first >= 0)
                    x += kerning(rdram, font, first, previous, glyph);
                const float y = float(top + guest<int16_t>(rdram, record + 0xa));
                uint32_t tint = colour;
                if (uint32_t(glyph - 0x5b) < 0xeu) {
                    const uint32_t alpha = gpWord(rdram, ctx, kFontAlpha);
                    tint = (alpha == 0xffffffffu ? colour >> 24 : alpha) << 24 | 0x606060u;
                }
                drawQuad(texture, x, y, x + float(u1 - u0), y + float(v1 - v0), float(u0) / width, float(v0) / height,
                         float(u1) / width, float(v1) / height, tint);
                x += float(guest<int16_t>(rdram, record + 0xc));
                previous = glyph;
            }
        }
    }

    void endFrame()
    {
        bozo::render::Screen screen;
        screen.originX = kOriginX;
        screen.originY = kOriginY;
        screen.width = 640.0f;
        screen.height = 448.0f;
        bozo::render::Renderer::instance().publishOverlay(screen);
    }

    void install(bozo::port::Registry &runtime)
    {
        runtime.registerFunction(kTextureDrawColor, &textureDrawColor);
        runtime.registerFunction(kFontPrint, &fontPrint);
    }
}
