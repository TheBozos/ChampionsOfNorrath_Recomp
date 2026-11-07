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

        void textureDrawColor(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            const auto texture = lookUp(rdram, ctx, getRegU32(ctx, 4));
            if (texture) {
                const float x0 = kOriginX + float(int32_t(getRegU32(ctx, 5)));
                const float y0 = kOriginY + float(int32_t(getRegU32(ctx, 6))) + 1.0f;
                const float x1 = x0 + float(texture->width), y1 = y0 + float(texture->height);
                const uint32_t colour = getRegU32(ctx, 8);
                const auto corner = [&](float x, float y, float u, float v) {
                    bozo::render::Vertex vertex;
                    vertex.x = x;
                    vertex.y = y;
                    vertex.u = u;
                    vertex.v = v;
                    vertex.rgba = colour;
                    return vertex;
                };
                const bozo::render::Vertex quad[6] = {corner(x0, y0, 0, 0), corner(x1, y0, 1, 0), corner(x0, y1, 0, 1),
                                                      corner(x1, y0, 1, 0), corner(x1, y1, 1, 1), corner(x0, y1, 0, 1)};
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
            ctx->pc = getRegU32(ctx, 31);
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
    }
}
