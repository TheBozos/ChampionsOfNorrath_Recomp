// The level's geometry chunks drawn natively: decoded once from the stored packet, with the game's camera.
#include "con_world.h"

#include "con_characters.h"
#include "con_decals.h"
#include "con_lights.h"
#include "con_particles.h"
#include "con_shadows.h"
#include "con_textures.h"

#include "bozo/context.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace con::world
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1ffffffu;
        constexpr uint32_t kLevelName = 0x0037f988u, kViewProjection = 0x0037ef30u, kCharacterViewProjection = 0x0037efb0u;
        constexpr uint32_t kView = 0x0037ef00u, kScrollingEnvironment = 0x4;
        constexpr int32_t kLevelOptions = -0x3530, kTick = -0x354c;
        constexpr float kScreenTerm = 0.07f, kGsOffset = 2048.0f;
        constexpr int32_t kWaterTexture = -0x3850, kWaterColour = -0x389c, kWaterFix = -0x38a4, kWaterScale = -0x3894;
        constexpr int32_t kWaterColour2 = -0x3898, kWaterFix2 = -0x38a0, kWaterScale2 = -0x3890, kWaterStep = -0x79a0;
        constexpr uint32_t kWaterSecondSet = 0x1000;
        constexpr float kWaterSlope = 0.0499200001f;
        constexpr float kCharacterDepth = 1.0f / 1125899906842624.0f;
        constexpr uint32_t kQuarterTurns = 0x1, kEnvironment = 0x10, kMirrored = 0x40, kLateLayer = 0x200, kAlphaTested = 0x400;

        template <typename T>
        T guest(const uint8_t *rdram, uint32_t address)
        {
            T value;
            std::memcpy(&value, rdram + (address & kRamMask), sizeof(T));
            return value;
        }

        struct Mesh
        {
            std::vector<bozo::render::Vertex> vertices;
            std::shared_ptr<const bozo::render::Texture> texture;
            bozo::render::Material material;
            std::array<int32_t, 3> translation{};
            bool late = false;
            std::vector<std::array<float, 3>> normals;
            std::array<float, 3> position{};
            uint32_t lighting = 0;
            bool environment = false;
            std::vector<std::array<float, 3>> local, stored;
            std::array<float, 3> low{}, high{};
            std::vector<bozo::render::Vertex> lit;
            con::lights::ChunkLights litBy;
        };

        struct Queued
        {
            const Mesh *mesh = nullptr;
            std::vector<bozo::render::Vertex> shaded;
            const std::vector<bozo::render::Vertex> *lit = nullptr;
            std::array<float, 16> matrix{};
            std::vector<bozo::render::Vertex> environment;
        };

        struct Unpack
        {
            uint32_t count = 0, data = 0;
        };

        struct Chunk
        {
            int32_t tx = 0, ty = 0, tz = 0;
            uint32_t flags = 0;
            int16_t cosine = 0, sine = 0;
        };

        void place(const Chunk &c, int16_t x, int16_t y, int16_t z, bozo::render::Vertex &out)
        {
            const float px = float(x + c.tx) * 0.25f, py = float(y + c.ty) * 0.25f;
            out.z = float(z + c.tz) * 0.25f;
            if (c.flags & kQuarterTurns) {
                const int code = c.cosine & 7;
                const float a = code & 1 ? -px : px, b = code & 2 ? -py : py;
                out.x = code & 4 ? b : a;
                out.y = code & 4 ? a : b;
                return;
            }
            const float cosine = float(c.cosine) / 32767.0f, sine = float(c.sine) / 32767.0f;
            if (c.flags & kMirrored) {
                out.x = px * cosine + py * sine;
                out.y = px * sine - py * cosine;
            } else {
                out.x = px * cosine - py * sine;
                out.y = px * sine + py * cosine;
            }
        }

        using Placement = std::function<void(int16_t, int16_t, int16_t, bozo::render::Vertex &)>;
        using Turn = std::function<std::array<float, 3>(const std::array<float, 3> &)>;

        struct Normals
        {
            Turn turn;
            std::vector<std::array<float, 3>> *out = nullptr;
            std::vector<std::array<float, 3>> *local = nullptr, *stored = nullptr;
        };

        void addBatch(const uint8_t *rdram, const Placement &place, const Unpack (&u)[6], float width, float height,
                      std::vector<bozo::render::Vertex> &out, const Normals &normals = {})
        {
            const uint32_t count = u[0].count;
            const uint32_t tag = guest<uint32_t>(rdram, u[4].data), prim = guest<uint32_t>(rdram, u[4].data + 4);
            const uint32_t strip = tag & 0x7fff, registers = prim >> 28;
            if (!registers || strip > u[5].count || u[2].count < count + 2)
                return;
            std::vector<int32_t> slot(strip, -1);
            std::vector<uint8_t> restart(strip, 0);
            const auto copy = [&](uint32_t source, uint32_t target) {
                if (source == target)
                    return;
                const uint32_t from = source / registers, to = (target & 0x7fff) / registers;
                if (from < strip && to < strip) {
                    slot[to] = slot[from];
                    if (target & 0x8000)
                        restart[to] = 1;
                }
            };
            for (uint32_t r = 0; r < count; ++r) {
                const uint32_t row = u[2].data + (r + 2) * 6;
                const uint16_t a = guest<uint16_t>(rdram, row);
                copy(guest<uint16_t>(rdram, row + 2), guest<uint16_t>(rdram, row + 4));
                const uint32_t to = (a & 0x7fffu) / registers;
                if (to < strip) {
                    slot[to] = int32_t(r);
                    if (a & 0x8000)
                        restart[to] = 1;
                }
            }
            for (uint32_t r = 1; r < u[3].count; ++r) {
                const uint32_t row = u[3].data + r * 8;
                copy(guest<uint16_t>(rdram, row), guest<uint16_t>(rdram, row + 2));
                copy(guest<uint16_t>(rdram, row + 4), guest<uint16_t>(rdram, row + 6));
            }
            const auto vertex = [&](uint32_t at) {
                const uint32_t position = u[0].data + uint32_t(slot[at]) * 6;
                bozo::render::Vertex v;
                place(guest<int16_t>(rdram, position), guest<int16_t>(rdram, position + 2),
                      guest<int16_t>(rdram, position + 4), v);
                const uint32_t uv = u[5].data + at * 4;
                v.u = float(guest<uint16_t>(rdram, uv)) / 16.0f / width;
                v.v = float(guest<uint16_t>(rdram, uv + 2)) / 16.0f / height;
                v.rgba = 0x80808080u;
                if (normals.local) {
                    normals.local->push_back({float(guest<int16_t>(rdram, position)), float(guest<int16_t>(rdram, position + 2)),
                                              float(guest<int16_t>(rdram, position + 4))});
                    const uint32_t at3 = u[1].data + uint32_t(slot[at]) * 3;
                    normals.stored->push_back({float(guest<int8_t>(rdram, at3)), float(guest<int8_t>(rdram, at3 + 1)),
                                               float(guest<int8_t>(rdram, at3 + 2))});
                }
                if (normals.out) {
                    const uint32_t at3 = u[1].data + uint32_t(slot[at]) * 3;
                    normals.out->push_back(normals.turn({float(guest<int8_t>(rdram, at3)) / 127.0f,
                                                         float(guest<int8_t>(rdram, at3 + 1)) / 127.0f,
                                                         float(guest<int8_t>(rdram, at3 + 2)) / 127.0f}));
                }
                return v;
            };
            for (uint32_t p = 2; p < strip; ++p) {
                if (restart[p] || slot[p] < 0 || slot[p - 1] < 0 || slot[p - 2] < 0)
                    continue;
                if (slot[p] == slot[p - 1] || slot[p - 1] == slot[p - 2] || slot[p] == slot[p - 2])
                    continue;
                out.push_back(vertex(p - 2));
                out.push_back(vertex(p - 1));
                out.push_back(vertex(p));
            }
        }

        void decodePacket(const uint8_t *rdram, const Placement &place, uint32_t at, uint32_t end, float width, float height,
                          std::vector<bozo::render::Vertex> &out, const Normals &normals = {})
        {
            Unpack unpacks[6];
            int found = 0;
            while (at + 4 <= end) {
                const uint32_t code = guest<uint32_t>(rdram, at);
                at += 4;
                const uint32_t command = (code >> 24) & 0x7f, number = (code >> 16) & 0xff, immediate = code & 0xffff;
                if (command >= 0x60) {
                    const uint32_t components = ((command >> 2) & 3) + 1, bits = 32u >> (command & 3);
                    const uint32_t count = number ? number : 256;
                    if (found < 6)
                        unpacks[found++] = {count, at};
                    at += ((components * bits * count + 31) / 32) * 4;
                    if (found == 6) {
                        addBatch(rdram, place, unpacks, width, height, out, normals);
                        found = 0;
                    }
                } else if (command == 0x50 || command == 0x51) {
                    at += (immediate ? immediate : 0x10000u) * 16;
                } else if (command == 0x20) {
                    at += 4;
                } else if (command == 0x30 || command == 0x31) {
                    at += 16;
                }
            }
        }

        // A model packet: positions, normals, slot rows, weights, copy rows, GIF tags, [shell normals], MSCAL, then the
        // texture coordinates. Only the blocks the strip needs are kept, in addBatch's order.
        void decodeModelMesh(const uint8_t *rdram, const Placement &place, uint32_t at, uint32_t end, float width, float height,
                             std::vector<bozo::render::Vertex> &out, const Normals &normals)
        {
            Unpack unpacks[6];
            int found = 0;
            bool called = false;
            while (at + 4 <= end) {
                const uint32_t code = guest<uint32_t>(rdram, at);
                at += 4;
                const uint32_t command = (code >> 24) & 0x7f, number = (code >> 16) & 0xff, immediate = code & 0xffff;
                if (command >= 0x60) {
                    const uint32_t components = ((command >> 2) & 3) + 1, bits = 32u >> (command & 3);
                    const uint32_t count = number ? number : 256;
                    const Unpack block{count, at};
                    at += ((components * bits * count + 31) / 32) * 4;
                    if (called) {
                        unpacks[5] = block;
                        if (found == 5)
                            addBatch(rdram, place, unpacks, width, height, out, normals);
                        found = 0;
                        called = false;
                    } else if (found == 0) {
                        unpacks[0] = block;
                        found = 1;
                    } else if (found == 1) {
                        unpacks[1] = block;
                        found = 2;
                    } else if (found == 2) {
                        unpacks[2] = block;
                        found = 3;
                    } else if ((command & 0xf) == 0xd) {
                        unpacks[3] = block;
                        found = 4;
                    } else if ((command & 0xf) == 0xc && found == 4) {
                        unpacks[4] = block;
                        found = 5;
                    }
                } else if (command == 0x14 || command == 0x15 || command == 0x17) {
                    called = found == 5;
                    if (!called)
                        found = 0;
                } else if (command == 0x50 || command == 0x51) {
                    at += (immediate ? immediate : 0x10000u) * 16;
                } else if (command == 0x20) {
                    at += 4;
                } else if (command == 0x30 || command == 0x31) {
                    at += 16;
                }
            }
        }

        std::string g_discImage, g_levelName;
        std::unique_ptr<con::textures::Level> g_level;
        bool g_published = false;
        std::unordered_map<uint32_t, std::unique_ptr<Mesh>> g_meshes;
        std::vector<Queued> g_queue;

        void followLevel(const uint8_t *rdram)
        {
            const char *name = reinterpret_cast<const char *>(rdram + kLevelName);
            if (g_levelName == name)
                return;
            g_levelName = name;
            g_meshes.clear();
            g_queue.clear();
            g_level.reset();
            if (g_discImage.empty() || g_levelName.empty())
                return;
            auto level = std::make_unique<con::textures::Level>();
            std::string error;
            if (level->open(g_discImage, g_levelName, error))
                g_level = std::move(level);
            else
                std::fprintf(stderr, "con: no textures for level %s: %s\n", g_levelName.c_str(), error.c_str());
        }

        // ALPHA 2 is Cs * (1 - As) over nothing, ALPHA 1 the usual blend with As inverted: As has 0 as opaque.
        std::shared_ptr<const bozo::render::Texture> forBlend(const bozo::render::Texture &source, uint32_t flags)
        {
            auto texture = std::make_shared<bozo::render::Texture>(source);
            const bool blended = flags & (kLateLayer | kAlphaTested);
            for (uint32_t &texel : texture->rgba) {
                const uint32_t alpha = std::min(texel >> 24, 0x80u);
                if (blended) {
                    texel = (texel & 0xffffffu) | (0x80u - alpha) << 24;
                } else if (!(flags & kEnvironment)) {
                    const uint32_t keep = 0x80u - alpha;
                    const uint32_t r = (texel & 0xff) * keep >> 7, g = ((texel >> 8) & 0xff) * keep >> 7, b = ((texel >> 16) & 0xff) * keep >> 7;
                    texel = r | g << 8 | b << 16 | 0x80000000u;
                } else {
                    texel = (texel & 0xffffffu) | alpha << 24;
                }
            }
            return texture;
        }

        std::unique_ptr<Mesh> build(const uint8_t *rdram, uint32_t record, const Chunk &chunk)
        {
            const int16_t cell = guest<int16_t>(rdram, record + 0x34);
            const uint32_t offset = guest<uint32_t>(rdram, record + 0x20);
            if (!g_level || offset < 0x40)
                return nullptr;
            const auto source = g_level->texture(cell, offset / 0x40 - 1);
            if (!source || !source->width || !source->height)
                return nullptr;
            auto mesh = std::make_unique<Mesh>();
            mesh->texture = forBlend(*source, chunk.flags);
            mesh->translation = {chunk.tx, chunk.ty, chunk.tz};
            mesh->late = chunk.flags & kLateLayer;
            auto &material = mesh->material;
            const bool blended = chunk.flags & (kLateLayer | kAlphaTested);
            material.tex0 = source->key;
            material.blend = blended ? bozo::render::Material::Blend::Alpha : bozo::render::Material::Blend::Opaque;
            material.alphaTest = chunk.flags & kAlphaTested;
            material.alphaRef = 0x51;
            const uint32_t data = guest<uint32_t>(rdram, record);
            mesh->environment = (chunk.flags & kEnvironment) && !blended;
            const Turn turn = [&](const std::array<float, 3> &n) -> std::array<float, 3> {
                const float k = 127.0f / 16.0f;
                if (!(chunk.flags & kQuarterTurns))
                    return {n[0] * k, n[1] * k, n[2] * k};
                const int code = chunk.cosine & 7;
                const float a = code & 1 ? -n[0] : n[0], b = code & 2 ? -n[1] : n[1];
                return {(code & 4 ? b : a) * k, (code & 4 ? a : b) * k, n[2] * k};
            };
            Normals normals{turn, mesh->environment ? &mesh->normals : nullptr, &mesh->local, &mesh->stored};
            decodePacket(rdram, [&](int16_t x, int16_t y, int16_t z, bozo::render::Vertex &v) { place(chunk, x, y, z, v); }, data, data + guest<uint32_t>(rdram, record + 4) * 16, float(source->width),
                         float(source->height), mesh->vertices, normals);
            if (mesh->normals.size() != mesh->vertices.size())
                mesh->environment = false;
            for (auto &p : mesh->local)
                p = {(p[0] + float(chunk.tx)) / 16.0f, (p[1] + float(chunk.ty)) / 16.0f, (p[2] + float(chunk.tz)) / 16.0f};
            if (!mesh->local.empty()) {
                mesh->low = mesh->high = mesh->local.front();
                for (const auto &p : mesh->local)
                    for (uint32_t k = 0; k < 3; ++k) {
                        mesh->low[k] = std::min(mesh->low[k], p[k]);
                        mesh->high[k] = std::max(mesh->high[k], p[k]);
                    }
            }
            return mesh;
        }
    }

    void setDiscImage(const std::string &path) { g_discImage = path; }

    namespace
    {
        std::shared_ptr<const bozo::render::Texture> g_environment;
        uint64_t g_environmentKey = 0, g_environmentFrame = ~0ull, g_frames = 0;

        const bozo::render::Texture *environmentTexture(const uint8_t *rdram, uint32_t gp)
        {
            if (g_environmentFrame == g_frames && g_environment)
                return g_environment.get();
            g_environmentFrame = g_frames;
            const uint32_t lump = con::textures::findResource(rdram, gp, "env.lmp", "envtex.etex");
            if (!lump)
                return nullptr;
            const uint64_t key = con::textures::lumpKey(rdram, lump);
            if (key != g_environmentKey || !g_environment) {
                g_environment.reset();
                g_environmentKey = key;
                auto texture = std::make_shared<bozo::render::Texture>();
                if (!con::textures::decodeLump(rdram, lump, *texture) || !texture->width || !texture->height)
                    return nullptr;
                for (uint32_t &texel : texture->rgba)
                    texel |= 0x80000000u;
                texture->key = key ^ 0x454e560000000000ull;
                texture->version = 1;
                g_environment = std::move(texture);
            }
            return g_environment.get();
        }

    }

    std::shared_ptr<const bozo::render::Texture> environment(const uint8_t *rdram, uint32_t gp)
    {
        return environmentTexture(rdram, gp) ? g_environment : nullptr;
    }

    namespace
    {
        std::vector<bozo::render::Vertex> environmentVertices(const uint8_t *rdram, uint32_t gp, const Mesh &mesh,
                                                              const std::array<float, 16> &m)
        {
            float view[9];
            std::memcpy(view, rdram + kView, sizeof(view));
            const float scroll = guest<uint32_t>(rdram, gp + uint32_t(kLevelOptions)) & kScrollingEnvironment
                                     ? float(guest<uint32_t>(rdram, gp + uint32_t(kTick)) & 0x3ffu) * 0.25f
                                     : 0.0f;
            const float texelU = 1.0f / float(g_environment->width), texelV = 1.0f / float(g_environment->height);
            std::vector<bozo::render::Vertex> out = mesh.vertices;
            for (size_t i = 0; i < out.size(); ++i) {
                auto &v = out[i];
                const auto &n = mesh.normals[i];
                const float w = v.x * m[3] + v.y * m[7] + v.z * m[11] + m[15];
                const float sx = w > 0.0f ? (v.x * m[0] + v.y * m[4] + v.z * m[8] + m[12]) / w : 0.0f;
                const float sy = w > 0.0f ? (v.x * m[1] + v.y * m[5] + v.z * m[9] + m[13]) / w : 0.0f;
                const float u = 192.0f + scroll + 6.0f * (n[0] * view[0] + n[1] * view[3] + n[2] * view[6]) + kScreenTerm * (sx + kGsOffset);
                const float t = 192.0f + 6.0f * (n[0] * view[1] + n[1] * view[4] + n[2] * view[7]) + kScreenTerm * (sy + kGsOffset);
                v.u = u * texelU;
                v.v = t * texelV;
                v.rgba = 0x80808080u;
            }
            return out;
        }
    }

    namespace
    {
        struct Water
        {
            bozo::render::Material material;
            std::vector<bozo::render::Vertex> vertices;
            std::shared_ptr<const bozo::render::Texture> texture;
            std::array<float, 16> matrix{};
        };

        std::vector<Water> g_water;
        std::unordered_map<uint64_t, std::shared_ptr<const bozo::render::Texture>> g_waterTextures;

        std::shared_ptr<const bozo::render::Texture> waterTexture(const uint8_t *rdram, uint32_t lump)
        {
            const uint64_t key = con::textures::lumpKey(rdram, lump);
            if (const auto found = g_waterTextures.find(key); found != g_waterTextures.end())
                return found->second;
            auto texture = std::make_shared<bozo::render::Texture>();
            if (!con::textures::decodeLump(rdram, lump, *texture) || !texture->width || !texture->height)
                return nullptr;
            for (uint32_t &texel : texture->rgba)
                texel = (texel & 0x00ffffffu) | 0x80000000u;
            texture->key = key ^ 0x5741544552000000ull;
            texture->version = 1;
            if (g_waterTextures.size() >= 16)
                g_waterTextures.clear();
            g_waterTextures[key] = texture;
            return texture;
        }

        uint32_t gpWord(const uint8_t *rdram, const R5900Context *ctx, int32_t offset)
        {
            return guest<uint32_t>(rdram, getRegU32(ctx, 28) + uint32_t(offset));
        }
    }

    void drawWater(const uint8_t *rdram, const R5900Context *ctx)
    {
        const uint32_t chunk = getRegU32(ctx, 18), block = getRegU32(ctx, 19);
        const uint32_t lump = gpWord(rdram, ctx, kWaterTexture);
        if (!block || !lump)
            return;
        const auto texture = waterTexture(rdram, lump);
        if (!texture)
            return;
        const bool second = guest<uint32_t>(rdram, chunk + 0x24) & kWaterSecondSet;
        const uint32_t colour = gpWord(rdram, ctx, second ? kWaterColour2 : kWaterColour) & 0x00ffffffu;
        const int32_t fix = int32_t(gpWord(rdram, ctx, second ? kWaterFix2 : kWaterFix));
        const float scale = guest<float>(rdram, getRegU32(ctx, 28) + uint32_t(second ? kWaterScale2 : kWaterScale));
        const float minX = guest<float>(rdram, chunk + 0x8), minY = guest<float>(rdram, chunk + 0xc), maxX = guest<float>(rdram, chunk + 0x14);
        const float step = std::floor(maxX - minX + 0.5f) * guest<float>(rdram, getRegU32(ctx, 28) + uint32_t(kWaterStep));
        const float originX = std::floor(minX + 0.5f), originY = std::floor(minY + 0.5f);
        const float originZ = float(guest<int32_t>(rdram, chunk + 0x30)) * 0.25f;
        const auto height = [&](uint32_t row, uint32_t column) {
            return float(guest<int16_t>(rdram, block + 0x50 + row * 0x50 + column * 2));
        };
        const float texelU = 1.0f / float(texture->width), texelV = 1.0f / float(texture->height);
        const auto vertex = [&](uint32_t row, uint32_t column) {
            const float h = height(row, column + 1);
            const float previous = row ? height(row - 1, column + 1) : h;
            bozo::render::Vertex out;
            out.x = originX + float(row) * step;
            out.y = originY + float(column) * step;
            out.z = originZ + h / 16.0f * scale;
            out.u = (32.0f + kWaterSlope * (previous - h) / 16.0f) * texelU;
            out.v = (32.0f + kWaterSlope * (height(row, column) - h) / 16.0f) * texelV;
            out.rgba = colour | uint32_t(std::min(std::abs(fix), 0xff)) << 24;
            return out;
        };
        Water water;
        water.texture = texture;
        water.material.tex0 = texture->key;
        water.material.blend = fix < 0 ? bozo::render::Material::Blend::Additive : bozo::render::Material::Blend::Alpha;
        water.material.depthTest = false;
        water.material.depthWrite = false;
        std::memcpy(water.matrix.data(), rdram + kCharacterViewProjection, sizeof(water.matrix));
        for (const uint32_t i : {2u, 6u, 10u, 14u})
            water.matrix[i] *= kCharacterDepth;
        for (uint32_t row = 2; row < 32; ++row)
            for (uint32_t column = 0; column < 30; ++column) {
                const auto a = vertex(row, column), b = vertex(row - 1, column), c = vertex(row, column + 1), d = vertex(row - 1, column + 1);
                water.vertices.insert(water.vertices.end(), {a, b, c, b, c, d});
            }
        g_water.push_back(std::move(water));
    }

    void drawChunk(const uint8_t *rdram, const R5900Context *ctx)
    {
        followLevel(rdram);
        const uint32_t record = getRegU32(ctx, 16);
        if (!guest<int16_t>(rdram, record + 0x34))
            return;
        Chunk chunk;
        chunk.flags = guest<uint32_t>(rdram, record + 0x24);
        chunk.tx = guest<int32_t>(rdram, record + 0x28);
        chunk.ty = guest<int32_t>(rdram, record + 0x2c);
        chunk.tz = guest<int32_t>(rdram, record + 0x30);
        chunk.cosine = guest<int16_t>(rdram, record + 0x36);
        chunk.sine = guest<int16_t>(rdram, record + 0x38);
        auto &slot = g_meshes[record];
        const std::array<int32_t, 3> translation{chunk.tx, chunk.ty, chunk.tz};
        if (!slot || slot->translation != translation)
            slot = build(rdram, record, chunk);
        if (!slot)
            return;
        Queued queued;
        queued.mesh = slot.get();
        std::memcpy(queued.matrix.data(), rdram + kViewProjection, sizeof(queued.matrix));
        if (slot->local.size() == slot->vertices.size()) {
            const auto lights = con::lights::within(con::lights::forChunk(rdram, getRegU32(ctx, 28), record), slot->low, slot->high);
            if (slot->lit.empty() || !(lights == slot->litBy)) {
                static std::vector<uint32_t> colours;
                con::lights::shade(lights, slot->local, slot->stored, colours);
                slot->litBy = lights;
                if (slot->lit.empty())
                    slot->lit = slot->vertices;
                for (size_t i = 0; i < slot->lit.size(); ++i)
                    slot->lit[i].rgba = colours[i];
            }
            queued.lit = &slot->lit;
        }
        if (slot->environment && environmentTexture(rdram, getRegU32(ctx, 28))) {
            queued.environment = environmentVertices(rdram, getRegU32(ctx, 28), *slot, queued.matrix);
            for (size_t i = 0; queued.lit && i < queued.lit->size() && i < queued.environment.size(); ++i)
                queued.environment[i].rgba = (*queued.lit)[i].rgba;
        }
        g_queue.push_back(std::move(queued));
    }

    // A placed model (scenery): its .vif and .tex as the game loaded them, in world space once, drawn with the camera.
    void drawModel(const uint8_t *rdram, const R5900Context *ctx)
    {
        followLevel(rdram);
        const uint32_t record = getRegU32(ctx, 19), data = getRegU32(ctx, 20);
        const uint32_t vif = guest<uint32_t>(rdram, data + 0xa0), tex = guest<uint32_t>(rdram, data + 0xa4);
        if (!vif || !tex)
            return;
        auto &slot = g_meshes[record];
        Chunk chunk;
        chunk.flags = guest<uint32_t>(rdram, record + 0x24);
        chunk.tx = guest<int32_t>(rdram, record + 0x28);
        chunk.ty = guest<int32_t>(rdram, record + 0x2c);
        chunk.tz = guest<int32_t>(rdram, record + 0x30);
        chunk.cosine = guest<int16_t>(rdram, record + 0x36);
        chunk.sine = guest<int16_t>(rdram, record + 0x38);
        const std::array<int32_t, 3> translation{chunk.tx, chunk.ty, chunk.tz};
        if (!slot || slot->translation != translation) {
            slot.reset();
            bozo::render::Texture source;
            if (!con::textures::decodeLump(rdram, tex, source) || !source.width || !source.height)
                return;
            auto mesh = std::make_unique<Mesh>();
            auto texture = std::make_shared<bozo::render::Texture>(std::move(source));
            con::textures::modelBlend(guest<uint64_t>(rdram, vif + 0x18), *texture, mesh->material);
            texture->key = 0x4d4f444500000000ull | vif;
            texture->version = 1;
            mesh->texture = texture;
            mesh->translation = translation;
            mesh->late = guest<uint16_t>(rdram, vif + 0x10) & 0x08;
            mesh->material.tex0 = texture->key;
            const float x0 = float(chunk.tx) * 0.25f, y0 = float(chunk.ty) * 0.25f, z0 = float(chunk.tz) * 0.25f;
            const Placement place = [&](int16_t x, int16_t y, int16_t z, bozo::render::Vertex &v) {
                const float lx = float(x) / 16.0f, ly = float(y) / 16.0f;
                v.z = z0 + float(z) / 16.0f;
                if (chunk.flags & kQuarterTurns) {
                    const int code = chunk.cosine & 7;
                    const float a = code & 1 ? -lx : lx, b = code & 2 ? -ly : ly;
                    v.x = x0 + (code & 4 ? b : a);
                    v.y = y0 + (code & 4 ? a : b);
                    return;
                }
                const float cosine = float(chunk.cosine) / 32767.0f, sine = float(chunk.sine) / 32767.0f;
                v.x = x0 + lx * cosine - ly * sine;
                v.y = y0 + lx * sine + ly * cosine;
            };
            const Turn turn = [&](const std::array<float, 3> &n) -> std::array<float, 3> {
                if (chunk.flags & kQuarterTurns) {
                    const int code = chunk.cosine & 7;
                    const float a = code & 1 ? -n[0] : n[0], b = code & 2 ? -n[1] : n[1];
                    return {code & 4 ? b : a, code & 4 ? a : b, n[2]};
                }
                const float cosine = float(chunk.cosine) / 32767.0f, sine = float(chunk.sine) / 32767.0f;
                return {n[0] * cosine - n[1] * sine, n[0] * sine + n[1] * cosine, n[2]};
            };
            mesh->position = {x0, y0, z0};
            mesh->lighting = guest<uint32_t>(rdram, data + 0xac);
            const int32_t meshes = guest<int8_t>(rdram, vif + 0x12);
            const uint64_t hidden = guest<uint64_t>(rdram, vif + 8);
            const auto meshAt = [&](int32_t i) {
                const uint32_t offset = guest<uint32_t>(rdram, vif + 0x28 + uint32_t(i) * 4);
                return offset < vif ? vif + offset : offset;
            };
            for (int32_t i = 0; i < meshes && i < 64; ++i)
                if (!(hidden >> i & 1))
                    decodeModelMesh(rdram, place, meshAt(i), meshAt(i + 1), float(texture->width), float(texture->height),
                                    mesh->vertices, {turn, &mesh->normals});
            slot = std::move(mesh);
        }
        if (!slot)
            return;
        Queued queued;
        queued.mesh = slot.get();
        std::memcpy(queued.matrix.data(), rdram + kViewProjection, sizeof(queued.matrix));
        if (slot->normals.size() == slot->vertices.size()) {
            const auto lights = con::lights::forScenery(rdram, getRegU32(ctx, 28), slot->position, slot->lighting);
            const auto &m = queued.matrix;
            std::array<float, 3> scale{};
            for (uint32_t k = 0; k < 3; ++k) {
                const float l = std::sqrt(m[k] * m[k] + m[4 + k] * m[4 + k] + m[8 + k] * m[8 + k]);
                scale[k] = l > 0.0f ? 0.99f / l : 0.0f;
            }
            queued.shaded = slot->vertices;
            for (size_t i = 0; i < queued.shaded.size(); ++i) {
                const auto &n = slot->normals[i];
                std::array<float, 3> camera{};
                for (uint32_t k = 0; k < 3; ++k)
                    camera[k] = (m[k] * n[0] + m[4 + k] * n[1] + m[8 + k] * n[2]) * scale[k];
                queued.shaded[i].rgba = con::lights::shade(lights, camera);
            }
        }
        g_queue.push_back(std::move(queued));
    }

    void endFrame(uint32_t clearColour)
    {
        ++g_frames;
        if (g_queue.empty() && g_water.empty() && !(clearColour & 0x00ffffffu) && !g_published && !con::characters::pending() && !con::particles::pending() && !con::shadows::pending() && !con::decals::pending())
            return;
        auto &renderer = bozo::render::Renderer::instance();
        bozo::render::Screen screen;
        screen.originX = -640.0f;
        screen.originY = -224.0f;
        screen.width = 1280.0f;
        screen.height = 448.0f;
        screen.depthMax = 4096.0f;
        renderer.beginFrame(screen);
        if (clearColour & 0x00ffffffu) {
            static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            bozo::render::Material material;
            material.textured = false;
            material.directColour = true;
            material.blend = bozo::render::Material::Blend::Opaque;
            material.depthTest = false;
            material.depthWrite = false;
            const auto corner = [&](float x, float y) {
                bozo::render::Vertex vertex;
                vertex.x = x;
                vertex.y = y;
                vertex.rgba = (clearColour & 0x00ffffffu) | 0x80000000u;
                return vertex;
            };
            const bozo::render::Vertex quad[6] = {corner(-640, -224), corner(640, -224), corner(-640, 224),
                                                  corner(640, -224), corner(640, 224), corner(-640, 224)};
            renderer.draw(material, kIdentity, quad, 6u);
        }
        con::characters::submit(true);
        for (const bool late : {false, true}) {
            for (const Queued &queued : g_queue)
                if (queued.mesh->late == late && !queued.mesh->vertices.empty()) {
                    const auto &vertices = queued.lit ? *queued.lit : queued.shaded.empty() ? queued.mesh->vertices : queued.shaded;
                    if (!queued.environment.empty() && g_environment) {
                        bozo::render::Material environment;
                        environment.tex0 = g_environment->key;
                        environment.blend = bozo::render::Material::Blend::Opaque;
                        renderer.draw(environment, queued.matrix.data(), queued.environment.data(), uint32_t(queued.environment.size()),
                                      g_environment);
                        auto material = queued.mesh->material;
                        material.blend = bozo::render::Material::Blend::DestinationBySource;
                        renderer.draw(material, queued.matrix.data(), vertices.data(), uint32_t(vertices.size()), queued.mesh->texture);
                        continue;
                    }
                    renderer.draw(queued.mesh->material, queued.matrix.data(), vertices.data(), uint32_t(vertices.size()),
                                  queued.mesh->texture);
                }
            if (!late) {
                con::decals::submit();
                con::shadows::submit();
                con::characters::submit(false);
                for (const Water &water : g_water)
                    renderer.draw(water.material, water.matrix.data(), water.vertices.data(), uint32_t(water.vertices.size()),
                                  water.texture);
            }
        }
        con::particles::submit();
        renderer.endFrame();
        g_published = !g_queue.empty() || !g_water.empty() || con::characters::pending() || con::particles::pending() || con::shadows::pending() || con::decals::pending();
        con::characters::clear();
        con::particles::clear();
        con::shadows::clear();
        con::decals::clear();
        g_water.clear();
        g_queue.clear();
    }
}
