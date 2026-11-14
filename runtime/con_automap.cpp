// The automap (map::draw) drawn natively into the 2D overlay: the explored tile grid, rotated about the player.
#include "con_overlay.h"
#include "con_textures.h"

#include "bozo/context.h"
#include "bozo/port.h"
#include "bozo/render/renderer.h"
#include "bozo/runtime_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace con::automap
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu;
        constexpr uint32_t kMapDraw = 0x0015c628u, kGrid = 0x003347c0u, kGridColumns = 216;
        constexpr int32_t kMapOrigin = -0x3b70, kMapWidth = -0x3b30, kMapHeight = -0x3b2c;
        constexpr int32_t kExtentScale = -0x7ac0, kWorldScale = -0x7abc, kExtentFactor = -0x7ab8, kTileSpan = -0x7ab4;
        constexpr int32_t kLayer = 5, kIconCount = -0x1138, kMapScale = -0x7ab0;
        constexpr uint32_t kIcons = 0x006023c8u, kIconSize = 0x1c, kQuestShown = 0x001d39c0u, kQuestOpen = 0x001d3e88u;
        constexpr float kMarkerZoom = 0.125f, kShadowOffset = 2.0f, kIconScale = 0.75f;
        constexpr std::array<std::array<float, 2>, 3> kMarker = {{{0.0f, -1.0f}, {-0.75f, 1.0f}, {0.75f, 1.0f}}};
        constexpr float kCellPixels = 7.5f, kTilePitch = 10.0f, kTau = 6.28318530717958647692f;

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & kRamMask), sizeof(T));
            return value;
        }

        // The strip vertex (TL, BL, TR, BR) given the UV corners (u, v), (u, v + w), (u + w, v), (u + w, v + w), by the low three
        // bits of a cell's second byte: horizontal flip, vertical flip, then transpose.
        std::array<std::array<int, 4>, 8> orientations()
        {
            std::array<std::array<int, 4>, 8> table{};
            for (int k = 0; k < 8; ++k) {
                std::array<int, 4> slot = {0, 1, 2, 3};
                if (k & 1) {
                    std::swap(slot[0], slot[2]);
                    std::swap(slot[3], slot[1]);
                }
                if (k & 2) {
                    std::swap(slot[0], slot[1]);
                    std::swap(slot[2], slot[3]);
                }
                if (k & 4)
                    std::swap(slot[1], slot[2]);
                table[size_t(k)] = slot;
            }
            return table;
        }

        // A lump decoded as stored: the map's textures are drawn without the 2D calls' field interleaving.
        std::shared_ptr<const bozo::render::Texture> lumpTexture(const uint8_t *rdram, uint32_t lump)
        {
            static std::unordered_map<uint64_t, std::shared_ptr<const bozo::render::Texture>> cache;
            const uint64_t key = con::textures::lumpKey(rdram, lump);
            if (const auto found = cache.find(key); found != cache.end())
                return found->second;
            auto texture = std::make_shared<bozo::render::Texture>();
            if (!con::textures::decodeLump(rdram, lump, *texture) || !texture->width || !texture->height)
                return nullptr;
            texture->key = key;
            texture->version = 1;
            if (cache.size() >= 256)
                cache.clear();
            return cache[key] = texture;
        }

        float trig(int32_t angle, bool sine)
        {
            const float radians = float(int16_t(angle)) * kTau / 65536.0f;
            return sine ? std::sin(radians) : std::cos(radians);
        }

        // A guest function the game hands over (a callback, a quest check), called with up to six integer arguments.
        uint32_t callGuest(uint8_t *rdram, R5900Context *ctx, Runtime *runtime, uint32_t target, std::initializer_list<uint32_t> args)
        {
            const Gpr128 sp = ctx->r[29], ra = ctx->r[31];
            uint32_t reg = 4;
            for (uint32_t value : args)
                setRegU64(ctx, reg++, int64_t(int32_t(value)));
            setRegU64(ctx, 29, int64_t(int32_t(sp.uw[0] - 0x400u)));
            setRegU64(ctx, 31, int64_t(int32_t(kMapDraw)));
            bozo_call_indirect(rdram, ctx, runtime, target);
            const uint32_t result = getRegU32(ctx, 2);
            ctx->r[29] = sp;
            ctx->r[31] = ra;
            return result;
        }

        void draw(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            const uint32_t returnTo = getRegU32(ctx, 31);
            struct Return
            {
                R5900Context *ctx;
                uint32_t pc;
                ~Return() { ctx->pc = pc; }
            } leave{ctx, returnTo};
            const uint32_t gp = getRegU32(ctx, 28), world = getRegU32(ctx, 4), player = getRegU32(ctx, 5);
            const auto gpFloat = [&](int32_t offset) { return guest<float>(rdram, gp + uint32_t(offset)); };
            const float x = ctx->f[12], y = ctx->f[13], zoom = ctx->f[14], halfWidth = ctx->f[15], halfHeight = ctx->f[16];
            const float markerSize = ctx->f[17], iconSize = ctx->f[18] * kIconScale;
            const uint32_t callback = getRegU32(ctx, 6);
            const int32_t mapAngle = int16_t(getRegU32(ctx, 7));
            const uint32_t tiles = guest<uint32_t>(rdram, world + 0x6c);
            if ((!tiles && !guest<uint32_t>(rdram, world + 0x70)) || int32_t(halfWidth) < 2)
                return;
            const float width = float(int32_t(halfWidth));
            con::overlay::Clip clip;
            clip.x0 = float(std::max(int32_t(x - width) + 0x140, 0));
            clip.x1 = float(std::min(int32_t(x + width) + 0x140, 0x27f)) + 1.0f;
            clip.y0 = float(std::max(int32_t(y) - int32_t(halfHeight) + 0xdc, 0));
            clip.y1 = float(std::min(int32_t(y) + int32_t(halfHeight) + 0xdc, 0x1bf)) + 1.0f;

            const float angle = -float(mapAngle) * kTau / 65536.0f;
            const float k = kCellPixels * zoom, c = std::cos(angle) * k, s = std::sin(angle) * k;
            const float mapX = guest<float>(rdram, gp + uint32_t(kMapOrigin)) + guest<float>(rdram, player) * 0.25f * gpFloat(kWorldScale);
            const float mapY = guest<float>(rdram, gp + uint32_t(kMapOrigin) + 4) + guest<float>(rdram, player + 4) * 0.25f * gpFloat(kWorldScale);
            const float extent = width * gpFloat(kExtentScale) / zoom * gpFloat(kExtentFactor);
            const int32_t x0 = int32_t(std::floor(std::max(mapX - extent - 1.0f, 0.0f)));
            const int32_t y0 = int32_t(std::floor(std::max(mapY - extent - 1.0f, 0.0f)));
            const int32_t x1 = int32_t(std::ceil(std::min(mapX + extent, float(guest<int32_t>(rdram, gp + uint32_t(kMapWidth))))));
            const int32_t y1 = int32_t(std::ceil(std::min(mapY + extent, float(guest<int32_t>(rdram, gp + uint32_t(kMapHeight))))));
            if (x0 >= x1 || y0 >= y1)
                return;

            const auto screen = [&](float i, float j) {
                const float di = i - mapX, dj = j - mapY;
                return std::array<float, 2>{320.0f + x + c * di + s * dj, 224.0f + y + s * di - c * dj};
            };
            const auto corner = [&](int32_t i, int32_t j) { return screen(float(i), float(j)); };
            const auto cell = [&](int32_t i, int32_t j) { return kGrid + (uint32_t(j) * kGridColumns + uint32_t(i)) * 2u; };
            const auto toMap = [&](uint32_t point) {
                const float scale = 0.25f * gpFloat(kMapScale);
                return std::array<float, 2>{guest<float>(rdram, gp + uint32_t(kMapOrigin)) + guest<float>(rdram, point) * scale,
                                            guest<float>(rdram, gp + uint32_t(kMapOrigin) + 4) + guest<float>(rdram, point + 4) * scale};
            };
            const auto vertex = [](float vx, float vy, uint32_t rgba, float u = 0.0f, float v = 0.0f) {
                bozo::render::Vertex out;
                out.x = vx;
                out.y = vy;
                out.u = u;
                out.v = v;
                out.rgba = rgba;
                return out;
            };

            if (kMarkerZoom < zoom) {
                const uint32_t out = getRegU32(ctx, 29) - 0x200u;
                std::vector<bozo::render::Vertex> markers;
                for (uint32_t index = 0; index < 256; ++index) {
                    const uint32_t shown = callGuest(rdram, ctx, runtime, callback, {index, out, out + 0x10, out + 0x14, out + 0x18, out + 0x1c});
                    if (shown == 0xffffffffu)
                        break;
                    if (!shown)
                        continue;
                    const auto at = toMap(out);
                    const auto centre = screen(at[0], at[1]);
                    const int32_t turn = int32_t(guest<uint16_t>(rdram, out + 0x10)) + mapAngle - 0x3fff;
                    const float mc = trig(turn, false), ms = trig(turn, true);
                    const uint32_t colour = (guest<uint32_t>(rdram, out + 0x14) & 0xffu) | (guest<uint32_t>(rdram, out + 0x18) & 0xffu) << 8 |
                                            (guest<uint32_t>(rdram, out + 0x1c) & 0xffu) << 16 | 0x80000000u;
                    for (const auto &[offset, rgba] : {std::pair{kShadowOffset, 0x60000000u}, std::pair{0.0f, colour}})
                        for (const auto &point : kMarker) {
                            const float px = point[0] * markerSize, py = point[1] * markerSize;
                            markers.push_back(vertex(centre[0] + px * mc + py * ms + offset, centre[1] - px * ms + py * mc + offset, rgba));
                        }
                }
                con::overlay::queue(kLayer, nullptr, std::move(markers), clip);
            }

            const int32_t icons = guest<int32_t>(rdram, gp + uint32_t(kIconCount));
            for (int32_t n = 0; n < icons && n < 256; ++n) {
                const uint32_t record = kIcons + uint32_t(n) * kIconSize;
                const uint32_t shownBy = guest<uint32_t>(rdram, record + 0x14), openBy = guest<uint32_t>(rdram, record + 0x18);
                if (shownBy && !callGuest(rdram, ctx, runtime, kQuestShown, {shownBy}))
                    continue;
                if (openBy && !callGuest(rdram, ctx, runtime, kQuestOpen, {openBy}))
                    continue;
                const uint32_t lump = guest<uint32_t>(rdram, record + 8);
                const auto texture = lump ? lumpTexture(rdram, lump) : nullptr;
                const auto at = toMap(record);
                const int32_t cx = std::clamp(int32_t(at[0]), 0, 0xd7), cy = std::clamp(int32_t(at[1]), 0, 0xd7);
                const uint32_t revealed = guest<uint8_t>(rdram, cell(cx, cy) + 1) & 0xf0u;
                const uint32_t rgba = std::min(revealed, 0x80u) << 24 | 0x808080u;
                const auto centre = screen(at[0], at[1]);
                const bool rotated = guest<uint32_t>(rdram, record + 0xc) != 0;
                const float w = lump ? float(guest<int16_t>(rdram, lump)) : 0.0f, h = lump ? float(guest<int16_t>(rdram, lump + 2)) : 0.0f;
                std::array<bozo::render::Vertex, 4> strip;
                const std::array<std::array<float, 2>, 4> offsets = {{{-1, -1}, {-1, 1}, {1, -1}, {1, 1}}};
                const std::array<std::array<float, 2>, 4> uvs = {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}};
                const float ic = std::cos(angle), is = std::sin(angle);
                for (size_t n4 = 0; n4 < 4; ++n4) {
                    const float ox = offsets[n4][0] * iconSize, oy = offsets[n4][1] * iconSize;
                    const float vx = rotated ? centre[0] + ox * ic + oy * is : centre[0] + ox;
                    const float vy = rotated ? centre[1] + ox * is - oy * ic : centre[1] + oy;
                    strip[n4] = vertex(vx, vy, rgba, texture ? uvs[n4][0] * w / float(texture->width) : 0.0f,
                                       texture ? uvs[n4][1] * h / float(texture->height) : 0.0f);
                }
                con::overlay::queue(kLayer, texture, {strip[0], strip[1], strip[2], strip[2], strip[1], strip[3]}, clip);
            }

            if (!tiles)
                return;
            const auto texture = lumpTexture(rdram, tiles);
            if (!texture)
                return;
            const auto alpha = [&](int32_t i, int32_t j) { return uint32_t(guest<uint8_t>(rdram, cell(i, j) + 1)); };
            static const auto kOrientations = orientations();
            const float span = gpFloat(kTileSpan), texelU = 1.0f / float(texture->width), texelV = 1.0f / float(texture->height);
            std::vector<bozo::render::Vertex> triangles;
            for (int32_t j = y0 + 1; j < y1; ++j)
                for (int32_t i = x0 + 1; i < x1; ++i) {
                    const uint8_t tile = guest<uint8_t>(rdram, cell(i, j)), flags = guest<uint8_t>(rdram, cell(i, j) + 1);
                    const float u = float(tile & 31u) * kTilePitch, v = float(tile >> 5) * kTilePitch;
                    const std::array<std::array<float, 2>, 4> uv = {{{u, v}, {u, v + span}, {u + span, v}, {u + span, v + span}}};
                    const auto &slot = kOrientations[flags & 7u];
                    const std::array<std::array<float, 2>, 4> at = {corner(i, j), corner(i, j + 1), corner(i + 1, j), corner(i + 1, j + 1)};
                    const std::array<uint32_t, 4> a = {alpha(i - 1, j - 1), alpha(i - 1, j), alpha(i, j - 1), flags};
                    std::array<bozo::render::Vertex, 4> strip;
                    for (size_t n = 0; n < 4; ++n) {
                        auto &vertex = strip[n];
                        vertex.x = at[n][0];
                        vertex.y = at[n][1];
                        vertex.rgba = 0x00ffffffu | a[n] << 24;
                    }
                    for (size_t n = 0; n < 4; ++n) {
                        strip[size_t(slot[n])].u = uv[n][0] * texelU;
                        strip[size_t(slot[n])].v = uv[n][1] * texelV;
                    }
                    triangles.insert(triangles.end(), {strip[0], strip[1], strip[2], strip[2], strip[1], strip[3]});
                }
            con::overlay::queue(kLayer, texture, std::move(triangles), clip);
        }
    }

    void install(bozo::port::Registry &runtime) { runtime.registerFunction(kMapDraw, &draw); }
}
